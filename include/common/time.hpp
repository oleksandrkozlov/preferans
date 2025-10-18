#pragma once

#include <date/date.h>

#include <chrono>
#include <cstdint>

namespace pref {

// FIXME: consider time zone

[[nodiscard]] inline auto timeSinceEpochInSec() -> std::int64_t
{
    return date::floor<std::chrono::seconds>(std::chrono::system_clock::now()).time_since_epoch().count();
}

[[nodiscard]] inline auto durationInSec(const std::int64_t start) -> std::int32_t
{
    return static_cast<std::int32_t>(timeSinceEpochInSec() - start);
}

[[nodiscard]] inline auto toDateSysSec(const std::int64_t sec) -> date::sys_seconds
{
    return date::sys_seconds{std::chrono::seconds{sec}};
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
