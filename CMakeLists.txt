cmake_minimum_required(VERSION 3.13)

set(esp_idf_dir "${CMAKE_CURRENT_LIST_DIR}/freertos/vendors/espressif/esp-idf")

include(${esp_idf_dir}/tools/cmake/idf.cmake)

string(FIND "${CMAKE_TOOLCHAIN_FILE}" "esp32s2" SOC_TOOLCHAIN_ESP32S2)
string(FIND "${CMAKE_TOOLCHAIN_FILE}" "esp32" SOC_TOOLCHAIN_ESP32)
if (NOT(${SOC_TOOLCHAIN_ESP32S2} EQUAL -1))
    set(SOC_NAME "esp32s2")
elseif(NOT($SOC_TOOLCHAIN_ESP32) EQUAL -1)
    set(SOC_NAME "esp32")
endif()

project(freertos_examples)

file(GLOB SOURCES "src/*.c")
add_executable(afr_demo ${SOURCES})

include_directories(afr_demo PRIVATE src/include)

list(APPEND IDF_EXTRA_COMPONENT_DIRS ${EXTRA_COMPONENT_DIRS})
get_filename_component(
    EXTRA_COMPONENT_DIRS
    "components" ABSOLUTE
)
list(APPEND IDF_EXTRA_COMPONENT_DIRS ${EXTRA_COMPONENT_DIRS})

# As of now there's no official way to redefine config files outside of the FreeRTOS source tree.
# This is a temporary approach to inject an include path so that this takes precedence over the
# config file directory inside FreeRTOS.
include_directories(BEFORE freertos-configs)

# Add freertos as an subdirectory. AFR_BOARD tells which board to target.
if ("${SOC_NAME}" STREQUAL "esp32s2")
    set(AFR_BOARD espressif.esp32s2_saola_1 CACHE INTERNAL "")
elseif("${SOC_NAME}" STREQUAL "esp32")
    set(AFR_BOARD espressif.esp32_devkitc CACHE INTERNAL "")
endif()
add_subdirectory(freertos)

# Link against the mqtt demo so that we can use it. Dependencies of this demo are transitively
# linked.
target_link_libraries(
    afr_demo
    PRIVATE
    AFR::demo_core_mqtt
    AFR::common_io
    AFR::demo_device_shadow
    AFR::demo_core_mqtt_agent
    AFR::ble
    AFR::demo_gatt_server
    idf::esp_wifi
    idf::bt
    idf:esp32
  
)
