#include "async_websocket_client/url.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <sstream>

namespace awc {
namespace {

using ::testing::HasSubstr;

TEST(UrlTest, MinimalUrlUsesDefaults) {
    const auto url = parse_url("ws://example.com");
    ASSERT_TRUE(url.has_value()) << url.error();
    EXPECT_EQ(url->host, "example.com");
    EXPECT_EQ(url->port, "80");
    EXPECT_EQ(url->target, "/");
    EXPECT_EQ(url->host_header, "example.com");
}

TEST(UrlTest, ExplicitPortAndPathAndQuery) {
    const auto url = parse_url("ws://example.com:8080/chat/room?x=1&y=2#frag");
    ASSERT_TRUE(url.has_value()) << url.error();
    EXPECT_EQ(url->host, "example.com");
    EXPECT_EQ(url->port, "8080");
    EXPECT_EQ(url->target, "/chat/room?x=1&y=2");
    EXPECT_EQ(url->host_header, "example.com:8080");
}

TEST(UrlTest, ExplicitDefaultPortIsNotRepeatedInHostHeader) {
    const auto url = parse_url("ws://example.com:80/");
    ASSERT_TRUE(url.has_value()) << url.error();
    EXPECT_EQ(url->port, "80");
    EXPECT_EQ(url->host_header, "example.com");
}

TEST(UrlTest, Ipv4Literal) {
    const auto url = parse_url("ws://127.0.0.1:9001/echo");
    ASSERT_TRUE(url.has_value()) << url.error();
    EXPECT_EQ(url->host, "127.0.0.1");
    EXPECT_EQ(url->host_header, "127.0.0.1:9001");
}

TEST(UrlTest, Ipv6LiteralKeepsBracketsOnlyInHostHeader) {
    const auto url = parse_url("ws://[::1]:9001/echo");
    ASSERT_TRUE(url.has_value()) << url.error();
    EXPECT_EQ(url->host, "::1");
    EXPECT_EQ(url->port, "9001");
    EXPECT_EQ(url->host_header, "[::1]:9001");
}

TEST(UrlTest, PercentEncodedPathIsKeptEncoded) {
    const auto url = parse_url("ws://h/a%20b?q=%41");
    ASSERT_TRUE(url.has_value()) << url.error();
    EXPECT_EQ(url->target, "/a%20b?q=%41");
}

TEST(UrlTest, SecureSchemeIsRejected) {
    const auto url = parse_url("wss://example.com");
    ASSERT_FALSE(url.has_value());
    EXPECT_EQ(url.error().code, ErrorCode::UnsupportedScheme);
    EXPECT_THAT(url.error().message, HasSubstr("wss"));
}

TEST(UrlTest, OtherSchemesAreInvalid) {
    const auto url = parse_url("http://example.com");
    ASSERT_FALSE(url.has_value());
    EXPECT_EQ(url.error().code, ErrorCode::InvalidUrl);
}

TEST(UrlTest, GarbageIsInvalid) {
    for (const auto* input : {"", "not a url", "ws:// spaced", "://x", "ws://ex ample.com"}) {
        const auto url = parse_url(input);
        ASSERT_FALSE(url.has_value()) << input;
        EXPECT_EQ(url.error().code, ErrorCode::InvalidUrl) << input;
    }
}

TEST(UrlTest, MissingHostIsInvalid) {
    const auto url = parse_url("ws:///path");
    ASSERT_FALSE(url.has_value());
    EXPECT_EQ(url.error().code, ErrorCode::InvalidUrl);
    EXPECT_THAT(url.error().message, HasSubstr("no host"));
}

TEST(UrlTest, EqualityAndStreaming) {
    const auto a = parse_url("ws://example.com:8080/x?y");
    const auto b = parse_url("ws://example.com:8080/x?y");
    ASSERT_TRUE(a && b);
    EXPECT_EQ(*a, *b);
    std::ostringstream os;
    os << *a;
    EXPECT_EQ(os.str(), "ws://example.com:8080/x?y");
}

}  // namespace
}  // namespace awc
