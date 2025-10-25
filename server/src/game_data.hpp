#pragma once

#include "auth.hpp"
#include "proto/pref.pb.h"

#include <common/common.hpp>
#include <common/logger.hpp>
#include <fmt/core.h>
#include <fmt/format.h>
#include <range/v3/all.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

namespace pref {

[[nodiscard]] inline auto formatGame(const UserGame& game) -> std::string
{
    return fmt::format(
        "{{ ID: {}, DATE: {}, TIME: {}, MMR: {}, P/D/W: {}/{}/{}, DURATION: {}, TYPE: {} }}",
        game.id(),
        formatDate(game.timestamp()),
        formatTime(game.timestamp()),
        game.mmr(),
        game.pool(),
        game.dump(),
        game.whists(),
        formatDuration(game.duration()),
        GameType_Name(game.game_type()));
}

[[nodiscard]] inline auto formatUser(const User& user) -> std::string
{
    const auto games = user.games()
        | rv::transform([](const auto& game) { return fmt::format("      {}", formatGame(game)); })
        | rng::to_vector;
    return fmt::format(
        "  USER {{\n"
        "    NAME: {}\n"
        "    ID: {}\n"
        "    GAMES:\n"
        "{}\n"
        "  }}",
        user.player_name(),
        user.player_id(),
        fmt::join(games, "\n"));
}

[[nodiscard]] inline auto formatGameData(const GameData& data) -> std::string
{
    const auto users = data.users() | rv::transform(&formatUser) | rng::to_vector;
    return fmt::format(
        "\n"
        "{{\n"
        "{}\n"
        "}}",
        fmt::join(users, "\n"));
}

template<typename GameDataT>
[[nodiscard]] auto userByPlayerId(GameDataT&& data, const PlayerIdView playerId)
{
    if constexpr (std::is_const_v<std::remove_reference_t<GameDataT>>) {
        return pref::find(data.users(), playerId, &User::player_id);
    } else {
        return pref::find(*data.mutable_users(), playerId, &User::player_id);
    }
}

[[nodiscard]] inline auto userPlayerId(const GameData& data, const PlayerNameView playerName)
{
    return pref::find(data.users(), playerName, &User::player_name, &User::player_id);
}

[[nodiscard]] inline auto playerPasswordHash(const GameData& data, const PlayerNameView playerName)
{
    return pref::find(data.users(), playerName, &User::player_name, &User::password);
}

[[nodiscard]] inline auto verifyPlayerIdAndAuthToken(
    const GameData& data, const PlayerIdView playerId, const std::string_view authToken) -> bool
{
    INFO_VAR(playerId);
    return userByPlayerId(data, playerId)
        .transform([&](const User& user) { return rng::contains(user.auth_tokens(), authToken); })
        .value_or(false);
}

[[nodiscard]] inline auto verifyPlayerNameAndPassword(
    const GameData& data, const PlayerNameView playerName, const std::string_view password) -> bool
{
    INFO_VAR(playerName);
    return playerPasswordHash(data, playerName)
        .transform([&](const std::string& hash) { return verifyPassword(password, hash); })
        .value_or(false);
}

inline auto addOrUpdateUserGame(GameData& gameData, const PlayerIdView playerId, const UserGame& newGame) -> void
{
    auto& users = *gameData.mutable_users();
    const auto userIt = rng::find(users, playerId, &User::player_id);
    if (userIt == rng::end(users)) {
        PREF_WARN("error: {} not found", VAR(playerId));
        return;
    }
    auto& user = *userIt;
    auto& games = *user.mutable_games();
    const auto gameIt = rng::find(games, newGame.id(), &UserGame::id);
    if (gameIt != rng::end(games)) {
        gameIt->MergeFrom(newGame);
    } else {
        user.add_games()->CopyFrom(newGame);
    }
}

// TODO: support token expiration
inline auto addAuthToken(GameData& data, const PlayerIdView playerId, std::string serverAuthToken) -> void
{
    userByPlayerId(data, playerId) | onValue([&](User& user) {
        user.add_auth_tokens(std::move(serverAuthToken));
        const auto totalTokens = std::size(user.auth_tokens());
        INFO_VAR(playerId, totalTokens);
    });
}

inline auto revokeAuthToken(GameData& data, const PlayerIdView playerId, const std::string_view serverAuthToken) -> void
{
    INFO_VAR(playerId);
    userByPlayerId(data, playerId) | onValue([&](User& user) {
        auto& tokens = *user.mutable_auth_tokens();
        const auto tokensCount = std::size(tokens);
        tokens.erase(rng::remove(tokens, serverAuthToken), rng::end(tokens));
        const auto tokensLeft = std::size(tokens);
        const auto tokensRemoved = tokensCount - tokensLeft;
        INFO_VAR(playerId, tokensRemoved, tokensLeft);
    });
}

[[nodiscard]] inline auto makeUserGames(const GameData& data, const PlayerId& playerId) -> UserGames
{
    return userByPlayerId(data, playerId)
        .transform([&](const User& user) {
            auto result = UserGames{};
            for (const auto& game : user.games()) { *result.add_games() = game; }
            return result;
        })
        .value_or(UserGames{});
}

[[nodiscard]] inline auto makeUserGame(
    const std::int32_t gameId,
    const std::int32_t duration,
    const std::int32_t pool,
    const std::int32_t dump,
    const std::int32_t whists,
    const std::int32_t mmr) -> UserGame
{
    auto result = UserGame{};
    result.set_id(gameId);
    result.set_duration(duration);
    result.set_pool(pool);
    result.set_dump(dump);
    result.set_whists(whists);
    result.set_mmr(mmr);
    return result;
}

[[nodiscard]] inline auto makeUserGame(const std::int32_t gameId, const GameType gameType, const std::int64_t timestamp)
    -> UserGame
{
    auto result = UserGame{};
    result.set_id(gameId);
    result.set_game_type(gameType);
    result.set_timestamp(timestamp);
    return result;
}

inline auto storeGameData(const fs::path& path, const GameData& gameData) -> void
{
    auto out = std::ofstream{path, std::ios::binary};
    if (not out) {
        PREF_WARN("error: {}, {}", std::strerror(errno), VAR(path));
        return;
    }
    if (not gameData.SerializeToOstream(&out)) {
        PREF_WARN("error: failed to serialize GameData, {}", VAR(path));
        return;
    }
}

[[nodiscard]] inline auto loadGameData(const fs::path& path) -> GameData
{
    auto in = std::ifstream{path, std::ios::binary};
    if (not in) {
        PREF_WARN("error: {}, {}", std::strerror(errno), VAR(path));
        return {};
    }
    auto result = GameData{};
    if (not result.ParseFromIstream(&in)) {
        PREF_WARN("error: failed to parse GameData, {}", VAR(path));
        return {};
    }
    return result;
}

[[nodiscard]] inline auto lastGameId(const GameData& gameData) -> std::int32_t
{
    auto result = std::int32_t{};
    for (const auto& user : gameData.users()) {
        for (const auto& game : user.games()) { result = std::max(result, game.id()); }
    }
    return result;
}

} // namespace pref
