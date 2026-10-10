#pragma once

#include "async_websocket_client/error.hpp"

#include <string>
#include <string_view>

namespace awc {

/// Components of a parsed `ws://` URL needed to open a connection.
struct Url {
    /// Host for DNS resolution; IPv6 literals are stored without brackets.
    std::string host;
    /// Port as a string so it can be fed directly to the resolver.
    std::string port;
    /// Request target (path + query), never empty; defaults to "/".
    std::string target;
    /// Value for the HTTP `Host` header, e.g. "example.com:8080" or "[::1]:8080".
    std::string host_header;

    [[nodiscard]] bool operator==(const Url& other) const noexcept = default;
};

/// Parses a WebSocket URL. Only the `ws` scheme is accepted.
[[nodiscard]] Result<Url> parse_url(std::string_view url);

std::ostream& operator<<(std::ostream& os, const Url& url);

}  // namespace awc
