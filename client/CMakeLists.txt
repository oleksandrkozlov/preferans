cmake_minimum_required(VERSION 3.16.3)
project(client C CXX)

list(PREPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/../cmake)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

unset(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
include_directories(SYSTEM ${CMAKE_INSTALL_PREFIX}/include/c++/v1)

include(flags)
add_subdirectory(deps/fmt)
add_subdirectory(deps/range-v3)
add_subdirectory(deps/raylib)
add_subdirectory(deps/raylib-cpp)
add_subdirectory(deps/protobuf)
add_subdirectory(deps/spdlog)
add_subdirectory(deps/gsl)

add_executable(client src/client.cpp ../proto/pref.pb.cc)

set_property(TARGET client PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:client>)

add_custom_command(
    TARGET client PRE_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources
                                    $<TARGET_FILE_DIR:client>/../resources
)

target_link_libraries(
    client raylib fmt ${CMAKE_BINARY_DIR}/lib/libprotobuf-lited.a ${CMAKE_BINARY_DIR}/lib/libprotobufd.a
)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_BINARY_DIR}/_deps/raylib-build/raylib/include)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_BINARY_DIR}/_deps/absl-src)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/fmt/include/)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/GSL/include)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/raylib-cpp/include)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/range-v3/include)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/protobuf/src)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/raygui/src)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/spdlog/include)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/gsl/include)
target_include_directories(client PRIVATE ${PROJECT_SOURCE_DIR}/..)
target_include_directories(client PRIVATE ${PROJECT_SOURCE_DIR}/../include)

set_target_properties(client PROPERTIES SUFFIX ".html")
set_target_properties(client PROPERTIES OUTPUT_NAME "index")
target_link_options(
    client
    PUBLIC
    -sUSE_GLFW=3
    -sASYNCIFY
    -sNO_DISABLE_EXCEPTION_CATCHING
    -sALLOW_MEMORY_GROWTH
    -sFORCE_FILESYSTEM=1
    --shell-file
    resources/shell.html
    --preload-file
    resources
    -lwebsocket.js
)
