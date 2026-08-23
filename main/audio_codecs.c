/*
 * The BSP's audio bring-up, with all four ES7210 inputs enabled. See
 * audio_codecs.h for why this cannot just call the BSP.
 */

#include "audio_codecs.h"

#include "driver/i2s_std.h"
#include "esp_log.h"

#include "es7210_adc.h"
#include "es8311_codec.h"
#include "esp_codec_dev_defaults.h"

#include "bsp/esp-bsp.h"
#include "bsp/esp32_s3_touch_amoled_1_75c.h"

static const char *TAG = "audio_codecs";

/*
 * All four inputs.
 *
 * A literal rather than ES7210_INPUT_MIC1..4, because that enum lives in the
 * driver's PRIVATE es7210_reg.h (device/es7210/, not device/include/) and reaching
 * into it would couple this file to the driver's internals. The config field is a
 * plain uint8_t and the bits are MIC1=0x01 .. MIC4=0x08.
 *
 * Selecting four is what flips the part into TDM: es7210_is_tdm_mode() counts the
 * selected mics and returns true at ENABLE_TDM_MAX_NUM (3), which makes
 * es7210_mic_select() write ES7210_SDP_INTERFACE2_REG12 = 0x02.
 *
 * MIC4 is deliberately included even though the netlist AC-couples it to AGND
 * (C93-1 and C94-1 sit on AGND). A permanently-silent lane is the control that
 * pins down the slot ORDER -- without it, finding a lane that tracks playback
 * only proves something is there, not which slot it is.
 */
#define ES7210_ALL_MICS 0x0F

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static const audio_codec_data_if_t *s_data_if;

/*
 * Same shape as the BSP's own default. The width and rate here do not matter:
 * esp_codec_dev_open() reprograms both the clock and the slots, which is why the
 * BSP can get away with a 22050 Hz default it never uses.
 */
static esp_err_t i2s_bring_up(void)
{
    if (s_data_if != NULL) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; /* clear stale DMA data rather than replay it */
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * STANDARD mode, not TDM, and that is the point of the whole approach: RX
     * and TX share BCLK and WS in full duplex and must be configured
     * identically, so putting the I2S peripheral itself into TDM would have to be
     * done for playback too. Instead the ES7210 emits a 4x16-bit TDM frame and
     * the S3 reads it as 2x32-bit standard I2S -- 64 bits either way.
     */
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(22050),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws   = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din  = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_tx);
    }
    if (err == ESP_OK) {
        err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_rx);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s init: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx);
        i2s_del_channel(s_rx);
        s_tx = s_rx = NULL;
        return err;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    return (s_data_if != NULL) ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_codecs_init_tdm(esp_codec_dev_handle_t *out_spk,
                                esp_codec_dev_handle_t *out_mic)
{
    if (out_spk == NULL || out_mic == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_spk = NULL;
    *out_mic = NULL;

    /* Before anything audio. The BSP asserts inside its speaker init if the I2C
     * bus is not up, and this is the same trap recorded in MEMORY.md. */
    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_i2c_init: %s", esp_err_to_name(err));
        return err;
    }
    err = i2s_bring_up();
    if (err != ESP_OK) {
        return err;
    }

    i2c_master_bus_handle_t i2c = bsp_i2c_get_handle();
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    /* ---- speaker: ES8311, unchanged from the BSP ---- */
    audio_codec_i2c_cfg_t spk_i2c = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c,
    };
    const audio_codec_ctrl_if_t *spk_ctrl = audio_codec_new_i2c_ctrl(&spk_i2c);
    if (spk_ctrl == NULL) {
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = spk_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *es8311 = es8311_codec_new(&es8311_cfg);
    if (es8311 == NULL) {
        return ESP_FAIL;
    }
    esp_codec_dev_cfg_t spk_dev = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311,
        .data_if = s_data_if,
    };
    *out_spk = esp_codec_dev_new(&spk_dev);

    /* ---- microphone: ES7210, and the one field that matters ---- */
    audio_codec_i2c_cfg_t mic_i2c = {
        .port = BSP_I2C_NUM,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c,
    };
    const audio_codec_ctrl_if_t *mic_ctrl = audio_codec_new_i2c_ctrl(&mic_i2c);
    if (mic_ctrl == NULL) {
        return ESP_FAIL;
    }

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = mic_ctrl,
        .mic_selected = ES7210_ALL_MICS,
    };
    const audio_codec_if_t *es7210 = es7210_codec_new(&es7210_cfg);
    if (es7210 == NULL) {
        return ESP_FAIL;
    }
    esp_codec_dev_cfg_t mic_dev = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210,
        .data_if = s_data_if,
    };
    *out_mic = esp_codec_dev_new(&mic_dev);

    if (*out_spk == NULL || *out_mic == NULL) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "codecs up with ES7210 in 4-channel TDM (mic_selected=0x%02x)",
             ES7210_ALL_MICS);
    return ESP_OK;
}
