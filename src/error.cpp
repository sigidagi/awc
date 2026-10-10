#include "async_websocket_client/error.hpp"

#include <ostream>
#include <utility>

namespace awc {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::InvalidUrl:
            return "InvalidUrl";
        case ErrorCode::UnsupportedScheme:
            return "UnsupportedScheme";
        case ErrorCode::InvalidConfig:
            return "InvalidConfig";
        case ErrorCode::InvalidState:
            return "InvalidState";
        case ErrorCode::ResolveFailed:
            return "ResolveFailed";
        case ErrorCode::ConnectFailed:
            return "ConnectFailed";
        case ErrorCode::ConnectTimeout:
            return "ConnectTimeout";
        case ErrorCode::HandshakeFailed:
            return "HandshakeFailed";
        case ErrorCode::HandshakeTimeout:
            return "HandshakeTimeout";
        case ErrorCode::NotConnected:
            return "NotConnected";
        case ErrorCode::SendFailed:
            return "SendFailed";
        case ErrorCode::ReadFailed:
            return "ReadFailed";
        case ErrorCode::PongTimeout:
            return "PongTimeout";
        case ErrorCode::ConnectionClosed:
            return "ConnectionClosed";
        case ErrorCode::ReconnectExhausted:
            return "ReconnectExhausted";
        case ErrorCode::Cancelled:
            return "Cancelled";
    }
    return "Unknown";
}

std::string Error::to_string() const {
    std::string out{awc::to_string(code)};
    out += ": ";
    out += message;
    if (system_error) {
        out += " [";
        out += system_error.category().name();
        out += ':';
        out += std::to_string(system_error.value());
        out += ' ';
        out += system_error.message();
        out += ']';
    }
    return out;
}  // LCOV_EXCL_LINE (gcov attributes the unwinding cleanup of `out` to this brace)

Error make_error(ErrorCode code, std::string message, boost::system::error_code system_error) {
    return Error{.code = code, .message = std::move(message), .system_error = system_error};
}

std::ostream& operator<<(std::ostream& os, ErrorCode code) {
    return os << to_string(code);
}

std::ostream& operator<<(std::ostream& os, const Error& error) {
    return os << error.to_string();
}

}  // namespace awc
