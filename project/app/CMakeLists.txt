cmake_minimum_required(VERSION 3.20.0)

if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    list(APPEND EXTRA_CONF_FILE debug.conf)
    set(NO_BUILD_TYPE_WARNING ON)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app LANGUAGES C)
target_sources(app PRIVATE
    src/main.c
    src/sdcard.c
    src/audio.c
    src/mp3.c
    src/knob_uart.c
    src/libhelix-mp3/mp3dec.c
    src/libhelix-mp3/mp3tabs.c
    src/libhelix-mp3/real/bitstream.c
    src/libhelix-mp3/real/buffers.c
    src/libhelix-mp3/real/dct32.c
    src/libhelix-mp3/real/dequant.c
    src/libhelix-mp3/real/dqchan.c
    src/libhelix-mp3/real/huffman.c
    src/libhelix-mp3/real/hufftabs.c
    src/libhelix-mp3/real/imdct.c
    src/libhelix-mp3/real/polyphase.c
    src/libhelix-mp3/real/scalfact.c
    src/libhelix-mp3/real/stproc.c
    src/libhelix-mp3/real/subband.c
    src/libhelix-mp3/real/trigtabs.c
)
target_include_directories(app PRIVATE src/libhelix-mp3/pub)
