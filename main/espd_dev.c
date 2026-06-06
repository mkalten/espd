/*
 * Host dev sync over TinyUSB CDC (rapid patching to local storage target).
 *
 * Protocol (host -> device):
 *   STATUS
 *   PUT <relpath> <nbytes> <crc32hex>
 *       -> +OK PUT skip (file already matches) or +OK PUT ready then <nbytes>
 *          raw bytes -> +OK PUT done <crc> (writes to <path>.tmp, rename on success)
 *   RELOAD
 *   MSG <pd-message>  (queued; evaluated on audio thread via pd_sendmsg)
 *   RESET  (reboot ESP after reply)
 *
 * Device replies (one line each, prefixed for filtering):
 *   +OK ...
 *   -ERR ...
 */

#include "espd_dev.h"
#include <stddef.h>

#if CONFIG_ESPD_DEV_SYNC

#include "driver/uart.h"
#if CONFIG_ESPD_DEV_CDC_SYNC
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#endif

#include "espd.h"
#include "espd_usb.h"
#include "espd_storage.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_crc.h"
#if CONFIG_ESPD_DEV_CDC_SYNC
#include "tinyusb_cdc_acm.h"
#endif
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

static const char *TAG = "espd_dev";

#define ESPD_DEV_TASK_CORE          0
/* Below TinyUSB device task (4) so CDC RX is not starved during PUT payload. */
#define ESPD_DEV_TASK_PRIO          3
#define ESPD_DEV_RX_CHUNK           4096
#define ESPD_DEV_PUT_FILEBUF        16384
#define ESPD_DEV_HASH_CHUNK         4096
#define ESPD_DEV_RX_RING            16384
/* fwrite/fsync + 4 KiB CDC read buffer; 6 KiB stack overflowed after RX_CHUNK bump. */
#define ESPD_DEV_TASK_STACK         12288
/* Room for PUT <path-with-spaces> <size> <crc> (path up to ESPD_DEV_PATH_MAX). */
#define ESPD_DEV_LINE_MAX           256
#define ESPD_DEV_PATH_MAX           384

typedef enum {
    DEV_CMD_NONE = 0,
    DEV_CMD_PUT,
    DEV_CMD_RELOAD,
    DEV_CMD_STATUS,
} dev_cmd_t;

typedef enum {
    DEV_TARGET_SD = 0,
    DEV_TARGET_FLASH,
} dev_target_t;

static TaskHandle_t s_dev_task;
static volatile bool s_reload_pending;
static volatile bool s_pdmsg_pending;
static char s_pdmsg[ESPD_DEV_LINE_MAX];
static portMUX_TYPE s_rx_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_rx_ring[ESPD_DEV_RX_RING];
static size_t s_rx_head;
static size_t s_rx_tail;

static dev_cmd_t s_cmd;
static char s_put_rel[ESPD_DEV_PATH_MAX];
static size_t s_put_remain;
static uint32_t s_put_expect_crc;
static uint32_t s_put_crc;
static FILE *s_put_fp;
static char s_put_tmp[ESPD_DEV_PATH_MAX];
static SemaphoreHandle_t s_put_mux;
static dev_target_t s_target = DEV_TARGET_SD;
/* Single drain buffer (espd_dev task only — not re-entrant). */
static uint8_t s_cdc_rx_buf[ESPD_DEV_RX_CHUNK];

static void dev_reply(const char *msg);
static void dev_put_data(const uint8_t *data, size_t len);

static void dev_rx_flush(void)
{
    portENTER_CRITICAL(&s_rx_lock);
    s_rx_head = 0;
    s_rx_tail = 0;
    portEXIT_CRITICAL(&s_rx_lock);
}

static void dev_rx_push(const uint8_t *data, size_t len)
{
    size_t i;
    size_t dropped = 0;

    portENTER_CRITICAL(&s_rx_lock);
    for (i = 0; i < len; i++) {
        size_t next = (s_rx_head + 1) % ESPD_DEV_RX_RING;
        if (next == s_rx_tail) {
            dropped++;
            continue;
        }
        s_rx_ring[s_rx_head] = data[i];
        s_rx_head = next;
    }
    portEXIT_CRITICAL(&s_rx_lock);
    if (dropped)
        ESP_LOGW(TAG, "CDC RX ring overflow, dropped %u bytes", (unsigned)dropped);
}

static size_t dev_rx_pop(uint8_t *out, size_t max)
{
    size_t n = 0;

    portENTER_CRITICAL(&s_rx_lock);
    while (n < max && s_rx_tail != s_rx_head) {
        out[n++] = s_rx_ring[s_rx_tail];
        s_rx_tail = (s_rx_tail + 1) % ESPD_DEV_RX_RING;
    }
    portEXIT_CRITICAL(&s_rx_lock);
    return n;
}

static void dev_drain_cdc_hw(void)
{
#if CONFIG_ESPD_DEV_SERIAL_SYNC
    int rx;
    while ((rx = uart_read_bytes(UART_NUM_0, s_cdc_rx_buf, sizeof(s_cdc_rx_buf), pdMS_TO_TICKS(0))) > 0)
        dev_rx_push(s_cdc_rx_buf, rx);
#elif CONFIG_ESPD_DEV_CDC_SYNC
    size_t rx;

    if (!tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0))
        return;

    while (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, s_cdc_rx_buf, sizeof(s_cdc_rx_buf), &rx)
               == ESP_OK
           && rx > 0)
        dev_rx_push(s_cdc_rx_buf, rx);
#endif
}

/* PUT payload bypasses the 16 KiB line ring (large files overflow it otherwise). */
static void dev_drain_cdc_put(void)
{
#if CONFIG_ESPD_DEV_SERIAL_SYNC
    int rx;
    while ((rx = uart_read_bytes(UART_NUM_0, s_cdc_rx_buf, sizeof(s_cdc_rx_buf), pdMS_TO_TICKS(0))) > 0)
        dev_put_data(s_cdc_rx_buf, rx);
#elif CONFIG_ESPD_DEV_CDC_SYNC
    size_t rx;

    if (!tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0))
        return;

    while (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, s_cdc_rx_buf, sizeof(s_cdc_rx_buf), &rx) == ESP_OK
           && rx > 0)
        dev_put_data(s_cdc_rx_buf, rx);
#endif
}

#if CONFIG_ESPD_DEV_CDC_SYNC
void espd_dev_cdc_rx_cb(int itf, cdcacm_event_t *event)
{
    (void)event;
    if (itf != TINYUSB_CDC_ACM_0)
        return;
    /* All CDC reads run on espd_dev task (TinyUSB callback stack is too small for
     * ESPD_DEV_RX_CHUNK drain loops; host probes the port even without espd_sync). */
    if (s_dev_task)
        xTaskNotifyGive(s_dev_task);
}
#endif

static void dev_reply(const char *msg)
{
    char line[192];
    int n;

    if (!msg)
        return;
    n = snprintf(line, sizeof(line), "%s\r\n", msg);
    if (n <= 0)
        return;
    espd_usb_cdc_write(line, (size_t)n);
}

static const char *dev_target_mount(dev_target_t target)
{
    if (target == DEV_TARGET_SD)
        return ESPD_SDCARD_MOUNT;
    return ESPD_STORAGE_MOUNT;
}

static const char *dev_target_name(dev_target_t target)
{
    if (target == DEV_TARGET_SD)
        return "sd";
    return "flash";
}

static int dev_sdcard_available(void)
{
#ifdef ESPD_USE_SDCARD
    return espd_storage_sdcard_ready();
#else
    return 0;
#endif
}

static int dev_flash_available(void)
{
    struct stat st;
    if (stat(ESPD_STORAGE_MOUNT, &st) == 0 && S_ISDIR(st.st_mode))
        return 1;

    return 0;
}

static dev_target_t dev_default_target(void)
{
#ifdef ESPD_USE_SDCARD
    if (dev_sdcard_available())
        return DEV_TARGET_SD;
#endif
    return DEV_TARGET_FLASH;
}

static void dev_refresh_target(void)
{
    s_target = dev_default_target();
}

static int dev_target_ready(dev_target_t target)
{
    if (target == DEV_TARGET_FLASH) {
        struct stat st;
        if (stat(ESPD_STORAGE_MOUNT, &st) != 0 || !S_ISDIR(st.st_mode))
            return 0;
        return 1;
    }
#ifdef ESPD_USE_SDCARD
    struct stat st;
    if (stat(ESPD_SDCARD_MOUNT, &st) != 0 || !S_ISDIR(st.st_mode))
        return 0;
    return 1;
#else
    return 0;
#endif
}

static int dev_rel_path_ok(const char *rel)
{
    size_t n;

    if (!rel || !rel[0] || rel[0] == '/')
        return 0;
    if (strstr(rel, "..") != NULL)
        return 0;
    n = strlen(rel);
    if (n >= ESPD_DEV_PATH_MAX)
        return 0;
    return 1;
}

static int dev_build_path(char *out, size_t outsz, const char *rel)
{
    int n = snprintf(out, outsz, "%s/%s", dev_target_mount(s_target), rel);
    if (n < 0 || (size_t)n >= outsz)
        return 0;
    return 1;
}

/* CRC-32 (same polynomial as Python zlib.crc32). Runs on espd_dev task only. */
static int dev_file_hash(const char *full, size_t *out_size, uint32_t *out_crc)
{
    static uint8_t buf[ESPD_DEV_HASH_CHUNK];
    FILE *fp;
    size_t n, total = 0;
    uint32_t crc = 0;
    struct stat st;
    unsigned chunks = 0;

    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;
    fp = fopen(full, "rb");
    if (!fp)
        return -2;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        crc = esp_crc32_le(crc, buf, (uint32_t)n);
        total += n;
        if ((++chunks & 7u) == 0)
            vTaskDelay(1);
    }
    fclose(fp);
    if (total != (size_t)st.st_size)
        return -2;
    *out_size = total;
    *out_crc = crc;
    return 0;
}

static void dev_put_cleanup_temp(void)
{
    if (s_put_tmp[0]) {
        unlink(s_put_tmp);
        s_put_tmp[0] = '\0';
    }
}

static int dev_put_commit(const char *final_full)
{
    if (!s_put_tmp[0])
        return -1;
    unlink(final_full);
    if (rename(s_put_tmp, final_full) != 0) {
        ESP_LOGW(TAG, "rename %s failed (%d)", s_put_rel, errno);
        dev_put_cleanup_temp();
        return -1;
    }
    s_put_tmp[0] = '\0';
    return 0;
}

static int dev_mkdir_parents(const char *fullpath)
{
    char tmp[ESPD_DEV_PATH_MAX];
    char *p;
    struct stat st;

    if (strlen(fullpath) >= sizeof(tmp))
        return -1;
    strcpy(tmp, fullpath);
    for (p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (stat(tmp, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                ESP_LOGW(TAG, "path component is not a directory: %s", tmp);
                *p = '/';
                return -1;
            }
        } else if (mkdir(tmp, 0755) != 0) {
            ESP_LOGW(TAG, "mkdir %s failed (%d)", tmp, errno);
            *p = '/';
            return -1;
        }
        *p = '/';
    }
    return 0;
}

static void dev_put_offer(const char *rel, size_t nbytes, uint32_t expect_crc)
{
    char full[ESPD_DEV_PATH_MAX];
    int open_errno = 0;
    size_t on_disk = 0;
    uint32_t disk_crc = 0;
    int err;

    dev_refresh_target();
#if CONFIG_ESPD_USE_USB_MSC
    if (s_target == DEV_TARGET_FLASH && espd_usb_msc_storage_present()) {
        esp_err_t mnt = espd_usb_ensure_msc_app_mount();
        if (mnt != ESP_OK) {
            dev_reply("-ERR reclaim /storage from host failed (eject USB volume on host)");
            return;
        }
    }
#endif
    if (!dev_target_ready(s_target)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "-ERR target %s not mounted", dev_target_name(s_target));
        dev_reply(msg);
        return;
    }
    if (!dev_rel_path_ok(rel)) {
        dev_reply("-ERR bad path");
        return;
    }
    if (nbytes == 0) {
        dev_reply("-ERR bad size");
        return;
    }
    if (!dev_build_path(full, sizeof(full), rel)) {
        dev_reply("-ERR path too long");
        return;
    }

    err = dev_file_hash(full, &on_disk, &disk_crc);
    if (err == 0 && on_disk == nbytes && disk_crc == expect_crc) {
        dev_reply("+OK PUT skip");
        return;
    }

    dev_rx_flush();
    dev_drain_cdc_hw();

    if (dev_mkdir_parents(full) != 0) {
        dev_reply("-ERR invalid parent path");
        return;
    }
    dev_put_cleanup_temp();
    if (snprintf(s_put_tmp, sizeof(s_put_tmp), "%s.tmp", full) >= (int)sizeof(s_put_tmp)) {
        dev_reply("-ERR path too long");
        return;
    }
    /* Parent creation for tmp path too (handles reconnect/mount races). */
    if (dev_mkdir_parents(s_put_tmp) != 0) {
        dev_reply("-ERR invalid parent path");
        s_put_tmp[0] = '\0';
        return;
    }
    /* Drop a partial temp from an earlier attempt; keep s_put_tmp for fopen. */
    unlink(s_put_tmp);
    s_put_fp = fopen(s_put_tmp, "wb");
    if (!s_put_fp && errno == ENOENT) {
        /* Deterministic fallback: ensure parent exists, then retry once. */
        if (dev_mkdir_parents(s_put_tmp) != 0) {
            dev_reply("-ERR invalid parent path");
            s_put_tmp[0] = '\0';
            return;
        }
        s_put_fp = fopen(s_put_tmp, "wb");
    }
    if (s_put_fp) {
        static char put_io_buf[ESPD_DEV_PUT_FILEBUF];
        setvbuf(s_put_fp, put_io_buf, _IOFBF, sizeof(put_io_buf));
    }
    if (!s_put_fp) {
        char msg[80];
        open_errno = errno;
        snprintf(msg, sizeof(msg), "-ERR open failed (%d)", open_errno);
        dev_reply(msg);
        ESP_LOGW(TAG, "PUT open %s failed (%d)", s_put_tmp, open_errno);
        s_put_tmp[0] = '\0';
        return;
    }

    strncpy(s_put_rel, rel, sizeof(s_put_rel) - 1);
    s_put_rel[sizeof(s_put_rel) - 1] = '\0';
    s_put_remain = nbytes;
    s_put_expect_crc = expect_crc;
    s_put_crc = 0;
    s_cmd = DEV_CMD_PUT;
    dev_reply("+OK PUT ready");
}

static void dev_put_data(const uint8_t *data, size_t len)
{
    char final_full[ESPD_DEV_PATH_MAX];
    size_t n;
    char reply[96];

    if (!s_put_mux || xSemaphoreTake(s_put_mux, portMAX_DELAY) != pdTRUE)
        return;
    if (!s_put_fp || s_cmd != DEV_CMD_PUT) {
        xSemaphoreGive(s_put_mux);
        return;
    }
    n = len;
    if (n > s_put_remain)
        n = s_put_remain;
    if (n) {
        if (fwrite(data, 1, n, s_put_fp) != n) {
            fclose(s_put_fp);
            s_put_fp = NULL;
            s_put_remain = 0;
            s_cmd = DEV_CMD_NONE;
            dev_put_cleanup_temp();
            xSemaphoreGive(s_put_mux);
            dev_reply("-ERR write failed");
            return;
        }
        s_put_crc = esp_crc32_le(s_put_crc, data, (uint32_t)n);
    }
    s_put_remain -= n;
    if (s_put_remain > 0) {
        xSemaphoreGive(s_put_mux);
        return;
    }

    {
        int fd = fileno(s_put_fp);
        fflush(s_put_fp);
#if CONFIG_ESPD_USE_USB_MSC
        /* fsync on internal flash is slow and can stall CDC; SD commit is fine after fflush. */
        if (fd >= 0 && s_target == DEV_TARGET_FLASH && espd_usb_msc_storage_present())
            fsync(fd);
#else
        (void)fd;
#endif
        fclose(s_put_fp);
        s_put_fp = NULL;
        s_cmd = DEV_CMD_NONE;

        if (s_put_crc != s_put_expect_crc) {
            dev_put_cleanup_temp();
            xSemaphoreGive(s_put_mux);
            snprintf(reply, sizeof(reply),
                "-ERR PUT crc exp %08" PRIx32 " got %08" PRIx32,
                s_put_expect_crc, s_put_crc);
            dev_reply(reply);
            return;
        }
        if (!dev_build_path(final_full, sizeof(final_full), s_put_rel)
                || dev_put_commit(final_full) != 0) {
            xSemaphoreGive(s_put_mux);
            dev_reply("-ERR commit failed");
            return;
        }
        xSemaphoreGive(s_put_mux);
        snprintf(reply, sizeof(reply), "+OK PUT done %08" PRIx32, s_put_crc);
        dev_reply(reply);
        espd_storage_resolve_paths();
    }
}

static void dev_queue_pdmsg(const char *text)
{
    size_t n;

    if (!g_espd_pd_running) {
        dev_reply("-ERR PD not running");
        return;
    }

    if (!text || !text[0]) {
        dev_reply("-ERR MSG empty");
        return;
    }
    n = strlen(text);
    if (n >= sizeof(s_pdmsg))
        n = sizeof(s_pdmsg) - 1;
    memcpy(s_pdmsg, text, n);
    s_pdmsg[n] = '\0';
    s_pdmsg_pending = true;
    dev_reply("+OK MSG queued");
}

static void dev_do_reload(void)
{
    dev_refresh_target();
#if CONFIG_ESPD_USE_USB_MSC
    if (s_target == DEV_TARGET_FLASH && espd_usb_msc_storage_present()) {
        esp_err_t mnt = espd_usb_ensure_msc_app_mount();
        if (mnt != ESP_OK) {
            dev_reply("-ERR reclaim /storage from host failed (eject USB volume on host)");
            return;
        }
    }
#endif
    if (!dev_target_ready(s_target)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "-ERR target %s not mounted", dev_target_name(s_target));
        dev_reply(msg);
        return;
    }
    if (!g_espd_pd_running) {
        dev_reply("-ERR PD not running");
        return;
    }
    s_reload_pending = true;
    ESP_LOGI(TAG, "RELOAD pending (main.pd on %s)", dev_target_mount(s_target));
    dev_reply("+OK RELOAD pending");
}

static void dev_trim_line(char *line)
{
    char *end;

    if (!line)
        return;
    while (*line == ' ' || *line == '\t')
        memmove(line, line + 1, strlen(line));
    end = line + strlen(line);
    while (end > line && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
}

/* PUT <relpath> <nbytes> <crc> — path may contain spaces; size/CRC are last tokens. */
static int dev_parse_put_line(char *line, char *rel, size_t relsz,
    size_t *nbytes, uint32_t *expect_crc)
{
    char *body = line + 4;
    char *crc_tok;
    char *size_tok;
    char *end;
    unsigned long nb;
    unsigned long crc;

    if (strncmp(line, "PUT ", 4) != 0)
        return 0;
    crc_tok = strrchr(body, ' ');
    if (!crc_tok)
        return 0;
    *crc_tok = '\0';
    size_tok = strrchr(body, ' ');
    if (!size_tok) {
        *crc_tok = ' ';
        return 0;
    }
    crc = strtoul(crc_tok + 1, &end, 16);
    if (end == crc_tok + 1)
        return 0;
    nb = strtoul(size_tok + 1, &end, 10);
    if (end == size_tok + 1 || nb == 0) {
        *crc_tok = ' ';
        return 0;
    }
    *size_tok = '\0';
    if (strlen(body) >= relsz) {
        *size_tok = ' ';
        *crc_tok = ' ';
        return 0;
    }
    strcpy(rel, body);
    *nbytes = (size_t)nb;
    *expect_crc = (uint32_t)crc;
    return 1;
}



static void dev_handle_line(char *line)
{
    if (!line)
        return;
    dev_trim_line(line);
    if (!line[0])
        return;

    if (!strcmp(line, "STATUS")) {
        char reply[128];
        dev_refresh_target();
        snprintf(reply, sizeof(reply), "+OK STATUS sdcard=%s internal=%s",
            dev_sdcard_available() ? "yes" : "no",
            dev_flash_available() ? "yes" : "no");
        dev_reply(reply);
        return;
    }
    if (!strcmp(line, "RESET")) {
        dev_reply("+OK RESET rebooting");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return;
    }
    if (!strcmp(line, "RELOAD")) {
        dev_do_reload();
        return;
    }
    if (!strncmp(line, "MSG ", 4)) {
        const char *body = line + 4;
        while (*body == ' ' || *body == '\t')
            body++;
        dev_queue_pdmsg(body);
        return;
    }
    if (!strncmp(line, "PUT ", 4)) {
        char rel[ESPD_DEV_PATH_MAX];
        size_t nbytes = 0;
        uint32_t expect_crc = 0;
        if (dev_parse_put_line(line, rel, sizeof(rel), &nbytes, &expect_crc))
            dev_put_offer(rel, nbytes, expect_crc);
        else
            dev_reply("-ERR PUT syntax");
        return;
    }
    dev_reply("-ERR unknown command");
}

static void dev_feed_bytes(const uint8_t *buf, size_t rx, int line_mode)
{
    static char line[ESPD_DEV_LINE_MAX];
    static size_t line_len;
    size_t off = 0;
    size_t i;

    if (!line_mode) {
        if (s_cmd == DEV_CMD_PUT && s_put_remain > 0)
            dev_put_data(buf, rx);
        return;
    }

    if (s_cmd == DEV_CMD_PUT && s_put_remain > 0) {
        size_t n = rx;
        if (n > s_put_remain)
            n = s_put_remain;
        dev_put_data(buf, n);
        off = n;
        if (off >= rx)
            return;
    }
    for (i = off; i < rx; i++) {
        uint8_t c = buf[i];

        if (c == '\r')
            continue;
        if (c == '\n') {
            if (line_len > 0) {
                line[line_len] = '\0';
                dev_handle_line(line);
                line_len = 0;
            }
            continue;
        }
        if (line_len + 1 < sizeof(line))
            line[line_len++] = (char)c;
    }
}

static void dev_poll_rx(void)
{
    size_t n;

    dev_drain_cdc_hw();
    while ((n = dev_rx_pop(s_cdc_rx_buf, sizeof(s_cdc_rx_buf))) > 0)
        dev_feed_bytes(s_cdc_rx_buf, n, 1);
}

static void dev_poll_put_payload(void)
{
    dev_drain_cdc_put();
}

static void espd_dev_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_cmd == DEV_CMD_PUT && s_put_remain > 0) {
            dev_poll_put_payload();
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(0));
        } else {
            dev_poll_rx();
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        }
    }
}

void espd_dev_init(void)
{
    if (s_dev_task)
        return;
    s_cmd = DEV_CMD_NONE;
    s_reload_pending = false;
    s_rx_head = 0;
    s_rx_tail = 0;
    s_put_tmp[0] = '\0';
    if (!s_put_mux)
        s_put_mux = xSemaphoreCreateMutex();
    dev_refresh_target();

#if CONFIG_ESPD_DEV_SERIAL_SYNC
    /* Initialize UART driver (safe to call if already installed by console) */
    uart_driver_install(UART_NUM_0, 2048, 2048, 0, NULL, 0);
    
    /* Only reconfigure baud rate if console is not using UART0.*/
#if !(CONFIG_ESP_CONSOLE_UART && CONFIG_ESP_CONSOLE_UART_NUM == 0)
    uart_config_t uart_config = {
        .baud_rate = 921600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_NUM_0, &uart_config);
#endif
#endif

    if (xTaskCreatePinnedToCore(espd_dev_task, "espd_dev", ESPD_DEV_TASK_STACK, NULL,
            ESPD_DEV_TASK_PRIO, &s_dev_task, ESPD_DEV_TASK_CORE) != pdPASS) {
        ESP_LOGW(TAG, "dev sync task create failed");
        s_dev_task = NULL;
        return;
    }
#if CONFIG_ESPD_DEV_CDC_SYNC
    ESP_LOGI(TAG, "CDC dev sync: PUT/RELOAD -> %s",
        dev_target_mount(s_target));
#else
    ESP_LOGI(TAG, "Serial dev sync: PUT/RELOAD -> %s (UART0)",
        dev_target_mount(s_target));
#endif
}

bool espd_dev_reload_pending(void)
{
    return s_reload_pending;
}

void espd_dev_clear_reload_pending(void)
{
    s_reload_pending = false;
}

const char *espd_dev_reload_dir(void)
{
    return dev_target_mount(s_target);
}

bool espd_dev_pdmsg_take(char *out, size_t outsz)
{
    size_t n;

    if (!s_pdmsg_pending || !out || outsz == 0)
        return false;
    n = strlen(s_pdmsg);
    if (n >= outsz)
        n = outsz - 1;
    memcpy(out, s_pdmsg, n);
    out[n] = '\0';
    s_pdmsg_pending = false;
    return true;
}

void espd_dev_sync_poll(void)
{
    if (espd_dev_reload_pending()) {
        pdmain_reload_patch_from(espd_dev_reload_dir());
        espd_dev_clear_reload_pending();
    }
    {
        char pdmsg[272];
        if (espd_dev_pdmsg_take(pdmsg, sizeof(pdmsg))) {
            size_t n = strlen(pdmsg);
            if (n > 0 && pdmsg[n - 1] != ';' && n + 1 < sizeof(pdmsg)) {
                pdmsg[n++] = ';';
                pdmsg[n] = '\0';
            }
            if (n > 0)
                pd_sendmsg(pdmsg, (int)n);
        }
    }
}

#else /* !CONFIG_ESPD_DEV_SYNC */

void espd_dev_init(void) {}
bool espd_dev_reload_pending(void) { return false; }
void espd_dev_clear_reload_pending(void) {}
const char *espd_dev_reload_dir(void) { return NULL; }
bool espd_dev_pdmsg_take(char *out, size_t outsz)
{
    (void)out;
    (void)outsz;
    return false;
}
void espd_dev_sync_poll(void) {}

#endif /* CONFIG_ESPD_DEV_SYNC */
