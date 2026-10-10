#include "async_websocket_client/url.hpp"

#include <boost/url/parse.hpp>
#include <boost/url/url_view.hpp>

#include <ostream>

namespace awc {

Result<Url> parse_url(std::string_view url) {
    const auto parsed = boost::urls::parse_uri(url);
    if (!parsed) {
        return fail(ErrorCode::InvalidUrl, "cannot parse URL '" + std::string(url) + "'", parsed.error());
    }
    const boost::urls::url_view& view = *parsed;

    if (view.scheme() != "ws") {
        const auto code = view.scheme() == "wss" ? ErrorCode::UnsupportedScheme : ErrorCode::InvalidUrl;
        return fail(code, "unsupported scheme '" + std::string(view.scheme()) + "', only ws:// is supported");
    }
    if (view.host().empty()) {
        return fail(ErrorCode::InvalidUrl, "URL '" + std::string(url) + "' has no host");
    }

    Url result;
    result.host = view.host_address();  // strips the [] from IPv6 literals
    result.port = view.has_port() ? std::string(view.port()) : "80";
    result.target = view.encoded_path().empty() ? "/" : std::string(view.encoded_target());
    result.host_header = std::string(view.encoded_host());
    if (result.port != "80") {
        result.host_header += ':' + result.port;
    }
    return result;
}

std::ostream& operator<<(std::ostream& os, const Url& url) {
    return os << "ws://" << url.host_header << url.target;
}

}  // namespace awc
