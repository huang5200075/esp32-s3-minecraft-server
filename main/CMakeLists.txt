# ESP-IDF 应用入口组件（原仓库叫 src/，IDF 只认 main/ 作为 app_main 提供者）

FILE(GLOB app_sources ${CMAKE_CURRENT_LIST_DIR}/*.c)

idf_component_register(SRCS ${app_sources}
  INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../include"
  PRIV_REQUIRES esp_wifi nvs_flash esp_timer esp_psram esp_driver_tsens
                esp_wifi_portal esp_netif esp_event)

# bareiron 的 C 代码早于 -Werror 时代：缩进告警、switch 贯穿都是有意为之。
# IDF 在组件级 COMPILE_OPTIONS 之后追加 -Werror，所以要显式关掉这些告警本身。
target_compile_options(${COMPONENT_LIB} PRIVATE
  -Wno-error
  -Wno-misleading-indentation
  -Wno-implicit-fallthrough
  -Wno-unused-variable
  -Wno-unused-but-set-variable
  -Wno-sign-compare
  -Wno-format
  -Wno-stringop-truncation
  -Wno-address
  -Wno-discarded-qualifiers)
