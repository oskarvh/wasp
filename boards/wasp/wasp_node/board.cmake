# SPDX-License-Identifier: Apache-2.0
#
# Bring-up runner is openocd: this board is debugged through a bare ST-Link
# V2 on the TC2030 (J6), and there is no STM32CubeProgrammer or J-Link on
# the bench. openocd is listed first so plain `west flash` picks it.

# keep first
board_runner_args(openocd --target-handle=_CHIPNAME.cpu0)
board_runner_args(stm32cubeprogrammer "--port=swd" "--reset-mode=hw")
board_runner_args(jlink "--device=STM32H723ZG" "--speed=4000")

# keep first
include(${ZEPHYR_BASE}/boards/common/openocd-stm32.board.cmake)
include(${ZEPHYR_BASE}/boards/common/stm32cubeprogrammer.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
