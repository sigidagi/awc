#pragma once

#include "async_websocket_client/client.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/websocket/rfc6455.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace awc::test {

/**
 * In-process WebSocket server with scriptable misbehaviour, used to exercise
 * every failure path of the client deterministically.
 */
class TestServer {
public:
    enum class Behaviour : std::uint8_t {
        Echo,                 ///< Normal server: echoes every message back.
        SilentTcp,            ///< Accepts TCP, discards whatever arrives, never answers (handshake timeout).
        RejectHandshake,      ///< Answers the upgrade request with HTTP 403.
        DropAfterHandshake,   ///< Completes the handshake and then kills the TCP connection.
        CloseAfterHandshake,  ///< Completes the handshake and then closes gracefully (1001).
        NeverRead,            ///< Completes the handshake but never reads from the socket at all.
        IgnoreFrames,  ///< Completes the handshake, then discards raw bytes (never pongs, never closes).
    };

    explicit TestServer(boost::asio::io_context& ioc, Behaviour behaviour = Behaviour::Echo);
    ~TestServer();

    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;
    TestServer(TestServer&&) = delete;
    TestServer& operator=(TestServer&&) = delete;

    /// Starts listening. The port is chosen on the first call and reused afterwards.
    void start();
    /// Stops listening and closes all sessions (connections are refused from now on).
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] std::string url(std::string_view target = "/") const;

    /// Behaviour applied to connections accepted from now on.
    void set_behaviour(Behaviour behaviour) noexcept;

    [[nodiscard]] std::size_t sessions_accepted() const noexcept;
    [[nodiscard]] std::size_t open_sessions() const noexcept;
    [[nodiscard]] std::size_t messages_received() const noexcept;
    /// `User-Agent` header of every upgrade request seen so far.
    [[nodiscard]] const std::vector<std::string>& user_agents() const noexcept;

    /// Abruptly closes the TCP socket of every open session.
    void drop_all();
    /// Sends a message to every open WebSocket session.
    boost::asio::awaitable<void> broadcast(Message message);

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace awc::test
