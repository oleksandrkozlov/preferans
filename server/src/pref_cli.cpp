// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2025 Oleksandr Kozlov

#include "auth.hpp"
#include "common/common.hpp"
#include "common/logger.hpp"
#include "common/time.hpp"
#include "game_data.hpp"
#include "proto/pref.pb.h"

#include <docopt/docopt.h>
#include <range/v3/all.hpp>

#include <filesystem>
#include <functional>
#include <iterator>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace pref {
namespace {

constexpr std::string_view Usage = R"(
Preferans CLI

Usage:
  pref-cli <path> add --user <name> <password>
  pref-cli <path> show --users
  pref-cli <path> show --user <id>
  pref-cli <path> show --games <id>
  pref-cli <path> show --tokens <id>
  pref-cli <path> remove --user <id>
  pref-cli <path> remove --games <id>
  pref-cli <path> remove --tokens <id> [--token=<token>]
  pref-cli (-h | --help)
)";

constexpr auto LogOnNone
    = [](const PlayerIdView playerId) { return OnNone([playerId] { PREF_W("{} not found", PREF_V(playerId)); }); };

auto listUsers(const GameData& data) -> void
{
    rng::for_each(
        data.users(), [](const auto& user) { std::println("{} | {}", user.player_id(), user.player_name()); });
}

auto showUser(const GameData& data, const PlayerIdView playerId) -> void
{
    userByPlayerId(data, playerId) | LogOnNone(playerId) | OnValue([](const User& user) {
        std::println("Name:     {}", user.player_name());
        std::println("ID:       {}", user.player_id());
        std::println("Password: {}", user.password());
        std::println("Tokens:   {}", user.auth_tokens_size());
        std::println("Games:    {}", user.games_size());
    });
}

auto showTokens(const GameData& data, const PlayerIdView playerId) -> void
{
    userByPlayerId(data, playerId) | LogOnNone(playerId) | OnValue([](const User& user) {
        rng::for_each(user.auth_tokens(), [](const std::string_view token) { std::println("{}", token); });
    });
}

auto showGames(const GameData& data, const PlayerIdView playerId) -> void
{
    userByPlayerId(data, playerId) | LogOnNone(playerId) | OnValue([](const User& user) {
        rng::for_each(user.games(), [](const UserGame& game) {
            std::println(
                "| #{:<2} | {} {} | {} | {} | {:>+4} | {}/{}/{}",
                game.id(),
                formatDate(game.timestamp()),
                formatTime(game.timestamp()),
                formatDuration(game.duration()),
                game.game_type() == GameType::NORMAL ? "Normal" : "Ranked",
                game.mmr(),
                game.pool(),
                game.dump(),
                game.whists());
        });
    });
}

auto removeTokens(GameData& data, const PlayerIdView playerId, const std::optional<std::string>& authToken) -> void
{
    userByPlayerId(data, playerId) | LogOnNone(playerId) | OnValue([&authToken, playerId](User& user) {
        authToken | OnNone([&user, playerId] {
            PREF_I("Removed {} tokens for {}", user.auth_tokens_size(), PREF_V(playerId));
            user.clear_auth_tokens();
        }) | OnValue([&user, playerId](const std::string& token) {
            auto& tokens = *user.mutable_auth_tokens();
            const auto it = rng::remove(tokens, token);
            if (it == rng::end(tokens)) {
                PREF_W("{} not found for {}", PREF_V(token), PREF_V(playerId));
                return;
            }
            tokens.erase(it, rng::end(tokens));
            PREF_I("Removed {} from {}", PREF_V(token), PREF_V(playerId));
        });
    });
}

auto addUser(GameData& data, const PlayerName& name, const std::string& password) -> void
{
    auto& newUser = *data.add_users();
    newUser.set_player_id(generateUuid());
    newUser.set_player_name(name);
    newUser.set_password(hashPassword(password));
    newUser.set_version(1);
    PREF_I("Added profileId: {}", newUser.player_id());
}

auto removeUser(GameData& data, const PlayerNameView playerId) -> void
{
    auto& users = *data.mutable_users();
    const auto it = rng::remove(users, playerId, &User::player_id);
    if (it == rng::end(users)) {
        PREF_W("{} not found", PREF_V(playerId));
        return;
    }
    users.erase(it, rng::end(users));
    PREF_I("Removed {}", PREF_V(playerId));
}

auto removeGames(GameData& data, const PlayerNameView playerId) -> void
{
    userByPlayerId(data, playerId) | LogOnNone(playerId) | OnValue([](User& user) {
        PREF_I("Removed {} games", user.games_size());
        user.clear_games();
    });
}

} // namespace
} // namespace pref

auto main(int argc, char** argv) -> int
{
    try {
        const auto args = docopt::docopt(std::string{pref::Usage}, {std::next(argv), std::next(argv, argc)});
        const auto path = args.at("<path>").asString();
        auto data = pref::loadGameData(path);
        if (args.at("show").asBool()) {
            if (args.at("--users").asBool()) {
                pref::listUsers(data);
            } else if (args.at("--user").asBool()) {
                pref::showUser(data, args.at("<id>").asString());
            } else if (args.at("--tokens").asBool()) {
                pref::showTokens(data, args.at("<id>").asString());
            } else if (args.at("--games").asBool()) {
                pref::showGames(data, args.at("<id>").asString());
            }
        } else if (args.at("remove").asBool()) {
            if (args.at("--tokens").asBool()) {
                const auto tokenOpt
                    = args.at("--token").isString() ? std::optional{args.at("--token").asString()} : std::nullopt;
                pref::removeTokens(data, args.at("<id>").asString(), tokenOpt);
            } else if (args.at("--user").asBool()) {
                pref::removeUser(data, args.at("<id>").asString());
            } else if (args.at("--games").asBool()) {
                pref::removeGames(data, args.at("<id>").asString());
            }
            pref::storeGameData(path, data);
        } else if (args.at("add").asBool()) {
            if (args.at("--user").asBool()) {
                pref::addUser(data, args.at("<name>").asString(), args.at("<password>").asString());
            }
            pref::storeGameData(path, data);
        }
        return 1;
    } catch (...) {
        return 0;
    }
}
