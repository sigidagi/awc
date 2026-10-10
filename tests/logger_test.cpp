#include "async_websocket_client/logger.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <sstream>

namespace awc::log {
namespace {

using ::testing::HasSubstr;

TEST(LoggerTest, DefaultLoggerIsCreatedLazilyAndRegistered) {
    set(nullptr);
    const auto logger = get();
    ASSERT_NE(logger, nullptr);
    EXPECT_EQ(logger->name(), logger_name);
    EXPECT_EQ(get(), logger);                     // cached
    EXPECT_EQ(spdlog::get(logger_name), logger);  // registered with spdlog

    // After reset the registered instance is reused instead of a new one being created.
    set(nullptr);
    EXPECT_EQ(get(), logger);
}

TEST(LoggerTest, CustomLoggerReceivesOutput) {
    std::ostringstream stream;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(stream);
    auto custom = std::make_shared<spdlog::logger>("awc-test", sink);
    custom->set_pattern("%v");
    set(custom);

    EXPECT_EQ(get(), custom);
    get()->info("hello {}", 42);
    EXPECT_THAT(stream.str(), HasSubstr("hello 42"));

    set(nullptr);
}

TEST(LoggerTest, SetLevelAppliesToLibraryLogger) {
    set(nullptr);
    set_level(spdlog::level::trace);
    EXPECT_EQ(get()->level(), spdlog::level::trace);
    set_level(spdlog::level::warn);
    EXPECT_EQ(get()->level(), spdlog::level::warn);
}

}  // namespace
}  // namespace awc::log
