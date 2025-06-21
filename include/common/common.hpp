#pragma once

#include <string>
#include <string_view>

[[nodiscard]] auto suitOf(const std::string_view card) -> std::string
{
    return std::string{card.substr(card.find("_of_") + 4)};
}

[[nodiscard]] constexpr auto getTrump(const std::string_view bid) noexcept -> std::string_view
{ // clang-format off
    if (bid.contains("WT") or bid.contains("WP") or bid.contains("MISER") or bid.contains("PASS")) { return {}; }
    if (bid.contains('S')) { return "spades"; }
    if (bid.contains('C')) { return "clubs"; }
    if (bid.contains('H')) { return "hearts"; }
    if (bid.contains('D')) { return "diamonds"; }
    return {}; // clang-format on
}
