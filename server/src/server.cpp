#include "proto/pref.pb.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <cstdlib>
#include <iostream>

namespace beast = boost::beast;
namespace asio = boost::asio;

namespace pref {
namespace {

using TcpAcceptor = asio::use_awaitable_t<>::as_default_on_t<asio::ip::tcp::acceptor>;

using Stream = beast::websocket::stream<typename beast::tcp_stream::rebind_executor<
    typename asio::use_awaitable_t<>::executor_with_default<asio::any_io_executor>>::other>;

template<typename T = void>
using Awaitable = asio::awaitable<T>;

const auto detached = [](const std::exception_ptr& eptr) {
    if (not eptr) {
        return;
    }
    try {
        std::rethrow_exception(eptr);
    } catch (std::exception& error) {
        std::cerr << "[detached] error: " << error.what() << '\n';
    }
};

auto launchSession(Stream ws) -> Awaitable<>
{
    std::cout << '[' << __func__ << "]\n";
    ws.set_option(beast::websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws.set_option(beast::websocket::stream_base::decorator([](beast::websocket::response_type& res) {
        res.set(beast::http::field::server, std::string(BOOST_BEAST_VERSION_STRING) + " preferans-server");
    }));

    co_await ws.async_accept();

    while (true) {
        try {
            auto buffer = beast::flat_buffer{};
            co_await ws.async_read(buffer);

            pref::RpcEnvelope env;
            env.ParseFromArray(buffer.data().data(), google::protobuf::internal::ToIntSize(buffer.size()));

            std::cout << "method: " << env.method() << '\n';

            if (env.method() == "SayHello") {
                pref::HelloRequest req;
                req.ParseFromString(env.payload());

                std::cout << "name: " << req.name() << '\n';

                pref::HelloResponse resp;
                resp.set_message("Hello, " + req.name());

                std::string respPayload;
                resp.SerializeToString(&respPayload);

                pref::RpcEnvelope responseEnv;
                responseEnv.set_method("SayHelloResponse");
                responseEnv.set_payload(respPayload);

                std::string outData;
                responseEnv.SerializeToString(&outData);

                ws.binary(true);
                co_await ws.async_write(asio::buffer(outData));
            }
        } catch (const boost::system::system_error& error) {
            if (error.code() != beast::websocket::error::closed)
                throw;
        }
    }
}

auto acceptConnectionAndLaunchSession(const asio::ip::tcp::endpoint endpoint) -> Awaitable<>
{
    std::cout << '[' << __func__ << "]\n";
    auto acceptor = TcpAcceptor{co_await asio::this_coro::executor};
    acceptor.open(endpoint.protocol());
    acceptor.set_option(asio::socket_base::reuse_address(true));
    acceptor.bind(endpoint);
    acceptor.listen(asio::socket_base::max_listen_connections); // 4096

    while (true) {
        try {
            co_await launchSession(Stream{co_await acceptor.async_accept()});
        } catch (const std::exception& error) {
            std::cerr << '[' << __func__ << "] error: " << error.what() << '\n';
        } catch (...) {
            std::cerr << '[' << __func__ << "] error: unknown\n";
        }
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
    auto const address = asio::ip::make_address(argv[1]);
    auto const port = static_cast<unsigned short>(std::atoi(argv[2]));

    auto loop = asio::io_context{};
    asio::co_spawn(loop, pref::acceptConnectionAndLaunchSession({address, port}), pref::detached);
    loop.run();

    return EXIT_SUCCESS;
}
