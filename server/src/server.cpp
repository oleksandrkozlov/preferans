#include "common/logger.hpp"
#include "proto/pref.pb.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <docopt/docopt.h>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include <range/v3/all.hpp>

#include <array>
#include <cstdlib>
#include <exception>
#include <gsl/gsl>
#include <iterator>
#include <memory>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace web = beast::websocket;

using namespace std::literals;

namespace boost::system {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunneeded-internal-declaration"
[[nodiscard]] static auto format_as(const error_code& error) -> std::string // NOLINT(readability-identifier-naming)
{
    return error.message();
}
#pragma GCC diagnostic pop
} // namespace boost::system

namespace pref {
namespace {

// NOLINTEND(performance-unnecessary-value-param, cppcoreguidelines-avoid-reference-coroutine-parameters)

constexpr auto numberOfPlayers = 3UZ;

using SteadyTimer = net::steady_timer;
using TcpAcceptor = net::ip::tcp::acceptor;
using Stream = web::stream<beast::tcp_stream>;

template<typename T = void>
using Awaitable = net::awaitable<T>;

template<typename Callable>
[[nodiscard]] auto unpack(Callable callable)
{
    return [cb = std::move(callable)](const auto& pair) { return cb(pair.first, pair.second); };
}
constexpr auto detached = [](const std::string_view func) { // NOLINT(fuchsia-statically-constructed-objects)
    return [func](const std::exception_ptr& eptr) {
        if (not eptr) {
            return;
        }
        try {
            std::rethrow_exception(eptr);
        } catch (const boost::system::system_error& error) {
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

[[nodiscard]] auto makeMessage(const beast::flat_buffer& buffer) -> std::optional<Message>
{
    auto result = Message{};
    if (not result.ParseFromArray(buffer.data().data(), gsl::narrow<int>(buffer.size()))) {
        WARN("error: failed to make Message");
        return {};
    }
    return result;
}

struct PlayerSession {
    using Id = std::uint64_t;

    Id id{};
    std::string playerId;
    std::string playerName;
};

struct Player {
    using Id = std::string;
    using Name = std::string;

    Id id;
    Name name;
    PlayerSession::Id sessionId{};
    std::shared_ptr<Stream> stream;
    std::optional<SteadyTimer> reconnectTimer;
};

[[maybe_unused]] auto generateUuid() -> Player::Id
{
    return boost::uuids::to_string(std::invoke(boost::uuids::random_generator{}));
}

struct GameState {
    using Players = std::map<Player::Id, Player>;
    Players players;
};

constexpr auto getPlayerId = &GameState::Players::value_type::first;

[[nodiscard]] auto isNewPlayer(const Player::Id& playerId, GameState::Players& players) -> bool
{
    return std::empty(playerId) or not players.contains(playerId);
}

auto sendToOne(const Message& msg, const std::shared_ptr<Stream>& stream) -> Awaitable<boost::system::error_code>
{
    if (not stream->is_open()) {
        co_return net::error::not_connected;
    }
    const auto data = msg.SerializeAsString();
    const auto buf = net::buffer(data);
    const auto [error, _] = co_await stream->async_write(buf, net::as_tuple);
    co_return error;
}

auto sendToMany(const Message& msg, const std::vector<std::shared_ptr<Stream>>& streams) -> Awaitable<>
{
    for (const auto& stream : streams) {
        if (const auto error = co_await sendToOne(msg, stream)) {
            WARN("{}: failed to send message", VAR(error));
        }
    }
}

auto sendToAll(const Message& msg, const GameState::Players& players) -> Awaitable<>
{
    const auto streams = players //
        | ranges::views::values //
        | ranges::views::transform(&Player::stream) //
        | ranges::to_vector; //
    co_await sendToMany(msg, streams);
}

auto sendToAllExcept(const Message& msg, const GameState::Players& players, const Player::Id& excludedId) -> Awaitable<>
{
    const auto streams = players //
        | ranges::views::filter(std::bind_front(std::not_equal_to{}, excludedId), getPlayerId) //
        | ranges::views::values //
        | ranges::views::transform(&Player::stream) //
        | ranges::to_vector;
    co_await sendToMany(msg, streams);
}

auto joinPlayer(const std::shared_ptr<Stream>& stream, GameState::Players& players, PlayerSession& session) -> void
{
    session.playerId = generateUuid();
    INFO_VAR(session.playerId, session.playerName, session.id);
    players.emplace(
        session.playerId,
        Player{
            .id = session.playerId,
            .name = session.playerName,
            .sessionId = session.id,
            .stream = stream,
            .reconnectTimer = {}});
}

auto prepareNewSession(const Player::Id& playerId, GameState::Players& players, PlayerSession& session) -> Awaitable<>
{
    INFO_VAR(playerId, session.playerId, session.playerName, session.id);
    assert(players.contains(playerId));
    auto& player = players[playerId];
    session.id = ++player.sessionId;
    session.playerId = playerId;
    session.playerName = player.name; // keep the first connected player's name
    if (player.reconnectTimer) {
        player.reconnectTimer->cancel();
    }
    if (player.stream->is_open()) {
        if (const auto [error] = co_await player.stream->async_close(
                web::close_reason(web::close_code::policy_error, "Another tab connected"), net::as_tuple);
            error) {
            WARN("{}: failed to close stream", VAR(error));
        }
    }
}

auto replaceStream(const Player::Id& playerId, GameState::Players& players, const std::shared_ptr<Stream>& stream)
    -> void
{
    assert(players.contains(playerId));
    auto& player = players[playerId];
    player.stream = stream;
}

auto reconnectPlayer(
    const std::shared_ptr<Stream>& stream,
    const Player::Id& playerId,
    GameState::Players& players,
    PlayerSession& session) -> Awaitable<>
{
    co_await prepareNewSession(playerId, players, session);
    replaceStream(playerId, players, stream);
    INFO_VAR(session.playerName, session.playerId, session.id);
}

auto handleJoinRequest(Message msg, const std::shared_ptr<Stream> stream, GameState& state) -> Awaitable<PlayerSession>
{
    auto joinRequest = JoinRequest{};
    if (not joinRequest.ParseFromString(msg.payload())) {
        WARN("error: failed to parse JoinRequest payload");
        co_return PlayerSession{};
    }
    auto session = PlayerSession{};
    const auto& playerId = joinRequest.player_id();
    session.playerName = joinRequest.player_name();
    const auto address = stream->next_layer().socket().remote_endpoint().address().to_string();
    INFO_VAR(playerId, session.playerName, address);

    if (isNewPlayer(playerId, state.players)) {
        joinPlayer(stream, state.players, session);
    } else {
        co_await reconnectPlayer(stream, playerId, state.players, session);
    }

    auto joinResponse = JoinResponse{};
    joinResponse.set_player_id(session.playerId);

    for (const auto& [id, player] : state.players) {
        auto p = joinResponse.add_players();
        p->set_player_id(id);
        p->set_player_name(player.name);
    }
    msg = Message{};
    msg.set_method("JoinResponse");
    msg.set_payload(joinResponse.SerializeAsString());
    if (const auto error = co_await sendToOne(msg, stream)) {
        WARN("{}: failed to send JoinResponse", VAR(error));
    }
    if (playerId != session.playerId) {
        auto playerJoined = PlayerJoined{};
        playerJoined.set_player_id(session.playerId);
        playerJoined.set_player_name(session.playerName);
        msg = Message{};
        msg.set_method("PlayerJoined");
        msg.set_payload(playerJoined.SerializeAsString());
        co_await sendToAllExcept(msg, state.players, session.playerId);
    }
    co_return session;
}

auto handleDisconnect(const Player::Id playerId, GameState& state) -> Awaitable<>
{
    INFO_VAR(playerId);
    assert(state.players.contains(playerId) and "player exists");
    auto& player = state.players[playerId];
    if (not player.reconnectTimer) {
        player.reconnectTimer.emplace(co_await net::this_coro::executor);
    }
    player.reconnectTimer->expires_after(10s);
    if (const auto [error] = co_await player.reconnectTimer->async_wait(net::as_tuple); error) {
        if (error != net::error::operation_aborted) {
            WARN_VAR(error);
        }
        co_return;
    }
    assert(state.players.contains(playerId) and "player exists");
    INFO("removed {} after timeout", VAR(playerId));
    state.players.erase(playerId);
    auto playerLeft = PlayerLeft{};
    playerLeft.set_player_id(playerId);
    auto msg = Message{};
    msg.set_method("PlayerLeft");
    msg.set_payload(playerLeft.SerializeAsString());
    co_await sendToAll(msg, state.players);
}

auto dealCards(GameState& state) -> Awaitable<>
{
    const auto suits = std::vector<std::string>{"spades", "diamonds", "clubs", "hearts"};
    const auto ranks = std::vector<std::string>{"7", "8", "9", "10", "jack", "queen", "king", "ace"};
    const auto toCard = [](const auto& card) {
        const auto& [rank, suit] = card;
        return fmt::format("{}_of_{}", rank, suit);
    };
    const auto deck = ranges::views::cartesian_product(ranks, suits) //
        | ranges::views::transform(toCard) //
        | ranges::to_vector //
        | ranges::actions::shuffle(std::mt19937{std::invoke(std::random_device{})});
    const auto chunks = deck | ranges::views::chunk(10);
    const auto hands = chunks | ranges::views::take(numberOfPlayers) | ranges::to_vector;
    [[maybe_unused]] const auto talon = chunks //
        | ranges::views::drop(numberOfPlayers) //
        | ranges::views::join //
        | ranges::to_vector;
    INFO_VAR(talon, hands);
    const auto streams = state.players //
        | ranges::views::values //
        | ranges::views::transform(&Player::stream) //
        | ranges::to_vector;
    assert((std::size(streams) == numberOfPlayers) and (std::size(hands) == numberOfPlayers));
    for (const auto& [stream, hand] : ranges::views::zip(streams, hands)) {
        auto dealCards = DealCards{};
        for (const auto& card : hand) {
            *dealCards.add_cards() = card;
        }
        auto msg = Message{};
        msg.set_method("DealCards");
        msg.set_payload(dealCards.SerializeAsString());
        co_await sendToOne(msg, stream);
    }
}

auto launchSession(const std::shared_ptr<Stream> stream, GameState& state) -> Awaitable<>
{
    INFO();

    // TODO: What if we received a text?
    stream->binary(true);
    stream->set_option(web::stream_base::timeout::suggested(beast::role_type::server));
    stream->set_option(web::stream_base::decorator([](web::response_type& res) {
        res.set(beast::http::field::server, std::string{BOOST_BEAST_VERSION_STRING} + " preferans-server");
    }));

    co_await stream->async_accept();
    auto session = PlayerSession{};

    while (true) {
        auto buffer = beast::flat_buffer{};
        if (const auto [error, _] = co_await stream->async_read(buffer, net::as_tuple); error) {
            if (error == web::error::closed //
                or error == net::error::not_connected //
                or error == net::error::connection_reset //
                or error == net::error::eof) {
                INFO("{}: disconnected: {}, {}", VAR(error), VAR(session.playerName), VAR(session.playerId));
            } else if (error == net::error::operation_aborted) {
                WARN("{}: read: {}, {}", VAR(error), VAR(session.playerName), VAR(session.playerId));
            } else {
                ERROR_VAR(session.playerName, session.playerId, error);
                // TODO(olkozlo): maybe throw to not handle a disconnection?
            }
            break;
        }

        auto maybeMsg = makeMessage(buffer);
        if (not maybeMsg) {
            continue;
        }
        const auto& msg = *maybeMsg;

        if (msg.method() == "JoinRequest") {
            session = co_await handleJoinRequest(msg, stream, state);
            if (std::size(state.players) == numberOfPlayers) {
                co_await dealCards(state);
            }
        } else {
            WARN("error: unknown method: {}", msg.method());
            continue;
        }
    }
    if (std::empty(session.playerId) or not state.players.contains(session.playerId)) {
        // TODO: Can `playerId` ever be set but not present in `players`? If not, replace with `assert()`.
        WARN("error: empty or unknown {}", VAR(session.playerId));
        co_return;
    }
    auto& player = state.players[session.playerId];
    if (session.id != player.sessionId) {
        INFO("{} reconnected with {} => {}", VAR(session.playerId), VAR(session.id), player.sessionId);
        co_return;
    }
    WARN("disconnected {}, waiting for reconnection", VAR(session.playerId));
    net::co_spawn(
        co_await net::this_coro::executor, handleDisconnect(session.playerId, state), detached("handleDisconnect"));
}

auto acceptConnectionAndLaunchSession(const net::ip::tcp::endpoint endpoint, GameState& state) -> Awaitable<>
{
    INFO();
    const auto ex = co_await net::this_coro::executor;
    auto acceptor = TcpAcceptor{ex, endpoint};

    while (true) {
        net::co_spawn(
            ex,
            launchSession(std::make_shared<Stream>(co_await acceptor.async_accept()), state),
            detached("launchSession"));
    }
}

auto handleSignals() -> Awaitable<>
{
    const auto ex = co_await net::this_coro::executor;
    auto signals = net::signal_set{ex, SIGINT, SIGTERM};
    const auto signal = co_await signals.async_wait();
    INFO_VAR(signal);
    static_cast<net::io_context&>(ex.context()).stop(); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
}


constexpr auto usage = R"(
Usage:
    preferans-server <address> <port>

Options:
    -h --help     Show this screen.
)";

// NOLINTEND(performance-unnecessary-value-param, cppcoreguidelines-avoid-reference-coroutine-parameters)
} // namespace
} // namespace pref

int main(const int argc, const char* const argv[])
{
    try {
        const auto args = docopt::docopt(pref::usage, {std::next(argv), std::next(argv, argc)});
        spdlog::set_pattern("[%^%l%$][%!] %v");

        auto const address = net::ip::make_address(args.at("<address>").asString());
        auto const port = gsl::narrow<std::uint16_t>(args.at("<port>").asLong());

        auto loop = net::io_context{};
        auto state = pref::GameState{};
        net::co_spawn(loop, pref::handleSignals(), pref::detached("handleStop"));
        net::co_spawn(
            loop,
            pref::acceptConnectionAndLaunchSession({address, port}, state),
            pref::detached("acceptConnectionAndLaunchSession"));
        loop.run();

        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        ERROR("{}", VAR(error));
        return EXIT_FAILURE;
    }
}
