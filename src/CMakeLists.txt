# Route sources by chip: src_c6/ for C6, src/ for S3.
# build_src_filter in platformio.ini is ignored by ESP-IDF framework;
# IDF_TARGET is the correct mechanism here.
if("${IDF_TARGET}" STREQUAL "esp32c6")
    FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src_c6/*.c ${CMAKE_SOURCE_DIR}/src_c6/*.cpp)
    idf_component_register(SRCS ${app_sources}
        INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/src_c6
        REQUIRES esp-zigbee-lib ieee802154 json nvs_flash driver mbedtls)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--allow-multiple-definition")
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=temperature_sensor_install")
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=psa_import_key")
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=apsme_update_device_indication")
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_ieee802154_transmit")
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_ieee802154_transmit_done")
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_ieee802154_transmit_failed")
else()
    FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c ${CMAKE_SOURCE_DIR}/src/*.cpp)
    idf_component_register(SRCS ${app_sources}
        INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/src
        REQUIRES bt nvs_flash esp_http_server esp_wifi esp_event esp_netif json driver)
endif()
