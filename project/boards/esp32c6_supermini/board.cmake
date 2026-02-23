# SPDX-License-Identifier: Apache-2.0

if(NOT "${OPENOCD}" MATCHES "^${ESPRESSIF_TOOLCHAIN_PATH}/.*")
  set(OPENOCD OPENOCD-NOTFOUND)
endif()
find_program(OPENOCD openocd PATHS ${ESPRESSIF_TOOLCHAIN_PATH}/openocd-esp32/bin NO_DEFAULT_PATH)

board_runner_args(openocd --target-handle=_TARGETNAME_0)
# ESP32 openocd doesn't support the generic "reset" command;
# use the ESP-specific reset via init instead.
board_runner_args(openocd "--cmd-reset-halt=init")

include(${ZEPHYR_BASE}/boards/common/esp32.board.cmake)
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
