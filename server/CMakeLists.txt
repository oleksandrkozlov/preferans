# SPDX-License-Identifier: AGPL-3.0-only
#
# Copyright (c) 2025 Oleksandr Kozlov

cmake_minimum_required(VERSION 3.16.3)
project(server CXX)

list(PREPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/../cmake)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(ENABLE_SANITIZERS "Enable undefined behavior, leak, and address sanitizers" OFF)
option(ENABLE_THREAD_SANITIZER "Enable the thread sanitizer" OFF)

include(flags)
include(sanitizers)

find_package(PkgConfig REQUIRED)
find_package(boost_headers REQUIRED)
find_package(Protobuf REQUIRED)
find_package(spdlog REQUIRED)
find_package(docopt REQUIRED)
find_package(Catch2 REQUIRED)
find_package(date REQUIRED)
pkg_check_modules(botan-3 REQUIRED IMPORTED_TARGET "botan-3")

file(GLOB_RECURSE PREF_PROTO_FILES ${PROJECT_SOURCE_DIR}/../proto/*.proto)
protobuf_generate_cpp(
    PREF_PROTO_CPP_SOURCES PREF_PROTO_CPP_HEADERS EXPORT_MACRO PROTOBUF_EXPORT ${PREF_PROTO_FILES} PROTOC_OUT_DIR
                                                                               ${PROJECT_SOURCE_DIR}/../proto
)
protobuf_generate_python(PREF_PROTO_PY_FILES ${PREF_PROTO_FILES} PROTOC_OUT_DIR ${PROJECT_SOURCE_DIR}/../tests)
add_custom_target(pref_proto_py_files DEPENDS ${PREF_PROTO_PY_FILES})

if(CMAKE_BUILD_TYPE STREQUAL Debug)
    include(ccache)
    include(clang-format)
    include(clang-tidy)
    include(cmake-format)
    include(iwyu)
endif()

add_library(
    serverlib STATIC
    src/auth.cpp
    src/game_data.cpp
    src/main.cpp
    src/pref_cli.cpp
    src/senders.cpp
    src/serialization.cpp
    src/server.cpp
    src/transport.cpp
    ${PREF_PROTO_CPP_SOURCES}
)
add_dependencies(serverlib pref_proto_py_files)
target_link_libraries(
    serverlib
    PUBLIC Boost::headers
           protobuf::libprotobuf
           spdlog::spdlog
           docopt
           date::date
           date::date-tz
           PkgConfig::botan-3
)
if(PREF_SSL)
    find_package(OpenSSL REQUIRED)
    message(STATUS "PREF_SSL: ${PREF_SSL}")
    add_compile_definitions("PREF_SSL")
    target_link_libraries(serverlib PUBLIC OpenSSL::SSL)
endif()

target_include_directories(serverlib PUBLIC ${PROJECT_SOURCE_DIR}/..)
target_include_directories(serverlib PUBLIC ${PROJECT_SOURCE_DIR}/src)

add_executable(server src/main.cpp)
target_link_libraries(server serverlib)

add_executable(pref-cli src/pref_cli.cpp)
target_link_libraries(pref-cli serverlib)

if(CMAKE_BUILD_TYPE STREQUAL Debug)
    add_executable(test_server tests/test_server.cpp)
    target_link_libraries(test_server PRIVATE Catch2::Catch2WithMain serverlib)
endif()
