# ESP32 Deepgram Voice Agent

[![build](https://github.com/dg-edcharbeneau/esp32-deepgram-voice-agent/actions/workflows/build.yml/badge.svg)](https://github.com/dg-edcharbeneau/esp32-deepgram-voice-agent/actions/workflows/build.yml)
[![license](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5.5-red.svg)](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/index.html)

A complete voice agent running on a $40 dev board: **mic → Deepgram (STT / LLM /
TTS) → speaker**, over a single WebSocket, with a 466×466 AMOLED that reacts to
the conversation. No phone, no laptop, no cloud glue in between — the board holds
the session itself.

<!-- Uncomment once docs/images/device-orb.jpg exists -- see docs/images/README.md
     for the shot list. A broken image renders worse than no image, so this stays
     commented until the photo lands.
<p align="center">
  <img src="docs/images/device-orb.jpg" alt="The board mid-conversation, orb face lit" width="420">
</p>
-->

Talk to it and it answers. Ask it to change its voice, its name, its colour, or
its face, and it does — the agent has tools wired to the device's own hardware.

## The hardware

A **Waveshare ESP32-S3-Touch-AMOLED-1.75C**, unmodified and out of the box:

| | |
|---|---|
| Display | 466×466 round AMOLED over QSPI, capacitive touch |
| Microphone | ES7210 |
| Speaker | ES8311 |
| Both codecs | one duplex I2S peripheral |
| Extra input | BOOT button on GPIO 0 |

## Quick start — flash a release

You do not need a toolchain to try it. Download
`deepgram_agent-merged.bin` from the
[latest release](https://github.com/dg-edcharbeneau/esp32-deepgram-voice-agent/releases/latest),
install [esptool](https://github.com/espressif/esptool) (`pip install esptool`),
put the board in download mode, and:

```bash
esptool.py -c esp32s3 -p /dev/YOUR-PORT write_flash 0x0 deepgram_agent-merged.bin
```

Released images carry **no API key and no Wi-Fi network** — see the next section.

## First boot

With nothing saved, the device raises its own WPA2 access point and shows a QR
code on the panel. Scan it to join (the passphrase is random per boot and travels
inside the QR code), and a captive portal asks for your Wi-Fi network and your
[Deepgram API key](https://console.deepgram.com/). It saves both, reboots, and
starts talking.

There are four ways credentials can get in, and one precedence rule between them:
**[docs/wifi-setup.md](docs/wifi-setup.md)**.

## Build from source

```bash
. /path/to/esp-idf/export.sh          # built and verified against v5.5.5
idf.py set-target esp32s3
idf.py build
idf.py flash
idf.py -b 2000000 monitor
```

Everything in `menuconfig` → *Deepgram Agent Device* is optional; the portal asks
for what matters. `sdkconfig` is gitignored, so a key seeded there does not reach
git. Full detail, including the port-naming table and the 2 000 000 baud console:
**[docs/build.md](docs/build.md)**.

## What a healthy boot looks like

Deepgram speaks `agent.greeting` as soon as it applies the `Settings` message, so
a session produces audio with no microphone attached at all — the greeting
round-trips through the LLM and TTS and comes back as PCM. That is the
end-to-end proof, and it splits the failure modes in half: a boot that plays the
greeting but never answers you has a capture problem, not a session problem.

```
I (3456) wifi: got ip 192.168.1.87
I (4805) dg_agent: sent Settings (412 bytes)
I (5300) dg_agent: SettingsApplied -- session is live
I (7405) main: turn complete, 96000 audio bytes received
I (13400) main: ready | turns=1 mic=64000 B rx=96000 B played=96000 B dropped=0 B
```

The per-second telemetry line localises a fault to one stage. How to read it:
**[docs/verifying-a-boot.md](docs/verifying-a-boot.md)**.

## What is on the screen

Two interchangeable **faces**, switched by asking out loud: an **orb** of 456
depth-sorted dots whose behaviour tracks the session state, and a **radial
spectrum analyser** of 48 FFT bars. Both are drawn under a fixed RAM budget that
the docs explain rather than assume — **[docs/display.md](docs/display.md)**.

## Documentation

| | |
|---|---|
| [docs/architecture.md](docs/architecture.md) | the module map: what runs on which task, and the one rule the layout defends |
| [docs/build.md](docs/build.md) | prerequisites, menuconfig, ports, and where every file lives |
| [docs/wifi-setup.md](docs/wifi-setup.md) | every way to get credentials onto the device, and why it will not connect |
| [docs/verifying-a-boot.md](docs/verifying-a-boot.md) | reading the serial log and the counters |
| [docs/display.md](docs/display.md) | the two faces, their RAM budget, and the display test |
| [docs/audio-path.md](docs/audio-path.md) | mic to Deepgram to speaker: tasks, ring buffer, echo gate, BSP init-order trap |
| [docs/session-control.md](docs/session-control.md) | button, touch, the control task, and the idle timeout |
| [docs/voice-commands.md](docs/voice-commands.md) | what the agent can change about itself, and what each change costs |
| [docs/flux.md](docs/flux.md) | the Flux speech stack on this board |
| [docs/protocol-notes.md](docs/protocol-notes.md) | Agent API and WebSocket lessons worth keeping |
| [docs/tuning.md](docs/tuning.md) | speaker volume and the knobs worth turning |
| [docs/notes/](docs/notes/) | historical findings: echo cancellation, `UpdateSpeak`, an upstream WebSocket bug |

## Host harnesses

Three things run on a laptop with no board attached, and two of them are gates in
CI:

```bash
./host/run.sh        # diffs the C orb geometry against the upstream TypeScript, 38,304 numbers a run
./host/prompt.sh     # prints the assembled system prompt, exactly as the device sends it
./host/portal.sh     # opens the captive-portal page in a browser
```

See [host/README.md](host/README.md). The prompt itself is
[one markdown file per block](main/prompt/) — edit those, not a Kconfig string.

## Contributing

Bug reports and pull requests are welcome. [CONTRIBUTING.md](CONTRIBUTING.md)
covers the parts you cannot guess: the exact ESP-IDF version, why `sdkconfig` is
untracked, and how to re-apply the two local patches in
[components/tcp_transport/](components/tcp_transport/) after an IDF bump.

Security issues go to security@deepgram.com rather than the issue tracker — see
[SECURITY.md](SECURITY.md), which also documents exactly where the API key lives.

## License

Apache-2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
