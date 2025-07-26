#pragma once

#include "common/logger.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
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

namespace net = boost::asio;
namespace beast = boost::beast;
namespace web = beast::websocket;
namespace sys = boost::system;

namespace boost::system {
[[nodiscard]] inline auto format_as(const error_code& error) -> std::string // NOLINT(readability-identifier-naming)
{
    return error.message();
}
#pragma GCC diagnostic pop
} // namespace boost::system

namespace pref {

using SteadyTimer = net::steady_timer;
using Stream = web::stream<beast::tcp_stream>;
using CardName = std::string;
using Hand = std::set<CardName>;

template<typename T = void>
using Awaitable = net::awaitable<T>;

struct PlayerSession {
    using Id = std::uint64_t;

    Id id{};
    std::string playerId;
    std::string playerName;
};

struct Player {
    using Id = std::string;
    using Name = std::string;

    Player() = default;
    Player(Id aId, Name aName, PlayerSession::Id aSessionId, const std::shared_ptr<Stream>& aWs);

    Id id;
    Name name;
    PlayerSession::Id sessionId{};
    std::shared_ptr<Stream> ws;
    std::optional<SteadyTimer> reconnectTimer;
    Hand hand;
    std::string bid;
    std::string whistingChoice;
    int tricksTaken{};
};

struct PlayedCard {
    Player::Id playerId;
    CardName name;
};

struct Context {
    using Players = std::map<Player::Id, Player>;

    [[nodiscard]] auto whoseTurnId() const -> const Player::Id&;
    [[nodiscard]] auto player(const Player::Id& playerId) const -> Player&;
    [[nodiscard]] auto playerName(const Player::Id& playerId) const -> const std::string&;
    [[nodiscard]] auto isWhistingDone() const -> bool;

    mutable Players players;
    Players::const_iterator whoseTurnIt;
    std::vector<CardName> talon;
    std::vector<PlayedCard> trick;
    std::string trump;
    Player::Id forehandId;
};

constexpr auto Detached = [](const std::string_view func) { // NOLINT(fuchsia-statically-constructed-objects)
    return [func](const std::exception_ptr& eptr) {
        if (not eptr) {
            return;
        }
        try {
            std::rethrow_exception(eptr);
        } catch (const sys::system_error& error) {
            if (error.code() != net::error::operation_aborted) {
                WARN("[{}][Detached] error: {}", func, error);
            }
        } catch (const std::exception& error) {
            WARN("[{}][Detached] error: {}", func, error);
        } catch (...) {
            WARN("[{}][Detached] error: unknown", func);
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

[[nodiscard]] auto finishTrick(const std::vector<PlayedCard>& trick, std::string_view trump) -> Player::Id;

auto acceptConnectionAndLaunchSession(Context& ctx, net::ip::tcp::endpoint endpoint) -> Awaitable<>;

}; // namespace pref
