idf_component_register(
    SRCS "console_cmd.c"
    INCLUDE_DIRS "include"
    REQUIRES sys_state alarm_mgr buzzer oled_ui net_prov cloud_mqtt cloud_ota temp_sensor board_config
)
