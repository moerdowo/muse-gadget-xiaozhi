/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Waveshare ESP32-S3-Touch-AMOLED-1.8: 16 MB flash, 8 MB octal PSRAM, 368x448
 * CO5300 AMOLED with CST816S (v2) or FT5x06 (v1) touch, the BSP detects which;
 * one ES8311 for speaker and mic, AXP2101 PMU, TCA9554 expander. BOOT (GPIO0)
 * talks; PWR (on the PMU) is the power button. Same layout as the C6 1.8.
 */
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_lcd_touch.h"
#include "esp_lvgl_port.h"
#include "bsp/touch.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"
#include "muse_pmu.h"

static const char *TAG = "board";

#define BOOT_GPIO GPIO_NUM_0
#define PMU_KEY_EVERY 2         /* poll the PMU over I2C every 20 ms */
#define DRAW_BUF_LINES 16
/* Expander pins 0-2: panel and touch reset/enable. The BSP leaves them as
 * inputs; Waveshare's demos and xiaozhi pulse them before the panel starts. */
#define EXP_RESETS (IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2)

static muse_gpio_button_t s_boot;

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, BOOT_GPIO), TAG, "boot button");
    esp_io_expander_handle_t exp = bsp_io_expander_init();
    if (exp) {
        esp_io_expander_set_dir(exp, EXP_RESETS, IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(exp, EXP_RESETS, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_io_expander_set_level(exp, EXP_RESETS, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    } else {
        ESP_LOGW(TAG, "no IO expander: panel reset skipped");
    }
    if (muse_pmu_init(bsp_i2c_get_handle(), true) != ESP_OK) {
        ESP_LOGW(TAG, "no PMU: PWR button and battery unavailable");
    }
    return ESP_OK;
}

/* The CO5300 needs even-aligned update windows. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

/*
 * bsp_display_start(), with our own draw buffers: the BSP's ignore the config
 * and come from plain malloc, which lands in PSRAM here, and every flush from
 * PSRAM needs an internal DMA bounce buffer that isn't there once Wi-Fi and BLE
 * are up ("Failed to allocate priv TX buffer": nothing drawn, panel garbage).
 */
static lv_display_t *display_start(lv_indev_t **touch)
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;
    port_cfg.task_affinity = MUSE_UI_CORE;
    port_cfg.task_priority = MUSE_UI_PRIORITY;
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    if (bsp_display_new(&(bsp_display_config_t){ 0 }, &panel, &io) != ESP_OK) {
        return NULL;
    }
    /* Probes CST816S (v2) or FT5x06 (v1) and sets the v2 panel's 16 px column gap.
     * Before LVGL draws, or its first frame lands 16 px off and leaves a
     * strip of uninitialised panel RAM at the edge. */
    esp_lcd_touch_handle_t tp;
    if (bsp_touch_new(NULL, &tp) != ESP_OK) {
        return NULL;
    }
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = BSP_LCD_H_RES * DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) {
        return NULL;
    }
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    *touch = lvgl_port_add_touch(&(lvgl_port_touch_cfg_t){ .disp = disp, .handle = tp });
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return bsp_display_lock(timeout_ms < 0 ? 0 : timeout_ms);
}

static void set_brightness(int pct)
{
    bsp_display_brightness_set(pct);
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    /* Set up I2S the way muse_audio opens it, instead of the BSP's mono 22 kHz default. */
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
        },
    };
    ESP_RETURN_ON_ERROR(bsp_audio_init(&std_cfg), TAG, "i2s");
    *spk = bsp_audio_codec_speaker_init();
    *mic = bsp_audio_codec_microphone_init();
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    static unsigned tick;
    unsigned ev = muse_gpio_button_poll(&s_boot);
    if (tick++ % PMU_KEY_EVERY == 0) {
        unsigned key = muse_pmu_poll_key();
        ev |= (key & MUSE_PMU_KEY_PRESS ? MUSE_BTN_AUX_PRESS : 0) |
              (key & MUSE_PMU_KEY_RELEASE ? MUSE_BTN_AUX_RELEASE : 0);
    }
    return ev;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-AMOLED-1.8",
    .width = BSP_LCD_H_RES,
    .height = BSP_LCD_V_RES,
    .round = false,
    .touch = true,
    .diagonal_in = 1.8f,
    .talk_button = "boot",
    .aux_button = "pwr",
    /* Both buttons are on the right edge, about 100 px from the top and bottom. */
    .talk_hint = { LV_ALIGN_RIGHT_MID, -10, -124 },
    .aux_hint = { LV_ALIGN_RIGHT_MID, -10, 126 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = bsp_display_unlock,
    .set_brightness = set_brightness,
    .audio_init = audio_init,
    .mic_slot = 0,              /* one mic, on the left slot */
    .poll_buttons = poll_buttons,
    .read_power = muse_pmu_read_power,
    .power_off = muse_pmu_power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
