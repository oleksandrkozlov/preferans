set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
else()
    set(CMAKE_CXX_STANDARD ${CMAKE_CXX_STANDARD})
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

set(COMPILE_OPTIONS
    -Wall
    -Walloca
    -Wcast-align
    -Wcast-qual
    -Wconversion
    -Wdouble-promotion
    -Wextra
    -Wformat-security
    -Wformat=2
    -Wimplicit-fallthrough
    -Wmisleading-indentation
    -Wno-unknown-pragmas
    -Wnull-dereference
    -Wparentheses
    -Wpedantic
    -Wpointer-arith
    -Wshadow
    -Wshift-overflow
    -Wsign-conversion
    -Wswitch-enum
    -Wunused
    -fstack-protector-all
)
set(GNU_COMPILE_OPTIONS
    -Wcast-align=strict
    -Wduplicated-branches
    -Wduplicated-cond
    -Wformat-signedness
    -Wl,-z,noexecstack
    -Wl,-z,relro,-z,now
    -Wlogical-op
    -Wstringop-overflow
    -Wtrampolines
    -Wvla-larger-than=1048576
    -fdiagnostics-color=always
    -fstack-clash-protection
    -pie
)
set(CLANG_COMPILE_OPTIONS
    -Warray-bounds-pointer-arithmetic
    -Wassign-enum
    -Wbad-function-cast
    -Wcomma
    -Wconditional-uninitialized
    -Wfloat-equal
    -Wformat-type-confusion
    -Wgnu-empty-initializer
    -Widiomatic-parentheses
    -Wloop-analysis
    -Wshift-sign-overflow
    -Wshorten-64-to-32
    -Wtautological-constant-in-range-compare
    -Wthread-safety
    -Wunreachable-code-aggressive
    -fcolor-diagnostics
)
set(CXX_COMPILE_OPTIONS -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual)
set(GNU_CXX_COMPILE_OPTIONS -Wuseless-cast)
set(GNU_C_COMPILE_OPTIONS -Wbad-function-cast -Wjump-misses-init)

add_compile_options(
    ${COMPILE_OPTIONS}
    "$<$<CXX_COMPILER_ID:GNU>:${GNU_COMPILE_OPTIONS}>"
    "$<$<CXX_COMPILER_ID:Clang>:${CLANG_COMPILE_OPTIONS}>"
    "$<$<COMPILE_LANGUAGE:CXX>:${CXX_COMPILE_OPTIONS}>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:${GNU_CXX_COMPILE_OPTIONS}>"
    "$<$<AND:$<COMPILE_LANGUAGE:C>,$<C_COMPILER_ID:GNU>>:${GNU_C_COMPILE_OPTIONS}>"
)
