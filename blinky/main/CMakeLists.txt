if(CONFIG_IDF_TARGET_ESP32C3)
    set(APP_BSP_SRC "app_bsp_esp32c3.c")
else()
    set(APP_BSP_SRC "app_bsp_esp32.c")
endif()

idf_component_register(SRCS ${APP_BSP_SRC} "blinky.c" "app_bsp_button.c" "app_ota.c"
                    INCLUDE_DIRS ".")

function(_esp_env_unquoted var out_var)
    set(value "$ENV{${var}}")
    string(REGEX REPLACE "^\"(.*)\"$" "\\1" value "${value}")
    set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

_esp_env_unquoted(WIFI_SSID ESP_WIFI_SSID)
_esp_env_unquoted(WIFI_PWD ESP_WIFI_PWD)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    WIFI_SSID=\"${ESP_WIFI_SSID}\"
    WIFI_PWD=\"${ESP_WIFI_PWD}\"
)