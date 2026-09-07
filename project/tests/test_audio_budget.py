"""Protocol-level budget test for the firmware's continuous four-period DMA."""
from collections import deque
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "proxy" / "src"))
from recorder_proxy.config import Settings


def simulate(credit_samples, frames=100, delivery_delay_ticks=0):
    software = deque()
    wire = deque()
    dma = deque([None] * 4)
    sent = played = reported = gaps = peak_software = 0
    next_send = -2
    started = False
    prefill_frames = min(credit_samples, Settings.startup_prefill_samples) // 320
    for tick in range(frames * 10):
        # A completed DMA period is refilled now but will play four periods later.
        if tick:
            packet = dma.popleft()
            if packet is not None:
                played += 1
                started = True
            elif started and played < frames:
                gaps += 1
            dma.append(software.popleft() if software else None)
        if tick % 4 == 0:
            reported = played
        # Proxy sends one bounded prefill, then paces with at most 60 ms catch-up.
        while (sent < frames and (sent - reported + 1) * 320 <= credit_samples
               and (sent < prefill_frames or max(next_send, tick - 2) <= tick)):
            wire.append(sent)
            sent += 1
            if sent >= prefill_frames:
                next_send = max(next_send, tick - 2) + 1
        if tick >= delivery_delay_ticks:
            software.extend(wire)
            wire.clear()
            peak_software = max(peak_software, len(software))
        if played == frames:
            break
    return played, gaps, peak_software


def test_insufficient_credit_reproduces_dma_starvation():
    played, gaps, peak = simulate(1600)
    assert played == 100
    assert gaps > 0
    assert peak <= 5


def test_production_credit_preserves_continuous_dma_playback():
    played, gaps, peak = simulate(Settings.max_unplayed_samples)
    assert played == 100
    assert gaps == 0, "Credit must cover DMA residence plus the 80 ms played-progress delay."
    assert peak <= 50, "Traffic must fit the fixed 50-frame software ring."


def test_tcp_coalescing_can_deliver_the_entire_advertised_credit():
    played, gaps, peak = simulate(Settings.max_unplayed_samples, delivery_delay_ticks=8)
    assert played == 100
    assert gaps == 0
    assert peak <= 50, "A delayed TCP segment must fit the full fixed device ring."
