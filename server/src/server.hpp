// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2025 Oleksandr Kozlov

#pragma once

#include "common/common.hpp"
#include "common/logger.hpp"
#include "proto/pref.pb.h"
#include "transport.hpp"

#include <boost/asio.hpp>
#include <boost/system.hpp>
#include <range/v3/all.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
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
    ReadyCheckState readyCheckState = ReadyCheckState::NOT_REQUESTED;
    Offer offer = Offer::NO_OFFER;

    auto clear() -> void
    {
        hand.clear();
        playedCards.clear();
        bid.clear();
        whistingChoice.clear();
        howToPlayChoice.clear();
        tricksTaken = 0;
        readyCheckState = ReadyCheckState::NOT_REQUESTED;
        offer = Offer::NO_OFFER;
    }
};

struct PlayedCard {
    Player::Id playerId;
    CardName name;
};

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

[[nodiscard]] constexpr auto pickMinBid(int round, std::span<const std::string_view> values) noexcept
    -> std::string_view
{
    assert(not std::empty(values));
    return values[static_cast<std::size_t>(std::clamp(round, 0, static_cast<int>(std::size(values) - 1)))];
}

struct PassGame {
    static constexpr auto s_rounds = 3; // 1, 2, 3
    static constexpr auto s_progression = ProgressionArgs{.prog = Progression::Arithmetic, .first = 1, .step = 1};
    static constexpr auto s_minBids = std::array<std::string_view, 2>{PREF_SIX, PREF_SEVEN};
    int round{};
    bool now{};

    [[nodiscard]] constexpr auto minBid() const noexcept -> std::string_view
    {
        return pickMinBid(round, s_minBids);
    }

    auto update() -> void
    {
        now = true;
        if (round <= 1) {
            ++round;
        } else {
            round = s_rounds;
        }
    }

    auto resetRound() -> void
    {
        assert(round != 0);
        round = 0;
    }

    auto clear() -> void
    {
        now = false;
        // `round` is not reset between deals
    }
};

struct Context {
    using Players = std::map<Player::Id, Player, std::less<>>;

    explicit Context(net::any_io_executor executor)
        : ex{std::move(executor)}
    {
    }

    [[nodiscard]] auto whoseTurnId() const -> Player::IdView;
    [[nodiscard]] auto player(Player::IdView playerId) const -> Player&;
    [[nodiscard]] auto playerName(Player::IdView playerId) const -> Player::NameView;
    [[nodiscard]] static auto areWhistersPass() -> bool;
    [[nodiscard]] static auto areWhistersWhist() -> bool;
    [[nodiscard]] static auto areWhistersPassAndWhist() -> bool;
    [[nodiscard]] static auto isHalfWhistAfterPass() -> bool;
    [[nodiscard]] static auto isPassAfterHalfWhist() -> bool;
    [[nodiscard]] static auto isWhistAfterHalfWhist() -> bool;
    [[nodiscard]] static auto countWhistingChoice(WhistingChoice choice) -> std::ptrdiff_t;

    auto clear() -> void
    {
        talon.clear();
        trick.clear();
        lastTrick.clear();
        trump.clear();
        passGame.clear();
        isDeclarerFirstMiserTurn = false;
        for (auto&& [_, p] : players) { p.clear(); }
    }

    auto shutdown() -> void
    {
        players.clear();
    }

    net::any_io_executor ex;
    GameStage stage = GameStage::UNKNOWN;
    mutable Players players;
    Players::const_iterator whoseTurnIt;
    Talon talon;
    std::vector<CardName> lastTrick;
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

[[nodiscard]] inline auto ctx(net::any_io_executor ex = {}) -> Context&
{
    static auto ctx = Context{std::move(ex)};
    return ctx;
}

inline constexpr auto ToPlayerId = &Context::Players::value_type::first;
inline constexpr auto ToPlayer = &Context::Players::value_type::second;

struct Beat {
    std::string_view candidate;
    std::string_view best;
    std::string_view leadSuit;
    std::string_view trump;
};

[[nodiscard]] auto beats(Beat beat) -> bool;

[[nodiscard]] auto decideTrickWinner(const std::vector<PlayedCard>& trick, std::string_view trump) -> Player::Id;
[[nodiscard]] auto calculateDealScore(const Declarer& declarer, const std::vector<Whister>& whisters) -> DealScore;

auto createAcceptor(
#ifdef PREF_SSL
    net::ssl::context ssl,
#endif // PREF_SSL
    net::ip::tcp::endpoint endpoint,
    net::any_io_executor ex) -> task<>;

}; // namespace pref
