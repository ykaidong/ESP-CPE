/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Read-only AT telemetry.
 *
 * The ECM driver owns the network data port; this component opens a second CDC
 * port on the modem AT interface (vendor specific or CDC-ACM) and polls the
 * signal/registration information into cpe_status. Only read-only commands are
 * sent, so this cannot change the state of the data connection.
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
#define AT_RINGBUF_SIZE        1024
#define AT_TRANSFER_SIZE       512

typedef struct {
    uint8_t intf_num;
    bool cdc_acm;
} at_candidate_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static bool s_started;

/* Guarded by s_lock; written by the device event callback, read by the task. */
static uint8_t s_dev_addr;
static at_candidate_t s_cands[AT_MAX_CANDIDATES];
static int s_cand_count;
static uint32_t s_generation;

/* Owned by the AT task. */
static usbh_cdc_port_handle_t s_port;
static uint32_t s_port_generation;
static int s_failures;
static char s_model[32];
static char s_fw[32];
static char s_imei[16];

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
                                pdMS_TO_TICKS(1000));
}

typedef enum {
    AT_FINAL_NONE,
    AT_FINAL_OK,
    AT_FINAL_ERROR,
} at_final_t;

/* Only complete result lines terminate a command. Matching "OK" anywhere in
 * the buffer can pick up an unrelated token or a late answer to the previous
 * command and truncate the current response. */
static at_final_t at_final_line(const char *buf)
{
    at_final_t result = AT_FINAL_NONE;
    const char *p = buf;

    while (*p != '\0') {
        const char *eol = p;
        while (*eol != '\0' && *eol != '\r' && *eol != '\n') {
            eol++;
        }
        size_t len = (size_t)(eol - p);
        if (len == 2 && strncmp(p, "OK", 2) == 0) {
            result = AT_FINAL_OK;
        } else if ((len == 5 && strncmp(p, "ERROR", 5) == 0) ||
                   (len >= 11 && (strncmp(p, "+CME ERROR:", 11) == 0 ||
                                  strncmp(p, "+CMS ERROR:", 11) == 0))) {
            result = AT_FINAL_ERROR;
        }
        if (*eol == '\0') {
            break;
        }
        p = eol + 1;
    }
    return result;
}

static bool at_response_complete(const char *buf)
{
    return at_final_line(buf) != AT_FINAL_NONE;
}

static esp_err_t at_read_response(char *buf, size_t size, uint32_t timeout_ms)
{
    size_t used = 0;
    TickType_t start = xTaskGetTickCount();
    buf[0] = '\0';

    for (;;) {
        size_t avail = 0;
        if (usbh_cdc_get_rx_buffer_size(s_port, &avail) == ESP_OK && avail > 0) {
            if (used + 1 < size) {
                size_t want = size - 1 - used;
                if (want > avail) {
                    want = avail;
                }
                size_t len = want;
                if (usbh_cdc_read_bytes(s_port, (uint8_t *)buf + used, &len, 0) == ESP_OK) {
                    used += len;
                    buf[used] = '\0';
                }
            }
            if (at_response_complete(buf)) {
                return ESP_OK;
            }
            if (used + 1 >= size) {
                break;  /* Response does not fit the buffer */
            }
        }
        if ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS >= timeout_ms) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return at_response_complete(buf) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static bool at_command(const char *cmd, char *resp, size_t resp_size, uint32_t timeout_ms)
{
    if (s_port == NULL) {
        return false;
    }
    (void)usbh_cdc_flush_rx_buffer(s_port);
    /* The flush only drops bytes already queued in the ring buffer; a USB
     * transfer that was in flight can still land right after it. Drain once
     * more so a late answer is not taken for this command's response. */
    vTaskDelay(pdMS_TO_TICKS(30));
    (void)usbh_cdc_flush_rx_buffer(s_port);
    if (at_send_line(cmd) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send %s", cmd);
        return false;
    }
    if (at_read_response(resp, resp_size, timeout_ms) != ESP_OK) {
        ESP_LOGD(TAG, "No complete response to %s", cmd);
        return false;
    }
    ESP_LOGD(TAG, "AT %s -> %s", cmd, resp);
    return at_final_line(resp) == AT_FINAL_OK;
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

/* Returns -1 when the answer is not understood, 0 when no card is present,
 * 1 when a card is present (READY, PIN/PUK locked or still initialising). */
static int at_parse_cpin(const char *resp)
{
    if (resp == NULL) {
        return -1;
    }
    /* 10 is "SIM not inserted"; other 10x codes must not be mistaken for it. */
    const char *cme = strstr(resp, "+CME ERROR: 10");
    bool sim_missing = strstr(resp, "NOT INSERTED") != NULL ||
                       (cme != NULL && (cme[14] == '\0' || cme[14] == '\r' ||
                                        cme[14] == '\n' || cme[14] == ' '));
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
        snprintf(s_model, sizeof(s_model), "%s", model);
    }
    if (fw[0] != '\0') {
        snprintf(s_fw, sizeof(s_fw), "%s", fw);
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
        snprintf(s_imei, sizeof(s_imei), "%s", imei);
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
}

static void at_read_identity(void)
{
    char resp[512];
    if (at_command("ATI", resp, sizeof(resp), AT_CMD_TIMEOUT_MS)) {
        at_parse_ati(resp);
    }
    if (at_command("AT+CGSN", resp, sizeof(resp), AT_CMD_TIMEOUT_MS)) {
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

    bool ok = at_command("AT+CNUM", resp, sizeof(resp), AT_CMD_TIMEOUT_MS);
    char number[24] = { 0 };
    if (ok) {
        /* OK without a +CNUM line: the SIM has no MSISDN programmed. */
        if (at_parse_cnum(resp, number, sizeof(number))) {
            cpe_status_set_phone_number(number);
        } else {
            cpe_status_set_phone_number("");
        }
    } else if (at_final_line(resp) == AT_FINAL_ERROR) {
        cpe_status_set_phone_number("");
    }
}

static esp_err_t at_poll(void)
{
    char resp[256];
    bool csq_ok = at_command("AT+CSQ", resp, sizeof(resp), AT_CMD_TIMEOUT_MS);
    if (!csq_ok) {
        /* Distinguish "no AT port" from a modem that simply rejects CSQ: the
         * latter must not tear the session down. */
        if (!at_command("AT", resp, sizeof(resp), AT_CMD_TIMEOUT_MS)) {
            return ESP_FAIL;
        }
    } else {
        at_parse_csq(resp);
    }

    if (at_command("AT+COPS?", resp, sizeof(resp), AT_CMD_TIMEOUT_MS)) {
        at_parse_cops(resp);
    }

    if (at_command("AT+CEREG?", resp, sizeof(resp), AT_CMD_TIMEOUT_MS)) {
        if (strstr(resp, "+CEREG:") != NULL) {
            at_parse_registration(resp, "+CEREG:");
        } else if (at_command("AT+CREG?", resp, sizeof(resp), AT_CMD_TIMEOUT_MS) &&
                   strstr(resp, "+CREG:") != NULL) {
            at_parse_registration(resp, "+CREG:");
        }
    }

    at_poll_sim();
    return ESP_OK;
}

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
            s_port = NULL;
            continue;
        }
        if (at_generation() != generation) {
            s_port = NULL;
            return;
        }

        char resp[64];
        bool answered = at_command("AT", resp, sizeof(resp), AT_PROBE_TIMEOUT_MS);
        if (at_generation() != generation) {
            /* The device is gone: the CDC driver closes its ports itself, so
             * just forget the handle instead of closing it again. */
            s_port = NULL;
            return;
        }
        if (answered) {
            s_port_generation = generation;
            ESP_LOGI(TAG, "AT telemetry on interface %u (%s) of device %u",
                     cands[i].intf_num, cands[i].cdc_acm ? "CDC-ACM" : "vendor",
                     dev_addr);
            /* Ask for plain ASCII so carrier/model strings are readable; the
             * UCS2 decoder in at_parse_cops() covers firmware that ignores it. */
            (void)at_command("AT+CSCS=\"GSM\"", resp, sizeof(resp), AT_CMD_TIMEOUT_MS);
            (void)at_command("ATE0", resp, sizeof(resp), AT_CMD_TIMEOUT_MS);
            at_read_identity();
            return;
        }
        (void)usbh_cdc_port_close(s_port);
        s_port = NULL;
        ESP_LOGD(TAG, "Interface %u did not answer AT", cands[i].intf_num);
    }

    ESP_LOGW(TAG, "No AT interface answered on device %u", dev_addr);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(AT_RETRY_MS));
}

static void at_release_port(bool close)
{
    if (s_port != NULL && close) {
        (void)usbh_cdc_port_close(s_port);
    }
    s_port = NULL;
    s_port_generation = 0;
    s_failures = 0;
    at_clear_status();
}

static void at_register_events(void)
{
    bool warned = false;
    for (;;) {
        esp_err_t err = usbh_cdc_register_dev_event_cb(ESP_USB_DEVICE_MATCH_ID_ANY,
                                                       at_dev_event_cb, NULL);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Registered for USB device events");
            return;
        }
        /* The CDC driver is installed by the WAN link manager task, which may
         * still be starting. Retry until it is available. */
        if (!warned) {
            ESP_LOGW(TAG, "Cannot register for USB device events yet (%s), retrying",
                     esp_err_to_name(err));
            warned = true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
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

        if (s_port == NULL) {
            at_try_connect();
            continue;
        }

        if (at_poll() == ESP_OK) {
            s_failures = 0;
        } else if (++s_failures >= AT_MAX_FAILURES) {
            ESP_LOGW(TAG, "AT port stopped responding, reopening it");
            /* If the device went away meanwhile, the CDC driver owns the close. */
            at_release_port(at_generation() == s_port_generation);
            continue;
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONFIG_CPE_AT_POLL_INTERVAL_S * 1000U));
    }
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
    if (xTaskCreate(at_task, "cpe_at", 4096, NULL, 3, &s_task) != pdPASS) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
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

#endif /* CONFIG_CPE_AT_ENABLE */
