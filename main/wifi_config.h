/**
 * @file    wifi_config.h
 * @brief   HTTP configuration server and WiFi AP management.
 *
 * The HTTP server (port 80) starts after WiFi init and stays running for
 * the lifetime of the application — accessible on both STA and AP
 * interfaces.  UART baud rate and WiFi credentials can be changed at
 * runtime through the web UI.
 *
 * The AP is managed independently: it starts automatically when STA
 * fails at boot, can be toggled via the BOOT button, and stops when STA
 * obtains an IP address (to save power).
 *
 * Credentials and UART settings are persisted in NVS and take priority
 * over Kconfig compile-time defaults on subsequent boots.
 */

#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @name WiFi Event Group Bits (shared with serial2net.c)
 *  @{ */
#define WIFI_CONNECTED_BIT  BIT0  /**< Station connected to AP */
#define WIFI_FAIL_BIT       BIT1  /**< Station connection failed */
/** @} */

/* ================================================================
 *  HTTP Server (always-on)
 * ================================================================ */

/**
 * @brief Start the HTTP configuration server.
 *
 * Binds to port 80 on all interfaces (INADDR_ANY), so it is reachable
 * via both STA and AP.  Call once after WiFi init — the server runs
 * for the lifetime of the application.
 *
 * @retval ESP_OK              Server started.
 * @retval ESP_ERR_INVALID_STATE  Server is already running.
 */
esp_err_t wifi_config_init_http(void);

/* ================================================================
 *  AP Management (independent lifecycle)
 * ================================================================ */

/**
 * @brief Start the configuration AP (if not already running).
 *
 * Ensures the AP netif exists and the AP radio is on.  If the device
 * is in pure STA mode, it switches to APSTA mode and configures the
 * AP with the Kconfig SSID/password.
 *
 * @retval ESP_OK  AP is running.
 */
esp_err_t wifi_config_ap_start(void);

/**
 * @brief Stop the AP radio.
 *
 * If the device is in APSTA mode, switches to STA-only to save power.
 * The HTTP server is NOT affected — it remains reachable on the STA
 * interface.
 *
 * @retval ESP_OK  AP stopped (or was not running).
 */
esp_err_t wifi_config_ap_stop(void);

/**
 * @brief Returns true if the configuration AP is currently active.
 */
bool wifi_config_ap_is_active(void);

/**
 * @brief Schedule a delayed stop of the AP.
 *
 * Spawns a short-lived task that waits @p delay_ms milliseconds, then
 * calls wifi_config_ap_stop().  Used by the web UI's /api/close
 * handler to give the response time to flush before the AP goes down.
 *
 * @param delay_ms  Delay before stopping (e.g. 500 for 500 ms).
 */
void wifi_config_schedule_ap_stop(uint32_t delay_ms);

/* ================================================================
 *  NVS Persistence
 * ================================================================ */

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
 * @brief Load saved UART baud rate from NVS.
 *
 * Reads the "uart_baud" key from the "wifi_cfg" namespace.  If no
 * value has been saved, falls back to CONFIG_SERIAL2NET_UART_BAUD.
 *
 * @param[out] baud  Set to the saved baud rate or the Kconfig default.
 *
 * @retval ESP_OK              Value loaded (or fallback used).
 * @retval ESP_ERR_NVS_*       NVS access error (fallback still set).
 */
esp_err_t wifi_config_load_uart_baud(uint32_t *baud);

/**
 * @brief Save UART baud rate to NVS.
 *
 * Writes the "uart_baud" key to the "wifi_cfg" namespace and commits.
 *
 * @param baud  Baud rate to persist (e.g. 115200, 921600).
 *
 * @retval ESP_OK         Saved successfully.
 * @retval ESP_ERR_NVS_*  NVS write/commit error.
 */
esp_err_t wifi_config_save_uart_baud(uint32_t baud);

#ifdef __cplusplus
}
#endif
