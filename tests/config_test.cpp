#include "async_websocket_client/config.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace awc {
namespace {

using namespace std::chrono_literals;
using ::testing::HasSubstr;

ClientConfig valid_config() {
    ClientConfig config;
    config.url = "ws://localhost:8080/";
    return config;
}

TEST(ReconnectPolicyTest, DelayGrowsExponentiallyAndIsCapped) {
    ReconnectPolicy policy;
    policy.initial_delay = 100ms;
    policy.max_delay = 1'000ms;
    policy.backoff_multiplier = 2.0;

    EXPECT_EQ(policy.delay_for_attempt(0), 100ms);
    EXPECT_EQ(policy.delay_for_attempt(1), 100ms);
    EXPECT_EQ(policy.delay_for_attempt(2), 200ms);
    EXPECT_EQ(policy.delay_for_attempt(3), 400ms);
    EXPECT_EQ(policy.delay_for_attempt(4), 800ms);
    EXPECT_EQ(policy.delay_for_attempt(5), 1'000ms);
    EXPECT_EQ(policy.delay_for_attempt(1'000), 1'000ms);  // no overflow
}

TEST(ReconnectPolicyTest, MultiplierOfOneKeepsConstantDelay) {
    ReconnectPolicy policy;
    policy.initial_delay = 250ms;
    policy.max_delay = 10s;
    policy.backoff_multiplier = 1.0;
    EXPECT_EQ(policy.delay_for_attempt(1), 250ms);
    EXPECT_EQ(policy.delay_for_attempt(7), 250ms);
}

TEST(ReconnectPolicyTest, InitialDelayAboveMaxIsClamped) {
    ReconnectPolicy policy;
    policy.initial_delay = 5s;
    policy.max_delay = 1s;
    EXPECT_EQ(policy.delay_for_attempt(1), 1s);
}

TEST(ClientConfigTest, DefaultsAreValid) {
    const auto config = valid_config();
    EXPECT_TRUE(config.validate().has_value());
}

TEST(ClientConfigTest, UrlErrorsPropagate) {
    auto config = valid_config();
    config.url = "wss://secure.example.com";
    const auto result = config.validate();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::UnsupportedScheme);
}

struct InvalidCase {
    const char* name;
    void (*mutate)(ClientConfig&);
    const char* expected_message;
};

class ClientConfigInvalidTest : public ::testing::TestWithParam<InvalidCase> {};

TEST_P(ClientConfigInvalidTest, IsRejected) {
    auto config = valid_config();
    GetParam().mutate(config);
    const auto result = config.validate();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidConfig);
    EXPECT_THAT(result.error().message, HasSubstr(GetParam().expected_message));
}

INSTANTIATE_TEST_SUITE_P(
    AllFields, ClientConfigInvalidTest,
    ::testing::Values(
        InvalidCase{"connect_timeout", [](ClientConfig& c) { c.connect_timeout = 0ms; }, "connect_timeout"},
        InvalidCase{"ping_interval", [](ClientConfig& c) { c.ping_interval = -1ms; }, "ping_interval"},
        InvalidCase{"pong_timeout", [](ClientConfig& c) { c.pong_timeout = 0ms; }, "pong_timeout"},
        InvalidCase{"initial_delay", [](ClientConfig& c) { c.reconnect.initial_delay = -1ms; },
                    "initial_delay"},
        InvalidCase{"max_delay",
                    [](ClientConfig& c) {
                        c.reconnect.initial_delay = 2s;
                        c.reconnect.max_delay = 1s;
                    },
                    "max_delay"},
        InvalidCase{"multiplier_below_one", [](ClientConfig& c) { c.reconnect.backoff_multiplier = 0.5; },
                    "backoff_multiplier"},
        InvalidCase{"multiplier_nan",
                    [](ClientConfig& c) {
                        c.reconnect.backoff_multiplier = std::numeric_limits<double>::quiet_NaN();
                    },
                    "backoff_multiplier"},
        InvalidCase{"max_attempts_zero", [](ClientConfig& c) { c.reconnect.max_attempts = 0; },
                    "max_attempts"},
        InvalidCase{"queue_capacity", [](ClientConfig& c) { c.receive_queue_capacity = 0; },
                    "receive_queue_capacity"},
        InvalidCase{"max_message_size", [](ClientConfig& c) { c.max_message_size = 0; }, "max_message_size"}),
    [](const ::testing::TestParamInfo<InvalidCase>& param_info) { return param_info.param.name; });

}  // namespace
}  // namespace awc
