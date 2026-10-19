cmake_minimum_required(VERSION 3.16.0)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Prevent upstream patch commands from discovering unrelated parent repositories.
set(ENV{GIT_CEILING_DIRECTORIES} "$ENV{IDF_PATH}")

project(fw)