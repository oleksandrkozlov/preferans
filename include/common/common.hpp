#pragma once

#include "common/logger.hpp"
#include "common/time.hpp"
#include "proto/pref.pb.h"

#include <fmt/format.h>
#include <fmt/std.h>
#include <range/v3/all.hpp>

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
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

inline constexpr std::string_view SpadeSign = SPADE;
inline constexpr std::string_view ClubSign = CLUB;
inline constexpr std::string_view HeartSign = HEART;
inline constexpr std::string_view DiamondSign = DIAMOND;

#define ARROW_RIGHT "▶"

#define SIX "6"
#define SEVEN "7"
#define EIGHT "8"
#define NINE "9"
#define TEN "10"
#define JACK "jack"
#define QUEEN "queen"
#define KING "king"
#define ACE "ace"

#define PREF_WT "WT" // without talon
#define NINE_WT NINE " " PREF_WT
#define TEN_WT TEN " " PREF_WT
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

#define PREF_OF_ "_of_"

// NOLINTEND(cppcoreguidelines-macro-usage)

using namespace std::literals;
namespace rng = ranges;
namespace rv = rng::views;
namespace fs = std::filesystem;

using CardName = std::string;
using PlayerId = std::string;
using PlayerName = std::string;

using CardNameView = std::string_view;
using PlayerIdView = std::string_view;
using PlayerNameView = std::string_view;

// TODO: Support 4 players
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
        {ACE, 8}, {KING, 7}, {QUEEN, 6}, {JACK, 5}, {TEN, 4}, {NINE, 3}, {EIGHT, 2}, {SEVEN, 1}};
    return rankMap.at(rank);
}

[[nodiscard]] constexpr auto getTrump(const std::string_view bid) noexcept -> std::string_view
{
    if (bid.contains(PREF_WT) or bid.contains(PREF_MIS) or bid.contains(PREF_PASS)) { return {}; }
    if (bid.contains(SPADE)) { return SPADES; }
    if (bid.contains(CLUB)) { return CLUBS; }
    if (bid.contains(HEART)) { return HEARTS; }
    if (bid.contains(DIAMOND)) { return DIAMONDS; }
    return {};
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
[[nodiscard]] auto find(Range&& range, const Value& value, ProjIn projIn = {}, ProjOut projOut = {})
{
    const auto it = rng::find(range, value, projIn);
    return it != rng::cend(range) ? std::optional{std::ref(std::invoke(projOut, *it))} : std::nullopt;
}

template<typename Range, typename Pred, typename ProjIn = rng::identity, typename ProjOut = rng::identity>
[[nodiscard]] auto findIf(Range&& range, Pred pred, ProjIn projIn = {}, ProjOut projOut = {})
{
    const auto it = rng::find_if(range, std::move(pred), projIn);
    return it != rng::cend(range) ? std::optional{std::ref(std::invoke(projOut, *it))} : std::nullopt;
}

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
            const int amount = (score.*member + adjust) * price / numberOfPlayers + adjust * -numberOfPlayers;
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

#define DEFINE_METHOD_NAME(Type)                                                                                       \
    template<>                                                                                                         \
    [[nodiscard]]                                                                                                      \
    constexpr auto methodName<Type>() noexcept -> std::string_view                                                     \
    {                                                                                                                  \
        return #Type;                                                                                                  \
    }

DEFINE_METHOD_NAME(LoginRequest)
DEFINE_METHOD_NAME(LoginResponse)
DEFINE_METHOD_NAME(AuthRequest)
DEFINE_METHOD_NAME(AuthResponse)
DEFINE_METHOD_NAME(Logout)
DEFINE_METHOD_NAME(Bidding)
DEFINE_METHOD_NAME(DealCards)
DEFINE_METHOD_NAME(DiscardTalon)
DEFINE_METHOD_NAME(PlayCard)
DEFINE_METHOD_NAME(PlayerJoined)
DEFINE_METHOD_NAME(PlayerLeft)
DEFINE_METHOD_NAME(PlayerTurn)
DEFINE_METHOD_NAME(DealFinished)
DEFINE_METHOD_NAME(TrickFinished)
DEFINE_METHOD_NAME(Whisting)
DEFINE_METHOD_NAME(SpeechBubble)
DEFINE_METHOD_NAME(Log)
DEFINE_METHOD_NAME(HowToPlay)
DEFINE_METHOD_NAME(PingPong)
DEFINE_METHOD_NAME(OpenWhistPlay)
DEFINE_METHOD_NAME(UserGames)
DEFINE_METHOD_NAME(OpenTalon)

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
        WARN_VAR(error);
        return {};
    }
    return result;
}

// TODO: remove if unused
[[maybe_unused]] [[nodiscard]] inline auto readFile(const fs::path& path) -> std::string
{
    auto in = std::ifstream{path};
    if (not in) { throw std::runtime_error{fmt::format("{}: {}, {}", __func__, std::strerror(errno), VAR(path))}; }
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
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

inline constexpr auto onValue = [](auto&& f) {
    return [fn = std::forward<decltype(f)>(f)](auto&& opt) -> decltype(auto)
               requires ValueAction<decltype(f), decltype(opt)>
    {
        if (opt) { fn(*opt); }
        return std::forward<decltype(opt)>(opt);
    };
};

inline constexpr auto onNone = [](auto&& f) {
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
