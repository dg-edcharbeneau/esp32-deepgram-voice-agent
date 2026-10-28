#!/usr/bin/env python3
"""Turn a CONFIG_AUDIO_RECORD dump in a serial log back into audio.

    host/rec2wav.py capture.log [out.wav]

Writes a two-channel 16 kHz WAV -- left the raw microphone before echo
cancellation, right exactly what was sent to Deepgram (silence where the uplink
VAD held a block back) -- and prints a per-second timeline aligned to the
server's "seconds into call", which counts from Deepgram's Welcome.

Takes the LAST complete recording in the log. Lines are numbered on the device,
so a block lost on the serial link is reported rather than silently closing up
the timeline: it is written as silence in both channels.
"""
import base64
import re
import struct
import sys
import wave

FLAG_PLAYBACK = 0x01
FLAG_SENT = 0x02


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    text = open(sys.argv[1], "rb").read().decode("latin-1").replace("\r", "")

    begin = None
    for m in re.finditer(r"REC begin rid=(\S+) blocks=(\d+) frames=(\d+) rate=(\d+) "
                         r"start_us=(-?\d+) welcome_us=(-?\d+)", text):
        begin = m
    if begin is None:
        sys.exit("no 'REC begin' in that log")
    rid = begin.group(1)
    blocks, frames, rate = int(begin.group(2)), int(begin.group(3)), int(begin.group(4))
    start_us, welcome_us = int(begin.group(5)), int(begin.group(6))
    body = text[begin.end():]
    end = body.find("REC end")
    if end < 0:
        print("warning: no 'REC end' -- the capture stopped mid-dump", file=sys.stderr)
    else:
        body = body[:end]

    nbytes = frames * 2
    raw = {}
    sent = {}
    flags = {}
    for m in re.finditer(r"^REC (\d+) (\d+) (\S+) (\S+)$", body, re.M):
        i = int(m.group(1))
        try:
            a = base64.b64decode(m.group(3), validate=True)
            b = base64.b64decode(m.group(4), validate=True)
        except Exception:
            continue
        if len(a) != nbytes or len(b) != nbytes:
            continue
        raw[i], sent[i], flags[i] = a, b, int(m.group(2))

    missing = [i for i in range(blocks) if i not in raw]
    silence = b"\x00" * nbytes
    out = sys.argv[2] if len(sys.argv) > 2 else f"rec-{rid}.wav"
    with wave.open(out, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames_out = bytearray()
        for i in range(blocks):
            ra = struct.unpack(f"<{frames}h", raw.get(i, silence))
            sb = struct.unpack(f"<{frames}h", sent.get(i, silence))
            for x, y in zip(ra, sb):
                frames_out += struct.pack("<hh", x, y)
        w.writeframes(bytes(frames_out))

    block_s = frames / rate
    offset = (start_us - welcome_us) / 1e6 if welcome_us > 0 else 0.0
    print(f"request_id   {rid}")
    print(f"wrote        {out}  ({blocks * block_s:.1f} s, L = raw mic, R = sent to Deepgram)")
    print(f"alignment    recording t=0 is {offset:+.2f} s into the call (after Welcome)")
    if missing:
        print(f"warning      {len(missing)} block(s) lost on the serial link, written as silence")

    def peak(buf):
        v = struct.unpack(f"<{frames}h", buf)
        return max(abs(s) for s in v)

    print("\n  call s   playback  sent/blocks  raw peak  sent peak")
    per = int(round(1 / block_s))
    for start in range(0, blocks, per):
        idx = [i for i in range(start, min(start + per, blocks)) if i in raw]
        if not idx:
            continue
        pb = sum(1 for i in idx if flags[i] & FLAG_PLAYBACK)
        sn = sum(1 for i in idx if flags[i] & FLAG_SENT)
        rp = max(peak(raw[i]) for i in idx)
        sp = max(peak(sent[i]) for i in idx)
        t = start * block_s + offset
        print(f"  {t:6.1f}   {'yes' if pb else ' - ':>8}  {sn:>3}/{len(idx):<3}     "
              f"{rp:>7}  {sp:>9}")


if __name__ == "__main__":
    main()
