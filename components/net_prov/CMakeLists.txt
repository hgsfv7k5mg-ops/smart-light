idf_component_register(
    SRCS "net_prov.c"
    INCLUDE_DIRS "include"
    REQUIRES sys_state
    PRIV_REQUIRES wifi_provisioning esp_wifi esp_netif esp_event nvs_flash
)
