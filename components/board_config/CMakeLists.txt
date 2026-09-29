# header-only 组件：只导出头文件，但需要在依赖方传递 driver / esp_adc 的头文件路径
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES driver esp_adc
)
