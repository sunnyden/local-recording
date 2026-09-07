import asyncio

import anyio


async def finish_task(task):
    """Join owned work despite repeated cancellation, then preserve cancellation."""
    completed = asyncio.gather(task, return_exceptions=True)
    cancelled = None
    with anyio.CancelScope(shield=True):
        while not completed.done():
            try:
                await asyncio.shield(completed)
            except asyncio.CancelledError as exc:
                if cancelled is None:
                    cancelled = exc
        if cancelled is not None:
            raise cancelled
        return task.result()


async def cancel_tasks(tasks):
    tasks = tuple(tasks)
    for task in tasks:
        if not task.done() and not task.cancelling():
            task.cancel()
    await finish_task(asyncio.gather(*tasks, return_exceptions=True))
