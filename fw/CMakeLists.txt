cmake_minimum_required(VERSION 3.16.0)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/lib/audio"
    "${CMAKE_CURRENT_LIST_DIR}/lib/storage"
    "${CMAKE_CURRENT_LIST_DIR}/lib/network"
)

# Prevent upstream patch commands from discovering parent Git repositories.
set(ENV{GIT_CEILING_DIRECTORIES} "$ENV{IDF_PATH}")

project(fw)
