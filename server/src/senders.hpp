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

[[nodiscard]] inline auto players() -> decltype(auto)
{
    return ctx().players | rv::values;
}

[[nodiscard]] inline auto findDeclarerId()
{
    const auto players = pref::players();
    return pref::find_if(
        players,
        [](const std::string_view bid) { return not std::empty(bid) and bid != PREF_PASS; },
        &Player::bid,
        &Player::id);
}

[[nodiscard]] inline auto getDeclarer() -> Player&
{
    const auto declarerId = findDeclarerId();
    assert(declarerId and ctx().players.contains(*declarerId) and "declarer exists");
    return ctx().players.at(*declarerId);
}

[[nodiscard]] inline auto playersIdents() -> PlayersIdents
{
    return players()
        | rv::transform([](const Player& player) { return PlayerIdent{player.id, player.name}; })
        | rng::to_vector;
}

inline auto sendToAll(std::string payload) -> Awaitable<>
{
    const auto channels = players() //
        | rv::transform([](const Player& player) { return player.conn.ch; })
        | rng::to_vector;
    co_await sendToMany(channels, std::move(payload));
}

inline auto forwardToAll(const Message& msg) -> Awaitable<>
{
    return sendToAll(msg.SerializeAsString());
}

inline auto sendToAllExcept(std::string payload, const Player::Id& excludedId) -> Awaitable<>
{
    const auto channels = pref::players()
        | rv::filter(notEqualTo(excludedId), &Player::id)
        | rv::transform([](const Player& player) { return player.conn.ch; })
        | rng::to_vector;
    co_await sendToMany(channels, std::move(payload));
}

inline auto forwardToAllExcept(const Message& msg, const Player::Id& excludedId) -> Awaitable<>
{
    return sendToAllExcept(msg.SerializeAsString(), excludedId);
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
    return sendToAllExcept(makePlayerJoined(session.playerName, session.playerId), session.playerId);
}

inline auto sendPlayerLeft(const Player::Id& playerId) -> Awaitable<>
{
    return sendToAll(makePlayerLeft(playerId));
}

inline auto sendReadyCheckToOne(const ChannelPtr& ch, const Player::Id& playerId, const ReadyCheckState state)
    -> Awaitable<>
{
    co_await sendToOne(ch, makeReadyCheck(playerId, state));
}

inline auto sendForehand() -> Awaitable<>
{
    return sendToAll(makeForehand(ctx().forehandId));
}

inline auto sendDealCardsExcept(const Player::Id& playerId, const Hand& hand) -> Awaitable<>
{
    return sendToAllExcept(makeDealCards(playerId, hand | rng::to_vector), playerId);
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

inline auto sendBiddingToOne(const ChannelPtr& ch, const Player::Id& playerId, const std::string& bid) -> Awaitable<>
{
    return sendToOne(ch, makeBidding(playerId, bid));
}

inline auto sendBidding(const Player::Id& playerId, const std::string& bid) -> Awaitable<>
{
    return sendToAllExcept(makeBidding(playerId, bid), playerId);
}

inline auto sendWhistingToOne(const ChannelPtr& ch, const Player::Id& playerId, const std::string& choice)
    -> Awaitable<>
{
    return sendToOne(ch, makeWhisting(playerId, choice));
}

inline auto sendHowToPlayToOne(const ChannelPtr& ch, const Player::Id& playerId, const std::string& choice)
    -> Awaitable<>
{
    return sendToOne(ch, makeHowToPlay(playerId, choice));
}

inline auto sendWhisting(const Player::Id& playerId, const std::string& choice) -> Awaitable<>
{
    return sendToAll(makeWhisting(playerId, choice));
}

inline auto sendOpenWhistPlayToOne(
    const ChannelPtr& ch, const Player::Id& activeWhisterId, const Player::Id& passiveWhisterId) -> Awaitable<>
{
    return sendToOne(ch, makeOpenWhistPlay(activeWhisterId, passiveWhisterId));
}

inline auto sendOpenWhistPlay(const Player::Id& activeWhisterId, const Player::Id& passiveWhisterId) -> Awaitable<>
{
    return sendToAll(makeOpenWhistPlay(activeWhisterId, passiveWhisterId));
}

inline auto sendOpenTalonToOne(const ChannelPtr& ch) -> Awaitable<>
{
    return sendToOne(ch, makeOpenTalon(ctx().talon.current));
}

inline auto sendOpenTalon() -> Awaitable<>
{
    assert(ctx().talon.open < std::size(ctx().talon.cards));
    ctx().talon.current = ctx().talon.cards[ctx().talon.open];
    return sendToAll(makeOpenTalon(ctx().talon.current));
}

// TODO: combine sendMiserCardsToOne() and sendMiserCards()
inline auto sendMiserCardsToOne(const ChannelPtr& ch) -> Awaitable<>
{
    const auto& declarer = getDeclarer();
    const auto& discardedCards = ctx().talon.discardedCards;
    auto played = declarer.playedCards
        | rv::remove_if([&](const CardNameView card) { return rng::contains(discardedCards, card); })
        | rng::to_vector;
    auto remaining = rv::concat(declarer.hand, std::empty(discardedCards) ? ctx().talon.cards : discardedCards)
        | rv::remove_if([&](const CardNameView card) { return rng::contains(played, card); })
        | rng::to_vector;
    return sendToOne(ch, makeMiserCards(std::move(remaining), std::move(played)));
}

inline auto sendMiserCards() -> Awaitable<>
{
    const auto& declarer = getDeclarer();
    const auto& discardedCards = ctx().talon.discardedCards;
    auto played = declarer.playedCards
        | rv::remove_if([&](const CardNameView card) { return rng::contains(discardedCards, card); })
        | rng::to_vector;
    auto remaining = rv::concat(declarer.hand, std::empty(discardedCards) ? ctx().talon.cards : discardedCards)
        | rv::remove_if([&](const CardNameView card) { return rng::contains(played, card); })
        | rng::to_vector;
    return sendToAll(makeMiserCards(std::move(remaining), std::move(played)));
}

inline auto sendGameState(const ChannelPtr& ch) -> Awaitable<>
{
    const auto playersTakenTricks = players()
        | rv::transform([](const Player& player) { return std::pair{player.id, player.tricksTaken}; })
        | rng::to_vector;

    const auto cardsLeft = players()
        | rv::transform([](const Player& player) {
                               return std::pair{player.id, static_cast<int>(std::ssize(player.hand))};
                           })
        | rng::to_vector;
    co_await sendToOne(ch, makeGameState(ctx().lastTrick, playersTakenTricks, cardsLeft));
}

inline auto sendPlayedCards(const ChannelPtr& ch) -> Awaitable<>
{
    for (const auto& card : ctx().trick) { co_await sendToOne(ch, makePlayCard(card.playerId, card.name)); }
}

inline auto sendTrickFinished() -> Awaitable<>
{
    const auto playersTakenTricks = players()
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

inline auto sendUserGames(const Player& player) -> Awaitable<>
{
    co_await sendToOne(player.conn.ch, makeUserGames(ctx().gameData, player.id));
}

inline auto sendUserGames() -> Awaitable<>
{
    for (const auto& player : players()) { co_await sendUserGames(player); }
}

} // namespace pref
