#!/usr/bin/env python3
"""WSPR receiver for the ATS-Mini radio.

The radio streams its 6 kHz audio over /ws (audio=1). This script cuts it into 2-minute windows aligned to
even UTC minutes, writes a 12 kHz WAV, decodes it with WSJT-X's `wsprd` (run on a host that has it, e.g. over
ssh) and posts each spot back to the radio (/api/ft8tx action=wsprspot) so the web page lists them.

The radio must be in FT8 mode (USB) tuned to DIAL_KHZ; the script shifts the audio so the WSPR band
(dial 14095.6 kHz + 1400..1600 Hz) lands where wsprd expects it (1400..1600 Hz).

  python3 tools/wspr_rx.py --radio 192.168.3.39 --ssh pi@192.168.3.24 --dial-khz 14096
"""
import argparse
import asyncio
import subprocess
import sys
import tempfile
import time
import wave

import numpy as np
import requests
import websockets

AUD_RATE = 6000
WSPR_DIAL_MHZ = 14.0956   # nominal WSPR dial for 20 m (wsprd adds the audio frequency)
WIN_S = 114               # wsprd reads 114 s


def ulaw_table():
    t = np.zeros(256, dtype=np.float32)
    for i in range(256):
        u = ~i & 0xFF
        e = (u >> 4) & 7
        m = ((((u & 0x0F) << 3) + 0x84) << e) - 0x84
        t[i] = (-m if u & 0x80 else m) / 32768.0
    return t


ULAW = ulaw_table()


class AudioBuf:
    """Samples by absolute sample number, plus a map from sample number to UTC."""

    def __init__(self):
        self.data = {}          # chunk start -> float32 array
        self.offsets = []       # t_arrival - end_sample / rate, per frame

    def add(self, start, ulaw_bytes, t_arr):
        self.data[start] = ULAW[np.frombuffer(ulaw_bytes, dtype=np.uint8)]
        self.offsets.append(t_arr - (start + len(ulaw_bytes)) / AUD_RATE)
        if len(self.offsets) > 400:
            self.offsets = self.offsets[-400:]

    def clock(self):
        """UTC time of sample 0 (the minimum offset is the frame with the least latency)."""
        return float(np.min(self.offsets)) if self.offsets else None

    def extract(self, t0, seconds):
        """Samples for [t0, t0+seconds) UTC; gaps stay zero. None if nothing usable."""
        base = self.clock()
        n = int(seconds * AUD_RATE)
        first = int(round((t0 - base) * AUD_RATE))
        out = np.zeros(n, dtype=np.float32)
        got = 0
        for start, arr in self.data.items():
            lo, hi = max(start, first), min(start + len(arr), first + n)
            if hi > lo:
                out[lo - first:hi - first] = arr[lo - start:hi - start]
                got += hi - lo
        for start in [s for s, a in self.data.items() if s + len(a) < first - AUD_RATE]:
            del self.data[start]
        return out, got / n


def shift_and_upsample(x, shift_hz):
    """6 kHz real audio -> 12 kHz real audio with every frequency moved up by shift_hz."""
    n = len(x)
    spec = np.fft.fft(x)
    h = np.zeros(n)
    h[0] = 1
    if n % 2 == 0:
        h[n // 2] = 1
        h[1:n // 2] = 2
    else:
        h[1:(n + 1) // 2] = 2
    analytic = np.fft.ifft(spec * h)
    t = np.arange(n) / AUD_RATE
    y = (analytic * np.exp(2j * np.pi * shift_hz * t)).real
    up = np.interp(np.arange(2 * n) / 2.0, np.arange(n), y)
    return up


def write_wav(path, x):
    peak = float(np.max(np.abs(x))) or 1.0
    pcm = np.clip(x / peak * 20000, -32768, 32767).astype(np.int16)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(12000)
        w.writeframes(pcm.tobytes())


def decode(path, ssh):
    if ssh:
        subprocess.run(["scp", "-q", path, f"{ssh}:/tmp/wspr_rx.wav"], check=True, timeout=60)
        cmd = ["ssh", ssh, f"cd /tmp && wsprd -f {WSPR_DIAL_MHZ} wspr_rx.wav"]
    else:
        cmd = ["wsprd", "-f", str(WSPR_DIAL_MHZ), path]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=240)
    spots = []
    for line in r.stdout.splitlines():
        p = line.split()
        # time snr dt freq(MHz) drift call grid dBm   (the last line is '<DecodeFinished>')
        if len(p) >= 8 and p[0].isdigit():
            spots.append(f"{p[0]} {p[1]} {p[2]} {float(p[3]) * 1000:.3f} {p[5]} {p[6]} {p[7]}")
    return spots


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radio", default="192.168.3.39")
    ap.add_argument("--ssh", default="", help="user@host that has wsprd (empty = run wsprd here)")
    ap.add_argument("--dial-khz", type=float, default=14096.0, help="the radio's dial (USB)")
    ap.add_argument("--once", action="store_true", help="decode one window and exit")
    a = ap.parse_args()

    shift = 1500.0 - ((14097.1 - a.dial_khz) * 1000.0)
    print(f"dial {a.dial_khz} kHz USB: WSPR centre at {(14097.1 - a.dial_khz) * 1000:.0f} Hz audio, shift {shift:+.0f} Hz")

    buf = AudioBuf()

    async def reader():
        async for msg in ws:
            if isinstance(msg, bytes) and len(msg) > 5 and msg[0] == 0x41:   # 'A'
                start = int.from_bytes(msg[1:5], "little")
                buf.add(start, msg[5:], time.time())

    async with websockets.connect(f"ws://{a.radio}/ws", max_size=None) as ws:
        await ws.send("audio=1")
        task = asyncio.create_task(reader())
        while True:
            now = time.time()
            t0 = (now // 120 + 1) * 120    # next even minute
            if t0 - now < 1:
                t0 += 120
            print(f"waiting for {time.strftime('%H:%M:%S', time.gmtime(t0))} UTC, window {WIN_S} s")
            await asyncio.sleep(t0 + WIN_S + 2 - time.time())
            if buf.clock() is None:
                print("no audio received (is the radio in FT8 mode?)")
                continue
            x, frac = buf.extract(t0, WIN_S)
            print(f"audio coverage {frac * 100:.0f}%")
            if frac < 0.6:
                continue
            with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
                path = f.name
            write_wav(path, shift_and_upsample(x, shift))
            try:
                spots = await asyncio.get_running_loop().run_in_executor(None, decode, path, a.ssh)
            except Exception as e:
                print("decode failed:", e)
                continue
            print(f"{len(spots)} spot(s)")
            for s in spots:
                print("  ", s)
                try:
                    requests.post(f"http://{a.radio}/api/ft8tx", data={"action": "wsprspot", "s": s}, timeout=8)
                except Exception as e:
                    print("post failed:", e)
            if a.once:
                task.cancel()
                return


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
