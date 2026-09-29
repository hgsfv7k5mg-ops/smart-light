idf_component_register(
    SRCS "buzzer.c"
    INCLUDE_DIRS "include"
    REQUIRES board_config
    PRIV_REQUIRES driver
)
