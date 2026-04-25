idf_component_register(
    SRCS "gpio.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        driver
    PRIV_REQUIRES
        esp_adc
        freertos
        log
)
