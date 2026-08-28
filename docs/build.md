# Configure and build

Prerequisites, menuconfig options that matter, and where everything lives.

## Configure and build

**Nothing here needs a toolchain any more.** Leave the SSID, password and API
key empty and the device raises a setup portal on first boot that asks for all
three. The Kconfig values are first-boot seeds only, and anything saved beats
them. See **[docs/wifi-setup.md](wifi-setup.md)** for all four ways credentials get
in, and for what to do when it will not connect.

The provisioning AP is WPA2, and its passphrase reaches the phone inside the QR
code on the panel — it is encrypted because the portal carries the API key, which
unlike a Wi-Fi password works from anywhere and bills to you.

`sdkconfig` is gitignored, so a key seeded there does not reach git.

```bash
. /path/to/esp-idf/export.sh          # built and verified against v5.5.5
idf.py set-target esp32s3
idf.py menuconfig      # -> "Deepgram Agent Device" (all of it optional: the portal asks)
idf.py build
idf.py flash
idf.py -b 2000000 monitor
```

The console runs at **2 000 000 baud**, matching `spec_analyzer_radial` so the
same monitor command works for both projects.

**No `-p` above on purpose.** ESP-IDF finds the port itself when one board is
attached, which is the only form that reads the same on every OS. With more than
one attached, name it once rather than on every command:

| OS | Name the port once |
|---|---|
| macOS | `export ESPPORT=/dev/cu.usbmodem101` |
| Linux | `export ESPPORT=/dev/ttyACM0` |
| Windows (PowerShell) | `$env:ESPPORT = "COM3"` |
| Windows (cmd) | `set ESPPORT=COM3` |

This board is **native USB CDC**, not a UART bridge, so on Linux it appears as
`ttyACM*` — `ttyUSB*` is for boards with a CP210x or FTDI chip, and looking for
it here finds nothing. Same reason the macOS name is `cu.usbmodem*` rather than
`cu.SLAB_USBtoUART`. A port that exists but will not open on Linux is almost
always group membership: `sudo usermod -aG dialout $USER`, then log out and back
in.

`-p` still works per command and beats `ESPPORT` when set.

## Layout

| File | Role |
|---|---|
| [main/main.c](../main/main.c) | boot order, session callbacks, status loop |
| [main/wifi_sta.c](../main/wifi_sta.c) | station bring-up, blocks on `IP_EVENT_STA_GOT_IP` |
| [main/wifi_creds.c](../main/wifi_creds.c) | credentials in NVS, and why a saved network beats Kconfig |
| [main/api_key.c](../main/api_key.c) | the Deepgram API key in NVS, same precedence rule, never logged |
| [main/wifi_prov.c](../main/wifi_prov.c) | setup portal: SoftAP, the page, captive-portal DNS |
| [main/boot_button.c](../main/boot_button.c) | BOOT/GPIO 0: click toggles, 3 s hold forgets the network |
| [main/dg_agent.c](../main/dg_agent.c) | Agent API client: `Settings`, event decoding, KeepAlive |
| [main/agent_name.c](../main/agent_name.c) | what the agent is called: NVS, validation, and the factory default |
| [main/agent_prompt.c](../main/agent_prompt.c) | assembles the persona in PSRAM: block order, build gating, `{{placeholders}}` |
| [main/prompt/](../main/prompt/) | the persona itself, one `.md` per named block — edit these, not a Kconfig string |
| [main/audio_io.c](../main/audio_io.c) | both codecs: ES7210 capture, ES8311 playback, mono↔stereo, gating |
| [main/ui.c](../main/ui.c) | panel and touch bring-up, status label, QR overlay, frame timer, audio levels, face dispatch |
| [main/ui_face.h](../main/ui_face.h) | the face vtable and per-frame render context |
| [main/face_orb.c](../main/face_orb.c) | orb face: behaviour selection and the 280 ms crossfade |
| [main/orb_geometry.c](../main/orb_geometry.c) | the shell's maths. No LVGL, no ESP-IDF — compiles on the host so `host/run.sh` can diff it against upstream |
| [main/orb_raster.c](../main/orb_raster.c) | dots to RGB565: coverage-normalised blend, per-channel colour tint, dirty-rect clear, zero per-frame allocation |
| [main/face_spectrum.c](../main/face_spectrum.c) | spectrum face: FFT, sample handoff, ring render. Initialises lazily |
| [main/faces.c](../main/faces.c) | the face catalog, LVGL-free so `dg_agent` can build the `set_face` schema |
| [main/orb_colors.c](../main/orb_colors.c) | the orb colour catalog, same split, for the `set_color` schema |
| [host/](../host/) | off-device harnesses: geometry parity against the upstream TypeScript, and `prompt.sh` to print the assembled prompt |
| [main/session_ctl.c](../main/session_ctl.c) | stop/start worker: teardown order, gesture requests |
| [main/Kconfig.projbuild](../main/Kconfig.projbuild) | name / greeting / audio / display, and the Wi-Fi and API key seeds |
| [docs/wifi-setup.md](wifi-setup.md) | every way to get credentials onto the device, and why it will not connect |
| [sdkconfig.defaults](../sdkconfig.defaults) | board hardware, TLS, Wi-Fi buffer sizing |
| [components/tcp_transport/](../components/tcp_transport/) | two local patches to IDF's WS transport: the handshake Host header, and dropping a congested audio frame instead of killing the session — see [protocol-notes.md](protocol-notes.md) |

---

[Back to the README](../README.md)
