idf_component_register(
    SRCS "encryption.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        mbedtls
        esp_hw_support
)
