#pragma once

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cstddef>
#include <functional>
#include <iterator>
#include <string_view>

namespace pref {

template<std::size_t N>
inline constexpr auto FormatString = std::invoke([] {
    auto result = std::array<char, (N * 4U) - 2U>{};
    auto ptr = &result[0];
    for (auto i = 0U; i != N; ++i) {
        if (i > 0) {
            *ptr++ = ',';
            *ptr++ = ' ';
        }
        *ptr++ = '{';
        *ptr++ = '}';
    }

    return result;
});

// clang-format off
#define PREF_NARG(...) PREF_NARG_(__VA_ARGS__, PREF_RSEQ_N())
#define PREF_NARG_(...) PREF_ARG_N(__VA_ARGS__)
#define PREF_ARG_N( \
          _1,  _2,  _3,  _4,  _5,  _6,  _7,  _8,  _9, _10, \
         _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, \
         _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, \
         _31, _32, _33, _34, _35, _36, _37, _38, _39, _40, \
         _41, _42, _43, _44, _45, _46, _47, _48, _49, _50, \
         _51, _52, _53, _54, _55, _56, _57, _58, _59, _60, \
         _61, _62, _63,   N,...) N

#define PREF_RSEQ_N() \
         63, 62, 61, 60,                   \
         59, 58, 57, 56, 55, 54, 53, 52, 51, 50, \
         49, 48, 47, 46, 45, 44, 43, 42, 41, 40, \
         39, 38, 37, 36, 35, 34, 33, 32, 31, 30, \
         29, 28, 27, 26, 25, 24, 23, 22, 21, 20, \
         19, 18, 17, 16, 15, 14, 13, 12, 11, 10, \
          9,  8,  7,  6,  5,  4,  3,  2,  1,  0
// clang-format on

template<>
inline constexpr std::array<char, 0> FormatString<0>;

} // namespace pref

// clang-format off
#define PREF_V(var) fmt::format("{}: {}", #var, var) // PREF VAR
#define PREF_M(value) (not std::empty(value) ? fmt::format(", {}", PREF_V(value)) : "") // PREF (VAR) MAYBE
#define PREF_APPLY_0
#define PREF_APPLY_1(arg) PREF_V(arg)
#define PREF_APPLY_2(arg, ...) PREF_V(arg), PREF_APPLY_1(__VA_ARGS__)
#define PREF_APPLY_3(arg, ...) PREF_V(arg), PREF_APPLY_2(__VA_ARGS__)
#define PREF_APPLY_4(arg, ...) PREF_V(arg), PREF_APPLY_3(__VA_ARGS__)
#define PREF_APPLY_5(arg, ...) PREF_V(arg), PREF_APPLY_4(__VA_ARGS__)
#define PREF_APPLY_6(arg, ...) PREF_V(arg), PREF_APPLY_5(__VA_ARGS__)
#define PREF_APPLY_7(arg, ...) PREF_V(arg), PREF_APPLY_6(__VA_ARGS__)
#define PREF_APPLY_8(arg, ...) PREF_V(arg), PREF_APPLY_7(__VA_ARGS__)
#define PREF_GET_MACRO(_1, _2, _3, _4, _5, _6, _7, _8, PREF_NAME, ...) PREF_NAME
#define PREF_APPLY(...) PREF_GET_MACRO(__VA_ARGS__, PREF_APPLY_8, PREF_APPLY_7, PREF_APPLY_6, PREF_APPLY_5, PREF_APPLY_4, PREF_APPLY_3, PREF_APPLY_2, PREF_APPLY_1, PREF_APPLY_0) (__VA_ARGS__)
// clang-format on

#define PREF_I(...) SPDLOG_INFO(__VA_ARGS__ __VA_OPT__(, ) "") // PREF INFO
#define PREF_W(...) SPDLOG_WARN(__VA_ARGS__ __VA_OPT__(, ) "") // PREF WARNING
#define PREF_E(...) SPDLOG_ERROR(__VA_ARGS__ __VA_OPT__(, ) "") // PREF ERROR

#define PREF_DUMP(level, ...)                                                                                          \
    level(                                                                                                             \
        std::string_view{                                                                                              \
            std::data(pref::FormatString<PREF_NARG(__VA_ARGS__)>),                                                     \
            std::size(pref::FormatString<PREF_NARG(__VA_ARGS__)>)},                                                    \
        PREF_APPLY(__VA_ARGS__))

#define PREF_DI(...) PREF_DUMP(PREF_I, __VA_ARGS__) // PREF INFO DUMP
#define PREF_DW(...) PREF_DUMP(PREF_W, __VA_ARGS__) // PREF WARNING DUMP
#define PREF_DE(...) PREF_DUMP(PREF_E, __VA_ARGS__) // PREF ERROR DUMP
