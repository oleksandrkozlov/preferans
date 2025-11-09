# SPDX-License-Identifier: AGPL-3.0-only
#
# Copyright (c) 2025 Oleksandr Kozlov

find_program(IWYU_PROGRAM include-what-you-use)

if(NOT IWYU_PROGRAM)
    message(FATAL_ERROR "No program 'include-what-you-use' found")
endif()

if(RUN_IWYU_ON_BUILD)
    set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE "${IWYU_PROGRAM};-Xiwyu;--mapping_file=${PROJECT_SOURCE_DIR}/../.iwyu")
endif()

find_package(Python3 3.8 COMPONENTS Interpreter REQUIRED)

find_program(IWYU_TOOL_PROGRAM iwyu_tool)

if(NOT IWYU_TOOL_PROGRAM)
    message(FATAL_ERROR "No program 'iwyu_tool.py' found")
endif()

add_custom_target(
    iwyu
    COMMAND ${Python3_EXECUTABLE} ${IWYU_TOOL_PROGRAM} -o clang -p . -j `nproc` -- -Xiwyu
            --mapping_file=${PROJECT_SOURCE_DIR}/../.iwyu | tee iwyu.txt
    COMMAND ! grep error: iwyu.txt > /dev/null 2>&1
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
    COMMENT "Analyzing code by 'iwyu'"
)

find_program(FIX_INCLUDES_PROGRAM fix_include)

if(NOT FIX_INCLUDES_PROGRAM)
    message(FATAL_ERROR "No program 'fix_include' found")
endif()

add_custom_target(
    fix-includes
    COMMAND ${Python3_EXECUTABLE} ${IWYU_TOOL_PROGRAM} -p . -j `nproc` -- -Xiwyu
            --mapping_file=${PROJECT_SOURCE_DIR}/../.iwyu | tee iwyu.txt
    COMMAND ${Python3_EXECUTABLE} ${FIX_INCLUDES_PROGRAM} < iwyu.txt
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
    COMMENT "Fixing includes using 'iwyu'"
)
