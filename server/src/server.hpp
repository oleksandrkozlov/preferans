// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2025 Oleksandr Kozlov

#pragma once

#include "common/common.hpp"
#include "common/logger.hpp"
#include "proto/pref.pb.h"
#include "transport.hpp"

#include <boost/asio.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/beast.hpp>
#ifdef PREF_SSL
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/websocket/ssl.hpp>
#endif // PREF_SSL
#include <fmt/core.h>
#include <fmt/format.h>
#include <fmt/std.h>

#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace pref {

using Hand = std::set<CardName>;

struct PlayerSession {
    using Id = std::uint64_t;

    Id id{};
    std::string playerId;
    std::string playerName;
};

struct Player {
    using Id = PlayerId;
    using Name = PlayerName;

    using IdView = PlayerIdView;
    using NameView = PlayerNameView;

    Player() = default;
    Player(Id aId, Name aName, PlayerSession::Id aSessionId, const ChannelPtr& ch);

    Id id;
    Name name;
    PlayerSession::Id sessionId{};
    Connection conn;
    Hand hand;
    std::vector<CardName> playedCards;
    std::string bid;
    std::string whistingChoice;
    std::string howToPlayChoice;
    int tricksTaken{};

    auto clear() -> void
    {
        hand.clear();
        playedCards.clear();
        bid.clear();
        whistingChoice.clear();
        howToPlayChoice.clear();
        tricksTaken = 0;
    }
};

struct PlayedCard {
    Player::Id playerId;
    CardName name;
};

// NOLINTBEGIN(performance-enum-size)
enum class WhistingChoice {
    Pass,
    Whist,
    HalfWhist,
    PassWhist,
    PassPass,
};

struct Whister {
    Player::Id id;
    WhistingChoice choice = WhistingChoice::Pass;
    int tricksTaken{};
};

enum class ContractLevel {
    Six,
    Seven,
    Eight,
    Nine,
    Ten,
    Miser,
};
// NOLINTEND(performance-enum-size)

struct Declarer {
    Player::Id id;
    ContractLevel contractLevel = ContractLevel::Six;
    int tricksTaken{};
};

struct Talon {
    std::size_t open{};
    CardName current;
    std::vector<CardName> cards;
    std::vector<CardName> discardedCards;

    auto clear() -> void
    {
        current.clear();
        cards.clear();
        discardedCards.clear();
        open = 0;
    }
};

struct PassGame {
    bool now{};
    std::optional<ContractLevel> level;

    auto clear() -> void
    {
        now = false;
        if (level == ContractLevel::Eight) { level.reset(); }
    }
};

struct Context {
    using Players = std::map<Player::Id, Player>;

    [[nodiscard]] auto whoseTurnId() const -> const Player::Id&;
    [[nodiscard]] auto player(const Player::Id& playerId) const -> Player&;
    [[nodiscard]] auto playerName(const Player::Id& playerId) const -> Player::NameView;
    [[nodiscard]] auto areWhistersPass() const -> bool;
    [[nodiscard]] auto areWhistersWhist() const -> bool;
    [[nodiscard]] auto areWhistersPassAndWhist() const -> bool;
    [[nodiscard]] auto isHalfWhistAfterPass() const -> bool;
    [[nodiscard]] auto isPassAfterHalfWhist() const -> bool;
    [[nodiscard]] auto isWhistAfterHalfWhist() const -> bool;
    [[nodiscard]] auto countWhistingChoice(WhistingChoice choice) const -> std::ptrdiff_t;

    auto clear() -> void
    {
        talon.clear();
        trick.clear();
        trump.clear();
        passGame.clear();
        isDeclarerFirstMiserTurn = false;
        for (auto&& [_, p] : players) { p.clear(); }
    }

    auto shutdown() -> void
    {
        players.clear();
    }

    mutable Players players;
    Players::const_iterator whoseTurnIt;
    Talon talon;
    std::vector<PlayedCard> trick;
    std::string trump;
    PassGame passGame;
    Player::Id forehandId;
    ScoreSheet scoreSheet;
    fs::path gameDataPath;
    GameData gameData;
    bool isDeclarerFirstMiserTurn{};

    std::int32_t gameId{};
    std::int64_t gameStarted{};
    std::int32_t gameDuration{};
};

[[nodiscard]] inline auto ctx() -> Context&
{
    static auto ctx = Context{};
    return ctx;
}

inline constexpr auto ToPlayerId = &Context::Players::value_type::first;
inline constexpr auto ToPlayer = &Context::Players::value_type::second;

constexpr auto Detached = [](const std::string_view func) { // NOLINT(fuchsia-statically-constructed-objects)
    return [func](const std::exception_ptr& eptr) {
        if (not eptr) { return; }
        try {
            std::rethrow_exception(eptr);
        } catch (const sys::system_error& error) {
            if (error.code() != net::error::operation_aborted) { PREF_W("[{}][Detached] error: {}", func, error); }
        } catch (const std::exception& error) {
            PREF_W("[{}][Detached] error: {}", func, error);
        } catch (...) {
            PREF_W("[{}][Detached] error: unknown", func);
        }
    };
};

struct Beat {
    std::string_view candidate;
    std::string_view best;
    std::string_view leadSuit;
    std::string_view trump;
};

[[nodiscard]] auto beats(Beat beat) -> bool;

[[nodiscard]] auto decideTrickWinner(const std::vector<PlayedCard>& trick, std::string_view trump) -> Player::Id;
[[nodiscard]] auto calculateDealScore(const Declarer& declarer, const std::vector<Whister>& whisters) -> DealScore;

auto acceptConnectionAndLaunchSession(
#ifdef PREF_SSL
    net::ssl::context ssl,
#endif // PREF_SSL
    net::ip::tcp::endpoint endpoint) -> Awaitable<>;

// NOLINTNEXTLINE
[[maybe_unused]] auto inline format_as(const DealScoreEntry& entry) -> std::string
{
    return fmt::format("dump: {}, pool: {}, whist: {}", entry.dump, entry.pool, entry.whist);
}

// NOLINTNEXTLINE
[[maybe_unused]] auto inline format_as(const DealScore& entry) -> std::string
{ // clang-format off
    return fmt::format("{}", fmt::join(entry
        | rv::transform(unpair([](const auto& k, const auto& v) { return fmt::format("{}: {}", k, v); })), "\n"));
} // clang-format on

}; // namespace pref
