# SPDX-License-Identifier: AGPL-3.0-only
#
# Copyright (c) 2025 Oleksandr Kozlov

find_program(CLANG_FORMAT_PROGRAM clang-format)

if(NOT CLANG_FORMAT_PROGRAM)
    message(FATAL_ERROR "No program 'clang-format' found")
endif()

set(FIND_SOURCES
    "find ${PROJECT_SOURCE_DIR}/.. -type f \\( -iname \\*.cpp -o -iname \\*.hpp -o -iname \\*.h \\) -not -path '*build*' -not -path '*deps*' -not -path '*proto*'"
)

add_custom_target(clang-format VERBATIM COMMAND bash -c "${FIND_SOURCES} | xargs -n 1 ${CLANG_FORMAT_PROGRAM} -i")
