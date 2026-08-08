idf_component_register(
    SRCS "main.c" "wifi_ap.c" "bt_manager.c" "bt_state.c" "pan_manager.c" "nat_router.c" "captive_dns.c" "led_indicator.c" "http_server.c" "console_repl.c" "resilience.c"
    INCLUDE_DIRS "." "../components/tinypan/src"
    REQUIRES nvs_flash esp_netif esp_wifi esp_http_server bt spi_flash driver
    PRIV_REQUIRES tinypan spiffs esp_driver_gpio esp_coex
)
