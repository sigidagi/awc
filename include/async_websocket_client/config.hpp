#pragma once

#include "async_websocket_client/error.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace awc {

/// Controls what happens after the connection is lost (or cannot be established).
struct ReconnectPolicy {
    /// When false, the client gives up after the first failure/disconnect.
    bool enabled = true;
    /// Delay before the first reconnect attempt.
    std::chrono::milliseconds initial_delay{500};
    /// Upper bound for the (exponentially growing) delay.
    std::chrono::milliseconds max_delay{30'000};
    /// Growth factor applied to the delay after each failed attempt.
    double backoff_multiplier = 2.0;
    /// Maximum number of consecutive failed attempts; `nullopt` means unlimited.
    std::optional<unsigned> max_attempts;

    /// Delay to wait before attempt number `attempt` (1-based), honouring the cap.
    [[nodiscard]] std::chrono::milliseconds delay_for_attempt(unsigned attempt) const noexcept;
};

/// Configuration of a `WebSocketClient`.
struct ClientConfig {
    /// `ws://host[:port][/path[?query]]`
    std::string url;
    /// Timeout applied separately to TCP connect and to the WebSocket handshake.
    std::chrono::milliseconds connect_timeout{5'000};
    /// Interval between pings; zero disables the client-side keep-alive.
    std::chrono::milliseconds ping_interval{15'000};
    /// How long to wait for a pong before the connection is considered dead.
    std::chrono::milliseconds pong_timeout{5'000};
    /// Reconnect behaviour.
    ReconnectPolicy reconnect;
    /// Maximum number of received messages buffered before `receive()` is called.
    std::size_t receive_queue_capacity = 1024;
    /// Messages larger than this are rejected (and the connection is reset).
    std::size_t max_message_size = std::size_t{16} * 1024 * 1024;
    /// Sent in the `User-Agent` header of the upgrade request.
    std::string user_agent = "async_websocket_client/1.0";

    /// Checks that all values are sane. Returns `ErrorCode::InvalidConfig` otherwise.
    [[nodiscard]] Result<void> validate() const;
};

}  // namespace awc
