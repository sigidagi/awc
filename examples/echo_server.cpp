// Minimal coroutine based WebSocket echo server, handy for trying the client
// examples locally:   ./echo_server 9001
//
// It echoes every text/binary message and answers pings automatically (Beast
// does that during reads). Press Ctrl+C to stop it; restart it to watch the
// reconnecting_client example recover.

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <spdlog/spdlog.h>

#include <cstdint>
#include <string>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

namespace {

constexpr auto use_nothrow = asio::as_tuple(asio::use_awaitable);

asio::awaitable<void> serve(tcp::socket socket) {
    websocket::stream<beast::tcp_stream> ws(std::move(socket));
    ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

    auto [accept_ec] = co_await ws.async_accept(use_nothrow);
    if (accept_ec) {
        spdlog::warn("handshake failed: {}", accept_ec.message());
        co_return;
    }
    spdlog::info("client connected");

    beast::flat_buffer buffer;
    for (;;) {
        auto [ec, bytes] = co_await ws.async_read(buffer, use_nothrow);
        if (ec) {
            spdlog::info("client gone: {}", ec.message());
            co_return;
        }
        spdlog::info("echoing {} bytes", bytes);
        ws.text(ws.got_text());
        auto [write_ec, written] = co_await ws.async_write(buffer.data(), use_nothrow);
        buffer.clear();
        if (write_ec) {
            co_return;
        }
    }
}

asio::awaitable<void> listen(tcp::endpoint endpoint) {
    auto executor = co_await asio::this_coro::executor;
    tcp::acceptor acceptor(executor, endpoint);
    spdlog::info("listening on ws://{}:{}/", endpoint.address().to_string(), endpoint.port());
    for (;;) {
        auto [ec, socket] = co_await acceptor.async_accept(use_nothrow);
        if (ec) {
            co_return;
        }
        asio::co_spawn(executor, serve(std::move(socket)), asio::detached);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::stoul(argv[1]) : 9001UL);

    asio::io_context ioc;
    asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code&, int) {
        spdlog::info("shutting down");
        ioc.stop();
    });

    asio::co_spawn(ioc, listen(tcp::endpoint{asio::ip::make_address("127.0.0.1"), port}), asio::detached);
    ioc.run();
    return 0;
}
