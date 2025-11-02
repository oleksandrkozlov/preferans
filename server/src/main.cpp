#include "game_data.hpp"
#include "server.hpp"

#include <boost/asio.hpp>
#include <docopt/docopt.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <gsl/gsl>
#include <iterator>

namespace pref {
namespace {

#ifdef PREF_SSL
#define SSL_OPTS " --cert=<path> --key=<path> --dh=<path>"
[[nodiscard]] auto loadCertificate(const fs::path& cert, const fs::path& key, const fs::path& dh) -> net::ssl::context
{
    auto ssl = net::ssl::context{net::ssl::context::tlsv12};
    ssl.set_options(
        net::ssl::context::default_workarounds | net::ssl::context::no_sslv2 | net::ssl::context::single_dh_use);
    ssl.use_certificate_chain_file(cert);
    ssl.use_private_key_file(key, net::ssl::context::file_format::pem);
    ssl.use_tmp_dh_file(dh);
    return ssl;
}
#else // PREF_SSL
#define SSL_OPTS ""
#endif // PREF_SSL

auto handleSignals() -> Awaitable<>
{
    const auto ex = co_await net::this_coro::executor;
    auto signals = net::signal_set{ex, SIGINT, SIGTERM};
    const auto signal = co_await signals.async_wait();
    PREF_DI(signal);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    static_cast<net::io_context&>(ex.context()).stop();
}

constexpr auto usage = R"(
Usage:
    server <address> <port> [<data>])" SSL_OPTS R"(

Options:
    -h --help     Show this screen.
)";

} // namespace
} // namespace pref

int main(const int argc, const char* const argv[])
{
    try {
        const auto args = docopt::docopt(pref::usage, {std::next(argv), std::next(argv, argc)});
        auto const address = net::ip::make_address(args.at("<address>").asString());
        auto const port = gsl::narrow<std::uint16_t>(args.at("<port>").asLong());
        auto& ctx = pref::ctx();
        spdlog::set_pattern("[%^%l%$][%!] %v");
        auto loop = net::io_context{};
        if (args.contains("<data>") and args.at("<data>").isString()) {
            ctx.gameDataPath = args.at("<data>").asString();
            ctx.gameData = pref::loadGameData(ctx.gameDataPath);
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
        net::co_spawn(loop, std::move(accept), pref::Detached("acceptConnectionAndLaunchSession")); // clang-format on
        loop.run();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        PREF_DE(error);
    } catch (...) {
        PREF_E("error: uknown");
    }
    return EXIT_FAILURE;
}
