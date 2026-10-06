# SPDX-License-Identifier: Apache-2.0
zephyr_include_directories(../include ../arch/tricore/include)
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_UART_ASCLIN ../drivers/serial/uart_infineon_aurix.c)
zephyr_library_sources_ifdef(CONFIG_AURIX_IR ../drivers/interrupt_controller/intc_infineon_aurix.c)
zephyr_library_sources_ifdef(CONFIG_AURIX_STM ../drivers/timer/infineon_aurix_stm.c)
