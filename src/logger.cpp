#include "async_websocket_client/logger.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <mutex>

namespace awc::log {

namespace {

std::mutex& logger_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<spdlog::logger>& logger_slot() {
    static std::shared_ptr<spdlog::logger> logger;
    return logger;
}

}  // namespace

std::shared_ptr<spdlog::logger> get() {
    const std::scoped_lock lock(logger_mutex());
    auto& slot = logger_slot();
    if (!slot) {
        slot = spdlog::get(logger_name);
        if (!slot) {
            slot = spdlog::stderr_color_mt(logger_name);
        }
    }
    return slot;
}

void set(std::shared_ptr<spdlog::logger> logger) {
    const std::scoped_lock lock(logger_mutex());
    logger_slot() = std::move(logger);
}

void set_level(spdlog::level::level_enum level) {
    get()->set_level(level);
}

}  // namespace awc::log
