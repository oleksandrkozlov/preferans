find_program(CMAKE_FORMAT_PROGRAM cmake-format)

if(NOT CMAKE_FORMAT_PROGRAM)
    message(FATAL_ERROR "No program 'cmake-format' found")
endif()

set(FIND_SOURCES
    "find ${PROJECT_SOURCE_DIR}/.. -type f \\( -iname CMakeLists.txt -o -iname \\*.cmake \\) -not -path '*build*' -not -path '*deps*'"
)

add_custom_target(cmake-format VERBATIM COMMAND bash -c "${FIND_SOURCES} | xargs -n 1 ${CMAKE_FORMAT_PROGRAM} -i")
