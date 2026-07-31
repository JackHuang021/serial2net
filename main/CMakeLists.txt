idf_component_register(SRCS "wifi_config.c" "telnet.c" "serial2net.c"
                       PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server lwip esp_driver_uart esp_driver_gpio mdns app_update
                       INCLUDE_DIRS "")
