idf_component_register(SRCS "serial2net.c"
                       PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event lwip esp_driver_uart mdns
                       INCLUDE_DIRS "")
