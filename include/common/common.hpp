#pragma once

#include <range/v3/all.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>

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
namespace fs = std::filesystem;

using CardName = std::string;
using PlayerId = std::string;
using PlayerName = std::string;

// TODO: Support 4 players
constexpr auto NumberOfPlayers = 3uz;

struct ScoreEntry {
    auto operator<=>(const ScoreEntry&) const = default;

    std::int32_t dump{};
    std::int32_t pool{};
    std::int32_t whist{};
};

struct Score {
    std::vector<std::int32_t> dump;
    std::vector<std::int32_t> pool;
    std::map<PlayerId, std::vector<std::int32_t>> whists;
};

struct FinalScoreEntry {
    auto operator<=>(const FinalScoreEntry&) const = default;

    float dump{};
    float pool{};
    std::map<PlayerId, float> whists;
};

using ScoreSheet = std::map<PlayerId, Score>;
using FinalScore = std::map<PlayerId, FinalScoreEntry>;
using FinalResult = std::map<PlayerId, int>;

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

template<typename Value>
[[nodiscard]] auto notEqualTo(Value&& value)
{
    return std::bind_front(std::not_equal_to{}, std::forward<Value>(value));
}

[[nodiscard]] inline auto calculateFinalResult(FinalScore finalScore) -> FinalResult
{
    static constexpr auto numberOfPlayers = static_cast<float>(NumberOfPlayers);
    static constexpr auto price = 10.f;
    const auto adjustByMin = [](auto& scores, const auto member) {
        const auto minScore = rng::min(scores | rv::values, std::less{}, member);
        for (auto& score : scores | rv::values) {
            score.*member -= minScore.*member;
        }
    };
    const auto distributeWhists = [&](const auto member, const bool outgoing) {
        for (auto& [playerId, score] : finalScore | rv::filter([=](auto&& kv) { return kv.second.*member != 0.f; })) {
            const auto amount = (score.*member * price) / numberOfPlayers;
            for (const auto& otherId : finalScore | rv::keys | rv::filter(notEqualTo(playerId))) {
                auto& target = outgoing ? finalScore.at(playerId).whists.at(otherId)
                                        : finalScore.at(otherId).whists.at(playerId);
                target += amount;
            }
        }
    };
    adjustByMin(finalScore, &FinalScoreEntry::dump);
    adjustByMin(finalScore, &FinalScoreEntry::pool);
    distributeWhists(&FinalScoreEntry::dump, false);
    distributeWhists(&FinalScoreEntry::pool, true);
    auto finalScoreCopy = finalScore;
    for (auto& [playerId, score] : finalScore) {
        for (auto& [otherId, whists] : score.whists) {
            finalScoreCopy.at(playerId).whists.at(otherId) -= finalScore.at(otherId).whists.at(playerId);
        }
    }
    return finalScoreCopy | rv::transform(unpack([](const auto& playerId, const auto& score) { // clang-format off
        return std::pair{playerId, static_cast<int>(rng::accumulate(score.whists | rv::values, 0.f))}; }))
            | rng::to<FinalResult>; // clang-format on
}

} // namespace pref
