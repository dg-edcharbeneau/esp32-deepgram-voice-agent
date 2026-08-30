/*
 * Codec construction with the echo-reference channel enabled.
 *
 * Restored from commit 9479446, which proved the lane exists and measured it.
 * That version was a probe behind CONFIG_AEC_REF_PROBE; this one is the shipping
 * path, because a reference that only exists under a debug flag cannot be built
 * on.
 *
 * WHY THIS EXISTS RATHER THAN USING THE BSP
 *
 * Waveshare advertises "hardware echo cancellation" and calls the ES7210 an
 * "Echo Cancellation Algorithm Chip". Both are wrong. The ES7210 datasheet
 * titles it "High Performance Four Channels Audio ADC"; there is no echo
 * canceller anywhere on this board -- not in the ES7210, not in the ES8311, not
 * in the NS4150B amplifier.
 *
 * What the board actually provides is an echo REFERENCE channel, which is the
 * genuinely hard part to retrofit. The schematic's netlist shows a block labelled
 * `AEC ADC` tapping the ES8311's line outputs through a differential RC network
 * into ES7210 MIC3P/MIC3N -- every component populated. So the speaker signal is
 * captured by the same ADC, on the same clock, in the same frame as the
 * microphones, and is therefore sample-aligned by construction. The cancellation
 * ALGORITHM still has to run in software.
 *
 * Nothing enables it. bsp_audio_codec_microphone_init() builds the ES7210 with
 * `mic_selected` left at 0, and the driver then defaults to MIC1|MIC2, so MIC3 is
 * never powered and never clocked out. The BSP also keeps its `i2s_data_if`
 * file-static with no accessor, so there is no way to reach MIC3 through it --
 * hence this file, which is the BSP's own audio bring-up with one field changed.
 *
 * TDM IS THE ONLY ROUTE
 *
 * In non-TDM mode the ES7210 puts MIC1/MIC2 on SDOUT1 and MIC3/MIC4 on SDOUT2.
 * SDOUT2 is cut on this board: R48 is marked NC in the netlist, leaving ES7210
 * pin 12 unconnected, and only R47 wires SDOUT1/TDMOUT to GPIO10. So a 4-channel
 * TDM frame on SDOUT1 is the only way MIC3 reaches the S3.
 */
#pragma once

#include "esp_codec_dev.h"
#include "esp_err.h"

/*
 * Build both codec devices with all four ES7210 inputs enabled, which puts the
 * part into TDM mode. Call instead of bsp_audio_codec_speaker_init() and
 * bsp_audio_codec_microphone_init(), and before opening either.
 *
 * Either handle may come back NULL on failure; the caller reports which.
 */
esp_err_t audio_codecs_init_tdm(esp_codec_dev_handle_t *out_spk,
                                esp_codec_dev_handle_t *out_mic);

/*
 * THE SLOT ORDER, MEASURED -- not guessed, and not the order the part numbers
 * suggest. 9479446 read all four lanes with the speaker playing and with a voice
 * in the room, and used the permanently-dead lane as the control:
 *
 *   lane 0 = the echo reference (MIC3)   idle 2-3      playback 7867-9359
 *   lane 1 = a MEMS microphone           idle 30-115   playback 537-11439
 *   lane 2 = MIC4, AC-coupled to AGND    3 always      -- the control
 *   lane 3 = the other MEMS microphone   idle 30-115   playback 520-12353
 *
 * It is not MIC1/2/3/4 because a 32-bit word arrives MSB-first and stores
 * little-endian, so each 16-bit pair swaps in memory. Anything reading these
 * frames must use these indices.
 */
#define AEC_LANE_REF   0
#define AEC_LANE_MIC_A 1
#define AEC_LANE_DEAD  2
#define AEC_LANE_MIC_B 3
#define AEC_LANES      4

/* Mask for esp_codec_dev_set_in_channel_gain(). Indexes the ES7210's INPUTS
 * (MIC1..MIC4 = bits 0..3), where the reference is MIC3 -- a DIFFERENT index
 * space from the memory lanes above, and confusing the two silently attenuates
 * a microphone instead. */
#define AEC_REF_INPUT_MASK (1U << 2)

/*
 * How many times the RX DMA has overflowed since boot. Should be 0. Anything
 * else means microphone audio was silently lost -- esp_codec_dev's read path
 * never reports it.
 */
uint32_t audio_codecs_rx_overruns(void);
