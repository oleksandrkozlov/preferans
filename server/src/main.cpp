// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2025 Oleksandr Kozlov

#include "common/logger.hpp"
#include "game_data.hpp"
#include "proto/pref.pb.h"
#include "server.hpp"
#include "transport.hpp"

#include <boost/asio.hpp>
#include <boost/system.hpp>
#include <docopt/docopt.h>
#include <spdlog/spdlog.h>

#include <coroutine>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <gsl/gsl>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace pref {
namespace {

#ifdef PREF_SSL
#define PREF_SSL_OPTS " --cert=<path> --key=<path> --dh=<path>"
#else // PREF_SSL
#define PREF_SSL_OPTS ""
#endif // PREF_SSL

auto handleSignals() -> Awaitable<>
{
    // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
    const auto ex = co_await net::this_coro::executor;
    auto signals = net::signal_set{ex, SIGINT, SIGTERM};
    const auto signal = co_await signals.async_wait();
    PREF_I("signal: {} ({})", signal == SIGINT ? "SIGINT" : "SIGTERM", signal);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    static_cast<net::io_context&>(ex.context()).stop();
}

constexpr auto Usage = R"(
Usage:
    server <address> <port> [<data>])" PREF_SSL_OPTS R"(

Options:
    -h --help     Show this screen.
)";

} // namespace
} // namespace pref

auto main(const int argc, const char* const argv[]) -> int
{
    try {
        const auto args = docopt::docopt(pref::Usage, {std::next(argv), std::next(argv, argc)});
        auto const address = net::ip::make_address(args.at("<address>").asString());
        auto const port = gsl::narrow<std::uint16_t>(args.at("<port>").asLong());
        auto& ctx = pref::ctx();
        spdlog::set_pattern("[%^%l%$][%!] %v");
        auto loop = net::io_context{};
        if (args.contains("<data>") and args.at("<data>").isString()) {
            ctx.gameDataPath = args.at("<data>").asString();
            ctx.gameData = pref::loadGameData(ctx.gameDataPath);
        } else {
            PREF_W("game data is not provided");
        }
        ctx.gameId = pref::lastGameId(ctx.gameData);
        net::co_spawn(loop, pref::handleSignals(), pref::Detached("handleStop"));
#ifdef PREF_SSL
        auto accept = pref::acceptConnectionAndLaunchSession(
            pref::loadCertificate(
                args.at("--cert").asString(), args.at("--key").asString(), args.at("--dh").asString()),
            {address, port});
#else // PREF_SSL
        auto accept = pref::acceptConnectionAndLaunchSession({address, port});
#endif // PREF_SSL
        net::co_spawn(loop, std::move(accept), pref::Detached("acceptConnectionAndLaunchSession"));
        loop.run();
        ctx.shutdown();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        PREF_DE(error);
    } catch (...) {
        PREF_E("error: uknown");
    }
    return EXIT_FAILURE;
}
