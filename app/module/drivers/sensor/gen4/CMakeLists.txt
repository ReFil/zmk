# Copyright (c) 2023 The ZMK Contributors
# SPDX-License-Identifier: MIT

zephyr_include_directories(.)
include_directories(${CMAKE_SOURCE_DIR}/include)

zephyr_library()

zephyr_library_sources(gen4.c)