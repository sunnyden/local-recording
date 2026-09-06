import numpy as np
import soxr


class AudioError(Exception):
    pass


class OutputAudio:
    """Stateful, band-limited streaming conversion; reset only at item boundaries."""

    def __init__(self, input_rate=24000):
        if input_rate not in (16000, 24000):
            raise AudioError("Unsupported provider sample rate")
        self.resampler = (soxr.ResampleStream(input_rate, 16000, 1, dtype="int16", quality="HQ")
                          if input_rate != 16000 else None)
        self.pending = bytearray()
        self.finished = False

    def feed(self, pcm=b"", final=False):
        if self.finished or len(pcm) % 2 or len(pcm) > 65536:
            raise AudioError("Invalid provider PCM")
        samples = np.frombuffer(pcm, dtype="<i2").astype(np.int16, copy=False)
        if self.resampler:
            samples = self.resampler.resample_chunk(samples, last=final)
        self.pending.extend(samples.astype("<i2", copy=False).tobytes())
        packets = []
        while len(self.pending) >= 640:
            packets.append(bytes(self.pending[:640]))
            del self.pending[:640]
        if final:
            if self.pending:
                packets.append(bytes(self.pending))
                self.pending.clear()
            self.finished = True
        return packets
