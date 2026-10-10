#include "async_websocket_client/config.hpp"

#include "async_websocket_client/url.hpp"

#include <algorithm>
#include <cmath>

namespace awc {

std::chrono::milliseconds ReconnectPolicy::delay_for_attempt(unsigned attempt) const noexcept {
    if (attempt <= 1 || backoff_multiplier <= 1.0) {
        return std::min(initial_delay, max_delay);
    }
    const double factor = std::pow(backoff_multiplier, static_cast<double>(attempt - 1));
    const double delay_ms = static_cast<double>(initial_delay.count()) * factor;
    const double capped = std::min(delay_ms, static_cast<double>(max_delay.count()));
    return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(capped)};
}

Result<void> ClientConfig::validate() const {
    if (auto parsed = parse_url(url); !parsed) {
        return std::unexpected(parsed.error());
    }
    if (connect_timeout <= std::chrono::milliseconds::zero()) {
        return fail(ErrorCode::InvalidConfig, "connect_timeout must be positive");
    }
    if (ping_interval < std::chrono::milliseconds::zero()) {
        return fail(ErrorCode::InvalidConfig, "ping_interval must not be negative");
    }
    if (pong_timeout <= std::chrono::milliseconds::zero()) {
        return fail(ErrorCode::InvalidConfig, "pong_timeout must be positive");
    }
    if (reconnect.initial_delay < std::chrono::milliseconds::zero()) {
        return fail(ErrorCode::InvalidConfig, "reconnect.initial_delay must not be negative");
    }
    if (reconnect.max_delay < reconnect.initial_delay) {
        return fail(ErrorCode::InvalidConfig, "reconnect.max_delay must not be smaller than initial_delay");
    }
    if (!(reconnect.backoff_multiplier >= 1.0)) {  // also rejects NaN
        return fail(ErrorCode::InvalidConfig, "reconnect.backoff_multiplier must be >= 1.0");
    }
    if (reconnect.max_attempts.has_value() && *reconnect.max_attempts == 0) {
        return fail(ErrorCode::InvalidConfig, "reconnect.max_attempts must be at least 1 when set");
    }
    if (receive_queue_capacity == 0) {
        return fail(ErrorCode::InvalidConfig, "receive_queue_capacity must be at least 1");
    }
    if (max_message_size == 0) {
        return fail(ErrorCode::InvalidConfig, "max_message_size must be positive");
    }
    return {};
}

}  // namespace awc
