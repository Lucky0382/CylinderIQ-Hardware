# Route sources by chip: src_c6/ for C6, src/ for S3.
# build_src_filter in platformio.ini is ignored by ESP-IDF framework;
# IDF_TARGET is the correct mechanism here.
if("${IDF_TARGET}" STREQUAL "esp32c6")
    FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src_c6/*.c ${CMAKE_SOURCE_DIR}/src_c6/*.cpp)
    idf_component_register(SRCS ${app_sources}
        INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/src_c6
        REQUIRES esp-zigbee-lib esp_coex ieee802154 esp_http_server json nvs_flash)
else()
    FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c ${CMAKE_SOURCE_DIR}/src/*.cpp)
    idf_component_register(SRCS ${app_sources}
        INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/src
        REQUIRES bt nvs_flash esp_http_server json)
endif()
