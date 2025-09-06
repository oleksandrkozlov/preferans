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

auto handleSignals() -> Awaitable<>
{
    const auto ex = co_await net::this_coro::executor;
    auto signals = net::signal_set{ex, SIGINT, SIGTERM};
    const auto signal = co_await signals.async_wait();
    INFO_VAR(signal);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    static_cast<net::io_context&>(ex.context()).stop();
}

constexpr auto usage = R"(
Usage:
    server <address> <port>

Options:
    -h --help     Show this screen.
)";

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
        auto ctx = pref::Context{};
        net::co_spawn(loop, pref::handleSignals(), pref::Detached("handleStop"));
        net::co_spawn(
            loop,
            pref::acceptConnectionAndLaunchSession(ctx, {address, port}),
            pref::Detached("acceptConnectionAndLaunchSession"));
        loop.run();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        PREF_ERROR("{}", VAR(error));
        return EXIT_FAILURE;
    }
}
