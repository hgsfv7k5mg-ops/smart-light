idf_component_register(
    SRCS "sensor_service.c"
    INCLUDE_DIRS "include"
    REQUIRES sys_state light_sensor temp_sensor
)
