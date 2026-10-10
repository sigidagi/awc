#pragma once

#include "async_websocket_client/config.hpp"
#include "async_websocket_client/error.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace awc {

namespace detail {
class ClientImpl;
}

/// Lifecycle of a client.
enum class ConnectionState : std::uint8_t {
    Disconnected,  ///< Never connected, or gave up without reconnecting.
    Connecting,    ///< First connection attempt in progress.
    Connected,     ///< Handshake completed; messages flow.
    Reconnecting,  ///< Connection lost; waiting/retrying according to the policy.
    Closing,       ///< `close()` was called; close handshake in progress.
    Closed,        ///< Closed by `close()`.
    Failed,        ///< Reconnect attempts exhausted.
};

[[nodiscard]] std::string_view to_string(ConnectionState state) noexcept;
std::ostream& operator<<(std::ostream& os, ConnectionState state);

/// A WebSocket message (text or binary).
struct Message {
    std::string payload;
    bool is_text = true;

    [[nodiscard]] static Message text(std::string payload) {
        return {.payload = std::move(payload), .is_text = true};
    }
    [[nodiscard]] static Message binary(std::string payload) {
        return {.payload = std::move(payload), .is_text = false};
    }
    [[nodiscard]] static Message binary(std::span<const std::byte> payload);

    [[nodiscard]] bool operator==(const Message& other) const = default;
};

/// Counters useful for monitoring and tests.
struct Stats {
    std::uint64_t connect_attempts = 0;
    std::uint64_t connections_established = 0;
    std::uint64_t disconnections = 0;
    std::uint64_t messages_sent = 0;
    std::uint64_t messages_received = 0;
    std::uint64_t pings_sent = 0;
    std::uint64_t pongs_received = 0;
    std::optional<std::chrono::milliseconds> last_ping_rtt;
};

/**
 * Non-secure (`ws://`) WebSocket client driven entirely by C++20 coroutines.
 *
 * Features: automatic reconnect with exponential back-off, application level
 * ping/pong keep-alive, bounded receive queue, `std::expected` based errors.
 *
 * Threading: the client is *not* thread safe. All methods must be called from the
 * thread running the `io_context` (or dispatched to it). Running the `io_context`
 * on a single thread is the intended usage.
 */
class WebSocketClient {
public:
    using StateHandler = std::function<void(ConnectionState previous, ConnectionState current)>;
    using ErrorHandler = std::function<void(const Error& error)>;

    WebSocketClient(boost::asio::io_context& ioc, ClientConfig config);
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;
    WebSocketClient(WebSocketClient&&) = delete;
    WebSocketClient& operator=(WebSocketClient&&) = delete;

    /**
     * Starts the connection loop and waits until the client is connected.
     *
     * Retries follow the configured `ReconnectPolicy`. Resolves with an error when
     * the connection could not be established (policy exhausted / disabled), or
     * when the client is already running (`ErrorCode::InvalidState`).
     * After the connection is lost the loop keeps reconnecting in the background.
     */
    [[nodiscard]] boost::asio::awaitable<Result<void>> connect();

    /// Sends a message. Fails with `NotConnected` while reconnecting.
    [[nodiscard]] boost::asio::awaitable<Result<void>> send(Message message);
    [[nodiscard]] boost::asio::awaitable<Result<void>> send_text(std::string text);
    [[nodiscard]] boost::asio::awaitable<Result<void>> send_binary(std::span<const std::byte> data);

    /**
     * Waits for the next inbound message. Messages received while the caller is
     * busy are queued (bounded by `receive_queue_capacity`). Fails with
     * `ConnectionClosed` once the client has been closed or has given up.
     */
    [[nodiscard]] boost::asio::awaitable<Result<Message>> receive();

    /// Performs a graceful close (if connected), stops reconnecting and waits for shutdown.
    [[nodiscard]] boost::asio::awaitable<void> close();

    [[nodiscard]] ConnectionState state() const noexcept;
    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] Stats stats() const noexcept;
    [[nodiscard]] const ClientConfig& config() const noexcept;

    /// Called on every state transition. Called from the io_context thread.
    void on_state_change(StateHandler handler);
    /// Called for every error the connection loop encounters (also the ones it recovers from).
    void on_error(ErrorHandler handler);

private:
    std::shared_ptr<detail::ClientImpl> impl_;
};

}  // namespace awc
