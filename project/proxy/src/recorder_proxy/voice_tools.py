import asyncio
from dataclasses import dataclass
import json

from .providers import ProviderError


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

    @property
    def waiting(self):
        return bool(self.calls) and not self.continued

    def start(self, response_id):
        if self.waiting:
            raise ProviderError()
        self.response_id = response_id
        self.calls, self.items, self.events = {}, {}, set()
        self.finished = self.continued = False

    def cancel(self, *, new_turn=False):
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
            task = asyncio.create_task(self._execute(call_id, call, self.generation))
            self.tasks.add(task)
            task.add_done_callback(self._task_done)

    def response_done(self, response_id):
        if response_id != self.response_id:
            return
        self.finished = True
        if any(not call.complete for call in self.calls.values()):
            raise ProviderError()
        if self.calls:
            task = asyncio.create_task(self._continue(self.generation))
            self.tasks.add(task)
            task.add_done_callback(self._task_done)

    def _task_done(self, task):
        self.tasks.discard(task)
        if not task.cancelled() and task.exception() is not None and not self.failures.full():
            self.failures.put_nowait(ProviderError())

    async def _execute(self, call_id, call, generation):
        try:
            try:
                arguments = strict_arguments(call.arguments)
            except (ValueError, RecursionError):
                arguments = None
            output = await self.tools.execute(call.name, arguments)
            if generation != self.generation:
                return
            await self.provider.tool_output(call_id, output)
            if generation != self.generation:
                return
            call.sent = True
            await self._continue(generation)
        except ProviderError:
            if not self.failures.full():
                self.failures.put_nowait(ProviderError())

    async def _continue(self, generation):
        if (generation != self.generation or not self.finished or self.continued
                or not self.calls or not all(call.sent for call in self.calls.values())):
            return
        self.continued = True  # Atomic before the first await, including simultaneous tools.
        try:
            await self.continuation()
        except ProviderError:
            if not self.failures.full():
                self.failures.put_nowait(ProviderError())

    async def watch(self):
        raise await self.failures.get()

    async def close(self):
        self.cancel()
        await asyncio.gather(*self.tasks, return_exceptions=True)
