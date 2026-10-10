#include "async_websocket_client/client.hpp"

#include "async_websocket_client/logger.hpp"

#include "test_server.hpp"
#include "test_utils.hpp"

#include <boost/asio/detached.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace awc {
namespace {

using namespace std::chrono_literals;
namespace asio = boost::asio;
using Behaviour = test::TestServer::Behaviour;
using test::run_coroutine_test;
using test::sleep_for;
using test::wait_until;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::SizeIs;

class ClientTest : public ::testing::Test {
protected:
    void SetUp() override { log::set_level(spdlog::level::off); }

    /// Configuration with short timeouts so failure tests are fast.
    ClientConfig config(std::string_view target = "/") const {
        ClientConfig cfg;
        cfg.url = server.url(target);
        cfg.connect_timeout = 2s;
        cfg.ping_interval = 0ms;  // most tests do not care about keep-alive
        cfg.pong_timeout = 500ms;
        cfg.reconnect.initial_delay = 20ms;
        cfg.reconnect.max_delay = 50ms;
        return cfg;
    }

    std::unique_ptr<WebSocketClient> make_client(ClientConfig cfg) {
        auto client = std::make_unique<WebSocketClient>(ioc, std::move(cfg));
        client->on_state_change(
            [this](ConnectionState, ConnectionState current) { states.push_back(current); });
        client->on_error([this](const Error& error) { errors.push_back(error); });
        return client;
    }

    void run(std::function<asio::awaitable<void>()> body) { run_coroutine_test(ioc, std::move(body)); }

    asio::awaitable<bool> wait_for_state(const WebSocketClient& client, ConnectionState state) {
        co_return co_await wait_until([&] { return client.state() == state; });
    }

    asio::io_context ioc;
    test::TestServer server{ioc};
    std::vector<ConnectionState> states;
    std::vector<Error> errors;
};

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

TEST_F(ClientTest, ConnectSendReceiveClose) {
    auto client = make_client(config("/chat?room=1"));
    EXPECT_EQ(client->state(), ConnectionState::Disconnected);
    EXPECT_FALSE(client->is_connected());
    EXPECT_EQ(client->config().url, server.url("/chat?room=1"));

    run([&]() -> asio::awaitable<void> {
        const auto connected = co_await client->connect();
        EXPECT_TRUE(connected.has_value()) << connected.error();
        EXPECT_TRUE(client->is_connected());

        EXPECT_TRUE((co_await client->send_text("hello")).has_value());
        const auto echo = co_await client->receive();
        CO_ASSERT_TRUE(echo.has_value()) << echo.error();
        EXPECT_EQ(*echo, Message::text("hello"));

        const std::array<std::byte, 3> bytes{std::byte{0}, std::byte{255}, std::byte{7}};
        EXPECT_TRUE((co_await client->send_binary(bytes)).has_value());
        const auto binary_echo = co_await client->receive();
        CO_ASSERT_TRUE(binary_echo.has_value()) << binary_echo.error();
        EXPECT_FALSE(binary_echo->is_text);
        EXPECT_EQ(binary_echo->payload, std::string("\x00\xff\x07", 3));

        EXPECT_TRUE((co_await client->send(Message::binary("raw"))).has_value());
        const auto raw_echo = co_await client->receive();
        CO_ASSERT_TRUE(raw_echo.has_value()) << raw_echo.error();
        EXPECT_EQ(*raw_echo, Message::binary("raw"));

        co_await client->close();
        EXPECT_EQ(client->state(), ConnectionState::Closed);
        EXPECT_TRUE(co_await wait_until([&] { return server.open_sessions() == 0; }));
    });

    EXPECT_THAT(states, ElementsAre(ConnectionState::Connecting, ConnectionState::Connected,
                                    ConnectionState::Closing, ConnectionState::Closed));
    EXPECT_THAT(errors, IsEmpty());
    EXPECT_THAT(server.user_agents(), ElementsAre(client->config().user_agent));
    EXPECT_EQ(server.messages_received(), 3U);

    const auto stats = client->stats();
    EXPECT_EQ(stats.connect_attempts, 1U);
    EXPECT_EQ(stats.connections_established, 1U);
    EXPECT_EQ(stats.disconnections, 1U);
    EXPECT_EQ(stats.messages_sent, 3U);
    EXPECT_EQ(stats.messages_received, 3U);
    EXPECT_EQ(stats.pings_sent, 0U);
}

TEST_F(ClientTest, ConcurrentSendsAreSerialised) {
    auto client = make_client(config());
    run([&]() -> asio::awaitable<void> {
        CO_ASSERT_TRUE((co_await client->connect()).has_value());
        constexpr int count = 25;
        for (int i = 0; i < count; ++i) {
            asio::co_spawn(ioc, client->send_text("msg-" + std::to_string(i)), asio::detached);
        }
        std::vector<std::string> received;
        for (int i = 0; i < count; ++i) {
            auto message = co_await client->receive();
            CO_ASSERT_TRUE(message.has_value()) << message.error();
            received.push_back(message->payload);
        }
        for (int i = 0; i < count; ++i) {
            EXPECT_THAT(received, Contains("msg-" + std::to_string(i)));
        }
        EXPECT_EQ(client->stats().messages_sent, static_cast<std::uint64_t>(count));
        co_await client->close();
    });
}

TEST_F(ClientTest, CanReconnectAfterClose) {
    auto client = make_client(config());
    run([&]() -> asio::awaitable<void> {
        CO_ASSERT_TRUE((co_await client->connect()).has_value());
        co_await client->close();
        EXPECT_EQ(client->state(), ConnectionState::Closed);

        const auto again = co_await client->connect();
        EXPECT_TRUE(again.has_value()) << again.error();
        EXPECT_TRUE((co_await client->send_text("second life")).has_value());
        const auto echo = co_await client->receive();
        CO_ASSERT_TRUE(echo.has_value()) << echo.error();
        EXPECT_EQ(echo->payload, "second life");
        co_await client->close();
    });
    EXPECT_EQ(client->stats().connections_established, 2U);
    EXPECT_EQ(server.sessions_accepted(), 2U);
}

TEST_F(ClientTest, BufferedMessagesAreDeliveredAfterClose) {
    auto client = make_client(config());
    run([&]() -> asio::awaitable<void> {
        CO_ASSERT_TRUE((co_await client->connect()).has_value());
        co_await server.broadcast(Message::text("one"));
        co_await server.broadcast(Message::text("two"));
        EXPECT_TRUE(co_await wait_until([&] { return client->stats().messages_received == 2; }));

        co_await client->close();

        const auto first = co_await client->receive();
        CO_ASSERT_TRUE(first.has_value()) << first.error();
        EXPECT_EQ(first->payload, "one");
        const auto second = co_await client->receive();
        CO_ASSERT_TRUE(second.has_value()) << second.error();
        EXPECT_EQ(second->payload, "two");

        const auto none = co_await client->receive();
        CO_ASSERT_FALSE(none.has_value());
        EXPECT_EQ(none.error().code, ErrorCode::ConnectionClosed);
        EXPECT_THAT(none.error().message, HasSubstr("closed"));
    });
}

// ---------------------------------------------------------------------------
// Ping / pong
// ---------------------------------------------------------------------------

TEST_F(ClientTest, PingPongKeepsConnectionAlive) {
    auto cfg = config();
    cfg.ping_interval = 20ms;
    cfg.pong_timeout = 300ms;
    auto client = make_client(cfg);
    run([&]() -> asio::awaitable<void> {
        CO_ASSERT_TRUE((co_await client->connect()).has_value());
        EXPECT_TRUE(co_await wait_until([&] { return client->stats().pongs_received >= 3; }));
        EXPECT_TRUE(client->is_connected());
        const auto stats = client->stats();
        EXPECT_GE(stats.pings_sent, stats.pongs_received);
        CO_ASSERT_TRUE(stats.last_ping_rtt.has_value());
        EXPECT_LT(*stats.last_ping_rtt, 300ms);
        co_await client->close();
    });
    EXPECT_THAT(errors, IsEmpty());
}

TEST_F(ClientTest, PongTimeoutDropsTheConnection) {
    server.set_behaviour(Behaviour::IgnoreFrames);
    auto cfg = config();
    cfg.ping_interval = 20ms;
    cfg.pong_timeout = 50ms;
    cfg.reconnect.enabled = false;
    auto client = make_client(cfg);
    run([&]() -> asio::awaitable<void> {
        CO_ASSERT_TRUE((co_await client->connect()).has_value());
        EXPECT_TRUE(co_await wait_for_state(*client, ConnectionState::Disconnected));
        EXPECT_TRUE(co_await wait_until([&] { return server.open_sessions() == 0; }));
    });
    ASSERT_THAT(errors, SizeIs(1));
    EXPECT_EQ(errors[0].code, ErrorCode::PongTimeout);
    EXPECT_THAT(errors[0].message, HasSubstr("50 ms"));
    EXPECT_GE(client->stats().pings_sent, 1U);
    EXPECT_EQ(client->stats().pongs_received, 0U);
    EXPECT_EQ(client->stats().disconnections, 1U);
}

TEST_F(ClientTest, BackPressureDoesNotCauseFalsePongTimeout) {
    auto cfg = config();
    cfg.ping_interval = 10ms;
    cfg.pong_timeout = 30ms;
    cfg.receive_queue_capacity = 1;
    auto client = make_client(cfg);
    run([&]() -> asio::awaitable<void> {
        CO_ASSERT_TRUE((co_await client->connect()).has_value());
        for (int i = 0; i < 4; ++i) {
            co_await server.broadcast(Message::text(std::to_string(i)));
        }
        co_await sleep_for(250ms);  // the application is "busy": the reader is stuck on the full queue
        EXPECT_TRUE(client->is_connected());
        EXPECT_THAT(errors, IsEmpty());

        for (int i = 0; i < 4; ++i) {
            const auto message = co_await client->receive();
            CO_ASSERT_TRUE(message.has_value()) << message.error();
            EXPECT_EQ(message->payload, std::to_string(i));
        }
        co_await sleep_for(100ms);  // pings resume normally once the reader is unblocked
        EXPECT_TRUE(client->is_connected());
        EXPECT_GE(client->stats().pongs_received, 1U);
        co_await client->close();
    });
}

// ---------------------------------------------------------------------------
// Connection failures
// ---------------------------------------------------------------------------

TEST_F(ClientTest, InvalidConfigurationFailsBeforeConnecting) {
    auto cfg = config();
    cfg.url = "wss://127.0.0.1/";
    auto client = make_client(cfg);
    run([&]() -> asio::awaitable<void> {
        const auto result = co_await client->connect();
        CO_ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, ErrorCode::UnsupportedScheme);
        EXPECT_EQ(client->state(), ConnectionState::Disconnected);
        co_await client->close();  // no-op on a client that never started
        EXPECT_EQ(client->state(), ConnectionState::Disconnected);
    });
    EXPECT_THAT(states, IsEmpty());
    EXPECT_EQ(server.sessions_accepted(), 0U);
}

TEST_F(ClientTest, ConnectWhileRunningIsRejected) {
    auto client = make_client(config());
    run([&]() -> asio::awaitable<void> {
        CO_ASSERT_TRUE((co_await client->connect()).has_value());
        const auto again = co_await client->connect();
        CO_ASSERT_FALSE(again.has_value());
        EXPECT_EQ(again.error().code, ErrorCode::InvalidState);
        EXPECT_THAT(again.error().message, HasSubstr("Connected"));
        co_await client->close();
    });
}

TEST_F(ClientTest, SendBeforeConnectFails) {
    auto client = make_client(config());
    run([&]() -> asio::awaitable<void> {
        const auto result = co_await client->send_text("x");
        CO_ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, ErrorCode::NotConnected);
        EXPECT_THAT(result.error().message, HasSubstr("Disconnected"));
    });
}

TEST_F(ClientTest, ConnectionRefusedWithoutReconnect) {
    server.stop();
    auto cfg = config();
    cfg.reconnect.enabled = false;
    auto client = make_client(cfg);
    run([&]() -> asio::awaitable<void> {
        const auto result = co_await client->connect();
        CO_ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, ErrorCode::ConnectFailed);
        EXPECT_TRUE(result.error().system_error);
        EXPECT_EQ(client->state(), ConnectionState::Disconnected);
    });
    EXPECT_THAT(states, ElementsAre(ConnectionState::Connecting, ConnectionState::Disconnected));
    ASSERT_THAT(errors, SizeIs(1));
    EXPECT_EQ(errors[0].code, ErrorCode::ConnectFailed);
    EXPECT_EQ(client->stats().connect_attempts, 1U);
}

TEST_F(ClientTest, ResolveFailure) {
    auto cfg = config();
    cfg.url = "ws://host.invalid:1/";
    cfg.reconnect.enabled = false;
    auto client = make_client(cfg);
    run([&]() -> asio::awaitable<void> {
        const auto result = co_await client->connect();
        CO_ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, ErrorCode::ResolveFailed);
        EXPECT_THAT(result.error().message, HasSubstr("host.invalid"));
    });
}

TEST_F(ClientTest, HandshakeRejected) {
    server.set_behaviour(Behaviour::RejectHandshake);
    auto cfg = config();
    cfg.reconnect.enabled = false;
    auto client = make_client(cfg);
    run([&]() -> asio::awaitable<void> {
        const auto result = co_await client->connect();
        CO_ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, ErrorCode::HandshakeFailed);
        EXPECT_THAT(result.error().message, HasSubstr("handshake"));
    });
    EXPECT_EQ(server.sessions_accepted(), 1U);
}

TEST_F(ClientTest, HandshakeTimeout) {
    server.set_behaviour(Behaviour::SilentTcp);
    auto cfg = config();
    cfg.connect_timeout = 100ms;
    cfg.reconnect.enabled = false;
    auto client = make_client(cfg);
    run([&]() -> asio::awaitable<void> {
        const auto result = co_await client->connect();
        CO_ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, ErrorCode::HandshakeTimeout);
    });
}

TEST_F(ClientTest, ReconnectAttemptsAreExhausted) {
    server.stop();
    auto cfg = config();
    cfg.reconnect.max_attempts = 2;
    auto client = make_client(cfg);
    run([&]() -> asio::awaitable<void> {
        const auto result = co_await client->connect();
        CO_ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, ErrorCode::ReconnectExhausted);
        EXPECT_THAT(result.error().message, HasSubstr("2 reconnect attempts"));
        EXPECT_THAT(result.error().message, HasSubstr("ConnectFailed"));
        EXPECT_EQ(client->state(), ConnectionState::Failed);

        const auto message = co_await client->receive();
        CO_ASSERT_FALSE(message.has_value());
        EXPECT_EQ(message.error().code, ErrorCode::ConnectionClosed);
        EXPECT_THAT(message.error().message, HasSubstr("ReconnectExhausted"));
    });
    EXPECT_EQ(client->stats().connect_attempts, 3U);  // initial + 2 retries
    // Staying in Reconnecting between attempts is not a transition, so it is reported once.
    EXPECT_THAT(states, ElementsAre(ConnectionState::Connecting, ConnectionState::Reconnecting,
                                    ConnectionState::Failed));
    ASSERT_THAT(errors, SizeIs(4));
    EXPECT_EQ(errors.back().code, ErrorCode::ReconnectExhausted);
}

// ---------------------------------------------------------------------------
// Reconnect
// ---------------------------------------------------------------------------

TEST_F(ClientTest, ReconnectsAfterServerDropsTheConnection) {
    auto client = make_client(config());
    run([&]() -> asio::awaitable<void> {
        CO_ASSERT_TRUE((co_await client->connect()).has_value());
        server.drop_all();
        EXPECT_TRUE(co_await wait_until([&] { return client->stats().connections_established == 2; }));
        EXPECT_TRUE(client->is_connected());

        EXPECT_TRUE((co_await client->send_text("after reconnect")).has_value());
        const auto echo = co_await client->receive();
        CO_ASSERT_TRUE(echo.has_value()) << echo.error();
        EXPECT_EQ(echo->payload, "after reconnect");
        co_await client->close();
    });
    EXPECT_THAT(states, ElementsAre(ConnectionState::Connecting, ConnectionState::Connected,
                                    ConnectionState::Reconnecting, ConnectionState::Connected,
                                    ConnectionState::Closing, ConnectionState::Closed));
    ASSERT_THAT(errors, SizeIs(1));
    EXPECT_EQ(errors[0].code, ErrorCode::ReadFailed);
    EXPECT_EQ(client->stats().disconnections, 2U);
    EXPECT_EQ(server.sessions_accepted(), 2U);
}

TEST_F(ClientTest, ReconnectsWhenServerComesBack) {
    auto client = make_client(config());
    run([&]() -> asio::awaitable<void> {
        CO_ASSERT_TRUE((co_await client->connect()).has_value());
        server.stop();
        EXPECT_TRUE(co_await wait_until([&] { return errors.size() >= 3; }));  // read error + refused x2
        EXPECT_EQ(client->state(), ConnectionState::Reconnecting);

        server.start();
        EXPECT_TRUE(co_await wait_for_state(*client, ConnectionState::Connected));
        co_await client->close();
    });
    EXPECT_THAT(errors, Contains(Field(&Error::code, ErrorCode::ConnectFailed)));
    EXPECT_EQ(client->stats().connections_established, 2U);
}

TEST_F(ClientTest, ServerInitiatedCloseIsReported) {
    server.set_behaviour(Behaviour::CloseAfterHandshake);
    auto cfg = config();
    cfg.reconnect.enabled = false;
    auto client = make_client(cfg);
    run([&]() -> asio::awaitable<void> {
        CO_ASSERT_TRUE((co_await client->connect()).has_value());
        EXPECT_TRUE(co_await wait_for_state(*client, ConnectionState::Disconnected));
    });
    ASSERT_THAT(errors, SizeIs(1));
    EXPECT_EQ(errors[0].code, ErrorCode::ConnectionClosed);
    EXPECT_THAT(errors[0].message, HasSubstr("1001"));
    EXPECT_THAT(errors[0].message, HasSubstr("bye"));
    EXPECT_EQ(client->stats().connections_established, 1U);
}

TEST_F(ClientTest, SuccessfulReconnectResetsTheAttemptCounter) {
    server.set_behaviour(Behaviour::CloseAfterHandshake);
    auto cfg = config();
    cfg.reconnect.max_attempts = 1;
    auto client = make_client(cfg);
    run([&]() -> asio::awaitable<void> {
        CO_ASSERT_TRUE((co_await client->connect()).has_value());
        // Every reconnect succeeds (and is then closed by the server again), so the
        // single allowed attempt is never exhausted.
        EXPECT_TRUE(co_await wait_until([&] { return client->stats().connections_established >= 3; }));
        EXPECT_NE(client->state(), ConnectionState::Failed);
        co_await client->close();
    });
    EXPECT_EQ(client->state(), ConnectionState::Closed);
    EXPECT_THAT(errors, Not(Contains(Field(&Error::code, ErrorCode::ReconnectExhausted))));
}

TEST_F(ClientTest, LostConnectionWithoutReconnectEndsDisconnected) {
    server.set_behaviour(Behaviour::DropAfterHandshake);
    auto cfg = config();
    cfg.reconnect.enabled = false;
    auto client = make_client(cfg);
    run([&]() -> asio::awaitable<void> {
        CO_ASSERT_TRUE((co_await client->connect()).has_value());
        EXPECT_TRUE(co_await wait_for_state(*client, ConnectionState::Disconnected));
        const auto send = co_await client->send_text("x");
        CO_ASSERT_FALSE(send.has_value());
        EXPECT_EQ(send.error().code, ErrorCode::NotConnected);
    });
    EXPECT_THAT(states, ElementsAre(ConnectionState::Connecting, ConnectionState::Connected,
                                    ConnectionState::Disconnected));
    ASSERT_THAT(errors, SizeIs(1));
    EXPECT_EQ(errors[0].code, ErrorCode::ReadFailed);
}

TEST_F(ClientTest, OversizedMessageResetsTheConnection) {
    auto cfg = config();
    cfg.max_message_size = 64;
    auto client = make_client(cfg);
    run([&]() -> asio::awaitable<void> {
        CO_ASSERT_TRUE((co_await client->connect()).has_value());
        EXPECT_TRUE((co_await client->send_text(std::string(200, 'x'))).has_value());
        EXPECT_TRUE(co_await wait_until([&] { return client->stats().connections_established == 2; }));
        co_await client->close();
    });
    ASSERT_THAT(errors, SizeIs(1));
    EXPECT_EQ(errors[0].code, ErrorCode::ReadFailed);
    EXPECT_THAT(errors[0].to_string(), HasSubstr("exceeded"));
}

TEST_F(ClientTest, SendFailsWhenConnectionDropsMidWrite) {
    server.set_behaviour(Behaviour::NeverRead);
    auto client = make_client(config());
    run([&]() -> asio::awaitable<void> {
        CO_ASSERT_TRUE((co_await client->connect()).has_value());
        std::optional<Result<void>> send_result;
        // Large enough to fill the loopback socket buffers while the server never reads.
        asio::co_spawn(ioc, client->send_text(std::string(std::size_t{64} * 1024 * 1024, 'x')),
                       [&](const std::exception_ptr&, Result<void> result) { send_result = result; });
        co_await sleep_for(100ms);
        EXPECT_FALSE(send_result.has_value());  // still blocked in the write
        server.drop_all();
        EXPECT_TRUE(co_await wait_until([&] { return send_result.has_value(); }));
        CO_ASSERT_FALSE(send_result->has_value());
        EXPECT_THAT((std::vector{ErrorCode::ConnectionClosed, ErrorCode::SendFailed}),
                    Contains(send_result->error().code));
        co_await client->close();
    });
}

// ---------------------------------------------------------------------------
// Closing
// ---------------------------------------------------------------------------

TEST_F(ClientTest, CloseWhileWaitingToReconnect) {
    server.stop();
    auto cfg = config();
    cfg.reconnect.initial_delay = 10s;
    cfg.reconnect.max_delay = 10s;
    auto client = make_client(cfg);
    run([&]() -> asio::awaitable<void> {
        std::optional<Result<void>> connect_result;
        asio::co_spawn(ioc, client->connect(),
                       [&](const std::exception_ptr&, Result<void> result) { connect_result = result; });
        EXPECT_TRUE(co_await wait_for_state(*client, ConnectionState::Reconnecting));

        co_await client->close();
        EXPECT_EQ(client->state(), ConnectionState::Closed);
        CO_ASSERT_TRUE(connect_result.has_value());
        CO_ASSERT_FALSE(connect_result->has_value());
        EXPECT_EQ(connect_result->error().code, ErrorCode::ConnectFailed);  // the last reported error
    });
}

TEST_F(ClientTest, CloseWhileHandshakeIsPending) {
    server.set_behaviour(Behaviour::SilentTcp);
    auto client = make_client(config());
    run([&]() -> asio::awaitable<void> {
        std::optional<Result<void>> connect_result;
        asio::co_spawn(ioc, client->connect(),
                       [&](const std::exception_ptr&, Result<void> result) { connect_result = result; });
        EXPECT_TRUE(co_await wait_until([&] { return server.sessions_accepted() == 1; }));

        co_await client->close();
        EXPECT_EQ(client->state(), ConnectionState::Closed);
        CO_ASSERT_TRUE(connect_result.has_value());
        CO_ASSERT_FALSE(connect_result->has_value());
        EXPECT_EQ(connect_result->error().code, ErrorCode::Cancelled);
    });
    EXPECT_THAT(states, ElementsAre(ConnectionState::Connecting, ConnectionState::Closed));
}

TEST_F(ClientTest, ConcurrentCloseCallsBothComplete) {
    auto client = make_client(config());
    run([&]() -> asio::awaitable<void> {
        CO_ASSERT_TRUE((co_await client->connect()).has_value());
        bool first_done = false;
        asio::co_spawn(ioc, client->close(), [&](const std::exception_ptr&) { first_done = true; });
        co_await client->close();
        EXPECT_TRUE(co_await wait_until([&] { return first_done; }));
        EXPECT_EQ(client->state(), ConnectionState::Closed);
    });
    EXPECT_THAT(states, ElementsAre(ConnectionState::Connecting, ConnectionState::Connected,
                                    ConnectionState::Closing, ConnectionState::Closed));
}

TEST_F(ClientTest, CloseWhenPeerDoesNotAnswerCloseFrame) {
    server.set_behaviour(Behaviour::IgnoreFrames);
    auto cfg = config();
    cfg.connect_timeout = 150ms;  // also bounds the close handshake
    auto client = make_client(cfg);
    run([&]() -> asio::awaitable<void> {
        CO_ASSERT_TRUE((co_await client->connect()).has_value());
        co_await client->close();
        EXPECT_EQ(client->state(), ConnectionState::Closed);
    });
}

TEST_F(ClientTest, DestructorAbortsEverything) {
    auto cfg = config();
    cfg.receive_queue_capacity = 1;
    run([&]() -> asio::awaitable<void> {
        {
            auto client = make_client(cfg);
            CO_ASSERT_TRUE((co_await client->connect()).has_value());
            for (int i = 0; i < 3; ++i) {  // leave the reader blocked on the full queue
                co_await server.broadcast(Message::text("x"));
            }
            co_await sleep_for(50ms);
            EXPECT_EQ(server.open_sessions(), 1U);
        }
        EXPECT_TRUE(co_await wait_until([&] { return server.open_sessions() == 0; }));
    });
    EXPECT_THAT(states, Not(Contains(ConnectionState::Closed)));  // callbacks are detached on destruction
}

TEST_F(ClientTest, DestructorWhileConnecting) {
    server.set_behaviour(Behaviour::SilentTcp);
    run([&]() -> asio::awaitable<void> {
        {
            auto client = make_client(config());
            asio::co_spawn(ioc, client->connect(), asio::detached);
            EXPECT_TRUE(co_await wait_until([&] { return server.sessions_accepted() == 1; }));
        }
        EXPECT_TRUE(co_await wait_until([&] { return server.open_sessions() == 0; }));
    });
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

TEST(ConnectionStateTest, NamesAndStreaming) {
    EXPECT_EQ(to_string(ConnectionState::Disconnected), "Disconnected");
    EXPECT_EQ(to_string(ConnectionState::Connecting), "Connecting");
    EXPECT_EQ(to_string(ConnectionState::Connected), "Connected");
    EXPECT_EQ(to_string(ConnectionState::Reconnecting), "Reconnecting");
    EXPECT_EQ(to_string(ConnectionState::Closing), "Closing");
    EXPECT_EQ(to_string(ConnectionState::Closed), "Closed");
    EXPECT_EQ(to_string(ConnectionState::Failed), "Failed");
    EXPECT_EQ(to_string(static_cast<ConnectionState>(42)), "Unknown");
    std::ostringstream os;
    os << ConnectionState::Reconnecting;
    EXPECT_EQ(os.str(), "Reconnecting");
}

TEST(MessageTest, Factories) {
    EXPECT_EQ(Message::text("a"), (Message{"a", true}));
    EXPECT_EQ(Message::binary("b"), (Message{"b", false}));
    const std::array<std::byte, 2> bytes{std::byte{'h'}, std::byte{'i'}};
    EXPECT_EQ(Message::binary(bytes), (Message{"hi", false}));
}

}  // namespace
}  // namespace awc
