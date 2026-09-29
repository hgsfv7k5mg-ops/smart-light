idf_component_register(
    SRCS "sntp_time.c"
    INCLUDE_DIRS "include"
    REQUIRES sys_state
    PRIV_REQUIRES esp_netif lwip
)
