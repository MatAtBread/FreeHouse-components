idf_component_register(
    SRCS
        "dns_server.c"
        "ota.c"
        "wifi-captiveportal.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_http_server
        esp_netif
        lwip
        app_update
)
