idf_component_register(
    SRCS "cloud_ota.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_https_ota
        esp_app_format
        esp_http_client
        esp-tls
        app_update
        esp_system
)
