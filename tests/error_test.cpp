#include "async_websocket_client/error.hpp"

#include <boost/asio/error.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <sstream>

namespace awc {
namespace {

using ::testing::HasSubstr;

TEST(ErrorCodeTest, EveryCodeHasAStableName) {
    EXPECT_EQ(to_string(ErrorCode::InvalidUrl), "InvalidUrl");
    EXPECT_EQ(to_string(ErrorCode::UnsupportedScheme), "UnsupportedScheme");
    EXPECT_EQ(to_string(ErrorCode::InvalidConfig), "InvalidConfig");
    EXPECT_EQ(to_string(ErrorCode::InvalidState), "InvalidState");
    EXPECT_EQ(to_string(ErrorCode::ResolveFailed), "ResolveFailed");
    EXPECT_EQ(to_string(ErrorCode::ConnectFailed), "ConnectFailed");
    EXPECT_EQ(to_string(ErrorCode::ConnectTimeout), "ConnectTimeout");
    EXPECT_EQ(to_string(ErrorCode::HandshakeFailed), "HandshakeFailed");
    EXPECT_EQ(to_string(ErrorCode::HandshakeTimeout), "HandshakeTimeout");
    EXPECT_EQ(to_string(ErrorCode::NotConnected), "NotConnected");
    EXPECT_EQ(to_string(ErrorCode::SendFailed), "SendFailed");
    EXPECT_EQ(to_string(ErrorCode::ReadFailed), "ReadFailed");
    EXPECT_EQ(to_string(ErrorCode::PongTimeout), "PongTimeout");
    EXPECT_EQ(to_string(ErrorCode::ConnectionClosed), "ConnectionClosed");
    EXPECT_EQ(to_string(ErrorCode::ReconnectExhausted), "ReconnectExhausted");
    EXPECT_EQ(to_string(ErrorCode::Cancelled), "Cancelled");
}

TEST(ErrorCodeTest, UnknownValueDoesNotCrash) {
    EXPECT_EQ(to_string(static_cast<ErrorCode>(255)), "Unknown");
}

TEST(ErrorCodeTest, StreamsItsName) {
    std::ostringstream os;
    os << ErrorCode::PongTimeout;
    EXPECT_EQ(os.str(), "PongTimeout");
}

TEST(ErrorTest, ToStringWithoutSystemError) {
    const Error error = make_error(ErrorCode::NotConnected, "cannot send");
    EXPECT_EQ(error.to_string(), "NotConnected: cannot send");
    EXPECT_FALSE(error.system_error);
}

TEST(ErrorTest, ToStringIncludesSystemError) {
    const Error error =
        make_error(ErrorCode::ConnectFailed, "cannot connect", boost::asio::error::connection_refused);
    EXPECT_THAT(error.to_string(), HasSubstr("ConnectFailed: cannot connect ["));
    EXPECT_THAT(error.to_string(), HasSubstr("refused"));
    EXPECT_EQ(error.system_error, boost::asio::error::connection_refused);
}

TEST(ErrorTest, StreamOperatorUsesToString) {
    const Error error = make_error(ErrorCode::ReadFailed, "boom");
    std::ostringstream os;
    os << error;
    EXPECT_EQ(os.str(), error.to_string());
}

TEST(ErrorTest, EqualityComparesAllFields) {
    const Error a = make_error(ErrorCode::ReadFailed, "boom");
    const Error b = make_error(ErrorCode::ReadFailed, "boom");
    const Error c = make_error(ErrorCode::ReadFailed, "bang");
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(ResultTest, FailProducesUnexpected) {
    const Result<int> result = fail(ErrorCode::InvalidConfig, "bad", boost::asio::error::timed_out);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidConfig);
    EXPECT_EQ(result.error().message, "bad");
    EXPECT_EQ(result.error().system_error, boost::asio::error::timed_out);
}

TEST(ResultTest, VoidResultHoldsValue) {
    const Result<void> ok{};
    EXPECT_TRUE(ok.has_value());
}

}  // namespace
}  // namespace awc
