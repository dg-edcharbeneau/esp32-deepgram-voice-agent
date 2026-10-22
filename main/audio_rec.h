/*
 * A recording of what the microphone heard and what went to Deepgram, for
 * working out why an interruption was or was not detected. Debug only:
 * CONFIG_AUDIO_RECORD, default off, and every function is an empty stub
 * without it.
 *
 * WHAT IS RECORDED
 *
 * Two channels per 64 ms capture block, from the moment a session's capture
 * opens until it closes or CONFIG_AUDIO_RECORD_SECONDS runs out:
 *   raw  -- the two microphone lanes mixed down, BEFORE echo cancellation:
 *           the room, the agent's own voice coming back, and the user.
 *   sent -- exactly the block handed to the uplink: after the canceller and
 *           the denoiser, and silence wherever the uplink VAD held it back.
 * Plus a flag per block: was playback active, and was the block sent.
 *
 * WHY PSRAM AND AFTERWARDS
 *
 * Into PSRAM during the session (2 x 2 kB a block; about 3.8 MB for 60 s), and
 * printed to the console only once the session has ended. Streaming it live
 * would put a 64 kB/s console write next to the audio path, and writing it to
 * flash would stall that path on every sector erase -- both of which change the
 * thing being measured. The dump is lines of base64 that host/rec2wav.py turns
 * back into a two-channel WAV, labelled with the session's request_id and with
 * the offset from Deepgram's Welcome so it lines up with the server's
 * "seconds into call".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Allocate the buffers and start the dump task. From audio_io_init(). */
void audio_rec_init(size_t frames_per_block);

/* Capture opened (a session started) or closed. Any task. */
void audio_rec_capture(bool enabled);

/* Capture task only, once per block: the mixed microphone before the canceller,
 * and then -- only if the block goes to the uplink -- the block as sent. */
void audio_rec_raw(const int16_t *mono, bool playback);
void audio_rec_sent(const int16_t *mono);
