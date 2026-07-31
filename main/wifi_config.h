/**
 * @file    wifi_config.h
 * @brief   On-demand WiFi configuration via web UI (AP mode).
 *
 * When the device cannot connect to a WiFi network at boot, or when the
 * BOOT button is pressed, an AP with a captive web UI is started so the
 * user can scan, select, and connect to a network.  Once connected, the
 * HTTP server stops — it only runs while needed.
 *
 * Credentials are persisted in NVS and take priority over Kconfig
 * compile-time defaults on subsequent boots.
 */

#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @name WiFi Event Group Bits (shared with serial2net.c)
 *  @{ */
#define WIFI_CONNECTED_BIT  BIT0  /**< Station connected to AP */
#define WIFI_FAIL_BIT       BIT1  /**< Station connection failed */
/** @} */

/**
 * @brief Start the WiFi configuration portal.
 *
 * Switches the WiFi interface to AP+STA mode (or pure AP if STA is
 * unavailable), starts the HTTP server on port 80, and serves the
 * embedded web UI.
 *
 * Call this when:
 *   - STA connection fails at boot
 *   - BOOT button is pressed during connection attempts
 *
 * @retval ESP_OK  Configuration portal started.
 */
esp_err_t wifi_config_start(void);

/**
 * @brief Stop the WiFi configuration portal immediately.
 *
 * Stops the HTTP server right away.  Prefer wifi_config_schedule_stop()
 * so the web UI has time to poll /api/status and show the "Connected"
 * confirmation before the server goes down.
 *
 * @retval ESP_OK  Configuration portal stopped.
 */
esp_err_t wifi_config_stop(void);

/**
 * @brief Schedule a delayed stop of the configuration portal.
 *
 * Spawns a short-lived task that waits @p delay_ms milliseconds, then
 * calls wifi_config_stop().  This gives the web UI enough time to
 * poll /api/status a few times and display the connection result.
 *
 * @param delay_ms  Delay before stopping (e.g. 12000 for 12 seconds).
 */
void wifi_config_schedule_stop(uint32_t delay_ms);

/**
 * @brief Load STA credentials from NVS.
 *
 * Looks up the "wifi_cfg" NVS namespace for a saved SSID/password pair.
 *
 * @param[out] ssid      Buffer to receive the SSID (max 32 bytes + NUL).
 * @param[in]  ssid_len  Size of @p ssid buffer in bytes.
 * @param[out] password  Buffer to receive the password (max 64 bytes + NUL).
 * @param[in]  pw_len    Size of @p password buffer in bytes.
 *
 * @retval ESP_OK             Credentials loaded.
 * @retval ESP_ERR_NOT_FOUND  No credentials have been saved yet.
 * @retval ESP_ERR_NVS_*      NVS access error.
 */
esp_err_t wifi_config_load_sta_creds(char *ssid, size_t ssid_len,
                                      char *password, size_t pw_len);

/**
 * @brief Returns true if the configuration portal is currently active.
 */
bool wifi_config_is_active(void);

#ifdef __cplusplus
}
#endif
