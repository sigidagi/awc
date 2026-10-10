#pragma once

#include <spdlog/spdlog.h>

#include <memory>

namespace awc::log {

/// Name of the spdlog logger used by the library.
inline constexpr const char* logger_name = "awc";

/// Returns the library logger, creating a default (stderr, coloured) one on first use.
[[nodiscard]] std::shared_ptr<spdlog::logger> get();

/// Replaces the library logger (e.g. to route output into your own sinks).
/// Passing `nullptr` restores the default logger on the next `get()`.
void set(std::shared_ptr<spdlog::logger> logger);

/// Convenience: sets the level of the library logger.
void set_level(spdlog::level::level_enum level);

}  // namespace awc::log
