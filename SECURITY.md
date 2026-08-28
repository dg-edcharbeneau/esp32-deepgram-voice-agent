# Security

This is a sample application for the Waveshare ESP32-S3-Touch-AMOLED-1.75C. It
is meant to be read and modified, not shipped as-is, so this document is mostly
about *where the secrets are* — what a fork of this code inherits, and what it
would have to change before putting a device somewhere untrusted.

## Reporting a vulnerability

Please do not open a public GitHub issue for a security problem. Report it to
Deepgram at **security@deepgram.com**. Include the commit SHA and, if the problem
is on the device, the serial log around the failure.

For a bug in ESP-IDF or in the Espressif/Waveshare components this project
depends on, report it upstream — those are fetched at build time and not
redistributed here. The one exception is the vendored
`components/tcp_transport/transport_ws.c` (see `NOTICE`); a problem in the two
local patches is ours.

## Where the secrets live

**The Deepgram API key** is stored in plaintext in NVS, namespace `deepgram`,
key `apikey` ([main/api_key.c](main/api_key.c)). It is deliberately in its own
namespace rather than sharing `wifi`, because the two are erased on different
occasions — a 3-second BOOT press forgets the network, and that must not cost
the key.

**Wi-Fi credentials** are in NVS namespace `wifi`
([main/wifi_creds.c](main/wifi_creds.c)), also plaintext.

Neither NVS encryption, flash encryption, nor secure boot is enabled in
[sdkconfig.defaults](sdkconfig.defaults). On this board that means **anyone with
physical access and a USB cable can read the API key and the Wi-Fi password out
of flash.** That is an acceptable trade for a development sample and is not
acceptable for a deployed device; a fork intended for one should enable NVS
encryption at minimum, and flash encryption plus secure boot if the device
leaves your desk.

## A built image can itself be a secret

`CONFIG_DEEPGRAM_API_KEY` and `CONFIG_WIFI_SSID`/`CONFIG_WIFI_PASSWORD` are
menuconfig *seeds*: if set, they are compiled into the firmware and used only
when NVS has nothing saved. So:

- **Never commit a `sdkconfig`.** It is gitignored for exactly this reason — it
  is generated, and it will contain whatever you typed into menuconfig. Only
  `sdkconfig.defaults` is tracked, and it sets no key and no SSID.
- **Do not share a `.bin` you built with a seed set.** `strings` finds it.
- Release artifacts published by this repo are built from tracked defaults in
  CI, so they contain no key and no network — they come up in provisioning mode.

## The provisioning portal

On first boot, or after the network is forgotten, the device runs a softAP and
serves a captive-portal page ([main/wifi_prov.c](main/wifi_prov.c)).

- The AP is **WPA2-PSK**, not open. The passphrase is 9 characters drawn from
  `esp_random()` (the hardware RNG, seeded by then because Wi-Fi is up) over an
  alphabet with the ambiguous glyphs removed, and it is **regenerated on every
  boot**. It is shown on the panel, which is the only place it exists — it is
  not derived from the MAC, because anything derived from the MAC is guessable
  by whoever can hear the beacon and would leave the encryption doing no work.
- The AP SSID *is* MAC-derived (`dg-agent-XXXX`), which is fine — it is a name.
- The portal itself is plain HTTP, protected by the WPA2 link rather than TLS.
  The Wi-Fi password and the API key arrive in a POST **body**, never in a
  request line, so they do not land in a URI or any log that records one.
- The portal exits after five minutes idle and restarts the device, so a unit
  whose router was merely rebooting does not sit in AP mode indefinitely.

## In flight

The session runs over `wss://agent.deepgram.com/v1/agent/converse`, with TLS
certificates verified against the ESP-IDF certificate bundle
(`esp_crt_bundle_attach`, [main/dg_agent.c](main/dg_agent.c)). Certificate
verification is not disabled anywhere in this project, and enabling
`CONFIG_ESP_TLS_INSECURE` to work around a handshake failure is not a fix.

`CONFIG_DEEPGRAM_LOG_WIRE_JSON` prints the WebSocket JSON to the serial console.
It is off by default and is a debugging aid; the frames include conversation
transcripts, so treat a log captured with it on as containing whatever was said.

## Audio and privacy

The microphone is gated rather than always-streaming, but while a session is
live, captured audio is sent to Deepgram for transcription. Conversation
transcripts also pass through the LLM. Anyone deploying this in a space where
other people can be overheard needs to say so; see Deepgram's privacy
documentation for what happens to audio server-side.
