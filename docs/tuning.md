# Tuning

Speaker volume, and the knobs worth turning once the thing works.

## Speaker volume

`AUDIO_OUT_VOLUME` is 100, and it is worth knowing what that does and does not
mean.

`esp_codec_dev`'s default volume curve maps 0-100 onto **-50 dB to 0 dB**, so 100
is unity gain, not the loudest the hardware goes. The value then passes through
`hw_gain` = `20*log10(codec_dac_voltage / pa_voltage)` = `20*log10(3.3/5.0)` =
**-3.6 dB**, which `es8311_set_vol()` *subtracts*, and lands in the ES8311's DAC
volume register — whose range is **-95.5 dB to +32 dB**. So volume 100 puts the
register at roughly 198 of 255, leaving about 28 dB unused.

`AUDIO_OUT_EXTRA_GAIN_DB` (default 0) opens that up by installing a volume curve
whose top point is above 0 dB instead of at it. It is digital gain on the PCM, so
it clips: speech has a high crest factor and takes a few dB happily, but past
that the loud passages distort before the quiet ones get usefully louder. Raise
it in ~6 dB steps and stop at the first hint of harshness.

Two ordering notes, both easy to get wrong:

- `esp_codec_dev_set_out_vol()` **must be called after the codec is open**. It
  runs `_verify_codec_ready()` first and returns without storing anything if the
  codec is closed, so a call placed too early is silently discarded and the
  device runs at `dev->volume`, which `calloc` left at 0. It works here only
  because `es8311_codec_new()` opens the codec itself, before
  `bsp_audio_codec_speaker_init()` returns.
- `esp_codec_dev_set_vol_curve()` must precede `esp_codec_dev_set_out_vol()`,
  since the curve is what converts the number into dB.

The boot line reports both: `codecs open: 16000 Hz, 16-bit, 2 ch | volume 100
(+0 dB), mic gain 24 dB`.

## Tuning

| Option | Default | When to change it |
|---|---|---|
| `CONFIG_MIC_IN_GAIN` | 24 dB | raise if mic peaks stay near zero while you talk |
| `CONFIG_AUDIO_OUT_VOLUME` | 70 | speaker too quiet, or clipping at the top of the range |
| `CONFIG_MIC_LEVEL_LOG` | on | turn off once the mic is trusted |
| `CONFIG_HEAP_PROBE` | off | chasing an allocation failure — names the size **and caps** that failed, which the `TLM` line cannot. This is what found the fault behind the v0.7.0 session drops; turn it on before touching any memory knob |
| `CONFIG_BATTERY_SAMPLE_MS` | 5000 | rarely: charge is slow, and every sample is traffic on the bus touch and the codec share |
| `CONFIG_BATTERY_LOW_PCT` | 20 | when the warning should start. Clears five points higher, so it cannot chatter |
| `CONFIG_BATTERY_CRITICAL_PCT` | 8 | when the panel should be held dark to stretch the rest of the charge. 0 disables it |
| `CONFIG_BATTERY_DUMP_REGS` | off | bring-up on a new board — logs the raw AXP2101 registers every sample. Off means the `EVT battery` state-change lines and the `TLM` fields are the instrument |

### Echo cancellation and full duplex

Full duplex is the shipping default: the canceller is on and the mic gate is off,
so the agent can be talked over. These are the knobs for changing that or for
bringing it up on different hardware. See
[notes/echo-cancellation.md](notes/echo-cancellation.md) for the measurements.

| Option | Default | When to change it |
|---|---|---|
| `CONFIG_AEC_ENABLE` | on | off returns the device to half duplex — no canceller, no voice barge-in, and ~14.7 kB internal RAM and ~6 fps back |
| `CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS` | off | on to close the mic during playback while keeping the canceller. The setting to reach for when bringing this up on unfamiliar hardware |
| `CONFIG_AEC_UPLINK_VAD` | on | leave on. Withholds blocks with no speech during playback; without it the uplink saturates TLS and the session drops |
| `CONFIG_AEC_UPLINK_VAD_PEAK` | 400 | raise if the reply keeps the uplink open, lower if quiet interruptions are missed. Read `out=` in the `mic peak` log |
| `CONFIG_AEC_NLP_LEVEL` | normal | more suppression, but it suppresses hardest during double-talk — the barge-in moment |
| `CONFIG_AEC_REF_GAIN_DB` | 12 dB | reference lane too quiet or clipping; check `ref=` in the `mic peak` log |
| `CONFIG_AEC_FULL_DUPLEX_MAX_VOLUME` | 100 | lower only if barge-in works quietly and fails loud. Below 100 it silently reverts to half duplex above that volume while the prompt still promises full |

---

[Back to the README](../README.md)

## The conversation store

These are `#define`s in `main/dg_agent.c`, not Kconfig, because they are a memory
budget rather than a preference. [persistence.md](persistence.md) has the
reasoning; this is the short version of what moves if you change them.

| Constant | Default | Change it when |
| --- | --- | --- |
| `HISTORY_BYTES` | 3072 | the device should remember more or fewer turns. PSRAM, so the cost is depth against nothing scarce. Bounded by `HISTORY_RECORD_MAX` fitting a 4 kB flash slot — a `_Static_assert` catches it |
| `HISTORY_MAX_TURNS` | 40 | rarely. An index cap, not a content cap; the byte budget usually binds first |
| `HISTORY_TURN_MAX` | 512 | a single turn is being truncated mid-sentence. Truncation backs off to a UTF-8 boundary, so this cannot split a character |
| `HISTORY_REPLAY_BYTES` | 1280 | **measure first.** What goes on the wire, not what is stored |
| `HISTORY_REPLAY_MAX_TURNS` | 6 | same. Each replayed turn costs ~10 small cJSON allocations in internal RAM, during the TLS handshake, which is the worst moment for it |
| `HISTORY_FLUSH_DEBOUNCE_MS` | 1500 | how much conversation a brownout may cost. Lower means more flash wear |
| `HISTORY_FLUSH_MAX_DEFER_MS` | 5000 | the hard ceiling on that exposure, because the debounce restarts on every turn |

**The two replay numbers are a hard limit, not a backstop, and raising them is
how v0.7.0's session drops were made worse before they were understood.** At 16
turns / 6 kB the `Settings` message reached 20,265 bytes and the session began
flapping — `esp-aes: Failed to allocate memory` on the TLS write, once the
DMA-capable largest block dipped. `send_json` logs the byte count and the `TLM`
line logs `dmamax`; both have to be read on a real device, because the failure is
a coin toss against a fragmented heap and a short clean run proves nothing.

## Internal RAM, and why `DRAW_ROWS` is not the first lever any more

It used to be. `DRAW_ROWS` in `main/ui.c` was 32 and is now 16 — that halving is
what fixed the v0.7.0 session drops, and it is spent.

What binds is not internal RAM in general but **DMA-capable** internal RAM, which
is a strict subset and diverges from it under load: measured, 832 B of DMA
largest block while plain internal still read 7,680. Read `dmamax` on the `TLM`
line, not `intmax`. Two things that look like the answer and are not:

- **`CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM`** — each buffer is ~1,600 B of exactly
  the right pool, so cutting it from 16 to 8 does free 12.8 kB. It also starves
  the MAC under a burst, and `sdkconfig.defaults` explains what that costs.
- **Anything mbedTLS** — the record buffers are already in PSRAM
  (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`), so their size has nothing to do with
  it. That is *why* esp-aes needs a DMA bounce buffer at all.
