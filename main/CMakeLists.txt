idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        sys_state
        board_config
        rgb_led
        buzzer
        light_sensor
        temp_sensor
        sensor_service
        light_ctrl
        alarm_mgr
        oled_ui
        net_prov
        sntp_time
        cloud_mqtt
        cloud_ota
        console_cmd
    PRIV_REQUIRES
        nvs_flash
        esp_app_format
        esp_partition
        app_update
        esp_system
        esp_timer
)
