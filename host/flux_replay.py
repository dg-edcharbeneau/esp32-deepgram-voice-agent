#!/usr/bin/env python3
"""Stream a 16 kHz mono WAV to Flux (/v2/listen) in real time and print every
turn event with its time in the file.

    flux_replay.py file.wav [offset_s]

offset_s is added to every printed time so it reads on the same clock as the
session's "seconds into call" (rec2wav prints it as the alignment).
The key comes from DEEPGRAM_API_KEY and is never printed.
"""
import asyncio
import json
import os
import sys
import time
import wave

import websockets

CHUNK_MS = 80


async def main(path, offset):
    key = os.environ["DEEPGRAM_API_KEY"]
    w = wave.open(path, "rb")
    assert w.getnchannels() == 1 and w.getsampwidth() == 2, "need 16-bit mono"
    rate = w.getframerate()
    pcm = w.readframes(w.getnframes())
    w.close()
    url = (f"wss://api.deepgram.com/v2/listen?model=flux-general-en"
           f"&encoding=linear16&sample_rate={rate}")
    step = rate * 2 * CHUNK_MS // 1000
    async with websockets.connect(url, additional_headers={"Authorization": f"Token {key}"},
                                  max_size=None) as ws:
        t0 = time.monotonic()

        async def send():
            for i in range(0, len(pcm), step):
                await ws.send(pcm[i:i + step])
                target = t0 + (i + step) / (rate * 2)
                delay = target - time.monotonic()
                if delay > 0:
                    await asyncio.sleep(delay)
            await asyncio.sleep(3)
            await ws.send(json.dumps({"type": "CloseStream"}))

        async def recv():
            async for raw in ws:
                if isinstance(raw, bytes):
                    continue
                m = json.loads(raw)
                ev = m.get("event") or m.get("type")
                if ev == "Update":
                    continue
                ws0 = m.get("audio_window_start")
                ws1 = m.get("audio_window_end")
                tr = (m.get("transcript") or "").strip()
                when = f"{ws0 + offset:6.2f}-{ws1 + offset:6.2f}s" if ws0 is not None else "      -       "
                rid = m.get("request_id")
                extra = f" request_id={rid}" if rid else ""
                print(f"  {when}  {ev:<16} {tr[:70]!r}{extra}", flush=True)

        await asyncio.gather(send(), recv())


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 0.0))
