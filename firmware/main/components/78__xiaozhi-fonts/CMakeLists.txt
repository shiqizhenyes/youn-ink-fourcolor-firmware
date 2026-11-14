idf_component_register(
    SRCS
        "src/cbin_font.c"
        "src/SourceHanSansSC_Medium_slim.c"
        "src/SourceHanSansSC_Regular_slim.c"
        "src/font_zectrix_16_1.c"
        "src/font_zectrix_48_1.c"
        "src/weather_icons/weather_icons_16.c"
        "src/weather_icons/weather_icons_48.c"
        "src/fa_settings/fa_settings_16.c"
        "src/inksight/inksight_cjk_12.c"
        "src/inksight/inksight_cjk_14.c"
        "src/inksight/inksight_cjk_16.c"
        "src/inksight/inksight_temp_48.c"
        "src/inksight/inksight_weather_icons.c"
        "src/inksight/inksight_almanac_ornaments.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        "lvgl"
)

# 为组件添加编译宏定义
target_compile_definitions(${COMPONENT_LIB} PUBLIC LV_LVGL_H_INCLUDE_SIMPLE)
