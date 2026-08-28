#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "pet_sprites.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define SAMPLE_RATE       16000
#define CARTOON_RATE      22050
#define MAX_RECORD_SEC        8
#define CHUNK_SAMPLES       256
#define RECORD_SAMPLES (SAMPLE_RATE * MAX_RECORD_SEC)
#define RECORD_BYTES   ((RECORD_SAMPLES + 1) / 2)

typedef enum {
    PET_IDLE = 0,
    PET_LISTENING,
    PET_PROCESSING,
    PET_TALKING,
    PET_ERROR,
} pet_state_t;

static const char *TAG = "talking_pet";
static volatile pet_state_t s_state = PET_IDLE;
static volatile bool s_start_requested;
static volatile bool s_stop_requested;
static volatile size_t s_recorded_samples;
static volatile uint8_t s_volume = 80;
static volatile bool s_volume_dirty = true;
static volatile uint8_t s_volume_notice;

static lv_obj_t *s_screen;
static lv_obj_t *s_status;
static lv_obj_t *s_hint;
static lv_obj_t *s_record_dot;
static lv_obj_t *s_pet_img;
static lv_obj_t *s_battery;
static uint32_t s_anim_tick;

static void button_cb(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (ev == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
        s_volume = s_volume >= 90 ? 100 : (uint8_t)(s_volume + 10);
        s_volume_dirty = true;
        s_volume_notice = 18;
    } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
        s_volume = s_volume <= 20 ? 10 : (uint8_t)(s_volume - 10);
        s_volume_dirty = true;
        s_volume_notice = 18;
    } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG && s_state == PET_IDLE) {
        s_stop_requested = false;
        s_start_requested = true;
    } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG_RELEASE && s_state == PET_LISTENING) {
        s_stop_requested = true;
    }
}

typedef struct {
    int predictor;
    int index;
} ima_state_t;

static const int16_t s_ima_step[89] = {
       7,    8,    9,   10,   11,   12,   13,   14,   16,   17,
      19,   21,   23,   25,   28,   31,   34,   37,   41,   45,
      50,   55,   60,   66,   73,   80,   88,   97,  107,  118,
     130,  143,  157,  173,  190,  209,  230,  253,  279,  307,
     337,  371,  408,  449,  494,  544,  598,  658,  724,  796,
     876,  963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493,10442,11487,12635,13899,
   15289,16818,18500,20350,22385,24623,27086,29794,32767
};

static const int8_t s_ima_index[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

static uint8_t ima_encode(int16_t sample, ima_state_t *state)
{
    int step = s_ima_step[state->index];
    int diff = (int)sample - state->predictor;
    uint8_t code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    int delta = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; delta += step; }
    if (diff >= (step >> 1)) { code |= 2; diff -= step >> 1; delta += step >> 1; }
    if (diff >= (step >> 2)) { code |= 1; delta += step >> 2; }
    state->predictor += (code & 8) ? -delta : delta;
    if (state->predictor > 32767) state->predictor = 32767;
    if (state->predictor < -32768) state->predictor = -32768;
    state->index += s_ima_index[code];
    if (state->index < 0) state->index = 0;
    if (state->index > 88) state->index = 88;
    return code;
}

static int16_t ima_decode(uint8_t code, ima_state_t *state)
{
    int step = s_ima_step[state->index];
    int delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    state->predictor += (code & 8) ? -delta : delta;
    if (state->predictor > 32767) state->predictor = 32767;
    if (state->predictor < -32768) state->predictor = -32768;
    state->index += s_ima_index[code & 0x0f];
    if (state->index < 0) state->index = 0;
    if (state->index > 88) state->index = 88;
    return (int16_t)state->predictor;
}

static void audio_task(void *arg)
{
    (void)arg;
    uint8_t *recording = malloc(RECORD_BYTES);
    if (!recording) {
        ESP_LOGE(TAG, "cannot allocate %u-byte ADPCM buffer", (unsigned)RECORD_BYTES);
        s_state = PET_ERROR;
        vTaskDelete(NULL);
        return;
    }

    int16_t samples[CHUNK_SAMPLES];
    int16_t playback[CHUNK_SAMPLES];
    for (;;) {
        if (s_volume_dirty) {
            uint8_t volume = s_volume;
            s_volume_dirty = false;
            bsp_audio_set_volume(volume);
        }
        if (!s_start_requested) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        s_start_requested = false;
        s_recorded_samples = 0;
        if (bsp_audio_set_format(SAMPLE_RATE, 16, 1) != ESP_OK) {
            s_state = PET_ERROR;
            continue;
        }

        ima_state_t encoder = {0};
        size_t encoded_bytes = 0;
        bool low_nibble = true;
        s_state = PET_LISTENING;
        while (!s_stop_requested && s_recorded_samples < RECORD_SAMPLES) {
            size_t count = RECORD_SAMPLES - s_recorded_samples;
            if (count > CHUNK_SAMPLES) count = CHUNK_SAMPLES;
            if (bsp_audio_read(samples, count * sizeof(samples[0])) != ESP_OK) {
                s_state = PET_ERROR;
                break;
            }
            for (size_t i = 0; i < count; i++) {
                uint8_t nibble = ima_encode(samples[i], &encoder);
                if (low_nibble) {
                    recording[encoded_bytes] = nibble;
                    low_nibble = false;
                } else {
                    recording[encoded_bytes++] |= (uint8_t)(nibble << 4);
                    low_nibble = true;
                }
            }
            s_recorded_samples += count;
        }
        if (!low_nibble) encoded_bytes++;

        if (s_state == PET_ERROR) continue;
        s_stop_requested = false;
        if (s_recorded_samples < SAMPLE_RATE / 5) {
            s_state = PET_IDLE;
            continue;
        }

        s_state = PET_PROCESSING;
        ima_state_t decoder = {0};
        size_t out_count = 0;
        if (bsp_audio_set_format(CARTOON_RATE, 16, 1) != ESP_OK) {
            s_state = PET_ERROR;
            continue;
        }
        bsp_audio_set_volume(s_volume);
        s_state = PET_TALKING;
        for (size_t i = 0; i < s_recorded_samples; i++) {
            uint8_t packed = recording[i >> 1];
            uint8_t nibble = (i & 1U) ? (packed >> 4) : (packed & 0x0f);
            int16_t decoded = ima_decode(nibble, &decoder);
            /* Suppress the codec/ADPCM noise floor without touching normal speech. */
            if (decoded > -180 && decoded < 180) decoded = 0;
            playback[out_count++] = decoded;
            if (out_count == CHUNK_SAMPLES) {
                if (bsp_audio_write(playback, sizeof(playback)) != ESP_OK) {
                    s_state = PET_ERROR;
                    break;
                }
                out_count = 0;
            }
        }
        if (s_state != PET_ERROR && out_count > 0 &&
            bsp_audio_write(playback, out_count * sizeof(playback[0])) != ESP_OK) {
            s_state = PET_ERROR;
        }
        if (s_state != PET_ERROR) s_state = PET_IDLE;
        ESP_LOGI(TAG, "recorded %.2fs, ADPCM=%u bytes",
                 (double)s_recorded_samples / SAMPLE_RATE, (unsigned)encoded_bytes);
    }
}

static lv_obj_t *make_shape(lv_obj_t *parent, int x, int y, int w, int h,
                            uint32_t color, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x17213A), 0);
    lv_obj_set_style_border_width(obj, 3, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void build_ui(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x67D0EE), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    lv_obj_t *title_plate = make_shape(s_screen, 10, 8, 220, 36, 0xFFF0C9, 7);
    lv_obj_t *title = lv_label_create(title_plate);
    lv_label_set_text(title, "TALKING PET");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x17213A), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 9, 0);

    s_battery = lv_label_create(title_plate);
    int soc = bsp_battery_soc();
    if (soc >= 0) lv_label_set_text_fmt(s_battery, "%d%%", soc);
    else lv_label_set_text(s_battery, "--");
    lv_obj_set_style_text_font(s_battery, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_battery, lv_color_hex(0x52647D), 0);
    lv_obj_align(s_battery, LV_ALIGN_RIGHT_MID, -8, 0);

    lv_obj_t *stage = make_shape(s_screen, 14, 53, 212, 190, 0xE8F8F4, 12);
    /* A few chunky pixels make the room feel illustrated without using RAM-heavy art. */
    lv_obj_t *pixel = make_shape(stage, 14, 18, 12, 12, 0xFFD45C, 2);
    lv_obj_set_style_border_width(pixel, 0, 0);
    pixel = make_shape(stage, 181, 42, 9, 9, 0x6ECBD1, 2);
    lv_obj_set_style_border_width(pixel, 0, 0);
    pixel = make_shape(stage, 18, 116, 8, 8, 0xFF8292, 2);
    lv_obj_set_style_border_width(pixel, 0, 0);

    lv_obj_t *shadow = make_shape(stage, 43, 162, 124, 17, 0x83BFC2, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(shadow, 0, 0);

    s_pet_img = lv_image_create(stage);
    lv_image_set_src(s_pet_img, &g_pet_sprites[PET_SPRITE_IDLE]);
    lv_obj_set_pos(s_pet_img, 41, 22);

    s_record_dot = lv_obj_create(stage);
    lv_obj_set_size(s_record_dot, 13, 13);
    lv_obj_set_pos(s_record_dot, 183, 12);
    lv_obj_set_style_radius(s_record_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_record_dot, lv_color_hex(0xFF4F5E), 0);
    lv_obj_set_style_bg_opa(s_record_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_record_dot, 0, 0);
    lv_obj_add_flag(s_record_dot, LV_OBJ_FLAG_HIDDEN);

    s_status = lv_label_create(s_screen);
    lv_obj_set_pos(s_status, 8, 250);
    lv_obj_set_size(s_status, 224, 26);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x17213A), 0);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_status, "READY!");

    s_hint = lv_label_create(s_screen);
    lv_obj_set_pos(s_hint, 8, 281);
    lv_obj_set_size(s_hint, 224, 24);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x17213A), 0);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_hint, "UP/DOWN: VOL   HOLD OK: TALK");

    lv_screen_load(s_screen);
}

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_anim_tick++;
    pet_state_t state = s_state;

    switch (state) {
        case PET_LISTENING:
            lv_label_set_text(s_status, "I'M LISTENING...");
            lv_obj_clear_flag(s_record_dot, LV_OBJ_FLAG_HIDDEN);
            lv_image_set_src(s_pet_img, &g_pet_sprites[PET_SPRITE_LISTEN]);
            break;
        case PET_PROCESSING:
            lv_label_set_text(s_status, "THINKING...");
            lv_obj_add_flag(s_record_dot, LV_OBJ_FLAG_HIDDEN);
            lv_image_set_src(s_pet_img, &g_pet_sprites[PET_SPRITE_THINK]);
            break;
        case PET_TALKING:
            lv_label_set_text(s_status, "MY TURN!");
            lv_obj_add_flag(s_record_dot, LV_OBJ_FLAG_HIDDEN);
            lv_image_set_src(s_pet_img, &g_pet_sprites[PET_SPRITE_TALK]);
            break;
        case PET_ERROR:
            lv_label_set_text(s_status, "AUDIO ERROR - RESTART ME");
            lv_obj_add_flag(s_record_dot, LV_OBJ_FLAG_HIDDEN);
            lv_image_set_src(s_pet_img, &g_pet_sprites[PET_SPRITE_THINK]);
            break;
        case PET_IDLE:
        default:
            if (s_volume_notice > 0) {
                lv_label_set_text_fmt(s_status, "VOLUME  %u%%", (unsigned)s_volume);
                s_volume_notice--;
            } else {
                lv_label_set_text(s_status, "READY!");
            }
            lv_obj_add_flag(s_record_dot, LV_OBJ_FLAG_HIDDEN);
            lv_image_set_src(s_pet_img, &g_pet_sprites[PET_SPRITE_IDLE]);
            break;
    }
    int bob = (state == PET_IDLE && ((s_anim_tick / 8U) & 1U)) ? 1 : 0;
    lv_obj_set_y(s_pet_img, 22 + bob);
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "Talking Pet starting");

    ESP_ERROR_CHECK(bsp_i2c_init());
    (void)bsp_i2c_scan();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display initialization failed");
        return;
    }
    bsp_display_backlight(100);

    esp_err_t audio_err = bsp_audio_init();
    esp_err_t battery_err = bsp_battery_init();
    esp_err_t button_err = bsp_button_init(button_cb, NULL);
    ESP_LOGI(TAG, "audio=%s battery=%s buttons=%s",
             esp_err_to_name(audio_err), esp_err_to_name(battery_err),
             esp_err_to_name(button_err));

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL lock timeout");
        return;
    }
    build_ui();
    lv_timer_create(ui_timer_cb, 80, NULL);
    bsp_lvgl_unlock();

    if (audio_err == ESP_OK) {
        if (xTaskCreate(audio_task, "pet_audio", 4096, NULL, 5, NULL) != pdPASS) {
            s_state = PET_ERROR;
            ESP_LOGE(TAG, "audio task creation failed");
        }
    } else {
        s_state = PET_ERROR;
    }
    ESP_LOGI(TAG, "Talking Pet ready; hold UP to record");
}
