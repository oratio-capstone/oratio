cmake_minimum_required(VERSION 3.16.0)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(fw)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

execute_process(
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/apply_bridge_patches.py"
        "$ENV{IDF_PATH}"
        "${CMAKE_SOURCE_DIR}/managed_components/espressif__iot_bridge"
    RESULT_VARIABLE bridge_patch_result
)

if(NOT bridge_patch_result EQUAL 0)
    message(FATAL_ERROR "Required IoT-Bridge framework patches could not be verified")
endif()