// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2025 Oleksandr Kozlov

#pragma once

#include "common/common.hpp"
#include "common/logger.hpp"
#include "proto/pref.pb.h"

#include <concepts>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pref {

template<std::convertible_to<std::string> T, typename MutRepeatedField>
auto moveVectorToRepeated(std::vector<T>& input, MutRepeatedField& output) -> void
{
    output.Reserve(static_cast<int>(std::size(input)));
    for (auto& value : input) { output.Add(std::move(value)); }
}

[[nodiscard]] inline auto makeMessage(const void* data, const std::size_t size) -> std::optional<Message>
{
    if (auto result = Message{}; result.ParseFromArray(data, static_cast<int>(size))) { return result; }
    PREF_W("error: failed to make Message from array");
    return {};
}

[[nodiscard]] inline auto makeLoginResponse(
    const PlayerId& playerId, const std::string& authToken, const PlayersIdentsView players, const std::string& error)
    -> std::string
{
    PREF_I("{}, {}{}", PREF_V(players), PREF_V(playerId), PREF_M(error));
    auto result = LoginResponse{};
    if (not std::empty(error)) {
        result.set_error(error);
        return makeMessage(result).SerializeAsString();
    }
    result.set_player_id(playerId);
    result.set_auth_token(authToken);
    for (const auto& [id, name] : players) {
        auto* p = result.add_players();
        p->set_player_id(id);
        p->set_player_name(name);
    }
    return makeMessage(result).SerializeAsString();
}

[[nodiscard]] inline auto makeAuthResponse(
    const PlayerName& playerName, const PlayersIdentsView players, const std::string& error) -> std::string
{
    PREF_I("{}, {}{}", PREF_V(playerName), PREF_V(players), PREF_M(error));
    auto result = AuthResponse{};
    if (not std::empty(error)) {
        result.set_error(error);
        return makeMessage(result).SerializeAsString();
    }
    result.set_player_name(playerName);
    for (const auto& [id, name] : players) {
        auto* p = result.add_players();
        p->set_player_id(id);
        p->set_player_name(name);
    }
    return makeMessage(result).SerializeAsString();
}

[[nodiscard]] inline auto makePlayerJoined(const PlayerName& playerName, const PlayerId& playerId) -> std::string
{
    PREF_DI(playerName, playerId);
    auto result = PlayerJoined{};
    result.set_player_id(playerId);
    result.set_player_name(playerName);
    return makeMessage(result).SerializeAsString();
}

[[nodiscard]] inline auto makePlayerLeft(const PlayerId& playerId) -> std::string
{
    PREF_DI(playerId);
    auto result = PlayerLeft{};
    result.set_player_id(playerId);
    return makeMessage(result).SerializeAsString();
}

[[nodiscard]] inline auto makeReadyCheck(const PlayerId& playerId, const ReadyCheckState state) -> std::string
{
    PREF_I("{}, state: {}", PREF_V(playerId), ReadyCheckState_Name(state));
    auto result = ReadyCheck{};
    result.set_player_id(playerId);
    result.set_state(state);
    return makeMessage(result).SerializeAsString();
}

[[nodiscard]] inline auto makeForehand(const PlayerId& playerId) -> std::string
{
    PREF_DI(playerId);
    auto result = Forehand{};
    result.set_player_id(playerId);
    return makeMessage(result).SerializeAsString();
}

[[nodiscard]] inline auto makeDealCards(const PlayerId& playerId, const CardsNamesView hand) -> std::string
{
    PREF_DI(playerId, hand);
    auto result = DealCards{};
    result.set_player_id(playerId);
    for (const auto& card : hand) { *result.add_cards() = card; }
    return makeMessage(result).SerializeAsString();
}

[[nodiscard]] inline auto makePlayerTurn(
    const PlayerId& playerId,
    const GameStage stage,
    const std::string& minBid,
    const bool canHalfWhist,
    const int passRound,
    const CardsNamesView talon) -> std::string
{
    PREF_I(
        "{}, {}, {}{}{}",
        PREF_V(playerId),
        GameStage_Name(stage),
        PREF_V(minBid),
        PREF_B(canHalfWhist),
        PREF_B(passRound),
        PREF_M(talon));
    auto result = PlayerTurn{};
    result.set_player_id(playerId);
    result.set_stage(stage);
    result.set_min_bid(minBid);
    result.set_can_half_whist(canHalfWhist);
    result.set_pass_round(passRound);
    for (const auto& card : talon) { result.add_talon(card); }
    return makeMessage(result).SerializeAsString();
}

[[nodiscard]] inline auto makeBidding(const PlayerId& playerId, const std::string& bid) -> std::string
{
    PREF_DI(playerId, bid);
    auto result = Bidding{};
    result.set_player_id(playerId);
    result.set_bid(bid);
    return makeMessage(result).SerializeAsString();
}

[[nodiscard]] inline auto makeWhisting(const PlayerId& playerId, const std::string& choice) -> std::string
{
    PREF_DI(playerId, choice);
    auto result = Whisting{};
    result.set_player_id(playerId);
    result.set_choice(choice);
    return makeMessage(result).SerializeAsString();
}

[[nodiscard]] inline auto makeHowToPlay(const PlayerId& playerId, const std::string& choice) -> std::string
{
    PREF_DI(playerId, choice);
    auto result = HowToPlay{};
    result.set_player_id(playerId);
    result.set_choice(choice);
    return makeMessage(result).SerializeAsString();
}

[[nodiscard]] inline auto makeOpenWhistPlay(const PlayerId& activeWhisterId, const PlayerId& passiveWhisterId)
    -> std::string
{
    PREF_DI(activeWhisterId, passiveWhisterId);
    auto result = OpenWhistPlay{};
    result.set_active_whister_id(activeWhisterId);
    result.set_passive_whister_id(passiveWhisterId);
    return makeMessage(result).SerializeAsString();
}

[[nodiscard]] inline auto makeOpenTalon(const CardName& card) -> std::string
{
    PREF_DI(card);
    auto result = OpenTalon{};
    result.set_card(card);
    return makeMessage(result).SerializeAsString();
}

[[nodiscard]] inline auto makeMiserCards(CardsNames remainingCards, CardsNames playedCards) -> std::string
{
    PREF_DI(remainingCards, playedCards);
    auto result = MiserCards{};
    moveVectorToRepeated(remainingCards, *result.mutable_remaining_cards());
    moveVectorToRepeated(playedCards, *result.mutable_played_cards());
    return makeMessage(result).SerializeAsString();
}

[[nodiscard]] inline auto makePlayCard(const PlayerId& playerId, const CardName& cardName) -> std::string
{
    PREF_DI(playerId, cardName);
    auto result = PlayCard{};
    result.set_player_id(playerId);
    result.set_card(cardName);
    return makeMessage(result).SerializeAsString();
}

[[nodiscard]] inline auto makeGameState(
    const std::span<const CardName> lastTrick,
    const std::span<const std::pair<PlayerId, int>> playersTakenTricks,
    const std::span<const std::pair<PlayerId, int>> playersCardsLeft) -> std::string
{
    PREF_DI(lastTrick, playersTakenTricks, playersCardsLeft);
    auto result = GameState{};
    for (const auto& card : lastTrick) { result.add_last_trick(card); }
    for (const auto& [playerId, tricksTaken] : playersTakenTricks) {
        auto* tricks = result.add_taken_tricks();
        tricks->set_player_id(playerId);
        tricks->set_taken(tricksTaken);
    }
    for (const auto& [playerId, cardsLeft] : playersCardsLeft) {
        auto* left = result.add_cards_left();
        left->set_player_id(playerId);
        left->set_count(cardsLeft);
    }
    return makeMessage(result).SerializeAsString();
}

[[nodiscard]] inline auto makeTrickFinished(const std::span<const std::pair<PlayerId, int>> playersTakenTricks)
    -> std::string
{
    PREF_DI(playersTakenTricks);
    auto result = TrickFinished{};
    for (const auto& [playerId, tricksTaken] : playersTakenTricks) {
        auto* tricks = result.add_tricks();
        tricks->set_player_id(playerId);
        tricks->set_taken(tricksTaken);
    }
    return makeMessage(result).SerializeAsString();
}

[[nodiscard]] inline auto makeDealFinished(const ScoreSheet& scoreSheet) -> std::string
{
    auto result = DealFinished{};
    for (const auto& [playerId, score] : scoreSheet) {
        auto& data = (*result.mutable_score_sheet())[playerId];
        for (const auto value : score.dump) { data.mutable_dump()->add_values(value); }
        for (const auto value : score.pool) { data.mutable_pool()->add_values(value); }
        for (const auto& [whistPlayerId, values] : score.whists) {
            for (const auto value : values) { (*data.mutable_whists())[whistPlayerId].add_values(value); }
        }
    }
    return makeMessage(result).SerializeAsString();
}

} // namespace pref
