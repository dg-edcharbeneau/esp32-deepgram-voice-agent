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

/*
 * Observer for mono 16-bit PCM at the moment it is handed to the hardware.
 *
 * The point of tapping here rather than at audio_io_play() is pacing: Deepgram
 * delivers a whole turn faster than it plays -- that is what the ring buffer
 * below absorbs -- so a visualizer fed from the network callback would race
 * ahead and finish while the speaker is still talking. The playback tap fires
 * from the drain loop, which is paced by the blocking codec write.
 *
 * Runs on the audio tasks: must not block, and must copy anything it keeps.
 */
typedef void (*audio_io_tap_t)(const int16_t *mono, size_t samples);

/* Opens both codecs at the shared rate and starts the playback task. */
esp_err_t audio_io_init(int sample_rate);

/* Starts the capture task feeding `sink`. Call after audio_io_init(). */
esp_err_t audio_io_capture_start(audio_io_capture_sink_t sink);

/* Queue mono PCM from the agent for playback. Non-blocking; returns
 * ESP_ERR_NO_MEM when the buffer was full and audio was dropped. */
esp_err_t audio_io_play(const uint8_t *pcm, size_t len);

/* Observe agent audio as it reaches the speaker. Pass NULL to detach. */
void audio_io_set_playback_tap(audio_io_tap_t tap);

/* Observe mic audio as it goes to the sink -- so the tap sees exactly what is
 * sent upstream, including nothing at all while capture is gated. */
void audio_io_set_capture_tap(audio_io_tap_t tap);

/* Drop everything queued -- used for barge-in. */
void audio_io_flush(void);

/*
 * Tell the playback path that the incoming byte stream has broken.
 *
 * Not a flush -- whatever is already queued still plays. This only says that the
 * NEXT bytes to arrive are discontinuous with the last, so the half-sample
 * audio_io_play() may be holding must be dropped rather than stitched onto them.
 *
 * Call it whenever the socket goes away underneath a turn. A websocket
 * auto-reconnect does not pass through session_ctl, so nothing else clears that
 * carry, and one orphaned byte offsets every sample after it for the rest of the
 * session -- audible as white noise, not as a join.
 */
void audio_io_note_stream_gap(void);

/*
 * Discard incoming agent audio until told otherwise.
 *
 * The other half of an interruption. audio_io_flush() silences what has already
 * arrived; this stops what is still coming, because Deepgram has no interrupt
 * message and keeps sending the rest of a reply regardless -- so a flush on its
 * own just makes the agent pause and then resume mid-word.
 *
 * The microphone is NOT gated by this -- see the gate in capture_task(). main.c
 * releases the mute when the user speaks again, so a device that could not hear
 * would never hear its own release.
 *
 * MUST BE SCOPED BY THE CALLER. Left set, the agent stops being audible.
 */
void audio_io_mute_playback(bool muted);

/*
 * Stop or resume streaming captured audio.
 *
 * The capture task keeps reading the codec either way -- the ES7210 is clocked
 * by the shared duplex I2S regardless, so stopping the read would only overflow
 * the RX descriptor ring and produce a stale burst on resume. What this gates is
 * everything downstream: the sink and the tap both stop, so the device goes
 * quiet on the network and the visualizer stops reacting to the room.
 */
void audio_io_capture_set_enabled(bool enabled);

/*
 * Feed the display tap while keeping the network sink shut.
 *
 * A deliberate, temporary exception to the gate above, for the display test:
 * the orb needs real microphone levels to demonstrate its states -- amplitude
 * scales how deep each gesture goes, so at zero half of them look identical --
 * but there is no session to stream to, and writing to a socket that is not
 * there is how the transport errors in the logs happen.
 *
 * Only consulted while capture is DISABLED, so it cannot loosen the gate on a
 * live session. It is the ordering that matters: session_ctl's stop path calls
 * audio_io_capture_set_enabled(false), so set the session down first and this
 * afterwards, or the stop will clear what you just asked for.
 */
void audio_io_capture_set_monitor(bool monitor);

/*
 * Clear producer-side state between sessions.
 *
 * audio_io_flush() only empties the ring; it cannot touch the odd-byte carry,
 * which lives on the WebSocket task. Left set, that byte is stitched onto the
 * first byte of the *next* session and shifts every sample after it by 8 bits --
 * the permanent full-scale noise described at the top of audio_io.c, not a
 * click. Call this once the WebSocket task is known to be idle.
 */
void audio_io_reset(void);

/*
 * Speaker level, 0-100, clamped to AUDIO_VOLUME_MIN..MAX.
 *
 * Unlike the voice, this never involves Deepgram -- it is one ES8311 register
 * write, so it takes effect on the very next sample. Safe to call from any task
 * including while playback is in flight: the write goes over I2C while audio
 * goes over I2S, and this codec supplies a hardware volume so esp_codec_dev
 * never touches the PCM buffer on this path.
 *
 * adjust is a single call rather than get-then-set so the clamp and the write
 * stay together; that is where a lock would go if a second caller ever appears.
 */
int audio_io_get_volume(void);
int audio_io_adjust_volume(int delta);   /* returns the resulting level */

/* True while agent audio is playing or has only just stopped. */
bool audio_io_playback_active(void);

void audio_io_stats(uint32_t *played, uint32_t *dropped, uint32_t *captured);
