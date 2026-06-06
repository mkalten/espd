/*
 * USB OTG: TinyUSB, MSC (storage), CDC (serial), dev-sync.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

esp_err_t espd_usb_mount_flash_early_vfs(void);

#if CONFIG_ESPD_USE_USB_OTG
bool espd_usb_start_after_wifi(void);
#endif

#if CONFIG_ESPD_USE_USB_OTG && CONFIG_ESPD_USE_USB_MSC
bool espd_usb_msc_storage_present(void);
bool espd_usb_msc_host_mounted(void);
esp_err_t espd_usb_expose_msc_to_host(void);
esp_err_t espd_usb_ensure_msc_app_mount(void);
void espd_usb_drive_mode_wait(void);
#endif

void espd_usb_cdc_write(const void *data, size_t len);
int espd_usb_cdc_log_vprintf(const char *fmt, va_list args);
