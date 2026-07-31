/**
 * @file    serial2net.c
 * @brief   UART-to-WiFi Transparent Bridge
 *
 * Bridges UART data to a TCP socket over WiFi, enabling wireless serial
 * debugging of embedded development boards.  Connect a host computer to
 * the ESP32 over WiFi and access the target board's UART remotely.
 *
 * Supports both raw TCP (port 8880) and Telnet RFC 854/856 (port 23).
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
#include "driver/gpio.h"
#include "mdns.h"
#include "led_strip.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "sdkconfig.h"
#include "telnet.h"
#include "wifi_config.h"

static const char *TAG = "serial2net";

/** @brief Shared with wifi_config.c for reconnection logic. */
EventGroupHandle_t wifi_event_group;
int  wifi_retry_count   = 0;
bool wifi_reconfiguring = false;  /**< Set by wifi_config during manual reconnect */

/** @name TCP Connection State
 *  @brief Shared client state — both raw TCP and Telnet code paths
 *         use the same socket and connection flag.
 *  @{ */
static int  listen_sock    = -1;
static int  client_sock    = -1;
static bool client_connected = false;
/** @} */

#if CONFIG_SERIAL2NET_TELNET_ENABLE
/** @cond TELNET */
static int  telnet_listen_sock = -1;
static bool client_is_telnet   = false;
/** @endcond */
#endif

/** @name UART State
 *  @{ */
static QueueHandle_t uart_queue;
/** @} */

/** @name LED State
 *  @{ */
static led_strip_handle_t led_strip;

/** @brief LED status states for visual indication */
typedef enum {
    LED_BOOT,             /**< Dim white — starting up                        */
    LED_WIFI_CONNECTING,  /**< Blue slow blink (1 Hz) — connecting to router  */
    LED_WIFI_STA_OK,      /**< Cyan solid — STA connected, no TCP client      */
    LED_WIFI_AP_OK,       /**< Magenta solid — AP mode active, no TCP client  */
    LED_WIFI_FAIL,        /**< Red solid — connection failed                  */
    LED_CLIENT_CONNECTED, /**< Green solid — TCP bridge active                */
} led_state_t;

static led_state_t current_led_state = LED_BOOT;
static bool        led_data_activity = false; /**< Flash on data activity */
static TickType_t  led_activity_until = 0;
/** @} */

/** @defgroup led_helpers LED Helpers
 *  @brief Low-level LED control functions.
 *  @{ */

/**
 * @brief Set the WS2812B LED to a raw RGB value.
 *
 * Wraps the led_strip API with a compile-time guard for
 * @ref CONFIG_SERIAL2NET_LED_ENABLE.
 *
 * @param r  Red   (0–255)
 * @param g  Green (0–255)
 * @param b  Blue  (0–255)
 */
static void led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
#if CONFIG_SERIAL2NET_LED_ENABLE
    led_strip_set_pixel(led_strip, 0, r, g, b);
    led_strip_refresh(led_strip);
#endif
}

/**
 * @brief Initialize the WS2812B LED driver (RMT backend, 10 MHz).
 */
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
        .resolution_hz = 10 * 1000 * 1000, /**< 10 MHz */
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,
        },
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_set_rgb(0, 0, 0); /**< Start with LED off */
    ESP_LOGI(TAG, "WS2812B LED on GPIO%d", CONFIG_SERIAL2NET_LED_PIN);
#endif
}

/**
 * @brief Update LED colour based on current state.
 *
 * Called at ~20 Hz by led_task().  A brief white flash (50 ms) is
 * overlaid when data-activity is signalled — it takes priority over
 * the steady-state colour.
 */
static void led_update(void)
{
#if CONFIG_SERIAL2NET_LED_ENABLE
    /* Data-activity flash has priority */
    if (led_data_activity) {
        TickType_t now = xTaskGetTickCount();
        if (now < led_activity_until) {
            led_set_rgb(20, 20, 20); /**< Brief white flash */
            return;
        }
        led_data_activity = false;
    }

    switch (current_led_state) {
    case LED_BOOT:
        led_set_rgb(10, 10, 10);  /**< Dim white */
        break;
    case LED_WIFI_CONNECTING: {
        int phase = (xTaskGetTickCount() / pdMS_TO_TICKS(500)) % 2; /**< 1 Hz, 50% duty */
        led_set_rgb(phase ? 0 : 0, phase ? 0 : 0, phase ? 20 : 0);
        break;
    }
    case LED_WIFI_STA_OK:
        led_set_rgb(0, 20, 20);   /**< Cyan */
        break;
    case LED_WIFI_AP_OK:
        led_set_rgb(20, 0, 20);   /**< Magenta */
        break;
    case LED_WIFI_FAIL:
        led_set_rgb(20, 0, 0);    /**< Red */
        break;
    case LED_CLIENT_CONNECTED:
        led_set_rgb(0, 20, 0);    /**< Green */
        break;
    }
#endif
}

/**
 * @brief Signal data activity for a brief LED flash.
 *
 * Triggers a 50 ms white overlay on the LED at the next led_update()
 * cycle.  Called from both forwarding tasks whenever data passes
 * through the bridge.
 */
static void led_signal_activity(void)
{
#if CONFIG_SERIAL2NET_LED_ENABLE
    led_data_activity = true;
    led_activity_until = xTaskGetTickCount() + pdMS_TO_TICKS(50);
#endif
}

/**
 * @brief LED update task — runs at ~20 Hz.
 *
 * Polls current_led_state and led_data_activity flags and refreshes
 * the physical LED via led_set_rgb().
 */
static void led_task(void *pvParameters)
{
    while (1) {
        led_update();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/**
 * @brief Revert LED to the appropriate WiFi state.
 *
 * Called when a TCP client disconnects.  Restores the LED to whichever
 * WiFi state is currently active (STA / AP / fail), depending on the
 * compiled WiFi mode.
 */
static void led_revert_to_wifi_state(void)
{
#if CONFIG_SERIAL2NET_WIFI_MODE_AP
    current_led_state = LED_WIFI_AP_OK;
#elif CONFIG_SERIAL2NET_WIFI_MODE_STA
    current_led_state = (wifi_event_group != NULL &&
                         (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT))
                        ? LED_WIFI_STA_OK : LED_WIFI_FAIL;
#else
    /**< STA+AP fallback: check what's currently available */
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

/** @} */ /* led_helpers */

/** @defgroup wifi WiFi
 *  @brief WiFi initialisation and event handling.
 *  @{ */

/**
 * @brief WiFi & IP event handler.
 *
 * Dispatches on (event_base, event_id):
 *   - @c WIFI_EVENT_STA_START        → trigger connect, LED to blue blink
 *   - @c WIFI_EVENT_STA_CONNECTED    → log
 *   - @c WIFI_EVENT_STA_DISCONNECTED → retry or mark fail
 *   - @c WIFI_EVENT_AP_STACONNECTED  → log
 *   - @c WIFI_EVENT_AP_STADISCONNECTED → log
 *   - @c IP_EVENT_STA_GOT_IP         → record IP, LED to cyan
 */
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            current_led_state = LED_WIFI_CONNECTING;
            ESP_LOGI(TAG, "WiFi STA started, connecting...");
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "WiFi STA connected to AP");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "WiFi STA disconnected, reason: %d", ev->reason);

#if CONFIG_SERIAL2NET_WIFI_MODE_STA || CONFIG_SERIAL2NET_WIFI_MODE_STA_AP_FALLBACK
            if (wifi_reconfiguring) {
                /*
                 * wifi_config is handling the reconnect — don't
                 * auto-retry or set WIFI_FAIL_BIT here.  The deferred
                 * connect task will clear this flag before connecting.
                 */
                ESP_LOGI(TAG, "Reconfiguring — deferred connect pending");
            } else if (wifi_retry_count < CONFIG_SERIAL2NET_WIFI_STA_MAX_RETRY) {
                current_led_state = LED_WIFI_CONNECTING;
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
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "AP client connected, MAC: " MACSTR, MAC2STR(ev->mac));
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "AP client disconnected, MAC: " MACSTR, MAC2STR(ev->mac));
            break;
        }
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
        case IP_EVENT_STA_GOT_IP: {
            ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "WiFi STA got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
            wifi_retry_count = 0;
            current_led_state = LED_WIFI_STA_OK;
            xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);

#if CONFIG_SERIAL2NET_HTTP_CONFIG_ENABLE
            /*
             * STA has an IP — AP no longer needed, disable it to save
             * power.  HTTP stays up on the STA interface regardless.
             */
            wifi_config_ap_stop();
#endif
            break;
        }
        }
    }
}

/**
 * @brief Initialise WiFi stack (STA / AP / STA+AP depending on Kconfig).
 *
 * Creates the default netif(s), event loop, and event group.
 * In STA+AP fallback mode, waits up to 15 s for a STA connection
 * before continuing with the AP active.
 */
static void wifi_init(void)
{
    esp_netif_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_event_group = xEventGroupCreate();

    /* Register WiFi & IP event handlers */
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

    wifi_config_t sta_cfg = {0};
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    /* Try NVS-saved credentials first, fall back to Kconfig defaults. */
    char nvs_ssid[33] = {0};
    char nvs_pw[65]   = {0};
    if (wifi_config_load_sta_creds(nvs_ssid, sizeof(nvs_ssid),
                                    nvs_pw, sizeof(nvs_pw)) == ESP_OK) {
        strncpy((char *)sta_cfg.sta.ssid, nvs_ssid,
                sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, nvs_pw,
                sizeof(sta_cfg.sta.password) - 1);
        ESP_LOGI(TAG, "STA mode: using saved credentials, SSID=%s", nvs_ssid);
    } else {
        strncpy((char *)sta_cfg.sta.ssid, CONFIG_SERIAL2NET_WIFI_STA_SSID,
                sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, CONFIG_SERIAL2NET_WIFI_STA_PASSWORD,
                sizeof(sta_cfg.sta.password) - 1);
        ESP_LOGI(TAG, "STA mode: using factory credentials, SSID=%s",
                 CONFIG_SERIAL2NET_WIFI_STA_SSID);
    }
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

    wifi_config_t sta_cfg = {0};
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    /* Try NVS-saved credentials first, fall back to Kconfig defaults. */
    char nvs_ssid[33] = {0};
    char nvs_pw[65]   = {0};
    if (wifi_config_load_sta_creds(nvs_ssid, sizeof(nvs_ssid),
                                    nvs_pw, sizeof(nvs_pw)) == ESP_OK) {
        strncpy((char *)sta_cfg.sta.ssid, nvs_ssid,
                sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, nvs_pw,
                sizeof(sta_cfg.sta.password) - 1);
        ESP_LOGI(TAG, "STA+AP: using saved credentials, SSID=%s", nvs_ssid);
    } else {
        strncpy((char *)sta_cfg.sta.ssid, CONFIG_SERIAL2NET_WIFI_STA_SSID,
                sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, CONFIG_SERIAL2NET_WIFI_STA_PASSWORD,
                sizeof(sta_cfg.sta.password) - 1);
        ESP_LOGI(TAG, "STA+AP: using factory credentials, SSID=%s",
                 CONFIG_SERIAL2NET_WIFI_STA_SSID);
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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "STA+AP fallback: trying STA to SSID=%s", CONFIG_SERIAL2NET_WIFI_STA_SSID);

    /**< Wait up to 15 seconds for STA connection, then continue regardless */
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

/** @} */ /* wifi */

/** @defgroup mdns mDNS
 *  @brief Multicast DNS service advertising.
 *  @{ */

/**
 * @brief Start mDNS and advertise the serial2net service.
 *
 * Publishes:
 *   - Hostname:  @c serial2net.local
 *   - Service:   @c _serial2net._tcp
 *   - TXT keys:  @c type=uart-bridge, @c port=&lt;tcp_port&gt;
 */
static void mdns_init_service(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("serial2net"));
    ESP_ERROR_CHECK(mdns_instance_name_set("Serial2Net UART Bridge"));

    mdns_txt_item_t txt_items[] = {
        {.key = "type", .value = "uart-bridge"},
        {.key = "port", .value = ""}, /**< filled below */
    };

    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", CONFIG_SERIAL2NET_TCP_PORT);
    txt_items[1].value = port_str;

    ESP_ERROR_CHECK(mdns_service_add("serial2net", "_serial2net._tcp", "_tcp",
                                     CONFIG_SERIAL2NET_TCP_PORT, txt_items, 2));
    ESP_LOGI(TAG, "mDNS: serial2net._tcp port %d", CONFIG_SERIAL2NET_TCP_PORT);
}

/** @} */ /* mdns */

/** @defgroup uart UART
 *  @brief UART initialisation for the target device.
 *  @{ */

/**
 * @brief Initialise the UART peripheral in 8N1 mode.
 *
 * @param baud  Baud rate (from NVS or Kconfig default).
 *              Pins and other params are taken from Kconfig.
 */
static void uart_init(uint32_t baud)
{
    uart_config_t uart_config = {
        .baud_rate  = (int)baud,
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

    ESP_LOGI(TAG, "UART%d: baud=%" PRIu32 " TX=GPIO%d RX=GPIO%d",
             CONFIG_SERIAL2NET_UART_PORT, baud,
             CONFIG_SERIAL2NET_UART_TX_PIN, CONFIG_SERIAL2NET_UART_RX_PIN);
}

/** @} */ /* uart */

/** @defgroup tcp_server TCP Server
 *  @brief Raw TCP server (port 8880 by default).
 *  @{ */

/**
 * @brief Create, bind, and listen on the raw TCP port.
 *
 * Sets @c SO_REUSEADDR and @c TCP_NODELAY on the listen socket.
 */
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
    setsockopt(listen_sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)); /**< Disable Nagle */

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

/** @} */ /* tcp_server */

#if CONFIG_SERIAL2NET_TELNET_ENABLE
/** @defgroup telnet_server Telnet Server
 *  @brief Telnet (RFC 854/856) server — separate port, same UART bridge.
 *  @{
 */

/**
 * @brief Create, bind, and listen on the Telnet port.
 *
 * Identical pattern to tcp_server_init() but uses
 * @ref CONFIG_SERIAL2NET_TELNET_PORT.
 */
static void telnet_server_init(void)
{
    struct sockaddr_in server_addr = {0};

    telnet_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (telnet_listen_sock < 0) {
        ESP_LOGE(TAG, "Telnet: failed to create socket: errno %d", errno);
        return;
    }

    int opt = 1;
    setsockopt(telnet_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(telnet_listen_sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(CONFIG_SERIAL2NET_TELNET_PORT);

    if (bind(telnet_listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Telnet: socket bind failed: errno %d", errno);
        close(telnet_listen_sock);
        telnet_listen_sock = -1;
        return;
    }

    if (listen(telnet_listen_sock, 1) < 0) {
        ESP_LOGE(TAG, "Telnet: socket listen failed: errno %d", errno);
        close(telnet_listen_sock);
        telnet_listen_sock = -1;
        return;
    }

    ESP_LOGI(TAG, "Telnet server listening on port %d", CONFIG_SERIAL2NET_TELNET_PORT);
}

/**
 * @brief Telnet accept task.
 *
 * Blocks on accept() for the Telnet port.  When a client connects:
 *   1. Kicks any previous client (raw TCP or Telnet).
 *   2. Sets TCP_NODELAY on the new socket.
 *   3. Calls telnet_init_session() to send the IAC handshake.
 *   4. Sets LED to green.
 *
 * Runs at priority 9 — one notch below the forwarding tasks so that
 * data can flow as soon as client_connected is set.
 */
static void telnet_accept_task(void *pvParameters)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    while (1) {
        if (telnet_listen_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int sock = accept(telnet_listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Telnet accept failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Kick old client if any (regardless of whether it was raw or telnet) */
        if (client_connected && client_sock >= 0) {
            ESP_LOGI(TAG, "Telnet: new client — closing previous connection");
            close(client_sock);
        }

        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        client_sock       = sock;
        client_connected  = true;
        client_is_telnet  = true;
        current_led_state = LED_CLIENT_CONNECTED;

        /* Initialize Telnet protocol state and send negotiation */
        telnet_init_session(sock);

        ESP_LOGI(TAG, "Telnet client connected from %s:%d",
                 inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    }
}

/** @} */ /* telnet_server */
#endif /* CONFIG_SERIAL2NET_TELNET_ENABLE */

/** @defgroup forwarding Forwarding Tasks
 *  @brief UART ↔ TCP bidirectional data forwarding.
 *  @{ */

/**
 * @brief UART → TCP forwarding task.
 *
 * Polls the UART RX ring buffer every 50 ms.  Received data is sent
 * directly to the connected TCP client.
 *
 * During Telnet negotiation (before binary mode is established),
 * UART data is silently dropped — this prevents raw bytes (including
 * 0xFF) from being misinterpreted as IAC commands by the peer.
 */
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
#if CONFIG_SERIAL2NET_TELNET_ENABLE
                /*
                 * Don't send UART data during Telnet negotiation —
                 * raw 0xFF bytes would be misinterpreted as IAC commands
                 */
                if (client_is_telnet && !telnet_is_binary_mode()) {
                    continue;
                }
#endif
                int sent = send(client_sock, buf, len, 0);
                if (sent < 0) {
                    ESP_LOGE(TAG, "TCP send error: errno %d", errno);
                    client_connected = false;
#if CONFIG_SERIAL2NET_TELNET_ENABLE
                    client_is_telnet = false;
                    telnet_reset();
#endif
                    close(client_sock);
                    client_sock = -1;
                    led_revert_to_wifi_state();
                } else {
                    led_signal_activity();
                }
            }
            /* If no client connected, data is silently dropped */
        }
    }
}

/**
 * @brief TCP → UART forwarding task.
 *
 * Blocks on recv() from the connected TCP client.  Received data is
 * written to the UART TX pin.
 *
 * For Telnet connections, every byte is passed through the IAC filter
 * (telnet_process_rx_byte) even after binary mode is negotiated.  This
 * guards against stray IAC commands that some clients emit after the
 * handshake.  For raw TCP connections, data passes through unchanged.
 */
static void tcp_to_uart_task(void *pvParameters)
{
    uint8_t *buf = malloc(CONFIG_SERIAL2NET_UART_BUFFER_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate TCP→UART buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        /* Wait for a client connection */
        if (!client_connected || client_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        int len = recv(client_sock, buf, CONFIG_SERIAL2NET_UART_BUFFER_SIZE, 0);
        if (len > 0) {
#if CONFIG_SERIAL2NET_TELNET_ENABLE
            /* Check negotiation timeout before processing data */
            if (client_is_telnet && telnet_negotiation_timed_out()) {
                ESP_LOGW(TAG, "Telnet: binary negotiation timed out, closing");
                client_connected = false;
                client_is_telnet = false;
                telnet_reset();
                close(client_sock);
                client_sock = -1;
                led_revert_to_wifi_state();
                continue;
            }

            if (client_is_telnet) {
                /*
                 * Telnet connections always pass through the IAC filter,
                 * even after binary mode is negotiated.  Reason: the
                 * client may still emit IAC commands after it processes
                 * our WILL BINARY (delayed WILL/WONT/DONT for options
                 * that were never explicitly acknowledged).  If those
                 * IAC bytes leak to UART and the target echoes them
                 * back, they appear as garbage on the user's terminal.
                 *
                 * The filter correctly handles literal 0xFF bytes
                 * (sent by the client as IAC-IAC per RFC 854), so
                 * this is safe for all normal console traffic.
                 */
                for (int i = 0; i < len; i++) {
                    if (telnet_process_rx_byte(buf[i])) {
                        uart_write_bytes(CONFIG_SERIAL2NET_UART_PORT, &buf[i], 1);
                    }
                }
            } else {
                /* Raw TCP — true transparent passthrough */
                uart_write_bytes(CONFIG_SERIAL2NET_UART_PORT, buf, len);
            }
#else
            uart_write_bytes(CONFIG_SERIAL2NET_UART_PORT, buf, len);
#endif
            led_signal_activity();
        } else if (len == 0) {
            /* Client closed connection */
            ESP_LOGI(TAG, "TCP client disconnected");
            client_connected = false;
#if CONFIG_SERIAL2NET_TELNET_ENABLE
            client_is_telnet = false;
            telnet_reset();
#endif
            close(client_sock);
            client_sock = -1;
            led_revert_to_wifi_state();
        } else {
            /* Error */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(10));
            } else {
                ESP_LOGE(TAG, "TCP recv error: errno %d, closing", errno);
                client_connected = false;
#if CONFIG_SERIAL2NET_TELNET_ENABLE
                client_is_telnet = false;
                telnet_reset();
#endif
                close(client_sock);
                client_sock = -1;
                led_revert_to_wifi_state();
            }
        }
    }
}

/**
 * @brief Raw TCP accept task.
 *
 * Blocks on accept() for the raw TCP port.  When a raw client connects
 * it kicks any previous client, sets TCP_NODELAY, resets the Telnet
 * state machine, and marks the connection as non-Telnet.
 */
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

        /* Kick old client if any */
        if (client_connected && client_sock >= 0) {
            ESP_LOGI(TAG, "New client — closing previous connection");
            close(client_sock);
        }

        /* Disable Nagle's algorithm for low-latency keystroke forwarding */
        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        client_sock       = sock;
        client_connected  = true;
#if CONFIG_SERIAL2NET_TELNET_ENABLE
        client_is_telnet  = false;
        telnet_reset();
#endif
        current_led_state = LED_CLIENT_CONNECTED;
        ESP_LOGI(TAG, "TCP client connected from %s:%d",
                 inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    }
}

/** @} */ /* forwarding */

/** @defgroup main Main
 *  @brief Application entry point and initialisation sequence.
 *  @{ */

#if CONFIG_SERIAL2NET_HTTP_CONFIG_ENABLE

#define BOOT_BUTTON_DEBOUNCE_TICKS  8   /**< 8 × 50 ms = 400 ms press  */
#define BOOT_BUTTON_POLL_MS         50

/**
 * @brief Monitor the BOOT button (GPIO0, active-low) with debounce.
 *
 * A sustained press (≥400 ms) triggers the WiFi configuration portal.
 * Runs at low priority (3) to avoid interfering with data forwarding.
 */
static void boot_button_task(void *pvParameters)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(CONFIG_SERIAL2NET_BOOT_BUTTON_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    int press_ticks = 0;

    while (1) {
        if (gpio_get_level(CONFIG_SERIAL2NET_BOOT_BUTTON_PIN) == 0) {
            press_ticks++;
            if (press_ticks == BOOT_BUTTON_DEBOUNCE_TICKS) {
                if (wifi_config_ap_is_active()) {
                    ESP_LOGI(TAG, "BOOT button pressed — stopping AP");
                    wifi_config_ap_stop();
                } else {
                    ESP_LOGI(TAG, "BOOT button pressed — starting AP");
                    wifi_config_ap_start();
                }
            }
        } else {
            press_ticks = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(BOOT_BUTTON_POLL_MS));
    }
}
#endif /* CONFIG_SERIAL2NET_HTTP_CONFIG_ENABLE */

/**
 * @brief Application entry point.
 *
 * Initialisation order:
 *   1. LED         — early visual feedback
 *   2. NVS         — required by WiFi
 *   3. WiFi        — STA / AP / STA+AP (NVS credentials take priority)
 *   4. mDNS        — advertise serial2net.local
 *   5. HTTP config — WiFi setup web UI on port 80
 *   6. UART        — target device UART
 *   7. TCP server  — raw bridge on port CONFIG_SERIAL2NET_TCP_PORT
 *   8. Telnet      — RFC 854 bridge on CONFIG_SERIAL2NET_TELNET_PORT
 *   9. FreeRTOS tasks — uart2tcp, tcp2uart, tcp_accept, telnet_accept
 */
void app_main(void)
{
    ESP_LOGI(TAG, "=== Serial2Net UART-WiFi Bridge ===");
    ESP_LOGI(TAG, "Chip: %s, Free heap: %" PRIu32 " bytes",
             CONFIG_IDF_TARGET, esp_get_free_heap_size());

    /* Initialize LED early for status feedback */
    led_init();
    xTaskCreate(led_task, "led", 2048, NULL, 5, NULL);

    /* Initialize NVS (required for WiFi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize WiFi (LED transitions to WIFI_CONNECTING internally) */
    wifi_init();

    /* Start mDNS so we can be discovered as serial2net.local */
    mdns_init_service();

#if CONFIG_SERIAL2NET_HTTP_CONFIG_ENABLE
    /*
     * HTTP configuration server — always on after WiFi init.
     * Binds INADDR_ANY:80, reachable via both STA and AP interfaces.
     */
    wifi_config_init_http();

    /*
     * If STA didn't connect, start the configuration AP so the
     * user can reach the web UI at http://192.168.4.1/.
     */
    if (!(xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT)) {
        wifi_config_ap_start();
    }
#endif

#if CONFIG_SERIAL2NET_HTTP_CONFIG_ENABLE
    /* Start BOOT button monitor (long-press toggles AP). */
    xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 3, NULL);
#endif

    /* Load UART baud rate from NVS (falls back to Kconfig default). */
    uint32_t uart_baud;
    wifi_config_load_uart_baud(&uart_baud);

    /* Initialize UART */
    uart_init(uart_baud);

    /* Initialize TCP server (raw) */
    tcp_server_init();

#if CONFIG_SERIAL2NET_TELNET_ENABLE
    /* Initialize Telnet server (RFC 854/856) */
    telnet_server_init();
#endif

    /* Start forwarding tasks */
    xTaskCreate(uart_to_tcp_task, "uart2tcp", 4096, NULL, 10, NULL);
    xTaskCreate(tcp_to_uart_task, "tcp2uart", 4096, NULL, 10, NULL);
    xTaskCreate(tcp_accept_task, "tcp_accept", 4096, NULL, 9, NULL);

#if CONFIG_SERIAL2NET_TELNET_ENABLE
    xTaskCreate(telnet_accept_task, "telnet_accept", 4096, NULL, 9, NULL);
#endif

    ESP_LOGI(TAG, "Bridge running — connect via:");
    ESP_LOGI(TAG, "  Raw TCP: serial2net.local:%d   (socat/nc, low latency)",
             CONFIG_SERIAL2NET_TCP_PORT);
#if CONFIG_SERIAL2NET_HTTP_CONFIG_ENABLE
    if (wifi_config_ap_is_active()) {
        ESP_LOGI(TAG, "  Config:  http://192.168.4.1/   (WiFi setup AP)");
    }
#endif
#if CONFIG_SERIAL2NET_TELNET_ENABLE
    ESP_LOGI(TAG, "  Telnet:  serial2net.local:%d   (standard telnet client)",
             CONFIG_SERIAL2NET_TELNET_PORT);
#endif
}

/** @} */ /* main */
