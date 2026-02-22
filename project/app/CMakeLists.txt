cmake_minimum_required(VERSION 3.20.0)

if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    list(APPEND EXTRA_CONF_FILE debug.conf)
    set(NO_BUILD_TYPE_WARNING ON)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app LANGUAGES C)
target_sources(app PRIVATE 
    src/main.c 
)
