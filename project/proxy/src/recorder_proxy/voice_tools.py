import asyncio
from dataclasses import dataclass
import hashlib
import json
import logging
import time

from .providers import ProviderError
from .intelligence_errors import IntelligenceError
from .tools import TOOL_NAMES

logger = logging.getLogger("recorder_proxy.voice_tools")


@dataclass
class Call:
    name: str = ""
    arguments: str = ""
    complete: bool = False
    sent: bool = False


def strict_arguments(raw):
    def pairs(items):
        value = {}
        for key, item in items:
            if key in value:
                raise ValueError()
            value[key] = item
        return value

    return json.loads(raw, object_pairs_hook=pairs,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))


class VoiceTools:
    """Tool work never occupies the provider reader; a generation owns all results."""

    def __init__(self, tools, provider, continuation):
        self.tools, self.provider, self.continuation = tools, provider, continuation
        self.generation = 0
        self.response_id = None
        self.calls = {}
        self.items = {}
        self.events = set()
        self.tasks = set()
        self.finished = False
        self.continued = False
        self.failures = asyncio.Queue(maxsize=1)
        self.turn_calls = 0
        self.retired = {}
        self.outputs_sent = 0
        self.continuations_sent = 0
        self.delta_events = 0

    @property
    def waiting(self):
        return bool(self.calls) and not self.continued

    def start(self, response_id):
        if self.waiting:
            raise ProviderError()
        self._retire()
        self.response_id = response_id
        self.calls, self.items, self.events = {}, {}, set()
        self.finished = self.continued = False
        logger.warning("voice_tools_response_started")

    def _retire(self, cancelled=False):
        if self.response_id:
            self.retired[self.response_id] = None if cancelled else {
                "calls": {key: (call.name, hashlib.sha256(call.arguments.encode()).digest())
                          for key, call in self.calls.items() if call.complete},
                "items": self.items.copy()}
        while len(self.retired) > 32:
            del self.retired[next(iter(self.retired))]

    def finished_response(self, response_id):
        return response_id in self.retired or (response_id == self.response_id and self.finished)

    def cancel(self, *, new_turn=False):
        self._retire(cancelled=True)
        logger.warning("voice_tools_cancel tasks=%d new_turn=%s", len(self.tasks), new_turn)
        self.generation += 1
        for task in self.tasks:
            task.cancel()
        self.calls, self.items, self.events = {}, {}, set()
        self.response_id = None
        self.finished = self.continued = False
        if new_turn:
            self.tools.new_turn()
            self.turn_calls = 0

    def accept(self, event):
        if event.response_id != self.response_id:
            if event.response_id in self.retired:
                previous = self.retired[event.response_id]
                if previous is None:
                    return
                saved = previous["calls"].get(event.call_id or previous["items"].get(event.item_id))
                if (saved and (not event.name or event.name == saved[0])
                        and (event.type != "tool_done" or
                             hashlib.sha256(event.arguments.encode()).digest() == saved[1])):
                    logger.warning("voice_tools_late_duplicate_ignored")
                    return
            raise ProviderError()
        if event.event_id:
            if event.event_id in self.events:
                return
            if len(self.events) >= 512:
                raise ProviderError()
            self.events.add(event.event_id)
        call_id = event.call_id or self.items.get(event.item_id)
        if not call_id:
            raise ProviderError()
        if event.item_id:
            if event.item_id not in self.items and len(self.items) >= 16:
                raise ProviderError()
            if self.items.get(event.item_id, call_id) != call_id:
                raise ProviderError()
            self.items[event.item_id] = call_id
        call = self.calls.get(call_id)
        if call is None:
            if self.finished or self.turn_calls >= 8:
                raise ProviderError()
            self.turn_calls += 1
            call = self.calls[call_id] = Call()
        if event.name:
            if call.name and call.name != event.name:
                raise ProviderError()
            call.name = event.name
        if call.complete:
            if event.type == "tool_done" and event.arguments != call.arguments:
                raise ProviderError()
            return
        if event.type == "tool_delta":
            self.delta_events += 1
            call.arguments += event.arguments
        elif event.type == "tool_added":
            if event.arguments and not call.arguments:
                call.arguments = event.arguments
        else:
            if call.arguments and not event.arguments.startswith(call.arguments):
                raise ProviderError()
            call.arguments = event.arguments
            call.complete = True
        if len(call.arguments.encode()) > 8192:
            raise ProviderError()
        if call.complete:
            if not call.name:
                raise ProviderError()
            logger.warning("voice_tool_call_ready name=%s calls=%d argument_bytes=%d",
                           call.name if call.name in TOOL_NAMES else "unknown",
                           self.turn_calls, len(call.arguments.encode()))
            task = asyncio.create_task(self._execute(call_id, call, self.generation))
            self.tasks.add(task)
            task.add_done_callback(self._task_done)

    def response_done(self, response_id):
        if response_id != self.response_id or self.finished:
            return
        self.finished = True
        logger.warning("voice_tools_response_done calls=%d sent=%d delta_events=%d",
                       len(self.calls), sum(call.sent for call in self.calls.values()), self.delta_events)
        if any(not call.complete for call in self.calls.values()):
            raise ProviderError()
        self.retry_continuation()

    def retry_continuation(self):
        if self.finished and self.waiting:
            task = asyncio.create_task(self._continue(self.generation))
            self.tasks.add(task)
            task.add_done_callback(self._task_done)

    def _task_done(self, task):
        self.tasks.discard(task)
        if not task.cancelled() and task.exception() is not None and not self.failures.full():
            logger.warning("voice_tool_task_failed error_type=%s", type(task.exception()).__name__)
            self.failures.put_nowait(ProviderError())

    async def _execute(self, call_id, call, generation):
        started = time.monotonic()
        try:
            try:
                arguments = strict_arguments(call.arguments)
            except (ValueError, RecursionError):
                arguments = None
            try:
                output = await self.tools.execute(call.name, arguments)
            except IntelligenceError as exc:
                output = json.dumps({"error": exc.code, "retryable": exc.retryable})
            if generation != self.generation:
                return
            if not output:
                raise ProviderError()
            await self.provider.tool_output(call_id, output)
            if generation != self.generation:
                return
            call.sent = True
            self.outputs_sent += 1
            logger.warning("voice_tool_result_sent name=%s count=%d bytes=%d duration_ms=%d",
                           call.name if call.name in TOOL_NAMES else "unknown",
                           self.outputs_sent, len(output.encode()),
                           int((time.monotonic() - started) * 1000))
            await self._continue(generation)
        except ProviderError:
            logger.warning("voice_tool_result_send_failed")
            if not self.failures.full():
                self.failures.put_nowait(ProviderError())

    async def _continue(self, generation):
        if (generation != self.generation or not self.finished or self.continued
                or not self.calls or not all(call.sent for call in self.calls.values())):
            return
        self.continued = True  # Atomic before the first await, including simultaneous tools.
        try:
            sent = await self.continuation()
            if generation != self.generation:
                return
            if sent is False:
                self.continued = False
                logger.warning("voice_tools_continuation_deferred")
            else:
                self.continuations_sent += 1
                logger.warning("voice_tools_continuation_sent count=%d", self.continuations_sent)
        except ProviderError:
            if not self.failures.full():
                self.failures.put_nowait(ProviderError())

    async def watch(self):
        raise await self.failures.get()

    async def close(self):
        self.cancel()
        await asyncio.gather(*self.tasks, return_exceptions=True)
