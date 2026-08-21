/*
 * Full-duplex audio for the agent session: ES7210 capture in, ES8311 playback
 * out, both over the one shared I2S peripheral.
 *
 * WHY ONE MODULE OWNS BOTH
 *
 * bsp_audio_init() configures a single duplex I2S channel pair, and
 * esp_codec_dev_open() drives that shared clock. So the mic and speaker cannot
 * run at different sample rates or channel counts -- opening one reconfigures
 * the other. spec_analyzer_radial's bsp_extra makes the same call: its
 * bsp_extra_codec_set_fs() closes and reopens *both* codecs together with one
 * sample_info. Splitting this across two modules would mean whichever opened
 * last silently redefined the other.
 *
 * CONSEQUENCE FOR THE AGENT SETTINGS
 *
 * The Agent API is happy to send 24 kHz out while taking 16 kHz in, but this
 * hardware cannot do both at once, so both directions run at
 * DG_AUDIO_SAMPLE_RATE. 16 kHz is the rate spec_analyzer_radial proved on this
 * board.
 *
 * CHANNEL LAYOUT
 *
 * The codecs are opened with 2 channels, matching the proven configuration.
 * Deepgram is mono in both directions, so this module converts at the edges:
 * playback duplicates each mono sample into L+R, and capture averages L+R back
 * down to one channel.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Receives captured mono 16-bit PCM at DG_AUDIO_SAMPLE_RATE. Runs on the
 * capture task, so it must not block for long. */
typedef void (*audio_io_capture_sink_t)(const uint8_t *pcm, size_t len);

/* Opens both codecs at the shared rate and starts the playback task. */
esp_err_t audio_io_init(int sample_rate);

/* Starts the capture task feeding `sink`. Call after audio_io_init(). */
esp_err_t audio_io_capture_start(audio_io_capture_sink_t sink);

/* Queue mono PCM from the agent for playback. Non-blocking; returns
 * ESP_ERR_NO_MEM when the buffer was full and audio was dropped. */
esp_err_t audio_io_play(const uint8_t *pcm, size_t len);

/* Drop everything queued -- used for barge-in. */
void audio_io_flush(void);

/* True while agent audio is playing or has only just stopped. */
bool audio_io_playback_active(void);

void audio_io_stats(uint32_t *played, uint32_t *dropped, uint32_t *captured);
