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
| `CONFIG_AUDIO_OUT_VOLUME` | 100 | speaker too quiet or clipping |
| `CONFIG_MIC_LEVEL_LOG` | on | turn off once the mic is trusted |

---

[Back to the README](../README.md)
