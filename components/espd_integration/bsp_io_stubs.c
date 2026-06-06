/*
 * Weak defaults for optional bsp_io.h peripherals (no hardware linked).
 */

#include "bsp/bsp_io.h"
#include "espd_bsp_sdcard.h"
#include "espd_bsp_audio.h"

__attribute__((weak)) esp_err_t bsp_led_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) int bsp_led_count(void)
{
    return 0;
}

__attribute__((weak)) esp_err_t bsp_led_set(int idx, uint8_t r, uint8_t g, uint8_t b)
{
    (void)idx;
    (void)r;
    (void)g;
    (void)b;
    return ESP_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) esp_err_t bsp_led_fill(uint8_t r, uint8_t g, uint8_t b)
{
    (void)r;
    (void)g;
    (void)b;
    return ESP_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) esp_err_t bsp_led_clear(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) void bsp_led_mark_dirty(void)
{
}


__attribute__((weak)) esp_err_t bsp_button_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) int bsp_button_count(void)
{
    return 0;
}

__attribute__((weak)) void bsp_button_set_handler(void (*handler)(int idx, int pressed))
{
    (void)handler;
}

__attribute__((weak)) void bsp_button_poll(void)
{
}

__attribute__((weak)) esp_err_t bsp_sdcard_mount(const char *mount_point)
{
    (void)mount_point;
    return ESP_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) esp_err_t espd_bsp_sdcard_mount(const char *mount_point)
{
    return bsp_sdcard_mount(mount_point);
}

__attribute__((weak)) esp_err_t espd_bsp_audio_hw_init(
    const espd_bsp_audio_hw_params_t *params, espd_bsp_audio_hw_t *hw)
{
    (void)params;
    (void)hw;
    return ESP_ERR_NOT_SUPPORTED;
}
