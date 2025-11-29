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
#include <exec/when_any.hpp>
#include <execpools/asio/asio_thread_pool.hpp>
#include <spdlog/spdlog.h>
#include <stdexec/execution.hpp>

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

auto handleSignals() -> task<>
{
    auto signals = SignalSet{ctx().ex, SIGINT, SIGTERM};
    const auto [result, signal] = co_await signals.async_wait();
    PREF_I("{}, signal: {} ({})", PREF_V(result), signal == SIGINT ? "SIGINT" : "SIGTERM", signal);
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
        spdlog::set_pattern("[%^%l%$][%t][%!] %v");
        auto pool = execpools::asio_thread_pool{1};
        auto ex = pool.get_executor();
        auto& ctx = pref::ctx(ex);
        if (args.contains("<data>") and args.at("<data>").isString()) {
            ctx.gameDataPath = args.at("<data>").asString();
            ctx.gameData = pref::loadGameData(ctx.gameDataPath);
        } else {
            PREF_W("game data is not provided");
        }
        ctx.gameId = pref::lastGameId(ctx.gameData);
#ifdef PREF_SSL
        auto accept = pref::createAcceptor(
            pref::loadCertificate(
                args.at("--cert").asString(), args.at("--key").asString(), args.at("--dh").asString()),
            {address, port},
            std::move(ex));
#else // PREF_SSL
        auto accept = pref::createAcceptor({address, port}, std::move(ex));
#endif // PREF_SSL
        auto sch = pool.get_scheduler();
        stdx::sync_wait(
            ex::when_any(
                stdx::starts_on(sch, std::move(accept)), //
                stdx::starts_on(sch, pref::handleSignals())));
        PREF_I("shutdown");
        ctx.shutdown();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        PREF_DE(error);
    } catch (...) {
        PREF_E("error: unknown");
    }
    return EXIT_FAILURE;
}
