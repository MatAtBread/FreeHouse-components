idf_component_register(
    SRCS
        "dns_server.c"
        "ota.c"
        "wifi-captiveportal.cpp"
        "wifi_scan.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_http_server
        esp_netif
        lwip
        app_update
)

# dev_wifi_init() is an extern "C" callback that each project provides (e.g. in board.c).
# Without -u the linker may discard it from the project's archive before this component needs it.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-u,dev_wifi_init")
