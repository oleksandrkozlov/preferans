// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2025 Oleksandr Kozlov

#pragma once

#include "common/common.hpp"
#include "game_data.hpp"
#include "proto/pref.pb.h"
#include "serialization.hpp"
#include "server.hpp"
#include "transport.hpp"

#include <boost/asio.hpp>
#include <range/v3/all.hpp>

#include <cassert>
#include <coroutine>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace pref {

using PlayerTurnData = std::tuple<Player::Id, GameStage, std::string, bool, int, CardsNames>;

[[nodiscard]] inline auto findDeclarerId(const Context::Players& players)
{
    const auto values = players | rv::values;
    return pref::find_if(values, notEqualTo(PREF_PASS), &Player::bid, &Player::id);
}

[[nodiscard]] inline auto getDeclarer(Context::Players& players) -> Player&
{
    const auto declarerId = findDeclarerId(players);
    assert(declarerId and players.contains(*declarerId) and "declarer exists");
    return players[*declarerId];
}

[[nodiscard]] inline auto playersIdents() -> PlayersIdents
{
    return ctx().players
        | rv::values
        | rv::transform([](const Player& player) { return PlayerIdent{player.id, player.name}; })
        | rng::to_vector;
}

inline auto sendToAll(std::string payload) -> Awaitable<>
{
    const auto channels = ctx().players //
        | rv::values
        | rv::transform([](const Player& player) { return player.conn.ch; })
        | rng::to_vector;
    co_await sendToMany(channels, std::move(payload));
}

inline auto forwardToAll(const Message& msg) -> Awaitable<>
{
    return sendToAll(msg.SerializeAsString());
}

inline auto sendToAllExcept(std::string payload, const Context::Players& players, const Player::Id& excludedId)
    -> Awaitable<>
{
    static constexpr auto getPlayerId = &Context::Players::value_type::first;
    const auto channels = players
        | rv::filter(notEqualTo(excludedId), getPlayerId)
        | rv::values
        | rv::transform([](const Player& player) { return player.conn.ch; })
        | rng::to_vector;
    co_await sendToMany(channels, std::move(payload));
}

inline auto forwardToAllExcept(const Message& msg, const Player::Id& excludedId) -> Awaitable<>
{
    return sendToAllExcept(msg.SerializeAsString(), ctx().players, excludedId);
}

inline auto sendLoginResponse(
    const ChannelPtr& ch, const std::string& error, const Player::Id& playerId = {}, const std::string& authToken = {})
    -> Awaitable<>
{
    co_await sendToOne(ch, makeLoginResponse(playerId, authToken, playersIdents(), error));
}

inline auto sendAuthResponse(const ChannelPtr& ch, const std::string& errorMsg, const Player::Name& playerName = {})
    -> Awaitable<>
{
    co_await sendToOne(ch, makeAuthResponse(playerName, playersIdents(), errorMsg));
}

inline auto sendPlayerJoined(const PlayerSession& session) -> Awaitable<>
{
    return sendToAllExcept(makePlayerJoined(session.playerName, session.playerId), ctx().players, session.playerId);
}

inline auto sendPlayerLeft(const Player::Id& playerId) -> Awaitable<>
{
    return sendToAll(makePlayerLeft(playerId));
}

inline auto sendForehand() -> Awaitable<>
{
    return sendToAll(makeForehand(ctx().forehandId));
}

inline auto sendDealCardsExcept(const Player::Id& playerId, const Hand& hand) -> Awaitable<>
{
    return sendToAllExcept(makeDealCards(playerId, hand | rng::to_vector), ctx().players, playerId);
}

inline auto sendDealCardsFor(const ChannelPtr& ch, const Player::Id& playerId, const Hand& hand) -> Awaitable<>
{
    co_await sendToOne(ch, makeDealCards(playerId, hand | rng::to_vector));
}

inline auto sendPlayerTurn(const PlayerTurnData& playerTurn) -> Awaitable<>
{
    const auto& [playerId, stage, minBid, canHalfWhist, passRound, talon] = playerTurn;
    return sendToAll(makePlayerTurn(playerId, stage, minBid, canHalfWhist, passRound, talon));
}

inline auto sendBidding(const Player::Id& playerId, const std::string& bid) -> Awaitable<>
{
    return sendToAllExcept(makeBidding(playerId, bid), ctx().players, playerId);
}

inline auto sendWhisting(const Player::Id& playerId, const std::string& choice) -> Awaitable<>
{
    return sendToAll(makeWhisting(playerId, choice));
}

inline auto sendOpenWhistPlay(const Player::Id& activeWhisterId, const Player::Id& passiveWhisterId) -> Awaitable<>
{
    return sendToAll(makeOpenWhistPlay(activeWhisterId, passiveWhisterId));
}

inline auto sendOpenTalon() -> Awaitable<>
{
    assert(ctx().talon.open < std::size(ctx().talon.cards));
    ctx().talon.current = ctx().talon.cards[ctx().talon.open];
    return sendToAll(makeOpenTalon(ctx().talon.current));
}

inline auto sendMiserCards() -> Awaitable<>
{
    const auto& declarer = getDeclarer(ctx().players);
    const auto& discardedCards = ctx().talon.discardedCards;
    auto played = declarer.playedCards
        | rv::remove_if([&](const CardNameView card) { return rng::contains(discardedCards, card); })
        | rng::to_vector;
    auto remaining = rv::concat(declarer.hand, std::empty(discardedCards) ? ctx().talon.cards : discardedCards)
        | rv::remove_if([&](const CardNameView card) { return rng::contains(played, card); })
        | rng::to_vector;
    return sendToAll(makeMiserCards(std::move(remaining), std::move(played)));
}

inline auto sendTrickFinished() -> Awaitable<>
{
    const auto playersTakenTricks = ctx().players
        | rv::values
        | rv::transform([](const Player& player) { return std::pair{player.id, player.tricksTaken}; })
        | rng::to_vector;
    return sendToAll(makeTrickFinished(playersTakenTricks));
}

inline auto sendDealFinished() -> Awaitable<>
{
    return sendToAll(makeDealFinished(ctx().scoreSheet));
}

inline auto sendPingPong(const Message& msg, const ChannelPtr& ch) -> Awaitable<>
{
    co_await sendToOne(ch, msg.SerializeAsString());
}

inline auto sendUserGames() -> Awaitable<>
{
    for (const auto& player : ctx().players | rv::values) {
        co_await sendToOne(player.conn.ch, makeUserGames(ctx().gameData, player.id));
    }
}

} // namespace pref
