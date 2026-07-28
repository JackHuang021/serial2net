/*
 * Serial2Net: UART-to-WiFi Transparent Bridge
 *
 * Bridges UART data to a TCP socket over WiFi, enabling wireless serial
 * debugging of embedded development boards. Connect your MacBook to the
 * ESP32 over WiFi and access the target board's UART remotely.
 */

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "mdns.h"
#include "led_strip.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "sdkconfig.h"

static const char *TAG = "serial2net";

/* ---- WiFi Event Group Bits ---- */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t wifi_event_group;
static int wifi_retry_count = 0;

/* ---- TCP State ---- */
static int listen_sock = -1;
static int client_sock = -1;
static bool client_connected = false;

/* ---- UART State ---- */
static QueueHandle_t uart_queue;

/* ---- LED State ---- */
static led_strip_handle_t led_strip;

typedef enum {
    LED_BOOT,             // White — starting up
    LED_WIFI_CONNECTING,  // Blue slow blink — connecting to router
    LED_WIFI_STA_OK,      // Cyan — STA connected, no TCP client
    LED_WIFI_AP_OK,       // Magenta — AP mode active, no TCP client
    LED_WIFI_FAIL,        // Red — connection failed
    LED_CLIENT_CONNECTED, // Green — TCP bridge active
} led_state_t;

static led_state_t current_led_state = LED_BOOT;
static bool led_data_activity = false; // Flash on data activity
static TickType_t led_activity_until = 0;

/* ---- LED Helpers ---- */

static void led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
#if CONFIG_SERIAL2NET_LED_ENABLE
    led_strip_set_pixel(led_strip, 0, r, g, b);
    led_strip_refresh(led_strip);
#endif
}

static void led_init(void)
{
#if CONFIG_SERIAL2NET_LED_ENABLE
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_SERIAL2NET_LED_PIN,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,
        },
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_set_rgb(0, 0, 0); // Start with LED off
    ESP_LOGI(TAG, "WS2812B LED on GPIO%d", CONFIG_SERIAL2NET_LED_PIN);
#endif
}

// Update LED based on state (called periodically or on state change)
static void led_update(void)
{
#if CONFIG_SERIAL2NET_LED_ENABLE
    // If there's an active data-activity flash, show it briefly then revert
    if (led_data_activity) {
        TickType_t now = xTaskGetTickCount();
        if (now < led_activity_until) {
            led_set_rgb(20, 20, 20); // Brief white flash
            return;
        }
        led_data_activity = false;
    }

    switch (current_led_state) {
    case LED_BOOT:
        led_set_rgb(10, 10, 10);  // Dim white
        break;
    case LED_WIFI_CONNECTING: {
        // Blue slow blink (1 Hz, 50% duty)
        int phase = (xTaskGetTickCount() / pdMS_TO_TICKS(500)) % 2;
        led_set_rgb(phase ? 0 : 0, phase ? 0 : 0, phase ? 20 : 0);
        break;
    }
    case LED_WIFI_STA_OK:
        led_set_rgb(0, 20, 20);   // Cyan
        break;
    case LED_WIFI_AP_OK:
        led_set_rgb(20, 0, 20);   // Magenta
        break;
    case LED_WIFI_FAIL:
        led_set_rgb(20, 0, 0);    // Red
        break;
    case LED_CLIENT_CONNECTED:
        led_set_rgb(0, 20, 0);    // Green
        break;
    }
#endif
}

// Flash LED briefly to indicate data activity
static void led_signal_activity(void)
{
#if CONFIG_SERIAL2NET_LED_ENABLE
    led_data_activity = true;
    led_activity_until = xTaskGetTickCount() + pdMS_TO_TICKS(50);
#endif
}

// LED update task — runs at ~20 Hz
static void led_task(void *pvParameters)
{
    while (1) {
        led_update();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// Revert LED to the appropriate WiFi state (called when TCP client disconnects)
static void led_revert_to_wifi_state(void)
{
#if CONFIG_SERIAL2NET_WIFI_MODE_AP
    current_led_state = LED_WIFI_AP_OK;
#elif CONFIG_SERIAL2NET_WIFI_MODE_STA
    current_led_state = (wifi_event_group != NULL &&
                         (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT))
                        ? LED_WIFI_STA_OK : LED_WIFI_FAIL;
#else
    // STA+AP fallback: check what's available
    if (wifi_event_group != NULL) {
        EventBits_t bits = xEventGroupGetBits(wifi_event_group);
        if (bits & WIFI_CONNECTED_BIT) {
            current_led_state = LED_WIFI_STA_OK;
        } else {
            current_led_state = LED_WIFI_AP_OK;
        }
    }
#endif
}

/* ================================================================
 *  WiFi
 * ================================================================ */

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        current_led_state = LED_WIFI_CONNECTING;
        ESP_LOGI(TAG, "WiFi STA started, connecting...");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "WiFi STA connected to AP");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        current_led_state = LED_WIFI_CONNECTING;
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi STA disconnected, reason: %d", ev->reason);

#if CONFIG_SERIAL2NET_WIFI_MODE_STA || CONFIG_SERIAL2NET_WIFI_MODE_STA_AP_FALLBACK
        if (wifi_retry_count < CONFIG_SERIAL2NET_WIFI_STA_MAX_RETRY) {
            esp_wifi_connect();
            wifi_retry_count++;
            ESP_LOGI(TAG, "Retry %d/%d connecting to AP",
                     wifi_retry_count, CONFIG_SERIAL2NET_WIFI_STA_MAX_RETRY);
        } else {
            current_led_state = LED_WIFI_FAIL;
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "Max retries reached, STA connection failed");
        }
#endif
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi STA got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        wifi_retry_count = 0;
        current_led_state = LED_WIFI_STA_OK;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "AP client connected, MAC: " MACSTR, MAC2STR(ev->mac));
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "AP client disconnected, MAC: " MACSTR, MAC2STR(ev->mac));
    }
}

static void wifi_init(void)
{
    esp_netif_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_event_group = xEventGroupCreate();

    // Register WiFi & IP event handlers
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

#if CONFIG_SERIAL2NET_WIFI_MODE_AP
    /* ---- Pure AP mode ---- */
    esp_netif_create_default_wifi_ap();

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
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    current_led_state = LED_WIFI_AP_OK;
    ESP_LOGI(TAG, "AP mode: SSID=%s", CONFIG_SERIAL2NET_WIFI_AP_SSID);

#elif CONFIG_SERIAL2NET_WIFI_MODE_STA
    /* ---- Pure STA mode ---- */
    esp_netif_create_default_wifi_sta();

    wifi_config_t sta_cfg = {
        .sta = {
            .ssid = CONFIG_SERIAL2NET_WIFI_STA_SSID,
            .password = CONFIG_SERIAL2NET_WIFI_STA_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "STA mode: connecting to SSID=%s", CONFIG_SERIAL2NET_WIFI_STA_SSID);

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi STA connected");
    } else {
        ESP_LOGE(TAG, "WiFi STA failed to connect");
    }

#else
    /* ---- STA + AP fallback mode (default) ---- */
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_config_t sta_cfg = {
        .sta = {
            .ssid = CONFIG_SERIAL2NET_WIFI_STA_SSID,
            .password = CONFIG_SERIAL2NET_WIFI_STA_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "STA+AP fallback: trying STA to SSID=%s", CONFIG_SERIAL2NET_WIFI_STA_SSID);

    // Wait up to 15 seconds for STA connection, then continue regardless
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi STA connected successfully");
    } else {
        current_led_state = LED_WIFI_AP_OK;
        ESP_LOGW(TAG, "STA not connected, AP fallback active: SSID=%s", CONFIG_SERIAL2NET_WIFI_AP_SSID);
    }
#endif
}

/* ================================================================
 *  mDNS
 * ================================================================ */

static void mdns_init_service(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("serial2net"));
    ESP_ERROR_CHECK(mdns_instance_name_set("Serial2Net UART Bridge"));

    mdns_txt_item_t txt_items[] = {
        {.key = "type", .value = "uart-bridge"},
        {.key = "port", .value = ""}, // filled below
    };

    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", CONFIG_SERIAL2NET_TCP_PORT);
    txt_items[1].value = port_str;

    ESP_ERROR_CHECK(mdns_service_add("serial2net", "_serial2net._tcp", "_tcp",
                                     CONFIG_SERIAL2NET_TCP_PORT, txt_items, 2));
    ESP_LOGI(TAG, "mDNS: serial2net._tcp port %d", CONFIG_SERIAL2NET_TCP_PORT);
}

/* ================================================================
 *  UART
 * ================================================================ */

static void uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate  = CONFIG_SERIAL2NET_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(CONFIG_SERIAL2NET_UART_PORT,
                                        CONFIG_SERIAL2NET_UART_BUFFER_SIZE * 2,
                                        CONFIG_SERIAL2NET_UART_BUFFER_SIZE * 2,
                                        20, &uart_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(CONFIG_SERIAL2NET_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(CONFIG_SERIAL2NET_UART_PORT,
                                 CONFIG_SERIAL2NET_UART_TX_PIN,
                                 CONFIG_SERIAL2NET_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART%d: baud=%d TX=GPIO%d RX=GPIO%d",
             CONFIG_SERIAL2NET_UART_PORT, CONFIG_SERIAL2NET_UART_BAUD,
             CONFIG_SERIAL2NET_UART_TX_PIN, CONFIG_SERIAL2NET_UART_RX_PIN);
}

/* ================================================================
 *  TCP Server
 * ================================================================ */

static void tcp_server_init(void)
{
    struct sockaddr_in server_addr = {0};

    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // Disable Nagle — inherited by accepted connections
    setsockopt(listen_sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(CONFIG_SERIAL2NET_TCP_PORT);

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Socket bind failed: errno %d", errno);
        close(listen_sock);
        listen_sock = -1;
        return;
    }

    if (listen(listen_sock, 1) < 0) {
        ESP_LOGE(TAG, "Socket listen failed: errno %d", errno);
        close(listen_sock);
        listen_sock = -1;
        return;
    }

    ESP_LOGI(TAG, "TCP server listening on port %d", CONFIG_SERIAL2NET_TCP_PORT);
}

/* ================================================================
 *  Forwarding Tasks
 * ================================================================ */

// UART → TCP
static void uart_to_tcp_task(void *pvParameters)
{
    uint8_t *buf = malloc(CONFIG_SERIAL2NET_UART_BUFFER_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate UART→TCP buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        int len = uart_read_bytes(CONFIG_SERIAL2NET_UART_PORT, buf,
                                  CONFIG_SERIAL2NET_UART_BUFFER_SIZE,
                                  pdMS_TO_TICKS(50));
        if (len > 0) {
            if (client_connected && client_sock >= 0) {
                int sent = send(client_sock, buf, len, 0);
                if (sent < 0) {
                    ESP_LOGE(TAG, "TCP send error: errno %d", errno);
                    client_connected = false;
                    close(client_sock);
                    client_sock = -1;
                    led_revert_to_wifi_state();
                } else {
                    led_signal_activity();
                }
            }
            // If no client connected, data is silently dropped
        }
    }
}

// TCP → UART
static void tcp_to_uart_task(void *pvParameters)
{
    uint8_t *buf = malloc(CONFIG_SERIAL2NET_UART_BUFFER_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate TCP→UART buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        // Wait for a client connection
        if (!client_connected || client_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        int len = recv(client_sock, buf, CONFIG_SERIAL2NET_UART_BUFFER_SIZE, 0);
        if (len > 0) {
            uart_write_bytes(CONFIG_SERIAL2NET_UART_PORT, buf, len);
            led_signal_activity();
        } else if (len == 0) {
            // Client closed connection
            ESP_LOGI(TAG, "TCP client disconnected");
            client_connected = false;
            close(client_sock);
            client_sock = -1;
            led_revert_to_wifi_state();
        } else {
            // Error
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(10));
            } else {
                ESP_LOGE(TAG, "TCP recv error: errno %d, closing", errno);
                client_connected = false;
                close(client_sock);
                client_sock = -1;
                led_revert_to_wifi_state();
            }
        }
    }
}

// Accept incoming connections
static void tcp_accept_task(void *pvParameters)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    while (1) {
        if (listen_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Accept failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Kick old client if any
        if (client_connected && client_sock >= 0) {
            ESP_LOGI(TAG, "New client — closing previous connection");
            close(client_sock);
        }

        // Disable Nagle's algorithm for low-latency keystroke forwarding
        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        client_sock = sock;
        client_connected = true;
        current_led_state = LED_CLIENT_CONNECTED;
        ESP_LOGI(TAG, "TCP client connected from %s:%d",
                 inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    }
}

/* ================================================================
 *  Main
 * ================================================================ */

void app_main(void)
{
    ESP_LOGI(TAG, "=== Serial2Net UART-WiFi Bridge ===");
    ESP_LOGI(TAG, "Chip: %s, Free heap: %" PRIu32 " bytes",
             CONFIG_IDF_TARGET, esp_get_free_heap_size());

    // Initialize LED early for status feedback
    led_init();
    xTaskCreate(led_task, "led", 2048, NULL, 5, NULL);

    // Initialize NVS (required for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize WiFi (LED transitions to WIFI_CONNECTING internally)
    wifi_init();

    // Start mDNS so we can be discovered as serial2net.local
    mdns_init_service();

    // Initialize UART
    uart_init();

    // Initialize TCP server
    tcp_server_init();

    // Start forwarding tasks
    xTaskCreate(uart_to_tcp_task, "uart2tcp", 4096, NULL, 10, NULL);
    xTaskCreate(tcp_to_uart_task, "tcp2uart", 4096, NULL, 10, NULL);
    xTaskCreate(tcp_accept_task, "tcp_accept", 4096, NULL, 9, NULL);

    ESP_LOGI(TAG, "Bridge running — connect via:");
    ESP_LOGI(TAG, "  mDNS: serial2net.local:%d", CONFIG_SERIAL2NET_TCP_PORT);
    ESP_LOGI(TAG, "  UART%d: %d baud TX=%d RX=%d",
             CONFIG_SERIAL2NET_UART_PORT, CONFIG_SERIAL2NET_UART_BAUD,
             CONFIG_SERIAL2NET_UART_TX_PIN, CONFIG_SERIAL2NET_UART_RX_PIN);

    // Keep the main task alive for logging
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "Heartbeat: heap free=%" PRIu32 ", client=%s",
                 esp_get_free_heap_size(),
                 client_connected ? "yes" : "no");
    }
}
