# SPDX-License-Identifier: AGPL-3.0-only
#
# Copyright (c) 2025 Oleksandr Kozlov

find_program(CCACHE_PROGRAM ccache)

if(NOT CCACHE_PROGRAM)
    message(FATAL_ERROR "No program 'ccache' found")
endif()

set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
