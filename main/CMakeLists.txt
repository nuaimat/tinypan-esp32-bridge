idf_component_register(
    SRCS "main.c" "wifi_ap.c" "bt_manager.c" "bt_state.c" "pan_manager.c" "nat_router.c" "captive_dns.c" "led_indicator.c" "http_server.c" "console_repl.c" "resilience.c" "dhcp_manager.c" "netif_hooks.c" "router_globals.c" "nvs_config.c"
    INCLUDE_DIRS "." "../components/tinypan/src" "../include"
    REQUIRES nvs_flash esp_netif esp_wifi esp_http_server bt spi_flash driver
    PRIV_REQUIRES tinypan spiffs esp_driver_gpio esp_coex dhcpserver
)
