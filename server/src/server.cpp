#include "proto/pref.pb.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <iostream>

namespace beast = boost::beast;
namespace asio = boost::asio;

namespace pref {
namespace {

using TcpAcceptor = asio::deferred_t::as_default_on_t<asio::ip::tcp::acceptor>;

using Stream = beast::websocket::stream<typename beast::tcp_stream::rebind_executor<
    typename asio::as_tuple_t<asio::deferred_t>::executor_with_default<asio::any_io_executor>>::other>;

template<typename T = void>
using Awaitable = asio::awaitable<T>;

const auto detached = [](const std::string_view func) {
    return [func](const std::exception_ptr& eptr) {
        if (not eptr) {
            return;
        }
        try {
            std::rethrow_exception(eptr);
        } catch (const boost::system::system_error& error) {
            if (error.code() != boost::asio::error::operation_aborted) {
                SPDLOG_WARN("[{}][detached] error: {}", func, error.what());
            }
        } catch (const std::exception& error) {
            SPDLOG_WARN("[{}][detached] error: {}", func, error.what());
        } catch (...) {
            SPDLOG_WARN("[{}][detached] error: unknown", func);
        }
    };
};
/*
[[nodiscard]] auto makeHelloRequest(const std::string& payload) -> pref::HelloRequest
{
    auto result = pref::HelloRequest{};
    result.ParseFromString(payload);
    return result;
}

[[nodiscard]] auto makeHelloResponse(const std::string& name) -> pref::HelloResponse
{
    auto result = pref::HelloResponse{};
    result.set_message(name);
    return result;
}
*/
[[nodiscard]] auto makeRpcEnvelope(const beast::flat_buffer& buffer) -> pref::RpcEnvelope
{
    pref::RpcEnvelope result;
    const auto success = result.ParseFromArray(buffer.data().data(), static_cast<int>(buffer.size()));
    if (!success) {
        SPDLOG_WARN("ParseFromArray failed in makeRpcEnvelope");
    }
    return result;
}
/*
[[nodiscard]] auto makeRpcEnvelope(const pref::HelloResponse& resp) -> pref::RpcEnvelope
{
    auto result = pref::RpcEnvelope{};
    result.set_method("SayHelloResponse");
    result.set_payload(resp.SerializeAsString());
    return result;
}

[[nodiscard]] auto sayHello(const pref::RpcEnvelope& env) -> pref::RpcEnvelope
{
    const auto helloRequest = makeHelloRequest(env.payload());
    const auto helloResponse = makeHelloResponse("Hello, " + helloRequest.name());
    SPDLOG_INFO("name: {}", helloRequest.name());
    return makeRpcEnvelope(helloResponse);
}
*/

struct PlayerInfo {
    std::string id;
    std::string name;
    std::shared_ptr<Stream> ws;
};

struct GameState {
    std::vector<PlayerInfo> players;
    std::size_t nextPlayerId{};
};

auto launchSession(std::shared_ptr<Stream> wsPtr, GameState& gameState) -> Awaitable<>
{
    SPDLOG_INFO("New session started");

    auto& ws = *wsPtr;
    ws.set_option(beast::websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws.set_option(beast::websocket::stream_base::decorator([](beast::websocket::response_type& res) {
        res.set(beast::http::field::server, std::string(BOOST_BEAST_VERSION_STRING) + " preferans-server");
    }));

    {
        auto [ec] = co_await ws.async_accept();
        if (ec) {
            SPDLOG_WARN("WebSocket accept failed: {}", ec.message());
            co_return;
        }
    }
    std::optional<std::string> playerId;
    std::string playerName;

    while (true) {
        beast::flat_buffer buffer;
        {
            auto [ec, _] = co_await ws.async_read(buffer);
            if (ec) {
                if (ec == beast::websocket::error::closed) {
                    SPDLOG_INFO("Connection closed normally: {}", ec.message());
                } else {
                    SPDLOG_WARN("WebSocket read error: {}", ec.message());
                }
                break;
            }
        }

        const auto env = makeRpcEnvelope(buffer);

        SPDLOG_INFO("method: {}", env.method());

        if (env.method() == "JoinRequest") {
            pref::JoinRequest req;
            if (!req.ParseFromString(env.payload())) {
                SPDLOG_WARN("Invalid JoinRequest payload");
                continue;
            }

            playerName = req.player_name();
            playerId = fmt::format("p{}", gameState.nextPlayerId++);

            SPDLOG_INFO("New player joined: {} ({})", playerName, *playerId);

            gameState.players.push_back(PlayerInfo{.id = *playerId, .name = playerName, .ws = wsPtr});

            // Build JoinResponse
            pref::JoinResponse joinResp;
            joinResp.set_player_id(*playerId);

            for (const auto& p : gameState.players) {
                auto* playerEntry = joinResp.add_players();
                playerEntry->set_player_id(p.id);
                playerEntry->set_player_name(p.name);
            }

            pref::RpcEnvelope responseEnv;
            responseEnv.set_method("JoinResponse");
            responseEnv.set_payload(joinResp.SerializeAsString());

            auto data = responseEnv.SerializeAsString();
            auto buf = asio::buffer(data);

            for (const auto& p : gameState.players) {
                p.ws->binary(true);
                auto [ec, _] = co_await p.ws->async_write(buf);
                if (ec) {
                    SPDLOG_WARN("Failed to send JoinResponse to {}: {}", p.name, ec.message());
                }
            }
        }
    }

    if (playerId) {
        SPDLOG_INFO("Cleaning up player: {}", *playerId);

        gameState.players.erase(
            std::remove_if(
                gameState.players.begin(),
                gameState.players.end(),
                [&](const PlayerInfo& p) { return p.id == *playerId; }),
            gameState.players.end());

        pref::PlayerLeft leftMsg;
        leftMsg.set_player_id(*playerId);

        pref::RpcEnvelope env;
        env.set_method("PlayerLeft");
        env.set_payload(leftMsg.SerializeAsString());

        auto data = env.SerializeAsString();
        auto buf = asio::buffer(data);

        for (const auto& p : gameState.players) {
            p.ws->binary(true);
            auto [ec, _] = co_await p.ws->async_write(buf);
            if (ec) {
                SPDLOG_WARN("Failed to notify {} about player left: {}", p.name, ec.message());
            }
        }
    }

    co_return;
}

auto acceptConnectionAndLaunchSession(const asio::ip::tcp::endpoint endpoint, GameState& gameState) -> Awaitable<>
{
    SPDLOG_INFO("");
    auto executor = co_await asio::this_coro::executor;
    auto acceptor = TcpAcceptor{executor};
    acceptor.open(endpoint.protocol());
    acceptor.set_option(asio::socket_base::reuse_address(true));
    acceptor.bind(endpoint);
    acceptor.listen(asio::socket_base::max_listen_connections); // 4096

    while (true) {
        // co_spawn is needed here instead of co_await, to not block the new connections
        asio::co_spawn(
            executor,
            launchSession(std::make_shared<Stream>(co_await acceptor.async_accept()), gameState),
            detached("launchSession"));
    }
}

} // namespace
} // namespace pref

int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::cerr << "Usage: preferans-server <address> <port>\n"
                  << "Example:\n"
                  << "    preferans-server 0.0.0.0 8080\n";
        return EXIT_FAILURE;
    }
    spdlog::set_pattern("[%^%l%$][%!] %v");

    auto const address = asio::ip::make_address(argv[1]);
    auto const port = static_cast<unsigned short>(std::atoi(argv[2]));

    auto loop = asio::io_context{};
    auto gameState = pref::GameState{};
    asio::co_spawn(
        loop,
        pref::acceptConnectionAndLaunchSession({address, port}, gameState),
        pref::detached("acceptConnectionAndLaunchSession"));
    loop.run();

    return EXIT_SUCCESS;
}
