# SPDX-License-Identifier: AGPL-3.0-only
#
# Copyright (c) 2025 Oleksandr Kozlov

if(ENABLE_SANITIZERS AND ENABLE_THREAD_SANITIZER)
    message(FATAL_ERROR "undefined/leak/address and thread sanitizers are incompatible")
endif()

if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=undefined,leak,address)
    link_libraries(-fsanitize=undefined,leak,address)
endif()

if(ENABLE_THREAD_SANITIZER)
    add_compile_options(-fsanitize=thread)
    link_libraries(-fsanitize=thread)
endif()
