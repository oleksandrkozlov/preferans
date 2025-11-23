# SPDX-License-Identifier: AGPL-3.0-only
#
# Copyright (c) 2025 Oleksandr Kozlov

cmake_minimum_required(VERSION 3.16.3)
project(server CXX)

list(PREPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/../cmake)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(CMAKE_BUILD_TYPE STREQUAL Debug)
    option(ENABLE_SANITIZERS "Enable undefined behavior, leak, and address sanitizers" OFF)
    option(ENABLE_THREAD_SANITIZER "Enable the thread sanitizer" OFF)
endif()

include(flags)

if(CMAKE_BUILD_TYPE STREQUAL Debug)
    include(ccache)
    include(clang-format)
    include(clang-tidy)
    include(cmake-format)
    include(iwyu)
    include(sanitizers)
endif()

find_package(Microsoft.GSL REQUIRED)
find_package(boost_headers REQUIRED)
find_package(botan REQUIRED)
find_package(date REQUIRED)
find_package(docopt REQUIRED)
find_package(fmt REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(range-v3 REQUIRED)
find_package(spdlog REQUIRED)
find_package(stdexec REQUIRED)

if(CMAKE_BUILD_TYPE STREQUAL Debug)
    find_package(Catch2 REQUIRED)
endif()
if(PREF_SSL)
    find_package(OpenSSL REQUIRED)
    message(STATUS "PREF_SSL: ${PREF_SSL}")
endif()

add_library(proto-objects OBJECT ${PROJECT_SOURCE_DIR}/../proto/pref.proto)
protobuf_generate(
    TARGET
    proto-objects
    IMPORT_DIRS
    ${PROJECT_SOURCE_DIR}/../proto
    PROTOC_OUT_DIR
    ${PROJECT_SOURCE_DIR}/../proto
    LANGUAGE
    cpp
)
protobuf_generate(
    TARGET
    proto-objects
    IMPORT_DIRS
    ${PROJECT_SOURCE_DIR}/../proto
    PROTOC_OUT_DIR
    ${PROJECT_SOURCE_DIR}/../tests
    LANGUAGE
    python
)

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
)

target_link_libraries(
    serverlib
    PUBLIC Boost::headers
           Microsoft.GSL::GSL
           botan::botan
           date::date
           date::date-tz
           docopt
           fmt::fmt
           proto-objects
           protobuf::libprotobuf-lite
           range-v3::range-v3
           STDEXEC::stdexec
)

if(PREF_SSL)
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
