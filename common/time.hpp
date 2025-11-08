// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2025 Oleksandr Kozlov

#pragma once

#include <date/date.h>
#include <date/tz.h>

#include <chrono>
#include <cstdint>

namespace pref {

[[nodiscard]] inline auto toDateSysSec(const std::int64_t sec) -> date::sys_seconds
{
    return date::sys_seconds{std::chrono::seconds{sec}};
}

[[nodiscard]] inline auto toLocalTime(const std::int64_t timeSinceEpochInSec) -> std::int64_t
{
    const auto sec = toDateSysSec(timeSinceEpochInSec);
    return date::floor<std::chrono::seconds>(sec + date::current_zone()->get_info(sec).offset)
        .time_since_epoch()
        .count();
}

[[nodiscard]] inline auto localTimeSinceEpochInSec() -> std::int64_t
{
    return toLocalTime(date::floor<std::chrono::seconds>(std::chrono::system_clock::now()).time_since_epoch().count());
}

[[nodiscard]] inline auto durationInSec(const std::int64_t start) -> std::int32_t
{
    return static_cast<std::int32_t>(localTimeSinceEpochInSec() - start);
}

[[nodiscard]] inline auto formatDate(const std::int64_t sec) -> std::string
{
    return date::format("%m/%d/%Y", toDateSysSec(sec));
}

[[nodiscard]] inline auto formatTime(const std::int64_t sec) -> std::string
{
    return date::format("%I:%M %p", toDateSysSec(sec));
}

[[nodiscard]] inline auto formatDuration(const std::int32_t sec) -> std::string
{
    const auto hms = date::hh_mm_ss{std::chrono::seconds{sec}};
    return date::format(hms.hours().count() > 0 ? "%H:%M:%S" : "%M:%S", hms.to_duration());
}

} // namespace pref
