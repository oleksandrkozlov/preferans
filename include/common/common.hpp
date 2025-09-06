#pragma once

#include <range/v3/all.hpp>

#include <map>
#include <string>
#include <string_view>

namespace pref {

// NOLINTBEGIN(cppcoreguidelines-macro-usage)

#define SPADES "spades"
#define CLUBS "clubs"
#define HEARTS "hearts"
#define DIAMONDS "diamonds"

#define SPADE "♠"
#define CLUB "♣"
#define HEART "♥"
#define DIAMOND "♦"
#define ARROW_LEFT "←"
#define ARROW_RIGHT "→"

#define SIX "6"
#define SEVEN "7"
#define EIGHT "8"
#define NINE "9"
#define TEN "10"
#define JACK "jack"
#define QUEEN "queen"
#define KING "king"
#define ACE "ace"

#define WT "WT" // without talon
#define NINE_WT NINE " " WT
#define TEN_WT TEN " " WT
#define MISER "Miser"
#define MISER_WT "Mis." WT
#define PASS "Pass"

#define WHIST "Whist"
#define HALF_WHIST "Half-Whist"

#define _OF_ "_of_"

// NOLINTEND(cppcoreguidelines-macro-usage)

using namespace std::literals;
namespace rng = ranges;
namespace rv = rng::views;

using CardName = std::string;
using PlayerId = std::string;
using PlayerName = std::string;

// TODO: Support 4 players
constexpr auto NumberOfPlayers = 3uz;

[[nodiscard]] inline auto cardSuit(const std::string_view card) -> std::string
{
    return std::string{card.substr(card.find(_OF_) + 4)};
}

[[nodiscard]] inline auto cardRank(const std::string_view card) -> std::string
{
    return std::string{card.substr(0, card.find(_OF_))};
}

[[nodiscard]] inline auto rankValue(const std::string_view rank) -> int
{
    static const auto rankMap = std::map<std::string_view, int>{
        {ACE, 8}, {KING, 7}, {QUEEN, 6}, {JACK, 5}, {TEN, 4}, {NINE, 3}, {EIGHT, 2}, {SEVEN, 1}};
    return rankMap.at(rank);
}

[[nodiscard]] constexpr auto getTrump(const std::string_view bid) noexcept -> std::string_view
{ // clang-format off
    if (bid.contains(WT) or bid.contains(MISER) or bid.contains(PASS)) { return {}; }
    if (bid.contains(SPADE)) { return SPADES; }
    if (bid.contains(CLUB)) { return CLUBS; }
    if (bid.contains(HEART)) { return HEARTS; }
    if (bid.contains(DIAMOND)) { return DIAMONDS; }
    return {}; // clang-format on
}

template<typename Callable>
[[maybe_unused]] [[nodiscard]] auto unpack(Callable callable)
{
    return [cb = std::move(callable)](const auto& pair) { return cb(pair.first, pair.second); };
}

} // namespace pref
