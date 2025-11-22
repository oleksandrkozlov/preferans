# SPDX-License-Identifier: AGPL-3.0-only
#
# Copyright (c) 2025 Oleksandr Kozlov

cmake_minimum_required(VERSION 3.16.3)
project(client C CXX)

list(PREPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/../cmake)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

unset(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
include_directories(SYSTEM ${CMAKE_INSTALL_PREFIX}/include/c++/v1)

include(flags)

set(CMAKE_CXX_STANDARD 20)
add_subdirectory(deps/docopt.cpp)
set(CMAKE_CXX_STANDARD 26)

add_subdirectory(deps/fmt)
add_subdirectory(deps/range-v3)
add_subdirectory(deps/raylib)
add_subdirectory(deps/raylib-cpp)
add_subdirectory(deps/protobuf)
add_subdirectory(deps/spdlog)
add_subdirectory(deps/gsl)
add_subdirectory(deps/date)

add_executable(client src/client.cpp ../proto/pref.pb.cc)

set_property(TARGET client PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:client>)

add_custom_command(
    TARGET client PRE_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources
                                    $<TARGET_FILE_DIR:client>/../resources
)

target_link_libraries(client raylib fmt docopt)
if(CMAKE_BUILD_TYPE STREQUAL Debug)
    target_link_libraries(client ${CMAKE_BINARY_DIR}/lib/libprotobuf-lited.a)
else()
    target_link_libraries(client ${CMAKE_BINARY_DIR}/lib/libprotobuf-lite.a)
endif()

target_link_libraries(
    client
    ${CMAKE_BINARY_DIR}/lib/libabsl_log_internal_check_op.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_log_internal_message.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_log_internal_nullguard.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_examine_stack.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_log_internal_format.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_log_internal_log_sink_set.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_log_sink.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_log_internal_proto.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_log_internal_globals.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_log_globals.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_raw_hash_set.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_status.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_cord.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_cordz_info.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_cord_internal.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_hash.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_city.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_cordz_handle.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_crc_cord_state.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_crc32c.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_crc_internal.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_leak_check.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_strerror.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_str_format_internal.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_synchronization.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_stacktrace.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_symbolize.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_graphcycles_internal.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_kernel_timeout_internal.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_malloc_internal.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_time.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_time_zone.a
    ${CMAKE_BINARY_DIR}/lib/libutf8_validity.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_strings.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_strings_internal.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_base.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_spinlock_wait.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_throw_delegate.a
    ${CMAKE_BINARY_DIR}/lib/libabsl_raw_logging_internal.a
)

target_include_directories(client SYSTEM PUBLIC ${CMAKE_BINARY_DIR}/_deps/raylib-build/raylib/include)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_BINARY_DIR}/_deps/absl-src)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/fmt/include/)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/GSL/include)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/raylib-cpp/include)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/range-v3/include)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/protobuf/src)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/protobuf/third_party/utf8_range)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/raygui/src)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/spdlog/include)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/gsl/include)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/docopt.cpp)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/date/include)
target_include_directories(client PRIVATE ${PROJECT_SOURCE_DIR}/..)

set_target_properties(client PROPERTIES SUFFIX ".html")
set_target_properties(client PROPERTIES OUTPUT_NAME "index")
set(PREF_INDEX_HTML resources/html/index.html)

if(NOT DEFINED CMAKE_WEBSOCKET_URL)
    set(CMAKE_WEBSOCKET_URL "ws://0.0.0.0:8080")
endif()
configure_file(${PREF_INDEX_HTML}.in ${PREF_INDEX_HTML} @ONLY)
configure_file(resources/cards/favicon.png ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/favicon.ico COPYONLY)

if(CMAKE_BUILD_TYPE STREQUAL Debug)
    target_link_options(client PRIVATE -sDISABLE_EXCEPTION_CATCHING=0)
else()
    target_link_options(client PRIVATE -sDISABLE_EXCEPTION_CATCHING=1)
    target_compile_options(client PRIVATE -fno-exceptions)
endif()

target_link_options(
    client
    PRIVATE
    -sSTACK_SIZE=1048576
    -sINITIAL_MEMORY=128MB
    -sALLOW_MEMORY_GROWTH=0
    -sEXPORTED_RUNTIME_METHODS=requestFullscreen,HEAPF32
    --shell-file
    ${PREF_INDEX_HTML}
    --preload-file
    resources
    -lwebsocket.js
)

add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/src/client.cpp COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_SOURCE_DIR}/src/client.cpp
    DEPENDS ${PREF_INDEX_HTML}.in
)
