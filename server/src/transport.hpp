// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2025 Oleksandr Kozlov

#pragma once

#include "common/common.hpp"
#include "common/logger.hpp"

#include <boost/asio.hpp>
#include <boost/asio/experimental/channel.hpp> // IWYU pragma: keep
#include <boost/beast.hpp>
#include <boost/system.hpp>

#ifdef PREF_SSL
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/websocket/ssl.hpp>
#endif // PREF_SSL

#include <cassert>
#include <chrono>
#include <coroutine>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace web = beast::websocket;
namespace sys = boost::system;

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
using Stream = web::stream<beast::tcp_stream>;
#endif // PREF_SSL

using Channel = net::experimental::channel<void(sys::error_code, std::string)>;
using ChannelPtr = std::shared_ptr<Channel>;
using SteadyTimer = net::steady_timer;

template<typename T = void>
using Awaitable = net::awaitable<T>;

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
auto sleepFor(const std::chrono::duration<Rep, Period> duration) -> Awaitable<>
{
    co_await SteadyTimer{co_await net::this_coro::executor, duration}.async_wait(net::as_tuple);
}

inline auto sendToOne(const ChannelPtr& ch, std::string payload) -> Awaitable<>
{
    assert(ch);
    if (const auto [error] = co_await ch->async_send({}, std::move(payload), net::as_tuple); error) { PREF_DW(error); };
}

inline auto sendToMany(const std::span<const ChannelPtr> channels, std::string payload) -> Awaitable<>
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

    auto closeStream() -> Awaitable<>
    {
        if (ch->is_open()) { co_await sendToOne(ch, std::string{"\0", 1}.append("Another tab connected")); }
    }

    auto cancelReconnectTimer() -> void
    {
        if (reconnectTimer) { reconnectTimer->cancel(); }
    }

    auto replaceChannel(const ChannelPtr& channel) -> void
    {
        ch = channel;
    }

    ChannelPtr ch;
    std::optional<SteadyTimer> reconnectTimer;
};

inline auto payloadSender(Stream& ws, ChannelPtr ch) -> Awaitable<>
{
    while (true) {
        auto [receiveError, data] = co_await ch->async_receive(net::as_tuple);
        const auto payload = std::string{std::move(data)};
        if (receiveError) {
            PREF_DW(receiveError);
            co_return;
        };
        assert(ws.is_open());
        if (not std::empty(payload) and payload.front() == '\0') {
            if (const auto [closeError] = co_await ws.async_close(
                    web::close_reason(web::close_code::policy_error, std::string_view{payload}.substr(1)),
                    net::as_tuple);
                closeError) {
                PREF_DW(closeError);
            }
            co_return;
        }
        if (const auto [writeError, size] = co_await ws.async_write(net::buffer(payload), net::as_tuple); writeError) {
            PREF_DW(writeError);
            co_return;
        }
    }
}

} // namespace pref
