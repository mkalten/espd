/*
 * Generic esp-bsp I/O glue (LED strip, iot_button, SD card).
 *
 * Optional per-board overrides in espd_board_io_config.h (button map, etc.).
 * Compiled from espd_boards when a non-Generic board is selected.
 */

#include "bsp/esp-bsp.h"
#include "espd_bsp_sdcard.h"

#define ESPD_BSP_IO_NO_SDCARD_DECL
#include "bsp/bsp_io.h"
#undef ESPD_BSP_IO_NO_SDCARD_DECL

#include "esp_log.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef BSP_CAPS_BUTTONS
#define BSP_CAPS_BUTTONS 0
#endif

#if BSP_CAPS_BUTTONS
#include "iot_button.h"
#endif

#if defined(BSP_LED_STRIP_NUM) && defined(BSP_LED_STRIP_IO)
#include "led_strip.h"
#define ESPD_BSP_HAVE_LED 1
#endif

#if __has_include("espd_board_io_config.h")
#include "espd_board_io_config.h"
#endif

#ifndef ESPD_BSP_BUTTON_COUNT
#define ESPD_BSP_BUTTON_MAP_DEFAULT 1
#endif

static const char *TAG = "espd_bsp";

#if BSP_CAPS_BUTTONS
#ifndef ESPD_BSP_BUTTON_MAP_DEFAULT
static const bsp_button_t s_button_map[] = ESPD_BSP_BUTTON_MAP;
#endif

static button_handle_t s_buttons[BSP_BUTTON_NUM];
static int s_button_exposed_count;
static bool s_buttons_ready;
static void (*s_button_handler)(int idx, int pressed);
#endif

#ifdef ESPD_BSP_HAVE_LED
static led_strip_handle_t s_strip;
static TaskHandle_t s_refresh_task;
static volatile int s_led_dirty;
#endif

#define ESPD_BSP_DIN_QUEUE_LEN 16

typedef struct {
    uint8_t idx;
    uint8_t pressed;
} espd_bsp_din_event_t;

static espd_bsp_din_event_t s_din_queue[ESPD_BSP_DIN_QUEUE_LEN];
static volatile uint8_t s_din_qhead;
static volatile uint8_t s_din_qtail;

#if BSP_CAPS_BUTTONS
static void espd_bsp_button_event(void *button_handle, void *usr_data);
#endif

#ifndef ESPD_LED_REFRESH_TASK_PRIO
#define ESPD_LED_REFRESH_TASK_PRIO 2
#endif
#ifndef ESPD_LED_REFRESH_TASK_CORE
#define ESPD_LED_REFRESH_TASK_CORE 0
#endif
#ifndef ESPD_LED_REFRESH_MIN_INTERVAL_MS
#define ESPD_LED_REFRESH_MIN_INTERVAL_MS 10
#endif

#ifdef ESPD_BSP_HAVE_LED
static void espd_bsp_led_refresh_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_led_dirty) {
            s_led_dirty = 0;
            led_strip_refresh(s_strip);
        }
        vTaskDelay(pdMS_TO_TICKS(ESPD_LED_REFRESH_MIN_INTERVAL_MS));
        ulTaskNotifyTake(pdTRUE, 0);
    }
}
#endif /* ESPD_BSP_HAVE_LED */

#if BSP_CAPS_BUTTONS
static void espd_bsp_button_register(button_handle_t btn, int map_idx)
{
#if BUTTON_VER_MAJOR >= 4
    iot_button_register_cb(btn, BUTTON_PRESS_DOWN, NULL,
        espd_bsp_button_event, (void *)(intptr_t)map_idx);
    iot_button_register_cb(btn, BUTTON_PRESS_UP, NULL,
        espd_bsp_button_event, (void *)(intptr_t)map_idx);
#else
    iot_button_register_cb(btn, BUTTON_PRESS_DOWN,
        espd_bsp_button_event, (void *)(intptr_t)map_idx);
    iot_button_register_cb(btn, BUTTON_PRESS_UP,
        espd_bsp_button_event, (void *)(intptr_t)map_idx);
#endif
}
#endif /* BSP_CAPS_BUTTONS */

#if BSP_CAPS_BUTTONS
static int espd_bsp_din_queue_push(int idx, int pressed)
{
    uint8_t head = s_din_qhead;
    uint8_t next = (uint8_t)((head + 1) % ESPD_BSP_DIN_QUEUE_LEN);

    if (next == s_din_qtail)
        return 0;
    s_din_queue[head].idx = (uint8_t)idx;
    s_din_queue[head].pressed = (uint8_t)(pressed ? 1 : 0);
    s_din_qhead = next;
    return 1;
}

static void espd_bsp_button_event(void *button_handle, void *usr_data)
{
    int map_idx = (int)(intptr_t)usr_data;
    button_event_t event = iot_button_get_event(button_handle);
    int pressed = -1;

    if (map_idx < 0 || map_idx >= s_button_exposed_count)
        return;

    if (event == BUTTON_PRESS_DOWN)
        pressed = 1;
    else if (event == BUTTON_PRESS_UP)
        pressed = 0;
    else
        return;

    if (!espd_bsp_din_queue_push(map_idx, pressed))
        ESP_LOGW(TAG, "din queue full (dropped btn %d %s)", map_idx,
            pressed ? "down" : "up");
}
#endif /* BSP_CAPS_BUTTONS */

#ifdef ESPD_BSP_HAVE_LED
int bsp_led_count(void)
{
    return s_strip ? BSP_LED_STRIP_NUM : 0;
}

esp_err_t bsp_led_init(void)
{
    esp_err_t err;

    if (s_strip)
        return ESP_OK;

    err = bsp_led_strip_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bsp_led_strip_init: %s", esp_err_to_name(err));
        return err;
    }

    s_strip = bsp_led_strip_get_handle();
    if (!s_strip) {
        ESP_LOGW(TAG, "no LED strip handle");
        return ESP_FAIL;
    }

    err = bsp_led_fill(0, 0, 0);
    if (err == ESP_OK)
        err = led_strip_refresh(s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "initial LED paint: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WS2812 ready: %d LEDs on GPIO%d", BSP_LED_STRIP_NUM,
        (int)BSP_LED_STRIP_IO);

    if (!s_refresh_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(espd_bsp_led_refresh_task,
            "led_refresh", 2048, NULL, ESPD_LED_REFRESH_TASK_PRIO,
            &s_refresh_task, ESPD_LED_REFRESH_TASK_CORE);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "led_refresh task create failed");
            s_refresh_task = NULL;
        }
    }
    return ESP_OK;
}

esp_err_t bsp_led_set(int idx, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip)
        return ESP_ERR_INVALID_STATE;
    if (idx < 0 || idx >= BSP_LED_STRIP_NUM)
        return ESP_ERR_INVALID_ARG;
    return led_strip_set_pixel(s_strip, (uint32_t)idx, r, g, b);
}

esp_err_t bsp_led_fill(uint8_t r, uint8_t g, uint8_t b)
{
    int i;

    if (!s_strip)
        return ESP_ERR_INVALID_STATE;
    for (i = 0; i < BSP_LED_STRIP_NUM; i++) {
        esp_err_t err = led_strip_set_pixel(s_strip, (uint32_t)i, r, g, b);
        if (err != ESP_OK)
            return err;
    }
    return ESP_OK;
}

esp_err_t bsp_led_clear(void)
{
    if (!s_strip)
        return ESP_ERR_INVALID_STATE;
    return led_strip_clear(s_strip);
}

void bsp_led_mark_dirty(void)
{
    s_led_dirty = 1;
    if (s_refresh_task)
        xTaskNotifyGive(s_refresh_task);
}
#endif /* ESPD_BSP_HAVE_LED */

#if BSP_CAPS_BUTTONS
int bsp_button_count(void)
{
    return s_buttons_ready ? s_button_exposed_count : 0;
}

esp_err_t bsp_button_init(void)
{
    int btn_cnt = 0;
    esp_err_t err;

    if (s_buttons_ready)
        return ESP_OK;

    err = bsp_iot_button_create(s_buttons, &btn_cnt, BSP_BUTTON_NUM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bsp_iot_button_create: %s", esp_err_to_name(err));
        return err;
    }

    s_button_exposed_count = 0;

#ifdef ESPD_BSP_BUTTON_MAP_DEFAULT
    for (bsp_button_t b = 0; b < BSP_BUTTON_NUM; b++) {
        button_handle_t btn = s_buttons[b];
        if (!btn)
            continue;
        espd_bsp_button_register(btn, s_button_exposed_count);
        s_button_exposed_count++;
    }
#else
    for (int i = 0; i < ESPD_BSP_BUTTON_COUNT; i++) {
        bsp_button_t b = s_button_map[i];
        button_handle_t btn = s_buttons[b];
        if (!btn)
            continue;
        espd_bsp_button_register(btn, i);
    }
    s_button_exposed_count = ESPD_BSP_BUTTON_COUNT;
#endif

    s_buttons_ready = true;
    ESP_LOGI(TAG, "buttons ready: %d → espd/din/0..%d",
        s_button_exposed_count, s_button_exposed_count > 0
        ? s_button_exposed_count - 1 : 0);
    return ESP_OK;
}

void bsp_button_set_handler(void (*handler)(int idx, int pressed))
{
    s_button_handler = handler;
}

void bsp_button_poll(void)
{
    /* iot_button runs callbacks on esp_timer (small stack). Drain queued
     * presses here from the audio loop before touching Pd (clone resize etc). */
    while (s_din_qtail != s_din_qhead) {
        espd_bsp_din_event_t ev = s_din_queue[s_din_qtail];
        s_din_qtail = (uint8_t)((s_din_qtail + 1) % ESPD_BSP_DIN_QUEUE_LEN);
        if (s_button_handler)
            s_button_handler(ev.idx, ev.pressed);
    }
}
#endif /* BSP_CAPS_BUTTONS */

esp_err_t espd_bsp_sdcard_mount(const char *mount_point)
{
#ifdef BSP_SD_MOUNT_POINT
    if (mount_point && strcmp(mount_point, BSP_SD_MOUNT_POINT) != 0)
        ESP_LOGW(TAG, "mount_point %s ignored (BSP uses %s)",
            mount_point, BSP_SD_MOUNT_POINT);
#endif

#ifdef BSP_SD_DET
    if (BSP_SD_DET != GPIO_NUM_NC) {
        esp_io_expander_handle_t exp = bsp_io_expander_init();

        if (exp) {
            esp_io_expander_set_dir(exp, BSP_SD_DET, IO_EXPANDER_OUTPUT);
            esp_io_expander_set_level(exp, BSP_SD_DET, 1);
        }
    }
#endif

#if __has_include("bsp/esp_bsp_sdcard.h")
    if (bsp_sdcard_get_handle() != NULL)
        return ESP_OK;

    {
        bsp_sdcard_cfg_t cfg = {0};
        return bsp_sdcard_sdmmc_mount(&cfg);
    }
#elif BSP_CAPS_SDCARD && defined(BSP_SDCARD_HAS_GET_HANDLE)
    if (bsp_sdcard_get_handle() != NULL)
        return ESP_OK;
    (void)mount_point;
    {
        bsp_sdcard_cfg_t cfg = {0};
        return bsp_sdcard_sdmmc_mount(&cfg);
    }
#elif BSP_CAPS_SDCARD
    (void)mount_point;
    {
        bsp_sdcard_cfg_t cfg = {0};
        return bsp_sdcard_sdmmc_mount(&cfg);
    }
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
