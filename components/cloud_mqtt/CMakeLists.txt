idf_component_register(
    SRCS "cloud_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES sys_state net_prov alarm_mgr
    PRIV_REQUIRES
        mqtt
        json
        esp_https_ota
        esp_app_format
        esp_partition
        esp_http_client
        esp-tls
        cloud_ota
)
