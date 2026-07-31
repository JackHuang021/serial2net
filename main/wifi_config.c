/**
 * @file    wifi_config.c
 * @brief   On-demand WiFi configuration portal (AP + HTTP).
 *
 * Started when STA fails to connect at boot or when the BOOT button is
 * pressed.  Stops automatically once STA obtains an IP address.
 *
 * Endpoints:
 *   GET  /              Embedded SPA (scan, select, connect)
 *   POST /api/scan      WiFi scan → JSON
 *   POST /api/connect   Save NVS + connect STA → JSON
 *   GET  /api/status    Current WiFi state → JSON
 */

#include "wifi_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "sdkconfig.h"

/* ---- Externs (owned by serial2net.c) ---- */
extern EventGroupHandle_t wifi_event_group;
extern int                wifi_retry_count;
extern bool               wifi_reconfiguring;

/* ---- Local constants ---- */
#define TAG                         "wifi_cfg"
#define WIFI_CFG_NVS_NAMESPACE      "wifi_cfg"
#define WIFI_CFG_NVS_KEY_SSID       "sta_ssid"
#define WIFI_CFG_NVS_KEY_PW         "sta_pw"
#define WIFI_CFG_MAX_SCAN_RESULTS   20
#define WIFI_CFG_JSON_BUF_SIZE      6144

/* ---- Local state ---- */
static httpd_handle_t    server              = NULL;
static bool              httpd_running       = false;
static bool              ap_active           = false;
static wifi_ap_record_t  scan_records[WIFI_CFG_MAX_SCAN_RESULTS];
static uint16_t          scan_count          = 0;
static bool              scan_in_progress    = false;

/* ================================================================
 *  JSON helpers
 * ================================================================ */

/**
 * @brief Append a JSON-escaped string to a buffer.
 */
static int json_escape_str(char *dst, size_t dst_len, const char *src)
{
    int n = 0;
    while (*src && (size_t)n < dst_len - 1) {
        unsigned char c = (unsigned char)*src;
        switch (c) {
        case '"':  dst[n++] = '\\'; dst[n++] = '"';  break;
        case '\\': dst[n++] = '\\'; dst[n++] = '\\'; break;
        case '\n': dst[n++] = '\\'; dst[n++] = 'n';  break;
        case '\r': dst[n++] = '\\'; dst[n++] = 'r';  break;
        case '\t': dst[n++] = '\\'; dst[n++] = 't';  break;
        default:
            if (c < 0x20) {
                n += snprintf(dst + n, dst_len - n, "\\u%04x", c);
            } else {
                dst[n++] = c;
            }
            break;
        }
        src++;
    }
    dst[n] = '\0';
    return n;
}

/**
 * @brief Parse `{"ssid":"...","password":"..."}` from a raw HTTP body.
 */
static bool json_parse_connect_body(const char *body, int body_len,
                                     char *ssid, size_t ssid_len,
                                     char *password, size_t pw_len)
{
    char buf[256];
    int copy_len = (body_len < (int)sizeof(buf) - 1) ? body_len : (int)sizeof(buf) - 1;
    memcpy(buf, body, copy_len);
    buf[copy_len] = '\0';

    const char *p;
    char *dst;
    size_t dst_rem;

    /* ---- Extract "ssid" ---- */
    p = strstr(buf, "\"ssid\"");
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p = strchr(p, '"');
    if (!p) return false;
    p++;
    dst = ssid;
    dst_rem = ssid_len - 1;
    while (*p && *p != '"' && dst_rem > 0) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
            case '"':  case '\\': case '/': *dst++ = *p;    break;
            case 'n':  *dst++ = '\n';       break;
            case 'r':  *dst++ = '\r';       break;
            case 't':  *dst++ = '\t';       break;
            case 'u':  *dst++ = '?';  p += 4; break;
            default:   *dst++ = *p;          break;
            }
        } else {
            *dst++ = *p;
        }
        p++;
        dst_rem--;
    }
    *dst = '\0';

    /* ---- Extract "password" ---- */
    p = strstr(buf, "\"password\"");
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p = strchr(p, '"');
    if (!p) return false;
    p++;
    dst = password;
    dst_rem = pw_len - 1;
    while (*p && *p != '"' && dst_rem > 0) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
            case '"':  case '\\': case '/': *dst++ = *p;    break;
            case 'n':  *dst++ = '\n';       break;
            case 'r':  *dst++ = '\r';       break;
            case 't':  *dst++ = '\t';       break;
            case 'u':  *dst++ = '?';  p += 4; break;
            default:   *dst++ = *p;          break;
            }
        } else {
            *dst++ = *p;
        }
        p++;
        dst_rem--;
    }
    *dst = '\0';

    return (ssid[0] != '\0');
}

/* ================================================================
 *  NVS persistence
 * ================================================================ */

esp_err_t wifi_config_load_sta_creds(char *ssid, size_t ssid_len,
                                      char *password, size_t pw_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CFG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t len = ssid_len;
    err = nvs_get_str(handle, WIFI_CFG_NVS_KEY_SSID, ssid, &len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    len = pw_len;
    err = nvs_get_str(handle, WIFI_CFG_NVS_KEY_PW, password, &len);
    nvs_close(handle);
    return err;
}

static esp_err_t save_sta_creds_to_nvs(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CFG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, WIFI_CFG_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_CFG_NVS_KEY_PW, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/* ================================================================
 *  UART config persistence
 * ================================================================ */

#define WIFI_CFG_NVS_KEY_UART_BAUD  "uart_baud"

esp_err_t wifi_config_load_uart_baud(uint32_t *baud)
{
    *baud = CONFIG_SERIAL2NET_UART_BAUD;  /* fallback default */

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CFG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    uint32_t val;
    err = nvs_get_u32(handle, WIFI_CFG_NVS_KEY_UART_BAUD, &val);
    if (err == ESP_OK) {
        *baud = val;
    }
    nvs_close(handle);
    return err;
}

esp_err_t wifi_config_save_uart_baud(uint32_t baud)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CFG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u32(handle, WIFI_CFG_NVS_KEY_UART_BAUD, baud);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/* ================================================================
 *  WiFi scan (blocking)
 * ================================================================ */

static esp_err_t start_scan(void)
{
    if (scan_in_progress) return ESP_ERR_INVALID_STATE;

    scan_in_progress = true;
    scan_count = 0;

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = { .min = 100, .max = 300 },
        },
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        scan_in_progress = false;
        return err;
    }

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&scan_count));
    if (scan_count > WIFI_CFG_MAX_SCAN_RESULTS) {
        scan_count = WIFI_CFG_MAX_SCAN_RESULTS;
    }
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&scan_count, scan_records));

    /* Sort by RSSI descending */
    for (int i = 0; i < (int)scan_count - 1; i++) {
        for (int j = i + 1; j < (int)scan_count; j++) {
            if (scan_records[j].rssi > scan_records[i].rssi) {
                wifi_ap_record_t tmp = scan_records[i];
                scan_records[i] = scan_records[j];
                scan_records[j] = tmp;
            }
        }
    }

    scan_in_progress = false;
    return ESP_OK;
}

/* ================================================================
 *  WiFi connection (called from /api/connect handler)
 * ================================================================ */

/**
 * @brief Task that waits 1 second, then resets retry state and connects.
 *
 * The delay gives the WiFi state machine time to settle after
 * esp_wifi_disconnect() — calling esp_wifi_connect() immediately
 * after a disconnect can return ESP_ERR_WIFI_CONN or produce
 * "Haven't to connect to a suitable AP now!" errors.
 *
 * Once the connect call is made, the existing event handler in
 * serial2net.c takes over retry handling.
 */
static void deferred_connect_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* Restore normal event-handler behaviour before connecting. */
    wifi_reconfiguring = false;
    wifi_retry_count = 0;
    if (wifi_event_group != NULL) {
        xEventGroupClearBits(wifi_event_group, WIFI_FAIL_BIT);
    }

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "STA connect initiated");
    }
    vTaskDelete(NULL);
}

/**
 * @brief Apply new STA credentials and schedule a connect attempt.
 *
 * We avoid calling esp_wifi_connect() directly from the HTTP handler
 * because the WiFi state machine may not be ready (e.g. still tearing
 * down a previous connection).  Instead we queue a 1-second delayed
 * task that does the actual connect.
 */
static esp_err_t apply_sta_config_and_connect(const char *ssid,
                                               const char *password)
{
#if CONFIG_SERIAL2NET_WIFI_MODE_AP
    ESP_LOGW(TAG, "STA not available in AP-only mode");
    return ESP_ERR_NOT_SUPPORTED;
#else
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, password,
            sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set STA config: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Disconnect current STA (if any), then schedule a delayed connect.
     *
     * The wifi_reconfiguring flag tells the WIFI_EVENT_STA_DISCONNECTED
     * handler to skip auto-reconnect AND WIFI_FAIL_BIT — the deferred
     * task handles everything cleanly after the state machine settles.
     */
    wifi_reconfiguring = true;

    err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGE(TAG, "Failed to disconnect STA: %s", esp_err_to_name(err));
    }
    /* ESP_OK or ESP_ERR_WIFI_NOT_CONNECT — either way, schedule connect. */

    xTaskCreate(deferred_connect_task, "defer_conn", 2048, NULL, 3, NULL);
    return ESP_OK;
#endif
}

/* ================================================================
 *  HTTP URI handlers
 * ================================================================ */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    extern const char wifi_config_html[];
    extern const size_t wifi_config_html_len;

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    return httpd_resp_send(req, wifi_config_html, wifi_config_html_len);
}

static esp_err_t api_scan_handler(httpd_req_t *req)
{
    esp_err_t err = start_scan();
    if (err != ESP_OK) {
        const char *msg = "{\"error\":\"scan failed\"}";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    }

    char *json = malloc(WIFI_CFG_JSON_BUF_SIZE);
    if (!json) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }

    int off = snprintf(json, WIFI_CFG_JSON_BUF_SIZE, "{\"networks\":[");
    for (int i = 0; i < scan_count; i++) {
        char escaped[65];
        json_escape_str(escaped, sizeof(escaped),
                        (const char *)scan_records[i].ssid);
        off += snprintf(json + off, WIFI_CFG_JSON_BUF_SIZE - off,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                        (i > 0) ? "," : "",
                        escaped,
                        scan_records[i].rssi,
                        scan_records[i].authmode);
    }
    snprintf(json + off, WIFI_CFG_JSON_BUF_SIZE - off, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

/**
 * @brief POST /api/connect — save credentials and start STA connection.
 *
 * Since the user is accessing this endpoint through the device's own AP,
 * STA is not currently connected — so we can call esp_wifi_connect()
 * directly without a prior disconnect.  The response is delivered
 * reliably because the HTTP client is on the AP interface which stays
 * up throughout.
 */
static esp_err_t api_connect_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"empty request body\"}");
    }

    char ssid[33]     = {0};
    char password[65] = {0};

    if (!json_parse_connect_body(body, received,
                                  ssid, sizeof(ssid),
                                  password, sizeof(password))) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid JSON\"}");
    }

    if (ssid[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"SSID is required\"}");
    }

    esp_err_t err = save_sta_creds_to_nvs(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"failed to save credentials\"}");
    }

    ESP_LOGI(TAG, "Saved credentials for SSID=%s, connecting...", ssid);

    /* Return the response first, then start connecting.
     * No disconnect needed — STA wasn't connected when the config
     * portal is active.  The event handler in serial2net.c will
     * call wifi_config_stop() when it sees IP_EVENT_STA_GOT_IP. */
    err = apply_sta_config_and_connect(ssid, password);
    const char *resp = (err == ESP_OK)
        ? "{\"status\":\"ok\",\"message\":\"connecting...\"}"
        : "{\"error\":\"connect failed\"}";

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t api_status_handler(httpd_req_t *req)
{
    char json[512];

    /* ---- Determine WiFi mode ---- */
    wifi_mode_t wm;
    const char *mode = "unknown";
    if (esp_wifi_get_mode(&wm) == ESP_OK) {
        if (wm == WIFI_MODE_AP)       mode = "ap";
        else if (wm == WIFI_MODE_STA) mode = "sta";
        else if (wm == WIFI_MODE_APSTA) mode = "apsta";
    }

    /* ---- Reliable connection check: does STA have an IP address? ---- */
    char ip_str[16] = "null";
    bool has_ip = false;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK
            && ip_info.ip.addr != 0) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            has_ip = true;
        }
    }

    /* ---- Supplementary: AP info for SSID / RSSI display ---- */
    wifi_ap_record_t ap_info;
    esp_err_t ap_err = esp_wifi_sta_get_ap_info(&ap_info);
    bool connected = has_ip;  /* IP is the authoritative signal */

    /* ---- Failed: retries exhausted, WIFI_FAIL_BIT is set ---- */
    bool failed = (!connected && wifi_event_group != NULL &&
                   (xEventGroupGetBits(wifi_event_group) & WIFI_FAIL_BIT));

    char ssid_str[64] = "";
    int  rssi_val = 0;
    if (ap_err == ESP_OK) {
        json_escape_str(ssid_str, sizeof(ssid_str), (const char *)ap_info.ssid);
        rssi_val = ap_info.rssi;
    }

    snprintf(json, sizeof(json),
             "{\"mode\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\","
             "\"rssi\":%d,\"connected\":%s,\"failed\":%s}",
             mode,
             (ap_err == ESP_OK) ? ssid_str : "null",
             ip_str,
             rssi_val,
             connected ? "true" : "false",
             failed ? "true" : "false");

    ESP_LOGD(TAG, "/api/status → %s", json);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief POST /api/close — shut down the config portal.
 *
 * Called by the web UI after it has confirmed the STA connection
 * and displayed the success message to the user.
 */
static esp_err_t api_close_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Web UI requested portal shutdown");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");

    /* Stop the AP after the response has been sent.
     * Use a short delay so the TCP stack can flush the response. */
    wifi_config_schedule_ap_stop(500);
    return ESP_OK;
}

/* ================================================================
 *  UART config API handlers
 * ================================================================ */

static esp_err_t api_uart_get_handler(httpd_req_t *req)
{
    char json[256];
    snprintf(json, sizeof(json),
             "{\"baud\":%d,\"port\":%d,\"tx_pin\":%d,\"rx_pin\":%d,"
             "\"data_bits\":8,\"parity\":\"none\",\"stop_bits\":1}",
             CONFIG_SERIAL2NET_UART_BAUD,
             CONFIG_SERIAL2NET_UART_PORT,
             CONFIG_SERIAL2NET_UART_TX_PIN,
             CONFIG_SERIAL2NET_UART_RX_PIN);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_uart_post_handler(httpd_req_t *req)
{
    char body[64] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"empty request body\"}");
    }

    /* Parse "baud" value from JSON body: {"baud":115200} */
    const char *p = strstr(body, "\"baud\"");
    if (!p) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"missing 'baud' field\"}");
    }
    p = strchr(p, ':');
    if (!p) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid JSON\"}");
    }
    p++;
    while (*p == ' ' || *p == '\t') p++;

    long baud = strtol(p, NULL, 10);
    if (baud < 1200 || baud > 4000000) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
            "{\"error\":\"invalid baud rate (1200–4000000)\"}");
    }

    /* Save to NVS */
    esp_err_t err = wifi_config_save_uart_baud((uint32_t)baud);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"failed to save to NVS\"}");
    }

    /* Apply immediately */
    err = uart_set_baudrate(CONFIG_SERIAL2NET_UART_PORT, (uint32_t)baud);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uart_set_baudrate failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "UART baud rate changed to %ld", baud);
    }

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"baud\":%ld}", baud);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

/* ================================================================
 *  OTA firmware update handlers
 * ================================================================ */

#if CONFIG_SERIAL2NET_OTA_ENABLE

static esp_err_t api_ota_status_handler(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *desc = esp_app_get_description();

    char json[512];
    snprintf(json, sizeof(json),
        "{\"version\":\"%s\","
        "\"compile_time\":\"%s %s\","
        "\"idf_version\":\"%s\","
        "\"partition\":\"%s\","
        "\"sha256\":\"%02x%02x%02x%02x%02x%02x%02x%02x\"}",
        desc->version,
        desc->date, desc->time,
        desc->idf_ver,
        running ? running->label : "unknown",
        desc->app_elf_sha256[0], desc->app_elf_sha256[1],
        desc->app_elf_sha256[2], desc->app_elf_sha256[3],
        desc->app_elf_sha256[4], desc->app_elf_sha256[5],
        desc->app_elf_sha256[6], desc->app_elf_sha256[7]);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_ota_post_handler(httpd_req_t *req)
{
    char content_len_str[16];
    if (httpd_req_get_hdr_value_str(req, "Content-Length",
                                    content_len_str, sizeof(content_len_str)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "411 Length Required");
        return httpd_resp_sendstr(req, "{\"error\":\"Content-Length required\"}");
    }

    int total_len = atoi(content_len_str);
    if (total_len <= 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid firmware size\"}");
    }

    /* Validate against the actual partition size */
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "OTA: no OTA partition found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"no OTA partition found\"}");
    }

    if (total_len > (int)part->size) {
        ESP_LOGE(TAG, "OTA: firmware too large: %d > %" PRIu32, total_len, part->size);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"firmware too large for partition\"}");
    }

    ESP_LOGI(TAG, "OTA: writing %d bytes to partition %s (size %" PRIu32 ")",
             total_len, part->label, part->size);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(part, total_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"OTA begin failed\"}");
    }

    uint8_t *buf = malloc(4096);
    if (!buf) {
        esp_ota_end(ota_handle);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }

    int remaining = total_len;
    int received_total = 0;
    bool write_failed = false;

    while (remaining > 0) {
        int to_read = (remaining < 4096) ? remaining : 4096;
        int received = httpd_req_recv(req, (char *)buf, to_read);
        if (received <= 0) {
            ESP_LOGE(TAG, "OTA: recv error at %d / %d bytes (ret=%d)",
                     received_total, total_len, received);
            write_failed = true;
            break;
        }
        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA: write error at %d / %d bytes: %s",
                     received_total, total_len, esp_err_to_name(err));
            write_failed = true;
            break;
        }
        remaining -= received;
        received_total += received;
    }
    free(buf);

    if (write_failed) {
        esp_ota_end(ota_handle);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"OTA write failed\"}");
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"OTA end failed\"}");
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"failed to set boot partition\"}");
    }

    ESP_LOGI(TAG, "OTA: %d bytes written to %s — restarting device",
             received_total, part->label);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"OTA complete, restarting...\"}");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

#endif /* CONFIG_SERIAL2NET_OTA_ENABLE */

/* ================================================================
 *  HTTP server lifecycle
 * ================================================================ */

/**
 * @brief Ensure the device's AP interface is running.
 *
 * If we're in pure STA mode and STA failed, there's no AP — create one.
 */
static esp_err_t ensure_ap_running(void)
{
    wifi_mode_t wm;
    esp_err_t err = esp_wifi_get_mode(&wm);
    if (err != ESP_OK) return err;

    /* AP is already active in AP or APSTA modes. */
    if (wm == WIFI_MODE_AP || wm == WIFI_MODE_APSTA) {
        return ESP_OK;
    }

    /* Pure STA mode — need to add AP. */
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = CONFIG_SERIAL2NET_WIFI_AP_SSID,
            .ssid_len = strlen(CONFIG_SERIAL2NET_WIFI_AP_SSID),
            .password = CONFIG_SERIAL2NET_WIFI_AP_PASSWORD,
            .max_connection = 4,
            .authmode = (strlen(CONFIG_SERIAL2NET_WIFI_AP_PASSWORD) == 0)
                         ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK,
        },
    };

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "AP enabled for configuration: SSID=%s",
             CONFIG_SERIAL2NET_WIFI_AP_SSID);
    return ESP_OK;
}

esp_err_t wifi_config_init_http(void)
{
    if (httpd_running) {
        ESP_LOGW(TAG, "HTTP server already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting HTTP configuration server...");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = 80;
    config.max_uri_handlers = 12;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- Register URI handlers ---- */
    const httpd_uri_t root_uri = {
        .uri = "/", .method = HTTP_GET,
        .handler = root_get_handler, .user_ctx = NULL,
    };
    const httpd_uri_t scan_uri = {
        .uri = "/api/scan", .method = HTTP_POST,
        .handler = api_scan_handler, .user_ctx = NULL,
    };
    const httpd_uri_t connect_uri = {
        .uri = "/api/connect", .method = HTTP_POST,
        .handler = api_connect_handler, .user_ctx = NULL,
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status", .method = HTTP_GET,
        .handler = api_status_handler, .user_ctx = NULL,
    };
    const httpd_uri_t close_uri = {
        .uri = "/api/close", .method = HTTP_POST,
        .handler = api_close_handler, .user_ctx = NULL,
    };

    const httpd_uri_t uart_get_uri = {
        .uri = "/api/uart", .method = HTTP_GET,
        .handler = api_uart_get_handler, .user_ctx = NULL,
    };
    const httpd_uri_t uart_post_uri = {
        .uri = "/api/uart", .method = HTTP_POST,
        .handler = api_uart_post_handler, .user_ctx = NULL,
    };

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &scan_uri);
    httpd_register_uri_handler(server, &connect_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &close_uri);
    httpd_register_uri_handler(server, &uart_get_uri);
    httpd_register_uri_handler(server, &uart_post_uri);

#if CONFIG_SERIAL2NET_OTA_ENABLE
    const httpd_uri_t ota_post_uri = {
        .uri = "/api/ota", .method = HTTP_POST,
        .handler = api_ota_post_handler, .user_ctx = NULL,
    };
    const httpd_uri_t ota_status_uri = {
        .uri = "/api/ota/status", .method = HTTP_GET,
        .handler = api_ota_status_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &ota_post_uri);
    httpd_register_uri_handler(server, &ota_status_uri);
#endif

    httpd_running = true;
    ESP_LOGI(TAG, "HTTP server on port 80");
    return ESP_OK;
}

esp_err_t wifi_config_ap_start(void)
{
    if (ap_active) {
        ESP_LOGW(TAG, "Config AP already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting configuration AP...");

    esp_err_t err = ensure_ap_running();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to ensure AP is running: %s", esp_err_to_name(err));
        return err;
    }

    ap_active = true;
    ESP_LOGI(TAG, "Config AP active — connect to '%s' and open http://192.168.4.1/",
             CONFIG_SERIAL2NET_WIFI_AP_SSID);
    return ESP_OK;
}

esp_err_t wifi_config_ap_stop(void)
{
    if (!ap_active) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping configuration AP");
    ap_active = false;

    /* Disable AP radio — no longer needed once STA is connected. */
#if !CONFIG_SERIAL2NET_WIFI_MODE_AP
    wifi_mode_t wm;
    if (esp_wifi_get_mode(&wm) == ESP_OK && wm == WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_LOGI(TAG, "AP disabled, STA-only mode");
    }
#endif

    return ESP_OK;
}

bool wifi_config_ap_is_active(void)
{
    return ap_active;
}

/**
 * @brief Task that waits delay_ms, then stops the AP.
 */
static void delayed_ap_stop_task(void *pvParameters)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    wifi_config_ap_stop();
    vTaskDelete(NULL);
}

void wifi_config_schedule_ap_stop(uint32_t delay_ms)
{
    if (!ap_active) return;

    ESP_LOGI(TAG, "Config AP will stop in %" PRIu32 " ms", delay_ms);
    xTaskCreate(delayed_ap_stop_task, "ap_stop", 2048,
                (void *)(uintptr_t)delay_ms, 1, NULL);
}

/* ================================================================
 *  Embedded Web UI  (R"HTML_END( ... )HTML_END" raw literal)
 * ================================================================ */

const char wifi_config_html[] = R"HTML_END(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>Serial2Net — WiFi Setup</title>
<style>
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
body{
  font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
  background:#0f0f1a;color:#d4d4d8;min-height:100vh;
  display:flex;justify-content:center;padding:16px;
}
.container{width:100%;max-width:440px}
h1{font-size:1.25rem;font-weight:600;text-align:center;
   padding:20px 0 24px;color:#f0f0f0;letter-spacing:0.02em}
h2{font-size:0.8rem;font-weight:500;text-transform:uppercase;
   letter-spacing:0.08em;color:#888;margin-bottom:10px}

.card{
  background:#1a1a2e;border:1px solid #2a2a40;border-radius:12px;
  padding:16px;margin-bottom:16px;
}

.status-row{display:flex;align-items:center;gap:10px;padding:4px 0}
.status-label{font-size:0.75rem;color:#888;min-width:56px}
.status-value{font-size:0.9rem;font-weight:500}
.badge{
  display:inline-block;padding:2px 10px;border-radius:10px;
  font-size:0.7rem;font-weight:600;text-transform:uppercase;
}
.badge-ok{background:#0d3b2e;color:#00d4aa}
.badge-warn{background:#3d2e00;color:#ffb800}
.badge-err{background:#3b0d0d;color:#ff5555}

.btn{
  width:100%;padding:12px 16px;border:none;border-radius:8px;
  font-size:0.9rem;font-weight:600;cursor:pointer;transition:all .15s;
}
.btn:disabled{opacity:.4;cursor:not-allowed}
.btn-primary{background:#00d4aa;color:#0f0f1a}
.btn-primary:hover:not(:disabled){background:#00e8bb}
.btn-connect{background:#4a6cf7;color:#fff}
.btn-connect:hover:not(:disabled){background:#5b7af8}
.btn-connect:disabled{background:#2a2a40;color:#666}

#network-list{list-style:none;max-height:360px;overflow-y:auto;margin-top:10px}
#network-list::-webkit-scrollbar{width:4px}
#network-list::-webkit-scrollbar-thumb{background:#3a3a50;border-radius:2px}
.net-item{
  display:flex;align-items:center;gap:10px;padding:10px 12px;
  border:1px solid transparent;border-radius:8px;cursor:pointer;
  transition:all .12s;margin-bottom:2px;
}
.net-item:hover{border-color:#3a3a50;background:#1e1e34}
.net-item.selected{border-color:#00d4aa;background:#0d2a24}
.net-ssid{flex:1;font-size:0.85rem;font-weight:500;overflow:hidden;
          text-overflow:ellipsis;white-space:nowrap}
.net-lock{font-size:0.75rem;color:#888;min-width:16px;text-align:center}

.bars{display:flex;align-items:flex-end;gap:2px;height:16px;min-width:28px}
.bars span{width:4px;border-radius:1px;transition:background .2s}

.input-group{margin-top:12px}
.input-group label{display:block;font-size:0.75rem;color:#888;margin-bottom:4px}
.input-row{display:flex;gap:8px}
.input-row input{
  flex:1;padding:10px 12px;background:#13132a;border:1px solid #2a2a40;
  border-radius:8px;color:#f0f0f0;font-size:0.9rem;outline:none;
  transition:border-color .15s;
}
.input-row input:focus{border-color:#00d4aa}
.input-row input:disabled{opacity:.4}

#scan-status,#connect-status{padding:8px 0 0;font-size:0.78rem}
#scan-status{color:#888}
#connect-status.error{color:#ff5555}
#connect-status.ok{color:#00d4aa}

#selected-ssid{color:#00d4aa;font-weight:500}

.pw-toggle{
  padding:10px 12px;background:#13132a;border:1px solid #2a2a40;
  border-radius:8px;color:#888;font-size:0.8rem;cursor:pointer;
  white-space:nowrap;user-select:none;transition:all .15s;
}
.pw-toggle:hover{color:#f0f0f0;border-color:#555}
.pw-toggle:disabled{opacity:.4;cursor:not-allowed}

.hidden{display:none}

@keyframes spin{to{transform:rotate(360deg)}}
.spinner{display:inline-block;width:16px;height:16px;border:2px solid #333;
         border-top-color:#00d4aa;border-radius:50%;animation:spin .6s linear infinite;
         vertical-align:middle;margin-right:6px}
</style>
</head>
<body>
<div class="container">
<h1>&#9889; Serial2Net WiFi Setup</h1>

<div class="card" id="status-card">
  <h2>Current Status</h2>
  <div id="status-content">Loading...</div>
</div>

<div class="card">
  <h2>UART Settings</h2>
  <div class="input-group">
    <label for="baud-select">Baud Rate</label>
    <div class="input-row">
      <select id="baud-select" style="flex:1;padding:10px 12px;background:#13132a;
        border:1px solid #2a2a40;border-radius:8px;color:#f0f0f0;font-size:0.9rem;outline:none;">
        <option value="1200">1200</option>
        <option value="2400">2400</option>
        <option value="4800">4800</option>
        <option value="9600">9600</option>
        <option value="14400">14400</option>
        <option value="19200">19200</option>
        <option value="38400">38400</option>
        <option value="57600">57600</option>
        <option value="115200">115200</option>
        <option value="230400">230400</option>
        <option value="460800">460800</option>
        <option value="921600">921600</option>
        <option value="1000000">1000000</option>
        <option value="1500000">1500000</option>
        <option value="2000000">2000000</option>
        <option value="3000000">3000000</option>
        <option value="4000000">4000000</option>
      </select>
    </div>
  </div>
  <div style="margin-top:12px">
    <button class="btn btn-primary" id="uart-apply-btn" onclick="doApplyUart()">
      Apply
    </button>
  </div>
  <div id="uart-status"></div>
</div>

<div class="card">
  <h2>Firmware Update</h2>
  <div id="ota-info" style="font-size:0.78rem;color:#888;margin-bottom:8px">Loading info...</div>
  <div class="input-group">
    <label for="ota-file">Select .bin firmware file</label>
    <input type="file" id="ota-file" accept=".bin" style="width:100%;padding:8px 0;color:#d4d4d8;font-size:0.85rem;margin-bottom:8px">
  </div>
  <div class="input-row">
    <button class="btn btn-primary" id="ota-btn" onclick="doOtaUpdate()">
      Upload &amp; Update
    </button>
  </div>
  <div id="ota-progress" class="hidden">
    <div id="ota-bar" style="height:6px;background:#00d4aa;border-radius:3px;width:0%;transition:width .2s;margin-top:8px"></div>
    <div id="ota-pct" style="font-size:0.75rem;color:#888;margin-top:2px"></div>
  </div>
  <div id="ota-status" style="padding:8px 0 0;font-size:0.78rem"></div>
</div>

<div class="card">
  <h2>WiFi Networks</h2>
  <div id="config-form">
    <button class="btn btn-primary" id="scan-btn" onclick="doScan()">
      &#128269; Scan for Networks
    </button>
    <div id="scan-status"></div>
    <ul id="network-list"></ul>
    <div class="input-group">
      <label for="pw-input">Password for <span id="selected-ssid">(select a network)</span></label>
      <div class="input-row">
        <input type="password" id="pw-input" placeholder="WiFi password" disabled>
        <button class="pw-toggle" id="pw-toggle" onclick="togglePw()" disabled>&#128065;</button>
      </div>
    </div>
    <div style="margin-top:12px">
      <button class="btn btn-connect" id="connect-btn" onclick="doConnect()" disabled>
        Connect
      </button>
    </div>
    <div id="connect-status"></div>
  </div>
</div>
</div>

<script>
var selectedSsid = null;
var networks = [];
var scanBusy = false;

function renderBars(rssi) {
  var b = 0;
  if (rssi > -50) b = 5;
  else if (rssi > -60) b = 4;
  else if (rssi > -70) b = 3;
  else if (rssi > -80) b = 2;
  else b = 1;
  var h = '';
  for (var i = 0; i < 5; i++) {
    var pct = 40 + i * 15;
    var c = i < b
      ? (b >= 4 ? '#00d4aa' : b >= 3 ? '#ffb800' : '#ff5555')
      : '#2a2a40';
    h += '<span style="height:' + pct + '%;background:' + c + '"></span>';
  }
  return '<div class="bars">' + h + '</div>';
}

function authIcon(auth) {
  return (auth === 0) ? '&#128275;' : '&#128274;';
}

function authLabel(auth) {
  if (auth === 0) return 'Open';
  if (auth === 3 || auth === 4) return 'WPA2';
  if (auth === 5 || auth === 6) return 'WPA3';
  return 'Secure';
}

function updateStatusContent(s) {
  var badge = '';
  if (s.connected) {
    badge = '<span class="badge badge-ok">Connected</span>';
  } else if (s.mode === 'ap' || s.mode === 'apsta') {
    badge = '<span class="badge badge-warn">AP Mode</span>';
  } else {
    badge = '<span class="badge badge-err">Disconnected</span>';
  }
  var html = '<div class="status-row">' +
    '<span class="status-label">Mode</span>' +
    '<span class="status-value">' + badge + '</span></div>';
  html += '<div class="status-row">' +
    '<span class="status-label">SSID</span>' +
    '<span class="status-value">' + (s.ssid && s.ssid !== 'null' ? escHtml(s.ssid) : '—') + '</span></div>';
  html += '<div class="status-row">' +
    '<span class="status-label">IP</span>' +
    '<span class="status-value">' + (s.ip !== 'null' ? s.ip : '—') + '</span></div>';
  if (s.connected) {
    html += '<div class="status-row">' +
      '<span class="status-label">Signal</span>' +
      '<span class="status-value">' + s.rssi + ' dBm ' + renderBars(s.rssi) + '</span></div>';
  }
  document.getElementById('status-content').innerHTML = html;
}

function escHtml(str) {
  return str.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')
            .replace(/"/g,'&quot;').replace(/'/g,'&#39;');
}

function doScan() {
  if (scanBusy) return;
  scanBusy = true;
  document.getElementById('config-form').classList.remove('hidden');
  var btn = document.getElementById('scan-btn');
  var st  = document.getElementById('scan-status');
  btn.disabled = true;
  st.innerHTML = '<span class="spinner"></span>Scanning...';
  fetch('/api/scan', {method:'POST',cache:'no-store'})
    .then(function(r){return r.json()})
    .then(function(data){
      scanBusy = false;
      btn.disabled = false;
      if (data.error) { st.textContent = data.error; return; }
      networks = data.networks || [];
      st.textContent = networks.length + ' network(s) found';
      renderNetworks();
    })
    .catch(function(e){
      scanBusy = false;
      btn.disabled = false;
      st.textContent = 'Scan error: ' + e.message;
    });
}

function renderNetworks() {
  var list = document.getElementById('network-list');
  var html = '';
  for (var i = 0; i < networks.length; i++) {
    var n = networks[i];
    var sel = (selectedSsid === n.ssid) ? ' selected' : '';
    html += '<li class="net-item' + sel + '" onclick="selectNetwork(\'' +
            escHtml(n.ssid).replace(/'/g,"\\'") + '\')">' +
            renderBars(n.rssi) +
            '<span class="net-ssid">' + escHtml(n.ssid) + '</span>' +
            '<span class="net-lock" title="' + authLabel(n.auth) + '">' +
            authIcon(n.auth) + '</span></li>';
  }
  list.innerHTML = html;
}

function selectNetwork(ssid) {
  selectedSsid = ssid;
  document.getElementById('selected-ssid').textContent = escHtml(ssid);
  document.getElementById('pw-input').disabled = false;
  document.getElementById('pw-toggle').disabled = false;
  document.getElementById('connect-btn').disabled = false;
  document.getElementById('connect-status').textContent = '';
  document.getElementById('connect-status').className = '';
  renderNetworks();
  setTimeout(function(){ document.getElementById('pw-input').focus(); }, 100);
}

function togglePw() {
  var inp = document.getElementById('pw-input');
  var btn = document.getElementById('pw-toggle');
  if (inp.type === 'password') {
    inp.type = 'text'; btn.textContent = '🙈';
  } else {
    inp.type = 'password'; btn.textContent = '👁';
  }
}

function doConnect() {
  var pw = document.getElementById('pw-input').value;
  if (!selectedSsid) return;
  var btn = document.getElementById('connect-btn');
  var st  = document.getElementById('connect-status');
  btn.disabled = true;
  st.className = '';
  st.innerHTML = '<span class="spinner"></span>Connecting...';
  fetch('/api/connect', {
    method: 'POST',
    cache: 'no-store',
    headers: {'Content-Type':'application/json'},
    body: JSON.stringify({ssid:selectedSsid, password:pw})
  })
    .then(function(r){return r.json()})
    .then(function(data){
      btn.disabled = false;
      if (data.error) {
        st.textContent = data.error;
        st.className = 'error';
      } else {
        st.innerHTML = '<span class="spinner"></span> ' + (data.message || 'Connecting...');
        st.className = 'ok';
        pollUntilConnected(0);
      }
    })
    .catch(function(e){
      btn.disabled = false;
      st.textContent = 'Error: ' + e.message;
      st.className = 'error';
    });
}

function pollUntilConnected(attempt) {
  if (attempt > 20) {
    document.getElementById('connect-status').textContent = 'Connection timed out';
    document.getElementById('connect-status').className = 'error';
    document.getElementById('config-form').classList.remove('hidden');
    return;
  }
  fetch('/api/status', {cache:'no-store'})
    .then(function(r){return r.json()})
    .then(function(s){
      updateStatusContent(s);
      if (s.connected) {
        document.getElementById('connect-status').textContent = 'Connected';
        document.getElementById('connect-status').className = 'ok';
        document.getElementById('config-form').classList.add('hidden');
        setTimeout(function(){
          fetch('/api/close', {method:'POST',cache:'no-store'}).catch(function(){});
        }, 5000);
      } else if (s.failed) {
        document.getElementById('connect-status').textContent =
          'Connection failed — check password and try again';
        document.getElementById('connect-status').className = 'error';
        document.getElementById('config-form').classList.remove('hidden');
      } else {
        setTimeout(function(){ pollUntilConnected(attempt + 1); }, 2000);
      }
    })
    .catch(function(){
      setTimeout(function(){ pollUntilConnected(attempt + 1); }, 2000);
    });
}

function updateStatus() {
  fetch('/api/status', {cache:'no-store'})
    .then(function(r){return r.json()})
    .then(function(s){ updateStatusContent(s); })
    .catch(function(){});
}

function loadUartConfig() {
  fetch('/api/uart', {cache:'no-store'})
    .then(function(r){return r.json()})
    .then(function(cfg){
      document.getElementById('baud-select').value = cfg.baud;
    })
    .catch(function(){});
}

function doApplyUart() {
  var baud = parseInt(document.getElementById('baud-select').value);
  var btn = document.getElementById('uart-apply-btn');
  var st  = document.getElementById('uart-status');
  btn.disabled = true;
  st.className = '';
  st.innerHTML = '<span class="spinner"></span>Applying...';
  fetch('/api/uart', {
    method: 'POST',
    cache: 'no-store',
    headers: {'Content-Type':'application/json'},
    body: JSON.stringify({baud: baud})
  })
    .then(function(r){return r.json()})
    .then(function(data){
      btn.disabled = false;
      if (data.error) {
        st.textContent = data.error;
        st.className = 'error';
      } else {
        st.textContent = 'Baud rate set to ' + data.baud;
        st.className = 'ok';
      }
    })
    .catch(function(e){
      btn.disabled = false;
      st.textContent = 'Error: ' + e.message;
      st.className = 'error';
    });
}

loadUartConfig();
updateStatus();
setInterval(updateStatus, 15000);

/* ---------- OTA firmware update ---------- */
function loadOtaInfo() {
  fetch('/api/ota/status', {cache:'no-store'})
    .then(function(r){return r.json()})
    .then(function(info){
      document.getElementById('ota-info').textContent =
        'Running: ' + info.partition + ' | v' + info.version +
        ' | Built: ' + info.compile_time;
    })
    .catch(function(e){
      document.getElementById('ota-info').textContent =
        'OTA not available (' + e.message + ')';
    });
}

function doOtaUpdate() {
  var file = document.getElementById('ota-file').files[0];
  if (!file) { alert('Select a .bin firmware file first'); return; }
  if (!confirm('Update firmware to ' + file.name +
      '?\\n\\nThe device will reboot after the update.')) return;

  var btn = document.getElementById('ota-btn');
  var bar = document.getElementById('ota-bar');
  var pct = document.getElementById('ota-pct');
  var status = document.getElementById('ota-status');
  var progress = document.getElementById('ota-progress');

  btn.disabled = true;
  progress.classList.remove('hidden');
  status.textContent = '';
  status.className = '';

  var xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota');

  xhr.upload.onprogress = function(e) {
    if (e.lengthComputable) {
      var p = Math.round(e.loaded / e.total * 100);
      bar.style.width = p + '%';
      pct.textContent = p + '%';
    }
  };

  xhr.onload = function() {
    try {
      var resp = JSON.parse(xhr.responseText);
      if (resp.error) {
        status.textContent = resp.error;
        status.className = 'error';
        btn.disabled = false;
        progress.classList.add('hidden');
      } else {
        status.textContent = resp.message + ' Reloading page after reboot...';
        status.className = 'ok';
        var attempts = 0;
        var poll = setInterval(function() {
          fetch('/api/status', {cache:'no-store'})
            .then(function() {
              clearInterval(poll);
              location.reload();
            })
            .catch(function(){});
          if (++attempts > 45) {
            clearInterval(poll);
            location.reload();
          }
        }, 1000);
      }
    } catch(e) {
      status.textContent = 'Unexpected response from device';
      status.className = 'error';
      btn.disabled = false;
    }
  };

  xhr.onerror = function() {
    status.textContent = 'Upload lost — device may be rebooting, page will reload...';
    status.className = 'error';
    setTimeout(function(){ location.reload(); }, 6000);
  };

  xhr.send(file);
}

loadOtaInfo();
</script>
</body>
</html>
)HTML_END";

const size_t wifi_config_html_len = sizeof(wifi_config_html) - 1;
