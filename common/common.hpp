// SPDX-FileCopyrightText: (c) 2025 Oleksandr Kozlov
// Copyright (c) 2025 Oleksandr Kozlov

#pragma once

#include "common/logger.hpp"
#include "common/time.hpp"
#include "proto/pref.pb.h"

#include <fmt/core.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include <range/v3/all.hpp>

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace pref {

#define PREF_SPADES "spades"
#define PREF_CLUBS "clubs"
#define PREF_HEARTS "hearts"
#define PREF_DIAMONDS "diamonds"

#define PREF_SPADE "♠"
#define PREF_CLUB "♣"
#define PREF_HEART "♥"
#define PREF_DIAMOND "♦"

inline constexpr std::string_view SpadeSign = PREF_SPADE;
inline constexpr std::string_view ClubSign = PREF_CLUB;
inline constexpr std::string_view HeartSign = PREF_HEART;
inline constexpr std::string_view DiamondSign = PREF_DIAMOND;

#define PREF_ARROW_RIGHT "▶"
#define PREF_FOREHAND_SIGN "\xF0\x9F\x83\x8F"

#define PREF_SIX "6"
#define PREF_SEVEN "7"
#define PREF_EIGHT "8"
#define PREF_NINE "9"
#define PREF_TEN "10"
#define PREF_JACK "jack"
#define PREF_QUEEN "queen"
#define PREF_KING "king"
#define PREF_ACE "ace"

#define PREF_WT "WT" // without talon
#define PREF_NINE_WT PREF_NINE " " PREF_WT
#define PREF_MIS "Mis"
#define PREF_MISER PREF_MIS "ère" // Misère
#define PREF_MISER_WT PREF_MIS "." PREF_WT // Mis.WT
#define PREF_PASS "Pass"

#define PREF_WHIST "Whist"
#define PREF_HALF_WHIST "Half-whist"
#define PREF_PASS_WHIST PREF_PASS PREF_WHIST
#define PREF_PASS_PASS PREF_PASS PREF_PASS
#define PREF_CATCH "Catch"
#define PREF_TRUST "Trust"
#define PREF_OPENLY "Openly"

#define PREF_OF_ "_of_"

using namespace std::literals;
namespace rng = ranges;
namespace rv = rng::views;
namespace fs = std::filesystem;

using PlayerId = std::string;
using PlayerIdView = std::string_view;

using PlayerName = std::string;
using PlayerNameView = std::string_view;

using PlayerIdent = std::pair<PlayerId, PlayerName>;
using PlayersIdents = std::vector<PlayerIdent>;
using PlayersIdentsView = std::span<const PlayerIdent>;

using CardName = std::string;
using CardNameView = std::string_view;

using CardsNames = std::vector<CardName>;
using CardsNamesView = std::span<const CardName>;

// TODO: support 4 players
inline constexpr auto NumberOfPlayers = 3uz;
inline constexpr auto WhistersCount = 2uz;
inline constexpr auto DeclarerCount = 1uz;

inline constexpr auto ToString = rng::to<std::string>;
inline constexpr auto ToLower = rv::transform([](unsigned char c) { return std::tolower(c); });

struct DealScoreEntry {
    auto operator<=>(const DealScoreEntry&) const = default;

    std::int32_t dump{};
    std::int32_t pool{};
    std::int32_t whist{};
};

// NOLINTNEXTLINE(readability-identifier-naming)
[[maybe_unused]] auto inline format_as(const DealScoreEntry& entry) -> std::string
{
    return fmt::format("dump: {}, pool: {}, whist: {}", entry.dump, entry.pool, entry.whist);
}

using AllWhists = std::map<PlayerId, std::vector<std::int32_t>>;
using FinalWhists = std::map<PlayerId, std::int32_t>;

struct Score {
    std::vector<std::int32_t> dump;
    std::vector<std::int32_t> pool;
    AllWhists whists;
};

struct FinalScoreEntry {
    auto operator<=>(const FinalScoreEntry&) const = default;

    std::int32_t dump{};
    std::int32_t pool{};
    FinalWhists whists;
};

using DealScore = std::map<PlayerId, DealScoreEntry>;
using ScoreSheet = std::map<PlayerId, Score>;
using FinalScore = std::map<PlayerId, FinalScoreEntry>;
using FinalResult = std::map<PlayerId, std::int32_t>;

[[nodiscard]] inline auto cardSuit(const std::string_view card) -> std::string
{
    return std::string{card.substr(card.find(PREF_OF_) + 4)};
}

[[nodiscard]] inline auto cardRank(const std::string_view card) -> std::string
{
    return std::string{card.substr(0, card.find(PREF_OF_))};
}

[[nodiscard]] inline auto rankValue(const std::string_view rank) -> int
{
    static const auto rankMap = std::map<std::string_view, int>{
        {PREF_ACE, 8},
        {PREF_KING, 7},
        {PREF_QUEEN, 6},
        {PREF_JACK, 5},
        {PREF_TEN, 4},
        {PREF_NINE, 3},
        {PREF_EIGHT, 2},
        {PREF_SEVEN, 1}};
    return rankMap.at(rank);
}

[[nodiscard]] constexpr auto getTrump(const std::string_view bid) noexcept -> std::string_view
{
    if (bid.contains(PREF_WT) or bid.contains(PREF_MIS) or bid.contains(PREF_PASS)) { return {}; }
    if (bid.contains(PREF_SPADE)) { return PREF_SPADES; }
    if (bid.contains(PREF_CLUB)) { return PREF_CLUBS; }
    if (bid.contains(PREF_HEART)) { return PREF_HEARTS; }
    if (bid.contains(PREF_DIAMOND)) { return PREF_DIAMONDS; }
    return {};
}

enum class Progression {
    Arithmetic,
    Geometric,
};

struct ProgressionArgs {
    Progression prog = Progression::Arithmetic;
    int first{};
    int step{};
};

[[nodiscard]] constexpr auto progressionTerm(const int n, const ProgressionArgs& p) noexcept -> int
{
    return (p.prog == Progression::Arithmetic) ? p.first + ((n - 1) * p.step)
                                               : static_cast<int>(p.first * std::pow(p.step, n - 1));
}

template<typename Callable>
[[nodiscard]] auto unpair(Callable&& callable)
{
    return [cb = std::forward<Callable>(callable)](const auto& pair) { return cb(pair.first, pair.second); };
}

template<typename Value>
[[nodiscard]] auto equalTo(Value&& value)
{
    return std::bind_front(std::equal_to{}, std::forward<Value>(value));
}

template<typename Value>
[[nodiscard]] auto notEqualTo(Value&& value)
{
    return std::bind_front(std::not_equal_to{}, std::forward<Value>(value));
}

template<typename Range, typename Value, typename ProjIn = rng::identity, typename ProjOut = rng::identity>
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
[[nodiscard]] auto find(Range&& range, const Value& value, ProjIn projIn = {}, ProjOut projOut = {})
{
    const auto it = rng::find(range, value, projIn);
    return it != rng::cend(range) ? std::optional{std::ref(std::invoke(projOut, *it))} : std::nullopt;
}

template<typename Range, typename Pred, typename ProjIn = rng::identity, typename ProjOut = rng::identity>
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
[[nodiscard]] auto find_if(Range&& range, Pred pred, ProjIn projIn = {}, ProjOut projOut = {})
{
    const auto it = rng::find_if(range, std::move(pred), projIn);
    return it != rng::cend(range) ? std::optional{std::ref(std::invoke(projOut, *it))} : std::nullopt;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] inline auto calculateFinalResult(FinalScore finalScore) -> FinalResult
{
    if (std::empty(finalScore)) { return {}; }
    static constexpr auto numberOfPlayers = static_cast<std::int32_t>(NumberOfPlayers);
    static constexpr auto price = 10;
    const auto adjustByMin = [](auto& scores, const auto member) {
        const auto minScore = rng::min(scores | rv::values, std::less{}, member);
        for (auto& score : scores | rv::values) { score.*member -= minScore.*member; }
    };
    const auto adjustScore = [&](const int score) {
        const auto value = score * price;
        if (value % numberOfPlayers == 0) { return 0; }
        return ((value - price) % numberOfPlayers == 0) ? -1 : +1;
    };
    const auto distributeWhists = [&](const auto member, const bool isDump) {
        for (auto& [playerId, score] : finalScore | rv::filter([&](auto&& kv) { return kv.second.*member != 0; })) {
            const auto adjust = adjustScore(score.*member);
            const int amount = ((score.*member + adjust) * price / numberOfPlayers) + (adjust * -numberOfPlayers);
            for (const auto& otherId : finalScore | rv::keys | rv::filter(notEqualTo(playerId))) {
                auto& other = finalScore.at(otherId);
                if (isDump) {
                    if (other.whists.contains(playerId)) {
                        other.whists[playerId] += amount;
                    } else {
                        other.whists.emplace(playerId, amount);
                    }
                } else { // pool
                    if (score.whists.contains(otherId)) {
                        score.whists[otherId] += amount;
                    } else {
                        score.whists.emplace(otherId, amount);
                    }
                }
            }
        }
    };
    adjustByMin(finalScore, &FinalScoreEntry::dump);
    adjustByMin(finalScore, &FinalScoreEntry::pool);
    distributeWhists(&FinalScoreEntry::dump, true);
    distributeWhists(&FinalScoreEntry::pool, false);
    auto finalScoreCopy = finalScore;
    for (const auto& [playerId, score] : finalScore) {
        for (const auto& otherId : finalScore | rv::keys | rv::filter(notEqualTo(playerId))) {
            assert(finalScoreCopy.contains(playerId));
            auto& player = finalScoreCopy[playerId];
            if (not player.whists.contains(otherId)) { player.whists.emplace(otherId, 0); }
            assert(finalScore.contains(otherId));
            const auto& other = finalScore[otherId];
            if (other.whists.contains(playerId)) { player.whists[otherId] -= other.whists.at(playerId); }
        }
    }
    return finalScoreCopy
        | rv::transform(unpair([](const auto& playerId, const auto& score) { // clang-format off
        return std::pair{playerId, rng::accumulate(score.whists | rv::values, 0)}; }))
            | rng::to<FinalResult>; // clang-format on
}

[[nodiscard]] inline auto makeFinalScore(const ScoreSheet& sheet) -> FinalScore
{
    const auto accumulate = [&](const auto& whists) { // clang-format off
        return whists | rv::transform(unpair([&](const auto& playerId, const auto& whist)  {
            return std::pair{playerId, rng::accumulate(whist, 0)};
        })) | rng::to<FinalWhists>;
    };
    return sheet | rv::transform(unpair([&](const auto& playerId, const auto& score) {
        return std::pair{playerId, FinalScoreEntry{
            .dump = rng::accumulate(score.dump, 0),
            .pool = rng::accumulate(score.pool, 0),
            .whists = accumulate(score.whists)}};
    })) | rng::to<FinalScore>; // clang-format on
}

template<typename T>
[[nodiscard]] constexpr auto methodName() noexcept -> std::string_view
{
    return "Unknown";
}

#define PREF_DEFINE_METHOD_NAME(Type)                                                                                  \
    template<>                                                                                                         \
    [[nodiscard]]                                                                                                      \
    constexpr auto methodName<Type>() noexcept -> std::string_view                                                     \
    {                                                                                                                  \
        return #Type;                                                                                                  \
    }

PREF_DEFINE_METHOD_NAME(AuthRequest)
PREF_DEFINE_METHOD_NAME(AuthResponse)
PREF_DEFINE_METHOD_NAME(Bidding)
PREF_DEFINE_METHOD_NAME(DealCards)
PREF_DEFINE_METHOD_NAME(DealFinished)
PREF_DEFINE_METHOD_NAME(DiscardTalon)
PREF_DEFINE_METHOD_NAME(Forehand)
PREF_DEFINE_METHOD_NAME(HowToPlay)
PREF_DEFINE_METHOD_NAME(Log)
PREF_DEFINE_METHOD_NAME(LoginRequest)
PREF_DEFINE_METHOD_NAME(LoginResponse)
PREF_DEFINE_METHOD_NAME(Logout)
PREF_DEFINE_METHOD_NAME(MiserCards)
PREF_DEFINE_METHOD_NAME(OpenTalon)
PREF_DEFINE_METHOD_NAME(OpenWhistPlay)
PREF_DEFINE_METHOD_NAME(PingPong)
PREF_DEFINE_METHOD_NAME(PlayCard)
PREF_DEFINE_METHOD_NAME(PlayerJoined)
PREF_DEFINE_METHOD_NAME(PlayerLeft)
PREF_DEFINE_METHOD_NAME(PlayerTurn)
PREF_DEFINE_METHOD_NAME(SpeechBubble)
PREF_DEFINE_METHOD_NAME(TrickFinished)
PREF_DEFINE_METHOD_NAME(UserGames)
PREF_DEFINE_METHOD_NAME(Whisting)

template<typename Method>
[[nodiscard]] auto makeMessage(const Method& method) -> Message
{
    auto result = Message{};
    result.set_method(std::string{methodName<Method>()});
    result.set_payload(method.SerializeAsString());
    return result;
}

template<typename Method>
[[nodiscard]] auto makeMethod(const Message& msg) -> std::optional<Method>
{
    auto result = Method{};
    if (not result.ParseFromString(msg.payload())) {
        const auto error = fmt::format("failed to make {} from string", methodName<Method>());
        PREF_DW(error);
        return {};
    }
    return result;
}

template<typename T>
inline constexpr bool IsOptionalV = false;

template<typename T>
inline constexpr bool IsOptionalV<std::optional<T>> = true;

template<typename T>
concept Optional = IsOptionalV<std::remove_cvref_t<T>>;

template<typename F, typename Opt>
concept ValueAction = Optional<Opt> and requires(F&& f, typename std::remove_cvref_t<Opt>::value_type v) {
    { std::invoke(std::forward<F>(f), v) } -> std::same_as<void>;
};

template<typename F, typename Opt>
concept NoneAction = Optional<Opt> and requires(F&& f) {
    { std::invoke(std::forward<F>(f)) } -> std::same_as<void>;
};

inline constexpr auto OnValue = [](auto&& f) {
    return [fn = std::forward<decltype(f)>(f)](auto&& opt) -> decltype(auto)
               requires ValueAction<decltype(f), decltype(opt)>
    {
        if (opt) { fn(*opt); }
        return std::forward<decltype(opt)>(opt);
    };
};

inline constexpr auto OnNone = [](auto&& f) {
    return [fn = std::forward<decltype(f)>(f)](auto&& opt) -> decltype(auto)
               requires NoneAction<decltype(f), decltype(opt)>
    {
        if (!opt) { fn(); }
        return std::forward<decltype(opt)>(opt);
    };
};

template<typename Opt, typename F>
    requires IsOptionalV<std::decay_t<Opt>>
auto operator|(Opt&& opt, F&& f)
{
    return std::invoke(std::forward<F>(f), std::forward<Opt>(opt));
}

} // namespace pref
