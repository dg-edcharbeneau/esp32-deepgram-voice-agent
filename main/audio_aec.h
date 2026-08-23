/*
 * Acoustic echo cancellation, using esp-sr's AFE and the board's echo reference.
 *
 * WHY THIS IS NEEDED AT ALL
 *
 * The speaker and microphones sit centimetres apart, so the microphones hear
 * whatever the agent is saying. Send that upstream and Deepgram transcribes the
 * agent's own words as if the user had said them, and the agent answers itself.
 *
 * The existing workaround, CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS, simply goes deaf
 * while playing. It works, and it costs barge-in: you cannot interrupt.
 *
 * WHAT DEEPGRAM DOES AND DOES NOT DO
 *
 * Deepgram does the barge-in DETECTION and signalling -- it decides the user has
 * started talking and sends UserStartedSpeaking, and dg_agent already flushes
 * playback in response. It does NOT do echo cancellation; its Audio Preprocessing
 * guide has no AEC setting and explicitly pushes it to the device.
 *
 * Those two are coupled, which is the point. With the gate on, the detection can
 * never fire mid-reply because the microphone is deaf. With the gate off and no
 * cancellation, the detection fires on the AGENT'S OWN VOICE leaking into the
 * microphone -- so barge-in does not merely fail, it misfires and the agent
 * interrupts itself. Cancellation is the precondition for the detection being
 * about the right speaker.
 *
 * WHAT MAKES IT POSSIBLE HERE
 *
 * The board wires the ES8311's output back into ES7210 MIC3 as an echo reference,
 * captured by the same ADC in the same TDM frame as the microphones -- so it
 * cannot drift relative to them. See audio_codecs.h for the proof and the
 * measured lane assignment. A firmware-synthesised reference is aligned only as
 * well as its DMA bookkeeping; this one is aligned by construction, which is what
 * lets the adaptive filter converge.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Cleaned mono audio, ready to go upstream. Called from the AEC's own task. */
typedef void (*audio_aec_out_t)(const int16_t *mono, size_t samples);

/*
 * Build the AFE and start the task that owns the pipeline. `on_output` receives
 * echo-cancelled mono at the project's 16 kHz, on that task.
 *
 * `block_frames` is how many frames per channel the caller will hand over per
 * audio_aec_feed() call. It must be a whole multiple of the AFE's own chunk size
 * or this fails rather than silently sliding the channel interleaving.
 */
esp_err_t audio_aec_start(audio_aec_out_t on_output, size_t block_frames);

/*
 * Hand over one block of raw interleaved capture, four channels per frame, in the
 * order the ES7210's TDM frame arrives in.
 *
 * Never blocks: it copies into a free block and returns, and DROPS the block if
 * the AEC task is behind. Safe to call from the codec-read task, where stalling
 * would cost audio rather than a frame of echo.
 */
void audio_aec_feed(const int16_t *interleaved, size_t frames);
