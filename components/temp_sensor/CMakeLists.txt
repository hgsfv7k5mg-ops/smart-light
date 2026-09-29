idf_component_register(
    SRCS "temp_sensor.c"
    INCLUDE_DIRS "include"
    REQUIRES board_config ds18b20 onewire_bus driver
)
