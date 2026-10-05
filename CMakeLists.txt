# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(vmcu)
target_include_directories(app PRIVATE include)
target_sources(app PRIVATE src/main.c lib/protocol.c lib/safety.c)
target_include_directories(app PRIVATE ${ZEPHYR_BASE}/drivers/can)
target_sources(app PRIVATE drivers/can_tc397.c src/vehicle_can.c lib/can_policy.c)
