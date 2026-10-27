/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * AT telemetry, plus a modem reset on request.
 *
 * The ECM driver owns the network data port; this component opens a second CDC
 * port on the modem AT interface (vendor specific or CDC-ACM) and polls the
 * signal/registration information into cpe_status. Telemetry uses read-only
 * commands only; the single exception is AT+CFUN=1,1, sent from this task when
 * a deliberate ESP restart asks for the modem to be reset as well.
 *
 * Enumeration is handled by the CDC device event callback: it only parses the
 * configuration descriptor and wakes the AT task, which then probes the
 * candidate interfaces (one "AT" command each) outside the callback context.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "cpe_at.h"
#include "cpe_at_urc_internal.h"
#include "cpe_at_urc_match.h"
#include "cpe_status.h"
#include "iot_usbh_cdc.h"
#include "iot_usbh_cdc_type.h"

static const char *TAG = "cpe_at";

#if CONFIG_CPE_AT_ENABLE

#define AT_MAX_CANDIDATES      4
#define AT_PROBE_TIMEOUT_MS    3000
#define AT_CMD_TIMEOUT_MS      2000
#define AT_RETRY_MS            5000
#define AT_MAX_FAILURES        3
/* Sized for the largest response the AT port sees: a full AT+CMGL=4 list (24
 * records, each PDU at most 306 hex characters). Too small and the CDC driver
 * drops bytes ("CDC in ringbuf is full"), truncating the list. */
#define AT_RINGBUF_SIZE        16384
/* One assembled response or URC line. A 140-octet PDU in a +CMGL answer is
 * 306 hex characters plus a short +CMGL header. */
#define AT_LINE_MAX            1024
#define AT_TRANSFER_SIZE       512
/* Writing a command line to the CDC port. */
#define AT_WRITE_TIMEOUT_MS    1000
/* Polling period while a response is being collected. */
#define AT_RESPONSE_POLL_MS    20
/* Drain window after flushing the RX ring buffer: a USB transfer that was
 * already in flight can still land right after the flush. */
#define AT_FLUSH_SETTLE_MS     30
/* How long a timed-out caller waits for the abandoned session body to finish
 * touching its context before returning anyway. */
#define AT_SESSION_CANCEL_WAIT_MS 1000
/* Idle URC poll: only runs while the registry has subscribers. Telemetry keeps
 * its own CONFIG_CPE_AT_POLL_INTERVAL_S cadence. */
#define AT_URC_POLL_MS         100
/* USB device event registration: the CDC driver is installed by the WAN link
 * manager, which may still be starting. Retry once per second at first, then
 * report the degradation and slow down instead of spinning forever. */
#define AT_EVENT_REGISTER_FAST_TRIES    10
#define AT_EVENT_REGISTER_FAST_MS       1000
#define AT_EVENT_REGISTER_SLOW_MS       5000
/* Stack and priority of the telemetry task. */
#define AT_TASK_STACK          4096
#define AT_TASK_PRIO           3

typedef struct {
    uint8_t intf_num;
    bool cdc_acm;
} at_candidate_t;

/* Restart-triggered modem reset: IDLE -> PENDING (caller) -> RUNNING (task),
 * then back to IDLE once the task has given up or the command was sent. */
typedef enum {
    AT_RESET_IDLE = 0,
    AT_RESET_PENDING,
    AT_RESET_RUNNING,
} at_reset_state_t;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_reset_done;
static TaskHandle_t s_task;
static bool s_started;

/* Written before the semaphore is given, read after it is taken. */
static bool s_reset_ok;
/* Completion counter of the modem reset, guarded by s_lock: a waiter only
 * accepts a completion whose sequence differs from the one it saw before
 * requesting, so a give left behind by an earlier timeout is ignored. */
static uint32_t s_reset_seq;
/* Read on every AT command, so it is accessed with atomics instead of s_lock. */
static at_reset_state_t s_reset_state;
/* Port readiness for callers outside the AT task; guarded by s_lock. s_port
 * itself stays owned by the AT task. */
static bool s_port_ready;

/* Guarded by s_lock; written by the device event callback, read by the task. */
static uint8_t s_dev_addr;
static at_candidate_t s_cands[AT_MAX_CANDIDATES];
static int s_cand_count;
static uint32_t s_generation;

/* Owned by the AT task. */
static usbh_cdc_port_handle_t s_port;
static uint32_t s_port_generation;
static int s_failures;
/* Owned by the AT task. */
static TickType_t s_next_poll_tick;

/* Serialized session slot: exactly one body may run at a time, on the AT task.
 * `s_session_done` is a binary semaphore created in cpe_at_start(); completion
 * is paired with `s_session_seq` so a stale give is never mistaken for a new
 * session's completion (same pattern as the modem reset). */
typedef struct {
    cpe_at_session_body_t body;
    void *arg;
    esp_err_t result;
} at_session_slot_t;

static at_session_slot_t s_session_slot;
static SemaphoreHandle_t s_session_done;
static bool s_session_active;
static uint32_t s_session_seq;
/* Set by a caller whose session ran out of time so the AT task abandons the
 * body before the caller's stack context disappears. Stores seq + 1 so 0
 * means "none". */
static volatile uint32_t s_session_cancel_seq;
/* Owned by the AT task: sequence number of the body it is running, 0 idle. */
static uint32_t s_session_running_seq;
/* Slack for the longest strings a modem answers with: model/firmware revisions
 * grow with build suffixes and the IMEI may come as a 16 digit IMEISV. */
static char s_model[40];
static char s_fw[40];
static char s_imei[20];

static bool at_reset_pending(void)
{
    return __atomic_load_n(&s_reset_state, __ATOMIC_ACQUIRE) == AT_RESET_PENDING;
}

static bool at_session_cancelled(void)
{
    uint32_t cancel = __atomic_load_n(&s_session_cancel_seq, __ATOMIC_ACQUIRE);
    return cancel != 0 && cancel == s_session_running_seq + 1;
}

static void at_set_reset_state(at_reset_state_t state)
{
    __atomic_store_n(&s_reset_state, state, __ATOMIC_RELEASE);
}

static void at_set_port_ready(bool ready)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_port_ready = ready;
    xSemaphoreGive(s_lock);
}

/*--------------------------------- Descriptor scan ---------------------------------*/

typedef struct {
    uint8_t num;
    uint8_t alt;
    uint8_t cls;
    uint8_t subcls;
    bool bulk_in;
    bool bulk_out;
} at_itf_scan_t;

static bool at_itf_is_candidate(const at_itf_scan_t *itf)
{
    if (itf->alt != 0) {
        return false;
    }
    if (itf->cls == USB_CLASS_VENDOR_SPEC) {
        return itf->bulk_in && itf->bulk_out;
    }
    /* The CDC data interface (0x0A) is resolved from the control interface by
     * the driver, so only the ACM control interface is listed here. ECM/NCM/
     * MBIM control interfaces use other subclasses and are skipped. */
    return itf->cls == USB_COMMUNICATIONS_INTERFACE_CLASS &&
           itf->subcls == USB_COMMUNICATIONS_INTERFACE_SUBCLASS_ABSTRACT_CONTROL_MODEL;
}

static void at_add_candidate(const at_itf_scan_t *itf,
                             at_candidate_t *out, int *count)
{
    if (*count >= AT_MAX_CANDIDATES || !at_itf_is_candidate(itf)) {
        return;
    }
    for (int i = 0; i < *count; i++) {
        if (out[i].intf_num == itf->num) {
            return;
        }
    }
    out[*count].intf_num = itf->num;
    out[*count].cdc_acm = (itf->cls != USB_CLASS_VENDOR_SPEC);
    (*count)++;
}

static void at_scan_interfaces(const usb_config_desc_t *cfg,
                               at_candidate_t *out, int *count)
{
    at_itf_scan_t cur = { 0 };
    bool have_cur = false;
    int offset = 0;
    const usb_standard_desc_t *desc = (const usb_standard_desc_t *)cfg;

    while ((desc = usb_parse_next_descriptor(desc, cfg->wTotalLength, &offset)) != NULL) {
        if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            if (have_cur) {
                at_add_candidate(&cur, out, count);
            }
            const usb_intf_desc_t *itf = (const usb_intf_desc_t *)desc;
            memset(&cur, 0, sizeof(cur));
            cur.num = itf->bInterfaceNumber;
            cur.alt = itf->bAlternateSetting;
            cur.cls = itf->bInterfaceClass;
            cur.subcls = itf->bInterfaceSubClass;
            have_cur = true;
        } else if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && have_cur) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)desc;
            if (USB_EP_DESC_GET_XFERTYPE(ep) == USB_TRANSFER_TYPE_BULK) {
                if (USB_EP_DESC_GET_EP_DIR(ep)) {
                    cur.bulk_in = true;
                } else {
                    cur.bulk_out = true;
                }
            }
        }
    }
    if (have_cur) {
        at_add_candidate(&cur, out, count);
    }
}

/*---------------------------------- Device events ----------------------------------*/

static void at_dev_event_cb(usbh_cdc_device_event_t event, usbh_cdc_device_event_data_t *data, void *ctx)
{
    (void)ctx;
    if (data == NULL || s_lock == NULL) {
        return;
    }

    if (event == CDC_HOST_DEVICE_EVENT_CONNECTED) {
        if (data->new_dev.active_config_desc == NULL || data->new_dev.device_desc == NULL) {
            return;
        }
        at_candidate_t cands[AT_MAX_CANDIDATES];
        int count = 0;
        at_scan_interfaces(data->new_dev.active_config_desc, cands, &count);
        if (count == 0) {
            return;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_dev_addr = data->new_dev.dev_addr;
        memcpy(s_cands, cands, sizeof(cands));
        s_cand_count = count;
        s_generation++;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "Device %u exposes %d AT interface candidate(s)",
                 data->new_dev.dev_addr, count);
        if (s_task != NULL) {
            xTaskNotifyGive(s_task);
        }
    } else if (event == CDC_HOST_DEVICE_EVENT_DISCONNECTED) {
        bool ours = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_cand_count > 0 && s_dev_addr == data->dev_gone.dev_addr) {
            s_cand_count = 0;
            s_generation++;
            ours = true;
        }
        xSemaphoreGive(s_lock);
        if (ours) {
            ESP_LOGI(TAG, "AT device %u disconnected", data->dev_gone.dev_addr);
            if (s_task != NULL) {
                xTaskNotifyGive(s_task);
            }
        }
    }
}

/*------------------------------------ Port I/O -------------------------------------*/

static esp_err_t at_port_open(uint8_t dev_addr, uint8_t intf_num)
{
    usbh_cdc_port_config_t cfg = {
        .dev_addr = dev_addr,
        .itf_num = intf_num,
        .in_ringbuf_size = AT_RINGBUF_SIZE,
        .out_ringbuf_size = 0,
        .in_transfer_buffer_size = AT_TRANSFER_SIZE,
        .out_transfer_buffer_size = AT_TRANSFER_SIZE,
        .flags = USBH_CDC_FLAGS_DISABLE_NOTIFICATION,
    };
    return usbh_cdc_port_open(&cfg, &s_port);
}

/* Deliberately no CDC SET_CONTROL_LINE_STATE (DTR/RTS): an interface that NAKs
 * the class request wedges the device's default pipe forever, because the host
 * stack does not implement usb_transfer_t::timeout_ms. That would also break the
 * ECM control requests. The AT interface of this modem answers without DTR. */

static esp_err_t at_send_line(const char *cmd)
{
    char line[64];
    int len = snprintf(line, sizeof(line), "%s\r", cmd);
    if (len < 0 || len >= (int)sizeof(line)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return usbh_cdc_write_bytes(s_port, (const uint8_t *)line, (size_t)len,
                                pdMS_TO_TICKS(AT_WRITE_TIMEOUT_MS));
}

typedef enum {
    AT_FINAL_NONE,
    AT_FINAL_OK,
    AT_FINAL_ERROR,
} at_final_t;

/* Response collection state for one read. Complete lines are written here;
 * everything after the final line is discarded so a late answer cannot leak
 * into the next command. */
typedef struct {
    char *buf;
    size_t size;
    size_t used;
    const char *const *extra;
    size_t n_extra;
    bool final_ok;
    bool final_error;
    bool extra_seen;
    bool overflow;
} at_rx_target_t;

/* Terminator state: "\r\n" arrives as two bytes, and a final line stops the
 * read at "\r". Remember whether that "\r" was kept so its "\n" is kept too,
 * and drop the "\n" that follows a discarded line. */
typedef enum {
    AT_CR_NONE = 0,
    AT_CR_KEEP,
    AT_CR_DROP,
} at_cr_t;

/* Assembly buffer for the line currently being received. */
static char s_line[AT_LINE_MAX];
static size_t s_line_len;
static bool s_line_truncated;
static at_cr_t s_cr;

static void at_line_reset(void)
{
    s_line_len = 0;
    s_line[0] = '\0';
    s_line_truncated = false;
    s_cr = AT_CR_NONE;
}

static void at_resp_append(at_rx_target_t *t, const char *data, size_t len)
{
    if (t == NULL || t->buf == NULL || t->size == 0) {
        return;
    }
    if (t->used + 1 >= t->size) {
        t->overflow = true;
        return;
    }
    size_t room = t->size - 1 - t->used;
    size_t n = len < room ? len : room;
    memcpy(t->buf + t->used, data, n);
    t->used += n;
    t->buf[t->used] = '\0';
    if (n < len) {
        t->overflow = true;
    }
}

static bool at_rx_target_done(const at_rx_target_t *t)
{
    return t != NULL &&
           (t->final_ok || t->final_error || t->extra_seen || t->overflow);
}

static void at_extra_check(at_rx_target_t *t)
{
    if (t == NULL || t->extra == NULL || t->extra_seen) {
        return;
    }
    for (size_t i = 0; i < t->n_extra; i++) {
        const char *tok = t->extra[i];
        if (tok == NULL || tok[0] == '\0') {
            continue;
        }
        if (t->buf != NULL && strstr(t->buf, tok) != NULL) {
            t->extra_seen = true;
            return;
        }
        if (s_line_len > 0 && strstr(s_line, tok) != NULL) {
            /* A prompt (">") has no terminator, so the token can show up in a
             * partial line. Keep it in the response text: callers look for
             * tokens such as "+CMGS:" there. */
            at_resp_append(t, s_line, s_line_len);
            at_line_reset();
            t->extra_seen = true;
            return;
        }
    }
}

/* Returns true when the completed line belongs in the response text, which
 * also means its terminator belongs there. A subscribed URC is delivered
 * instead of being appended; after the final line everything except further
 * URCs is dropped. */
static bool at_line_complete(at_rx_target_t *t)
{
    if (s_line_truncated) {
        if (t != NULL && !at_rx_target_done(t)) {
            t->overflow = true;
        }
        at_line_reset();
        return false;
    }

    cpe_at_line_kind_t kind = cpe_at_urc_line_kind(s_line, s_line_len);
    if (kind == CPE_AT_LINE_OTHER && cpe_at_urc_dispatch(s_line, s_line_len)) {
        at_line_reset();
        return false;
    }

    bool done = at_rx_target_done(t);
    if (!done) {
        at_resp_append(t, s_line, s_line_len);
        if (t != NULL) {
            if (kind == CPE_AT_LINE_FINAL_OK) {
                t->final_ok = true;
            } else if (kind == CPE_AT_LINE_FINAL_ERROR) {
                t->final_error = true;
            }
        }
    }
    at_line_reset();
    return !done && t != NULL;
}

static void at_rx_feed(at_rx_target_t *t, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\r') {
            bool keep = at_line_complete(t);
            if (keep) {
                at_resp_append(t, &c, 1);
            }
            s_cr = keep ? AT_CR_KEEP : AT_CR_DROP;
            at_extra_check(t);
        } else if (c == '\n') {
            if (s_line_len == 0 && s_cr != AT_CR_NONE) {
                if (s_cr == AT_CR_KEEP) {
                    at_resp_append(t, &c, 1);
                }
                s_cr = AT_CR_NONE;
                continue;
            }
            s_cr = AT_CR_NONE;
            bool keep = at_line_complete(t);
            if (keep) {
                at_resp_append(t, &c, 1);
            }
            at_extra_check(t);
        } else {
            s_cr = AT_CR_NONE;
            if (s_line_len + 1 < AT_LINE_MAX) {
                s_line[s_line_len++] = c;
                s_line[s_line_len] = '\0';
            } else {
                s_line_truncated = true;
            }
            at_extra_check(t);
        }
    }
}

/* Move everything the CDC ring buffer holds into the line pump. Bytes that
 * arrive after the final line are classified too: subscribed URCs are
 * delivered, other output is dropped. Draining past the final line also
 * consumes the "\n" that may follow the "\r" it stopped at. */
static void at_rx_drain(at_rx_target_t *t)
{
    uint8_t chunk[64];

    for (;;) {
        size_t avail = 0;
        if (usbh_cdc_get_rx_buffer_size(s_port, &avail) != ESP_OK || avail == 0) {
            return;
        }
        size_t want = avail < sizeof(chunk) ? avail : sizeof(chunk);
        size_t len = want;
        if (usbh_cdc_read_bytes(s_port, chunk, &len, 0) != ESP_OK || len == 0) {
            return;
        }
        at_rx_feed(t, chunk, len);
    }
}

/* Drop what is already queued before a command without losing a URC: complete
 * subscribed URCs are dispatched, and a partial line is kept only when it can
 * still become one. A USB transfer in flight may land right after the first
 * drain, so settle and drain again. */
static void at_rx_discard_stale(void)
{
    at_rx_drain(NULL);
    vTaskDelay(pdMS_TO_TICKS(AT_FLUSH_SETTLE_MS));
    at_rx_drain(NULL);
    if (!cpe_at_urc_partial_subscribed(s_line, s_line_len)) {
        at_line_reset();
    }
}

static esp_err_t at_rx_collect(at_rx_target_t *t, uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();

    for (;;) {
        if (at_reset_pending() || at_session_cancelled()) {
            return ESP_ERR_INVALID_STATE;
        }
        at_rx_drain(t);
        if (t->final_ok || t->final_error || t->extra_seen) {
            return ESP_OK;
        }
        if (t->overflow) {
            return ESP_ERR_INVALID_SIZE;
        }
        if ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS >= timeout_ms) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(AT_RESPONSE_POLL_MS));
    }
}

static esp_err_t at_io_send_line(const char *cmd)
{
    char line[128];
    int len = snprintf(line, sizeof(line), "%s\r", cmd);
    if (len < 0 || len >= (int)sizeof(line)) {
        return ESP_ERR_INVALID_SIZE;
    }
    at_rx_discard_stale();
    return usbh_cdc_write_bytes(s_port, (const uint8_t *)line, (size_t)len,
                                pdMS_TO_TICKS(AT_WRITE_TIMEOUT_MS));
}

static esp_err_t at_io_write(const uint8_t *data, size_t len)
{
    return usbh_cdc_write_bytes(s_port, data, len,
                                pdMS_TO_TICKS(AT_WRITE_TIMEOUT_MS));
}

static esp_err_t at_io_read_until(const char *const *extra, size_t n_extra,
                                  uint32_t timeout_ms, char *resp, size_t resp_size)
{
    at_rx_target_t target = {
        .buf = resp,
        .size = resp_size,
        .extra = extra,
        .n_extra = n_extra,
    };
    resp[0] = '\0';
    return at_rx_collect(&target, timeout_ms);
}

static const cpe_at_io_t s_at_io = {
    .send_line = at_io_send_line,
    .write = at_io_write,
    .read_until = at_io_read_until,
};

static at_final_t at_command(const char *cmd, char *resp, size_t resp_size,
                             uint32_t timeout_ms)
{
    /* A pending reset preempts telemetry; the reset command itself runs in the
     * RUNNING state, so it is not blocked by its own request. */
    if (s_port == NULL || at_reset_pending()) {
        return AT_FINAL_NONE;
    }
    at_rx_discard_stale();
    if (at_send_line(cmd) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send %s", cmd);
        return AT_FINAL_NONE;
    }
    at_rx_target_t target = { .buf = resp, .size = resp_size };
    if (at_rx_collect(&target, timeout_ms) != ESP_OK) {
        ESP_LOGD(TAG, "No complete response to %s", cmd);
        return AT_FINAL_NONE;
    }
    ESP_LOGD(TAG, "AT %s -> %s", cmd, resp);
    if (target.final_ok) {
        return AT_FINAL_OK;
    }
    return target.final_error ? AT_FINAL_ERROR : AT_FINAL_NONE;
}

/*------------------------------------ Parsing --------------------------------------*/

static int at_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* Some firmwares keep AT+CSCS at UCS2, so text fields such as the carrier name
 * arrive as UTF-16BE hex ("4E2D56FD"). Convert when the string is exactly that
 * and the result is printable text; otherwise keep the original. */
static bool at_ucs2_to_utf8(const char *in, char *out, size_t out_size)
{
    size_t len = strlen(in);
    if (len < 4 || len % 4 != 0 || out_size == 0) {
        return false;
    }

    size_t used = 0;
    for (size_t i = 0; i < len; i += 4) {
        uint32_t cp = 0;
        for (size_t k = 0; k < 4; k++) {
            int nibble = at_hex_nibble(in[i + k]);
            if (nibble < 0) {
                return false;
            }
            cp = (cp << 4) | (uint32_t)nibble;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 7 < len) {
            uint32_t low = 0;
            bool paired = true;
            for (size_t k = 0; k < 4; k++) {
                int nibble = at_hex_nibble(in[i + 4 + k]);
                if (nibble < 0) {
                    paired = false;
                    break;
                }
                low = (low << 4) | (uint32_t)nibble;
            }
            if (paired && low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 4;
            }
        }
        if ((cp < 0x20 && cp != '\t' && cp != '\r' && cp != '\n') ||
                (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }

        uint8_t utf8[4];
        size_t n;
        if (cp <= 0x7F) {
            utf8[0] = (uint8_t)cp;
            n = 1;
        } else if (cp <= 0x7FF) {
            utf8[0] = (uint8_t)(0xC0 | (cp >> 6));
            utf8[1] = (uint8_t)(0x80 | (cp & 0x3F));
            n = 2;
        } else if (cp <= 0xFFFF) {
            utf8[0] = (uint8_t)(0xE0 | (cp >> 12));
            utf8[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
            utf8[2] = (uint8_t)(0x80 | (cp & 0x3F));
            n = 3;
        } else {
            utf8[0] = (uint8_t)(0xF0 | (cp >> 18));
            utf8[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
            utf8[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
            utf8[3] = (uint8_t)(0x80 | (cp & 0x3F));
            n = 4;
        }
        if (used + n >= out_size) {
            return false;
        }
        memcpy(out + used, utf8, n);
        used += n;
    }
    out[used] = '\0';
    return used > 0;
}

static void at_parse_csq(const char *resp)
{
    const char *p = strstr(resp, "+CSQ:");
    if (p == NULL) {
        return;
    }
    p += 5;
    char *end = NULL;
    long csq = strtol(p, &end, 10);
    if (end == p) {
        return;
    }
    if (csq >= 0 && csq <= 31) {
        cpe_status_set_rssi((int)(-113 + 2 * csq), true);
    } else {
        cpe_status_set_rssi(0, false);
    }
}

static void at_parse_cops(const char *resp)
{
    char carrier[64] = { 0 };
    const char *p = strstr(resp, "+COPS:");
    if (p != NULL) {
        const char *begin = strchr(p, '"');
        const char *end = (begin != NULL) ? strchr(begin + 1, '"') : NULL;
        if (end != NULL) {
            size_t len = (size_t)(end - begin - 1);
            if (len >= sizeof(carrier)) {
                len = sizeof(carrier) - 1;
            }
            memcpy(carrier, begin + 1, len);
            carrier[len] = '\0';
        }
    }

    char decoded[sizeof(carrier)];
    if (carrier[0] != '\0' && at_ucs2_to_utf8(carrier, decoded, sizeof(decoded))) {
        snprintf(carrier, sizeof(carrier), "%s", decoded);
    }
    cpe_status_set_carrier(carrier);
}

static void at_parse_registration(const char *resp, const char *tag)
{
    const char *p = strstr(resp, tag);
    if (p == NULL) {
        return;
    }
    p += strlen(tag);
    char *end = NULL;
    (void)strtol(p, &end, 10);      /* <n> */
    if (end == p || *end != ',') {
        return;
    }
    char *stat_end = NULL;
    long stat = strtol(end + 1, &stat_end, 10);
    if (stat_end == end + 1) {
        return;
    }
    cpe_status_set_modem_registered(stat == 1 || stat == 5);
}

static const char *at_skip_ws(const char *p);

/* +CGATT: <state>, 1 = attached to the packet switched domain. */
static void at_parse_cgatt(const char *resp)
{
    const char *p = strstr(resp, "+CGATT:");
    if (p == NULL) {
        return;
    }
    char *end = NULL;
    long attached = strtol(p + 7, &end, 10);
    if (end == p + 7) {
        return;
    }
    cpe_status_set_ps(attached == 1, true);
}

/* +CGACT: <cid>,<state> is repeated per defined context; without any line
 * (no context defined) the answer is still OK, so "no line" means inactive. */
static void at_parse_cgact(const char *resp)
{
    bool active = false;
    const char *p = resp;

    while ((p = strstr(p, "+CGACT:")) != NULL) {
        p += 7;
        char *cid_end = NULL;
        (void)strtol(p, &cid_end, 10);
        if (cid_end == p || *cid_end != ',') {
            continue;
        }
        char *state_end = NULL;
        long state = strtol(cid_end + 1, &state_end, 10);
        if (state_end != cid_end + 1 && state == 1) {
            active = true;
        }
    }
    cpe_status_set_pdp(active, true);
}

/* Quectel LTE: +QCSQ: "LTE",<rssi>,<rsrp>,<sinr>,<rsrq>.
 * SINR is coded 0..250, actual dB = value / 5 - 20. Other RATs and
 * "NOSERVICE" carry no LTE metrics, so the values are marked invalid. */
static void at_parse_qcsq(const char *resp)
{
    const char *p = strstr(resp, "+QCSQ:");
    if (p == NULL) {
        return;
    }
    p = at_skip_ws(p + 6);
    if (*p == '"') {
        p++;
    }
    const char *rat_end = strchr(p, '"');
    if (rat_end != NULL && (size_t)(rat_end - p) == 3 && strncmp(p, "LTE", 3) == 0) {
        long values[4] = { 0 };
        const char *s = rat_end + 1;
        bool parsed = true;
        for (int i = 0; i < 4; i++) {
            s = at_skip_ws(s);
            if (*s == ',') {
                s = at_skip_ws(s + 1);
            }
            char *end = NULL;
            values[i] = strtol(s, &end, 10);
            if (end == s) {
                parsed = false;
                break;
            }
            s = end;
        }
        if (parsed) {
            long rsrp = values[1];
            long sinr_raw = values[2];
            long rsrq = values[3];
            if (rsrp >= -160 && rsrp <= -40 &&
                    sinr_raw >= 0 && sinr_raw <= 250 &&
                    rsrq >= -30 && rsrq <= 0) {
                cpe_status_set_signal_quality((int)rsrp, (int)rsrq,
                                              (int)(sinr_raw / 5 - 20), true);
                return;
            }
        }
    }
    cpe_status_set_signal_quality(0, 0, 0, false);
}

/* +QTEMP: "tsens0",36,"tsens1",37 on multi-sensor firmware, a single number
 * on others. Sensor names are skipped; the first plausible reading is used. */
static bool at_parse_qtemp(const char *resp, int *temp_c)
{
    const char *p = strstr(resp, "+QTEMP:");
    if (p == NULL) {
        return false;
    }
    p += 7;

    while (*p != '\0' && *p != '\r' && *p != '\n') {
        if (*p == '"') {
            const char *name_end = strchr(p + 1, '"');
            if (name_end == NULL) {
                return false;
            }
            p = name_end + 1;
        } else if ((*p >= '0' && *p <= '9') || *p == '-') {
            char *end = NULL;
            long value = strtol(p, &end, 10);
            if (end == p) {
                p++;
                continue;
            }
            if (value >= -40 && value <= 125) {
                *temp_c = (int)value;
                return true;
            }
            p = end;
        } else {
            p++;
        }
    }
    return false;
}

static const char *at_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r') {
        p++;
    }
    return p;
}

static size_t at_trim_end(const char *s, size_t len)
{
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n' ||
                       s[len - 1] == ' ' || s[len - 1] == '\t')) {
        len--;
    }
    return len;
}

static void at_copy_line(char *dst, size_t dst_size, const char *src, size_t len)
{
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Copies into a fixed field and reports a truncation instead of hiding it: a
 * silently cut model or IMEI looks like a modem reporting garbage. */
static void at_store_str(char *dst, size_t dst_size, const char *src,
                         const char *what)
{
    int n = snprintf(dst, dst_size, "%s", src);
    if (n < 0 || (size_t)n >= dst_size) {
        ESP_LOGW(TAG, "%s does not fit in %u bytes, truncated",
                 what, (unsigned)(dst_size - 1));
    }
}

/* True when the answer carries the given +CME ERROR code. The number is parsed
 * instead of matching a fixed substring, so "10" is not confused with the 10x
 * codes and any spacing after the colon is tolerated. */
static bool at_cme_error_is(const char *resp, long code)
{
    const char *cme = strstr(resp, "+CME ERROR:");
    if (cme == NULL) {
        return false;
    }
    const char *p = at_skip_ws(cme + strlen("+CME ERROR:"));
    char *end = NULL;
    long value = strtol(p, &end, 10);
    if (end == p) {
        return false;
    }
    /* The code must stand alone: "10" must not match "100" or "10abc". */
    return value == code && (*end == '\0' || *end == '\r' || *end == '\n' ||
                             *end == ' ');
}

/* Returns -1 when the answer is not understood, 0 when no card is present,
 * 1 when a card is present (READY, PIN/PUK locked or still initialising). */
static int at_parse_cpin(const char *resp)
{
    if (resp == NULL) {
        return -1;
    }
    /* 10 is "SIM not inserted"; other 10x codes must not be mistaken for it. */
    bool sim_missing = strstr(resp, "NOT INSERTED") != NULL ||
                       at_cme_error_is(resp, 10);
    if (sim_missing) {
        return 0;
    }
    const char *p = strstr(resp, "+CPIN:");
    if (p == NULL) {
        return -1;
    }
    p = at_skip_ws(p + 6);
    if (strncmp(p, "READY", 5) == 0 || strncmp(p, "SIM PIN", 7) == 0 ||
            strncmp(p, "SIM PUK", 7) == 0 || strncmp(p, "NOT READY", 9) == 0) {
        return 1;
    }
    return -1;
}

/* Extract the number from "+CNUM: <name>,<number>,<type>". Many carriers do
 * not write EF_MSISDN, so a missing number is a normal answer. */
static bool at_parse_cnum(const char *resp, char *number, size_t number_size)
{
    const char *p = strstr(resp, "+CNUM:");
    if (p == NULL) {
        return false;
    }
    const char *s = at_skip_ws(p + 6);
    if (*s == '"') {
        /* Skip the (quoted) name field; it may itself contain commas. */
        const char *name_end = strchr(s + 1, '"');
        if (name_end == NULL) {
            return false;
        }
        s = name_end + 1;
        if (*s == ',') {
            s++;
        }
    } else {
        s = strchr(s, ',');
        if (s == NULL) {
            return false;
        }
        s++;
    }
    s = at_skip_ws(s);
    const char *end;
    if (*s == '"') {
        s++;
        end = strchr(s, '"');
        if (end == NULL) {
            return false;
        }
    } else {
        end = s;
        while (*end != '\0' && *end != ',' && *end != '\r' && *end != '\n') {
            end++;
        }
    }
    size_t len = at_trim_end(s, (size_t)(end - s));
    if (len == 0 || len >= number_size) {
        return false;
    }
    at_copy_line(number, number_size, s, len);
    return true;
}

static void at_parse_ati(const char *resp)
{
    char model[sizeof(s_model)] = { 0 };
    char fw[sizeof(s_fw)] = { 0 };
    int index = 0;
    const char *p = resp;

    while (p != NULL && *p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t len = at_trim_end(p, eol != NULL ? (size_t)(eol - p) : strlen(p));
        if (len > 0) {
            char line[64];
            at_copy_line(line, sizeof(line), p, len);
            const char *s = at_skip_ws(line);
            if (strcmp(s, "OK") != 0 && strcmp(s, "ERROR") != 0 && strcmp(s, "ATI") != 0) {
                if (strncmp(s, "Revision:", 9) == 0) {
                    if (fw[0] == '\0') {
                        snprintf(fw, sizeof(fw), "%s", at_skip_ws(s + 9));
                    }
                } else if (strncmp(s, "Model:", 6) == 0) {
                    if (model[0] == '\0') {
                        snprintf(model, sizeof(model), "%s", at_skip_ws(s + 6));
                    }
                } else if (strncmp(s, "Manufacturer:", 13) != 0 && index >= 1 && model[0] == '\0') {
                    /* The first meaningful line is usually the vendor name
                     * ("Quectel", "SIMCOM_Ltd", ...), the next one the model. */
                    snprintf(model, sizeof(model), "%s", s);
                }
                index++;
            }
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }

    if (model[0] != '\0') {
        at_store_str(s_model, sizeof(s_model), model, "Modem model");
    }
    if (fw[0] != '\0') {
        at_store_str(s_fw, sizeof(s_fw), fw, "Firmware revision");
    }
}

static void at_parse_cgsn(const char *resp)
{
    char imei[sizeof(s_imei)] = { 0 };
    const char *p = resp;

    while (p != NULL && *p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t len = at_trim_end(p, eol != NULL ? (size_t)(eol - p) : strlen(p));
        if (len >= 15 && len <= 17) {
            char line[32];
            at_copy_line(line, sizeof(line), p, len);
            const char *s = at_skip_ws(line);
            size_t n = strlen(s);
            bool digits = (n == 15);   /* IMEI only; IMEISV is not shown */
            for (size_t i = 0; digits && i < n; i++) {
                digits = (s[i] >= '0' && s[i] <= '9');
            }
            if (digits) {
                snprintf(imei, sizeof(imei), "%s", s);
                break;
            }
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }

    if (imei[0] != '\0') {
        at_store_str(s_imei, sizeof(s_imei), imei, "IMEI");
    }
}

/*------------------------------------ AT session -----------------------------------*/

static uint32_t at_generation(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t generation = s_generation;
    xSemaphoreGive(s_lock);
    return generation;
}

static void at_clear_status(void)
{
    s_model[0] = '\0';
    s_fw[0] = '\0';
    s_imei[0] = '\0';
    cpe_status_set_rssi(0, false);
    cpe_status_set_modem_info("", "", "");
    cpe_status_set_carrier("");
    cpe_status_set_modem_registered(false);
    cpe_status_set_sim(false, false);
    cpe_status_set_phone_number("");
    cpe_status_set_ps(false, false);
    cpe_status_set_pdp(false, false);
    cpe_status_set_signal_quality(0, 0, 0, false);
    cpe_status_set_modem_temp(0, false);
}

static void at_read_identity(void)
{
    char resp[512];
    if (at_command("ATI", resp, sizeof(resp), AT_CMD_TIMEOUT_MS) == AT_FINAL_OK) {
        at_parse_ati(resp);
    }
    if (at_command("AT+CGSN", resp, sizeof(resp), AT_CMD_TIMEOUT_MS) == AT_FINAL_OK) {
        at_parse_cgsn(resp);
    }
    cpe_status_set_modem_info(s_model, s_fw, s_imei);
    if (s_model[0] != '\0' || s_fw[0] != '\0' || s_imei[0] != '\0') {
        ESP_LOGI(TAG, "Modem %s, firmware %s, IMEI %s",
                 s_model[0] ? s_model : "?",
                 s_fw[0] ? s_fw : "?",
                 s_imei[0] ? s_imei : "?");
    }
}

static void at_poll_sim(void)
{
    char resp[256] = { 0 };
    (void)at_command("AT+CPIN?", resp, sizeof(resp), AT_CMD_TIMEOUT_MS);
    int sim = at_parse_cpin(resp);
    if (sim < 0) {
        /* Unsupported command or no answer: keep the last known state. */
        return;
    }
    cpe_status_set_sim(sim == 1, true);
    if (sim == 0) {
        cpe_status_set_phone_number("");
        return;
    }

    at_final_t cnum = at_command("AT+CNUM", resp, sizeof(resp), AT_CMD_TIMEOUT_MS);
    char number[24] = { 0 };
    if (cnum == AT_FINAL_OK) {
        /* OK without a +CNUM line: the SIM has no MSISDN programmed. */
        if (at_parse_cnum(resp, number, sizeof(number))) {
            cpe_status_set_phone_number(number);
        } else {
            cpe_status_set_phone_number("");
        }
    } else if (cnum == AT_FINAL_ERROR) {
        cpe_status_set_phone_number("");
    }
}

static esp_err_t at_poll(void)
{
    char resp[256];
    bool csq_ok = at_command("AT+CSQ", resp, sizeof(resp), AT_CMD_TIMEOUT_MS) == AT_FINAL_OK;
    if (!csq_ok) {
        /* Distinguish "no AT port" from a modem that simply rejects CSQ: the
         * latter must not tear the session down. */
        if (at_command("AT", resp, sizeof(resp), AT_CMD_TIMEOUT_MS) != AT_FINAL_OK) {
            return ESP_FAIL;
        }
    } else {
        at_parse_csq(resp);
    }

    if (at_command("AT+COPS?", resp, sizeof(resp), AT_CMD_TIMEOUT_MS) == AT_FINAL_OK) {
        at_parse_cops(resp);
    }

    if (at_command("AT+CEREG?", resp, sizeof(resp), AT_CMD_TIMEOUT_MS) == AT_FINAL_OK) {
        if (strstr(resp, "+CEREG:") != NULL) {
            at_parse_registration(resp, "+CEREG:");
        } else if (at_command("AT+CREG?", resp, sizeof(resp), AT_CMD_TIMEOUT_MS) == AT_FINAL_OK &&
                   strstr(resp, "+CREG:") != NULL) {
            at_parse_registration(resp, "+CREG:");
        }
    }

    /* Packet switched attach and PDP state: LTE metrics are only meaningful
     * once the modem has a data context. All commands are read-only. */
    if (at_command("AT+CGATT?", resp, sizeof(resp), AT_CMD_TIMEOUT_MS) == AT_FINAL_OK) {
        at_parse_cgatt(resp);
    }
    if (at_command("AT+CGACT?", resp, sizeof(resp), AT_CMD_TIMEOUT_MS) == AT_FINAL_OK) {
        at_parse_cgact(resp);
    }
    if (at_command("AT+QCSQ", resp, sizeof(resp), AT_CMD_TIMEOUT_MS) == AT_FINAL_OK) {
        at_parse_qcsq(resp);
    }
    if (at_command("AT+QTEMP", resp, sizeof(resp), AT_CMD_TIMEOUT_MS) == AT_FINAL_OK) {
        int temp_c = 0;
        if (at_parse_qtemp(resp, &temp_c)) {
            cpe_status_set_modem_temp(temp_c, true);
        }
    }

    at_poll_sim();
    return ESP_OK;
}

/* The only place that forgets the port handle: everything else goes through it,
 * so readiness, failure count and telemetry status cannot drift apart from
 * s_port. `close` must be false when the device is gone, because the
 * CDC driver closes the ports of a removed device itself. */
static void at_release_port(bool close);

static void at_try_connect(void)
{
    at_candidate_t cands[AT_MAX_CANDIDATES];
    uint8_t dev_addr = 0;
    uint32_t generation = 0;
    int count = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    dev_addr = s_dev_addr;
    count = s_cand_count;
    generation = s_generation;
    memcpy(cands, s_cands, sizeof(cands));
    xSemaphoreGive(s_lock);

    if (count == 0) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(AT_RETRY_MS));
        return;
    }

    for (int i = 0; i < count; i++) {
        if (at_generation() != generation) {
            return;     /* Device changed while probing the previous candidate */
        }
        esp_err_t err = at_port_open(dev_addr, cands[i].intf_num);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Cannot open AT interface %u: %s",
                     cands[i].intf_num, esp_err_to_name(err));
            at_release_port(false);
            continue;
        }
        if (at_generation() != generation) {
            at_release_port(false);
            return;
        }

        char resp[64];
        bool answered = at_command("AT", resp, sizeof(resp), AT_PROBE_TIMEOUT_MS) == AT_FINAL_OK;
        if (at_generation() != generation) {
            /* The device is gone: the CDC driver closes its ports itself, so
             * just forget the handle instead of closing it again. */
            at_release_port(false);
            return;
        }
        if (answered) {
            s_port_generation = generation;
            at_set_port_ready(true);
            ESP_LOGI(TAG, "AT telemetry on interface %u (%s) of device %u",
                     cands[i].intf_num, cands[i].cdc_acm ? "CDC-ACM" : "vendor",
                     dev_addr);
            /* Ask for plain ASCII so carrier/model strings are readable; the
             * UCS2 decoder in at_parse_cops() covers firmware that ignores it. */
            (void)at_command("AT+CSCS=\"GSM\"", resp, sizeof(resp), AT_CMD_TIMEOUT_MS);
            (void)at_command("ATE0", resp, sizeof(resp), AT_CMD_TIMEOUT_MS);
            s_next_poll_tick = 0;
            at_read_identity();
            return;
        }
        /* The device is still there and the port is ours: close it here. */
        at_release_port(true);
        ESP_LOGD(TAG, "Interface %u did not answer AT", cands[i].intf_num);
    }

    ESP_LOGW(TAG, "No AT interface answered on device %u", dev_addr);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(AT_RETRY_MS));
}

static void at_release_port(bool close)
{
    at_set_port_ready(false);
    if (s_port != NULL && close) {
        (void)usbh_cdc_port_close(s_port);
    }
    s_port = NULL;
    s_port_generation = 0;
    s_failures = 0;
    s_next_poll_tick = 0;
    at_line_reset();
    at_clear_status();
}

/* Runs one pending session body on the AT task. Returns true when a body ran. */
static bool at_session_service(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_session_active;
    cpe_at_session_body_t body = s_session_slot.body;
    void *arg = s_session_slot.arg;
    uint32_t seq = s_session_seq;
    xSemaphoreGive(s_lock);
    if (!active) {
        return false;
    }
    s_session_running_seq = seq;

    esp_err_t r;
    if (s_port == NULL || at_reset_pending()) {
        r = ESP_ERR_INVALID_STATE;
    } else {
        r = body(&s_at_io, arg);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_session_slot.result = r;
    s_session_active = false;
    s_session_seq++;
    s_session_running_seq = 0;
    __atomic_store_n(&s_session_cancel_seq, 0, __ATOMIC_RELEASE);
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_session_done);
    return true;
}

static void at_register_events(void)
{
    for (int attempt = 1;; attempt++) {
        esp_err_t err = usbh_cdc_register_dev_event_cb(ESP_USB_DEVICE_MATCH_ID_ANY,
                                                       at_dev_event_cb, NULL);
        if (err == ESP_OK) {
            if (attempt > 1) {
                cpe_status_set_error(NULL);
                ESP_LOGI(TAG, "Registered for USB device events (attempt %d)", attempt);
            } else {
                ESP_LOGI(TAG, "Registered for USB device events");
            }
            return;
        }
        /* The CDC driver is installed by the WAN link manager task, which may
         * still be starting. Retry until it is available, but stop hiding it:
         * after AT_EVENT_REGISTER_FAST_TRIES the console shows why there is no
         * AT telemetry, and the cadence drops to save CPU. */
        if (attempt == 1) {
            ESP_LOGW(TAG, "Cannot register for USB device events yet (%s), retrying",
                     esp_err_to_name(err));
        } else if (attempt == AT_EVENT_REGISTER_FAST_TRIES) {
            cpe_status_set_error("AT telemetry unavailable (no USB device events: %s)",
                                 esp_err_to_name(err));
            ESP_LOGE(TAG, "Still no USB device events after %d attempts (%s), "
                     "slowing down", attempt, esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(attempt >= AT_EVENT_REGISTER_FAST_TRIES ?
                                 (uint32_t)AT_EVENT_REGISTER_SLOW_MS
                                 : (uint32_t)AT_EVENT_REGISTER_FAST_MS));
    }
}

/* Runs on the AT task, which owns the port. The port is deliberately not
 * closed here: the modem drops its USB device while resetting and the CDC
 * driver tears the port down; the generation check releases our handle. */
static void at_reset_service(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool pending = __atomic_load_n(&s_reset_state, __ATOMIC_ACQUIRE) == AT_RESET_PENDING;
    if (pending) {
        at_set_reset_state(AT_RESET_RUNNING);
    }
    xSemaphoreGive(s_lock);
    if (!pending) {
        return;
    }

    bool ok = false;
    if (s_port == NULL) {
        ESP_LOGW(TAG, "AT port is gone, skipping the modem reset");
    } else {
        char resp[64];
        ESP_LOGW(TAG, "Restarting the modem with AT+CFUN=1,1");
        /* The modem may drop the link before it answers, so a missing OK is
         * logged but never treated as a failure of the restart path. */
        ok = at_command("AT+CFUN=1,1", resp, sizeof(resp), AT_CMD_TIMEOUT_MS) == AT_FINAL_OK;
        if (ok) {
            ESP_LOGW(TAG, "Modem acknowledged the reset");
        } else {
            ESP_LOGW(TAG, "No OK to AT+CFUN=1,1, restarting anyway");
        }
    }

    /* Publish the result and the new sequence together, so a waiter can tell
     * this completion apart from one left behind by an earlier timeout. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_reset_ok = ok;
    s_reset_seq++;
    at_set_reset_state(AT_RESET_IDLE);
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_reset_done);
}

static void at_task(void *arg)
{
    (void)arg;
    at_register_events();

    for (;;) {
        if (s_port != NULL && at_generation() != s_port_generation) {
            /* Re-enumeration: the old port belongs to a gone device and the
             * CDC driver releases it, so it must not be closed here. */
            at_release_port(false);
        }

        if (at_session_service()) {
            /* A session ran: skip telemetry this pass. */
            continue;
        }

        if (at_reset_pending()) {
            at_reset_service();
            s_next_poll_tick = 0;
            continue;
        }

        if (s_port == NULL) {
            at_try_connect();
            continue;
        }

        bool urc_active = cpe_at_urc_has_subscribers();
        if (urc_active) {
            at_rx_drain(NULL);
        }

        if (s_next_poll_tick == 0 ||
            (int32_t)(xTaskGetTickCount() - s_next_poll_tick) >= 0) {
            esp_err_t poll = at_poll();
            if (at_reset_pending()) {
                /* The poll was abandoned for the reset: never count it as an
                 * AT failure, and service the request at the top of the
                 * loop. */
                s_next_poll_tick = 0;
                continue;
            }
            if (poll == ESP_OK) {
                s_failures = 0;
            } else if (++s_failures >= AT_MAX_FAILURES) {
                ESP_LOGW(TAG, "AT port stopped responding, reopening it");
                /* If the device went away meanwhile, the CDC driver owns the
                 * close. */
                at_release_port(at_generation() == s_port_generation);
                continue;
            }
            s_next_poll_tick = xTaskGetTickCount() +
                               pdMS_TO_TICKS(CONFIG_CPE_AT_POLL_INTERVAL_S * 1000U);
        }

        uint32_t wait_ms = CONFIG_CPE_AT_POLL_INTERVAL_S * 1000U;
        if (urc_active) {
            wait_ms = AT_URC_POLL_MS;
        } else {
            int32_t left = (int32_t)(s_next_poll_tick - xTaskGetTickCount());
            uint32_t left_ms = left > 0 ? (uint32_t)left * portTICK_PERIOD_MS : 0;
            if (left_ms < wait_ms) {
                wait_ms = left_ms;
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

esp_err_t cpe_at_modem_reset(uint32_t timeout_ms)
{
    if (!s_started || s_task == NULL || s_reset_done == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ready = s_port_ready;
    bool idle = __atomic_load_n(&s_reset_state, __ATOMIC_ACQUIRE) == AT_RESET_IDLE;
    uint32_t seq = s_reset_seq;
    if (ready && idle) {
        at_set_reset_state(AT_RESET_PENDING);
    }
    xSemaphoreGive(s_lock);

    if (!ready || !idle) {
        return ESP_ERR_INVALID_STATE;
    }

    xTaskNotifyGive(s_task);

    /* Wait for the completion of *this* request: a semaphore given for a reset
     * whose caller timed out earlier must not satisfy this one. */
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= limit) {
            return ESP_ERR_TIMEOUT;
        }
        if (xSemaphoreTake(s_reset_done, limit - elapsed) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t now = s_reset_seq;
        bool ok = s_reset_ok;
        xSemaphoreGive(s_lock);
        if (now != seq) {
            return ok ? ESP_OK : ESP_FAIL;
        }
    }
}

/* Ask the AT task to abandon a body whose caller ran out of time, and wait for
 * the body to finish: until then it may still touch the caller's context. */
static void at_session_abort(uint32_t seq)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool running = s_session_active && s_session_seq == seq;
    if (running) {
        __atomic_store_n(&s_session_cancel_seq, seq + 1, __ATOMIC_RELEASE);
    }
    xSemaphoreGive(s_lock);
    if (!running) {
        return;
    }

    TickType_t start = xTaskGetTickCount();
    for (;;) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= pdMS_TO_TICKS(AT_SESSION_CANCEL_WAIT_MS)) {
            return;
        }
        if (xSemaphoreTake(s_session_done,
                           pdMS_TO_TICKS(AT_SESSION_CANCEL_WAIT_MS) - elapsed) != pdTRUE) {
            return;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool finished = s_session_seq != seq;
        xSemaphoreGive(s_lock);
        if (finished) {
            return;
        }
    }
}

esp_err_t cpe_at_session(const cpe_at_session_body_t body, void *arg,
                         uint32_t timeout_ms)
{
    if (body == NULL || s_task == NULL || s_lock == NULL || s_session_done == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_session_active || !s_port_ready || at_reset_pending()) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_session_slot.body = body;
    s_session_slot.arg = arg;
    s_session_slot.result = ESP_ERR_TIMEOUT;
    s_session_active = true;
    uint32_t seq = s_session_seq;
    xSemaphoreGive(s_lock);

    xTaskNotifyGive(s_task);

    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= limit) {
            at_session_abort(seq);
            return ESP_ERR_TIMEOUT;
        }
        if (xSemaphoreTake(s_session_done, limit - elapsed) != pdTRUE) {
            at_session_abort(seq);
            return ESP_ERR_TIMEOUT;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t now = s_session_seq;
        esp_err_t result = s_session_slot.result;
        xSemaphoreGive(s_lock);
        if (now != seq) {
            return result;
        }
    }
}

bool cpe_at_ready(void)
{
    if (s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ready = s_port_ready;
    xSemaphoreGive(s_lock);
    return ready;
}

esp_err_t cpe_at_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_reset_done = xSemaphoreCreateBinary();
    if (s_reset_done == NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_session_done = xSemaphoreCreateBinary();
    if (s_session_done == NULL) {
        vSemaphoreDelete(s_reset_done);
        s_reset_done = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(at_task, "cpe_at", AT_TASK_STACK, NULL, AT_TASK_PRIO, &s_task) != pdPASS) {
        vSemaphoreDelete(s_session_done);
        s_session_done = NULL;
        vSemaphoreDelete(s_reset_done);
        s_reset_done = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t urc_err = cpe_at_urc_init();
    if (urc_err != ESP_OK) {
        /* URC delivery is optional: telemetry and sessions still work. */
        ESP_LOGW(TAG, "URC delivery unavailable (%s)", esp_err_to_name(urc_err));
    }
    s_started = true;
    ESP_LOGI(TAG, "AT telemetry started (poll every %d s)", CONFIG_CPE_AT_POLL_INTERVAL_S);
    return ESP_OK;
}

#else /* CONFIG_CPE_AT_ENABLE */

esp_err_t cpe_at_start(void)
{
    return ESP_OK;
}

esp_err_t cpe_at_modem_reset(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

bool cpe_at_ready(void)
{
    return false;
}

esp_err_t cpe_at_session(const cpe_at_session_body_t body, void *arg,
                         uint32_t timeout_ms)
{
    (void)body;
    (void)arg;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_CPE_AT_ENABLE */
