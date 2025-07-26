cmake_minimum_required(VERSION 3.16.3)
project(server CXX)

list(PREPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/../cmake)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(flags)
find_package(boost_headers REQUIRED)
find_package(Protobuf REQUIRED)
find_package(spdlog REQUIRED)
find_package(docopt REQUIRED)
find_package(Catch2 REQUIRED)

file(GLOB_RECURSE PREF_PROTO_FILES ${PROJECT_SOURCE_DIR}/../proto/*.proto)
protobuf_generate_cpp(
    PREF_PROTO_SOURCES PREF_PROTO_HEADERS EXPORT_MACRO PROTOBUF_EXPORT ${PREF_PROTO_FILES} PROTOC_OUT_DIR
                                                                       ${PROJECT_SOURCE_DIR}/../proto
)

if(CMAKE_BUILD_TYPE STREQUAL Debug)
    include(cmake-format)
    include(ccache)
endif()

add_library(serverlib STATIC src/server.cpp ${PREF_PROTO_SOURCES})
target_link_libraries(serverlib PUBLIC Boost::headers protobuf::libprotobuf spdlog::spdlog docopt)
target_include_directories(serverlib PUBLIC ${PROJECT_SOURCE_DIR}/..)
target_include_directories(serverlib PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_include_directories(serverlib PUBLIC ${PROJECT_SOURCE_DIR}/../include)

add_executable(server src/main.cpp)
target_link_libraries(server serverlib)

if(CMAKE_BUILD_TYPE STREQUAL Debug)
    add_executable(server_test tests/server_test.cpp)
    target_link_libraries(server_test PRIVATE Catch2::Catch2WithMain serverlib)
endif()
