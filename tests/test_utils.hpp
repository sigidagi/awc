#pragma once

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <functional>

namespace awc::test {

namespace asio = boost::asio;
using namespace std::chrono_literals;

/// Runs `body` as a coroutine on `ioc` with a watchdog. The io_context is stopped
/// as soon as the body completes so leftover work cannot hang the test.
inline void run_coroutine_test(asio::io_context& ioc, std::function<asio::awaitable<void>()> body,
                               std::chrono::milliseconds timeout = 15s) {
    std::exception_ptr failure;
    asio::steady_timer watchdog(ioc, timeout);
    watchdog.async_wait([&](const boost::system::error_code& ec) {
        if (!ec) {
            ADD_FAILURE() << "test body did not finish within " << timeout.count() << " ms";
            ioc.stop();
        }
    });
    asio::co_spawn(ioc, std::move(body), [&](const std::exception_ptr& ex) {
        failure = ex;
        watchdog.cancel();
        ioc.stop();
    });
    ioc.run();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

inline asio::awaitable<void> sleep_for(std::chrono::milliseconds duration) {
    asio::steady_timer timer(co_await asio::this_coro::executor, duration);
    co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
}

/// Polls `predicate` until it is true; returns false when `timeout` elapsed first.
inline asio::awaitable<bool> wait_until(std::function<bool()> predicate,
                                        std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            co_return false;
        }
        co_await sleep_for(5ms);
    }
    co_return true;
}

}  // namespace awc::test

// gtest's ASSERT_* macros contain a plain `return`, which is illegal inside a
// coroutine. These variants record the failure and `co_return` instead. They can
// be used inside `awaitable<void>` bodies only, and support `<< "message"`.
#define CO_ASSERT_TRUE(condition) \
    if (condition) {              \
    } else                        \
        co_return ADD_FAILURE() << "Expected: " #condition " to be true "

#define CO_ASSERT_FALSE(condition) \
    if (!(condition)) {            \
    } else                         \
        co_return ADD_FAILURE() << "Expected: " #condition " to be false "
