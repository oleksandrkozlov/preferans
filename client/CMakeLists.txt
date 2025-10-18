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
add_subdirectory(deps/PicoSHA2)
add_subdirectory(deps/date)

add_executable(client src/client.cpp ../proto/pref.pb.cc)

set_property(TARGET client PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:client>)

add_custom_command(
    TARGET client PRE_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources
                                    $<TARGET_FILE_DIR:client>/../resources
)

target_link_libraries(client raylib fmt docopt)
if(CMAKE_BUILD_TYPE STREQUAL Debug)
    target_link_libraries(client ${CMAKE_BINARY_DIR}/lib/libprotobuf-lited.a ${CMAKE_BINARY_DIR}/lib/libprotobufd.a)
else()
    target_link_libraries(client ${CMAKE_BINARY_DIR}/lib/libprotobuf-lite.a ${CMAKE_BINARY_DIR}/lib/libprotobuf.a)
endif()
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
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/docopt.cpp)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/PicoSHA2)
target_include_directories(client SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/deps/date/include)
target_include_directories(client PRIVATE ${PROJECT_SOURCE_DIR}/..)
target_include_directories(client PRIVATE ${PROJECT_SOURCE_DIR}/../include)

set_target_properties(client PROPERTIES SUFFIX ".html")
set_target_properties(client PROPERTIES OUTPUT_NAME "index")
set(SHELL resources/shells/shell_minimal.html)

if(NOT DEFINED CMAKE_WEBSOCKET_URL)
    set(CMAKE_WEBSOCKET_URL "ws://0.0.0.0:8080")
endif()
configure_file(${SHELL}.in ${SHELL} @ONLY)
target_link_options(
    client
    PUBLIC
    -sUSE_GLFW=3
    -sASYNCIFY
    -sNO_DISABLE_EXCEPTION_CATCHING
    -sALLOW_MEMORY_GROWTH
    -sFORCE_FILESYSTEM=1
    --shell-file
    ${SHELL}
    --preload-file
    resources
    -lwebsocket.js
)

add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/src/client.cpp COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_SOURCE_DIR}/src/client.cpp
    DEPENDS ${SHELL}.in
)
