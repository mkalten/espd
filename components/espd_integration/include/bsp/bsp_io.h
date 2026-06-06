/*
 * Optional board peripherals ESPD may use when a BSP provides them.
 *
 * Upstream BSP packages implement the symbols they support; default weak
 * stubs in espd_integration return ESP_ERR_NOT_SUPPORTED / zero.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifndef ESPD_BSP_IO_NO_LED_DECL
esp_err_t bsp_led_init(void);
int bsp_led_count(void);
esp_err_t bsp_led_set(int idx, uint8_t r, uint8_t g, uint8_t b);
esp_err_t bsp_led_fill(uint8_t r, uint8_t g, uint8_t b);
esp_err_t bsp_led_clear(void);
void bsp_led_mark_dirty(void);
#endif

esp_err_t bsp_button_init(void);
int bsp_button_count(void);
void bsp_button_set_handler(void (*handler)(int idx, int pressed));
void bsp_button_poll(void);

#ifndef ESPD_BSP_IO_NO_SDCARD_DECL
esp_err_t bsp_sdcard_mount(const char *mount_point);
#endif
