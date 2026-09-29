idf_component_register(
    SRCS "oled_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES sys_state board_config ssd1306
    PRIV_REQUIRES driver esp_timer
)
