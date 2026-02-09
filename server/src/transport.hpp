// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2025 Oleksandr Kozlov

#pragma once

#include "common/common.hpp"
#include "common/logger.hpp"

#include <asioexec/use_sender.hpp>
#include <boost/asio.hpp>
#include <boost/asio/experimental/channel.hpp> // IWYU pragma: keep
#include <boost/beast.hpp>
#include <boost/system.hpp>
#include <exec/repeat_effect_until.hpp>
#include <exec/task.hpp>
#include <exec/variant_sender.hpp>
#include <stdexec/execution.hpp>

#ifdef PREF_SSL
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/websocket/ssl.hpp>
#endif // PREF_SSL

#include <cassert>
#include <array>
#include <chrono>
#include <coroutine>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace web = beast::websocket;
namespace sys = boost::system;
namespace ex = exec;
namespace stdx = stdexec;
namespace netx = asioexec;
using tcp = net::ip::tcp;

namespace boost::system {
// NOLINTNEXTLINE(readability-identifier-naming)
[[maybe_unused]] inline auto format_as(const error_code& error) -> std::string
{
    return error.message();
}
} // namespace boost::system

namespace pref {

#ifdef PREF_SSL
using Stream = web::stream<net::ssl::stream<beast::tcp_stream>>;
#else // PREF_SSL
using Stream = netx::use_sender_t::as_default_on_t<web::stream<beast::tcp_stream>>;
#endif // PREF_SSL

using Channel = netx::use_sender_t::as_default_on_t<net::experimental::channel<void(sys::error_code, std::string)>>;
using ChannelPtr = std::shared_ptr<Channel>;
using SteadyTimer = net::as_tuple_t<netx::use_sender_t>::as_default_on_t<net::steady_timer>;
using Acceptor = netx::use_sender_t::as_default_on_t<tcp::acceptor>;
using SignalSet = net::as_tuple_t<netx::use_sender_t>::as_default_on_t<net::signal_set>;

template<class T = void>
using task = ex::task<T>; // NOLINT(readability-identifier-naming)

inline constexpr auto PrintError = [](const std::string_view func, const std::exception_ptr& eptr) {
    const auto shouldSuppress = [](const std::string_view message) {
        static constexpr auto suppressed = std::to_array<std::string_view>(
            {"Connection reset by peer",
             "The WebSocket handshake",
             "bad method",
             "bad target",
             "bad version",
             "http request",
             "no shared cipher",
             "packet length too long",
             "ssl/tls alert handshake failure",
             "stream truncated",
             "unexpected body",
             "unexpected message",
             "unexpected record",
             "unknown protocol",
             "unsupported protocol",
             "version too low",
             "wrong version number"});
        return rng::any_of(suppressed, [message](const std::string_view text) { return message.contains(text); });
    };
    const auto logErr = [func, shouldSuppress](const auto& error) {
        if (not shouldSuppress(error)) { PREF_W("[{}] {}", func, PREF_V(error)); };
    };
    if (not eptr) {
        logErr("success");
        return;
    }
    try {
        std::rethrow_exception(eptr);
    } catch (const sys::system_error& error) {
        if (const auto code = error.code(); code == net::experimental::error::channel_cancelled
            or code == web::error::closed
            or code == net::error::operation_aborted) {
            PREF_I("[{}] {}", func, code.message());
            return;
        }
        logErr(error.code().message());
    } catch (const std::exception& error) {
        logErr(error.what());
    } catch (...) {
        logErr("unknown");
    }
};

inline constexpr auto Detached = [](const std::string_view func) {
    return [func](const std::exception_ptr& eptr) { return PrintError(func, eptr); };
};

#ifdef PREF_SSL
[[nodiscard]] inline auto loadCertificate(const fs::path& cert, const fs::path& key, const fs::path& dh)
    -> net::ssl::context
{
    auto ssl = net::ssl::context{net::ssl::context::tlsv12};
    ssl.set_options(
        net::ssl::context::default_workarounds | net::ssl::context::no_sslv2 | net::ssl::context::single_dh_use);
    ssl.use_certificate_chain_file(cert);
    ssl.use_private_key_file(key, net::ssl::context::file_format::pem);
    ssl.use_tmp_dh_file(dh);
    return ssl;
}
#endif // PREF_SSL

template<typename Rep, typename Period>
auto sleepFor(const std::chrono::duration<Rep, Period> duration, net::any_io_executor ex) -> task<>
{
    co_await SteadyTimer{ex, duration}.async_wait();
}

inline auto sendToOne(const ChannelPtr& ch, std::string payload) -> task<>
{
    assert(ch);
    if (const auto [error] = co_await ch->async_send({}, std::move(payload), net::as_tuple); error) { PREF_DW(error); };
}

inline auto sendToMany(const std::span<const ChannelPtr> channels, std::string payload) -> task<>
{
    for (const auto& ch : channels) { co_await sendToOne(ch, payload); }
}

struct Connection {
    Connection() = default;
    // NOLINTNEXTLINE(modernize-pass-by-value)
    explicit Connection(const ChannelPtr& channel)
        : ch{channel}
    {
    }

    auto closeStream() -> task<>
    {
        PREF_I();
        assert(ch->is_open());
        co_await sendToOne(ch, std::string{"\0", 1}.append("Another tab connected"));
    }

    auto cancelReconnectTimer() -> void
    {
        PREF_I();
        assert(reconnectTimer);
        reconnectTimer->cancel();
    }

    auto replaceChannel(const ChannelPtr& channel) -> void
    {
        PREF_I();
        ch = channel;
    }

    ChannelPtr ch;
    std::optional<SteadyTimer> reconnectTimer;
};

inline auto sendOrClose(Stream& ws, std::string payload) -> task<bool>
{
    assert(not std::empty(payload));
    if (payload.front() == '\0') {
        if (ws.is_open()) {
            co_await ws.async_close({web::close_code::policy_error, payload.substr(1)}, netx::use_sender);
        }
        co_return true;
    }
    assert(ws.is_open());
    co_await ws.async_write(net::buffer(payload), netx::use_sender);
    co_return false;
}

inline auto payloadSender(Stream& ws, Channel& ch) -> stdx::sender auto
{
    PREF_I();
    return ex::repeat_effect_until(
        ch.async_receive()
        | stdx::let_value([&ws](std::string payload) { return sendOrClose(ws, std::move(payload)); })
        | stdx::upon_error([](const std::exception_ptr& error) {
              PrintError("payloadSender", error);
              return true;
          }));
}

} // namespace pref
