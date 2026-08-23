/*
 * Codec construction for the echo-reference probe.
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
