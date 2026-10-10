// The simplest possible use of the client: connect, send a few messages, print
// the replies, close.    ./echo_client ws://127.0.0.1:9001/ hello world

#include "async_websocket_client/client.hpp"
#include "async_websocket_client/logger.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <string>
#include <vector>

namespace asio = boost::asio;
using namespace std::chrono_literals;

namespace {

asio::awaitable<int> run(awc::WebSocketClient& client, std::vector<std::string> messages) {
    if (auto connected = co_await client.connect(); !connected) {
        spdlog::error("could not connect: {}", connected.error().to_string());
        co_return 1;
    }

    for (const auto& text : messages) {
        if (auto sent = co_await client.send_text(text); !sent) {
            spdlog::error("send failed: {}", sent.error().to_string());
            co_return 1;
        }
        auto reply = co_await client.receive();
        if (!reply) {
            spdlog::error("receive failed: {}", reply.error().to_string());
            co_return 1;
        }
        spdlog::info("sent '{}' -> received '{}'", text, reply->payload);
    }

    co_await client.close();
    const auto stats = client.stats();
    spdlog::info("done: {} messages sent, {} received", stats.messages_sent, stats.messages_received);
    co_return 0;
}

}  // namespace

int main(int argc, char** argv) {
    awc::log::set_level(spdlog::level::info);

    awc::ClientConfig config;
    config.url = argc > 1 ? argv[1] : "ws://127.0.0.1:9001/";
    config.connect_timeout = 3s;
    config.reconnect.enabled = false;  // a one-shot tool: fail fast instead of retrying

    std::vector<std::string> messages(argv + std::min(argc, 2), argv + argc);
    if (messages.empty()) {
        messages = {"hello", "world"};
    }

    asio::io_context ioc;
    awc::WebSocketClient client(ioc, config);

    int exit_code = 0;
    asio::co_spawn(ioc, run(client, std::move(messages)),
                   [&](const std::exception_ptr&, int code) { exit_code = code; });
    ioc.run();
    return exit_code;
}
