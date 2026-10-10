#pragma once

#include <boost/system/error_code.hpp>

#include <cstdint>
#include <expected>
#include <iosfwd>
#include <string>
#include <string_view>

namespace awc {

/// High level classification of everything that can go wrong in the client.
enum class ErrorCode : std::uint8_t {
    InvalidUrl,          ///< The URL could not be parsed.
    UnsupportedScheme,   ///< Only `ws://` is supported (no TLS).
    InvalidConfig,       ///< A configuration value is out of range.
    InvalidState,        ///< Operation is not allowed in the current connection state.
    ResolveFailed,       ///< DNS resolution failed.
    ConnectFailed,       ///< TCP connection failed (e.g. refused).
    ConnectTimeout,      ///< TCP connection did not complete in time.
    HandshakeFailed,     ///< WebSocket upgrade was rejected or malformed.
    HandshakeTimeout,    ///< WebSocket upgrade did not complete in time.
    NotConnected,        ///< Attempted to send while not connected.
    SendFailed,          ///< Writing a frame failed.
    ReadFailed,          ///< Reading a frame failed (includes protocol violations).
    PongTimeout,         ///< Server did not answer our ping in time.
    ConnectionClosed,    ///< Connection closed (by peer or locally).
    ReconnectExhausted,  ///< Reconnect attempts exceeded the configured maximum.
    Cancelled,           ///< Operation aborted because the client is closing.
};

/// Human readable, stable name of an error code (e.g. "ConnectFailed").
[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

/// Error value carried by every failed `Result`.
struct Error {
    ErrorCode code{ErrorCode::InvalidState};
    std::string message;
    /// Underlying system/Boost error, if there was one.
    boost::system::error_code system_error{};

    /// "ConnectFailed: could not connect to 127.0.0.1:9 [system:111 Connection refused]"
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] bool operator==(const Error& other) const noexcept = default;
};

/// Convenience factory.
[[nodiscard]] Error make_error(ErrorCode code, std::string message,
                               boost::system::error_code system_error = {});

std::ostream& operator<<(std::ostream& os, ErrorCode code);
std::ostream& operator<<(std::ostream& os, const Error& error);

/// Result type used throughout the library. `Result<void>` is valid.
template <class T>
using Result = std::expected<T, Error>;

/// Shorthand for building an unexpected result.
[[nodiscard]] inline std::unexpected<Error> fail(ErrorCode code, std::string message,
                                                 boost::system::error_code system_error = {}) {
    return std::unexpected(make_error(code, std::move(message), system_error));
}

}  // namespace awc
