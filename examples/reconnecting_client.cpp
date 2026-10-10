// A long-running client that survives server restarts:
//   ./reconnecting_client ws://127.0.0.1:9001/
// Start ./echo_server in another terminal, stop it, start it again and watch the
// state changes. Ctrl+C closes the connection gracefully.

#include "async_websocket_client/client.hpp"
#include "async_websocket_client/logger.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <string>

namespace asio = boost::asio;
using namespace std::chrono_literals;

namespace {

/// Prints every message the server sends until the client is closed.
asio::awaitable<void> receive_loop(awc::WebSocketClient& client) {
    for (;;) {
        auto message = co_await client.receive();
        if (!message) {
            spdlog::info("receive loop finished: {}", message.error().to_string());
            co_return;
        }
        spdlog::info("<- {} ({} bytes)", message->payload, message->payload.size());
    }
}

/// Sends a counter every second; sends simply fail while the connection is down.
asio::awaitable<void> send_loop(awc::WebSocketClient& client) {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    for (int counter = 1;; ++counter) {
        timer.expires_after(1s);
        if (auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable)); ec) {
            co_return;
        }
        const auto text = "tick " + std::to_string(counter);
        if (auto sent = co_await client.send_text(text); !sent) {
            spdlog::warn("-> {} not sent: {}", text, sent.error().to_string());
        } else {
            spdlog::info("-> {}", text);
        }
    }
}

/// Connects, then runs the send and receive loops until the client is closed.
asio::awaitable<void> run(awc::WebSocketClient& client, asio::signal_set& signals) {
    if (auto connected = co_await client.connect(); !connected) {
        spdlog::error("giving up: {}", connected.error().to_string());
        signals.cancel();
        co_return;
    }
    asio::co_spawn(co_await asio::this_coro::executor, send_loop(client), asio::detached);
    co_await receive_loop(client);  // returns once the client is closed
    signals.cancel();
    const auto stats = client.stats();
    spdlog::info("connections: {}, pings: {}, pongs: {}, last rtt: {} ms", stats.connections_established,
                 stats.pings_sent, stats.pongs_received,
                 stats.last_ping_rtt ? stats.last_ping_rtt->count() : -1);
}

}  // namespace

int main(int argc, char** argv) {
    awc::log::set_level(spdlog::level::info);

    awc::ClientConfig config;
    config.url = argc > 1 ? argv[1] : "ws://127.0.0.1:9001/";
    config.ping_interval = 5s;
    config.pong_timeout = 2s;
    config.reconnect.initial_delay = 500ms;
    config.reconnect.max_delay = 5s;
    config.reconnect.backoff_multiplier = 2.0;
    config.reconnect.max_attempts = std::nullopt;  // keep trying forever

    asio::io_context ioc;
    awc::WebSocketClient client(ioc, config);

    client.on_state_change([](awc::ConnectionState previous, awc::ConnectionState current) {
        spdlog::info("state: {} -> {}", awc::to_string(previous), awc::to_string(current));
    });
    client.on_error([](const awc::Error& error) { spdlog::warn("error: {}", error.to_string()); });

    asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code&, int) {
        spdlog::info("closing...");
        asio::co_spawn(ioc, client.close(), asio::detached);
    });

    asio::co_spawn(ioc, run(client, signals), asio::detached);

    ioc.run();
    return 0;
}
