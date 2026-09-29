idf_component_register(
    SRCS "light_sensor.c"
    INCLUDE_DIRS "include"
    REQUIRES board_config
    PRIV_REQUIRES esp_adc driver
)
