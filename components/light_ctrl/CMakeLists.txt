idf_component_register(
    SRCS "light_ctrl.c"
    INCLUDE_DIRS "include"
    REQUIRES sys_state board_config rgb_led buzzer
)
