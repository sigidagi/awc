#include "async_websocket_client/client.hpp"

#include "async_websocket_client/logger.hpp"
#include "async_websocket_client/url.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <exception>
#include <iterator>
#include <ostream>
#include <utility>

namespace awc {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using asio::ip::tcp;

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

std::string_view to_string(ConnectionState state) noexcept {
    switch (state) {
        case ConnectionState::Disconnected:
            return "Disconnected";
        case ConnectionState::Connecting:
            return "Connecting";
        case ConnectionState::Connected:
            return "Connected";
        case ConnectionState::Reconnecting:
            return "Reconnecting";
        case ConnectionState::Closing:
            return "Closing";
        case ConnectionState::Closed:
            return "Closed";
        case ConnectionState::Failed:
            return "Failed";
    }
    return "Unknown";
}

std::ostream& operator<<(std::ostream& os, ConnectionState state) {
    return os << to_string(state);
}

Message Message::binary(std::span<const std::byte> payload) {
    std::string bytes;
    bytes.reserve(payload.size());
    std::ranges::transform(payload, std::back_inserter(bytes),
                           [](std::byte b) { return static_cast<char>(b); });
    return {.payload = std::move(bytes), .is_text = false};
}

namespace detail {

namespace {

constexpr auto use_nothrow = asio::as_tuple(asio::use_awaitable);

/// Condition-variable-like primitive for single-threaded coroutine code.
/// Always wait in a loop on a predicate: `while (!ready) co_await event.wait();`
class AsyncEvent {
public:
    explicit AsyncEvent(const asio::any_io_executor& executor)
        : timer_(executor, asio::steady_timer::time_point::max()) {}

    asio::awaitable<void> wait() { co_await timer_.async_wait(use_nothrow); }
    void notify_all() { timer_.cancel(); }

private:
    asio::steady_timer timer_;
};

}  // namespace

// ---------------------------------------------------------------------------
// ClientImpl
// ---------------------------------------------------------------------------

class ClientImpl : public std::enable_shared_from_this<ClientImpl> {
public:
    using Stream = websocket::stream<beast::tcp_stream>;
    using InboundQueue = asio::experimental::channel<void(boost::system::error_code, Message)>;
    using WriteLock = asio::experimental::channel<void(boost::system::error_code)>;

    ClientImpl(asio::io_context& ioc, ClientConfig config)
        : executor_(ioc.get_executor()),
          config_(std::move(config)),
          logger_(log::get()),
          resolver_(executor_),
          reconnect_timer_(executor_),
          ping_timer_(executor_),
          connect_event_(executor_),
          session_event_(executor_),
          loop_done_event_(executor_),
          inbound_(executor_, config_.receive_queue_capacity == 0 ? 1 : config_.receive_queue_capacity),
          write_lock_(executor_, 1) {}

    // ---- public API (mirrors WebSocketClient) ------------------------------

    asio::awaitable<Result<void>> connect() {
        if (loop_running_) {
            co_return fail(ErrorCode::InvalidState,
                           "connect() called while client is " + std::string(to_string(state_)));
        }
        if (auto valid = config_.validate(); !valid) {
            co_return std::unexpected(valid.error());
        }
        url_ = *parse_url(config_.url);

        stop_requested_ = false;
        last_error_.reset();
        inbound_.reset();
        loop_running_ = true;
        set_state(ConnectionState::Connecting);

        const auto established_before = stats_.connections_established;
        asio::co_spawn(executor_, run_loop(), [self = shared_from_this()](const std::exception_ptr& ex) {
            self->on_loop_finished(ex);
        });

        while (loop_running_ && stats_.connections_established == established_before) {
            co_await connect_event_.wait();
        }
        if (stats_.connections_established > established_before) {
            co_return Result<void>{};
        }
        co_return std::unexpected(
            last_error_.value_or(make_error(ErrorCode::Cancelled, "client was closed before connecting")));
    }

    asio::awaitable<Result<void>> send(Message message) {
        if (state_ != ConnectionState::Connected) {
            co_return fail(ErrorCode::NotConnected,
                           "cannot send while client is " + std::string(to_string(state_)));
        }
        auto stream = stream_;

        // Beast allows only one outstanding write; serialise concurrent senders.
        co_await write_lock_.async_send(boost::system::error_code{}, use_nothrow);
        stream->text(message.is_text);
        auto [ec, bytes] = co_await stream->async_write(asio::buffer(message.payload), use_nothrow);
        write_lock_.try_receive([](boost::system::error_code) {});

        if (ec) {
            logger_->warn("[{}] write of {} bytes failed: {}", config_.url, message.payload.size(),
                          ec.message());
            if (ec == asio::error::operation_aborted) {
                co_return fail(ErrorCode::ConnectionClosed, "connection closed while sending", ec);
            }
            co_return fail(ErrorCode::SendFailed, "failed to send message", ec);
        }
        ++stats_.messages_sent;
        logger_->trace("[{}] sent {} message of {} bytes", config_.url, message.is_text ? "text" : "binary",
                       bytes);
        co_return Result<void>{};
    }

    asio::awaitable<Result<Message>> receive() {
        auto [ec, message] = co_await inbound_.async_receive(use_nothrow);
        if (ec) {
            std::string reason = "client is closed";
            if (last_error_) {
                reason += ", last error: " + last_error_->to_string();
            }
            co_return fail(ErrorCode::ConnectionClosed, std::move(reason), ec);
        }
        co_return message;
    }

    asio::awaitable<void> close() {
        if (!loop_running_) {
            co_return;
        }
        stop_requested_ = true;

        switch (state_) {
            case ConnectionState::Connected: {
                set_state(ConnectionState::Closing);
                auto stream = stream_;
                logger_->debug("[{}] sending close frame", config_.url);
                // The pending read completes with websocket::error::closed once the peer
                // answers; Beast's handshake_timeout bounds the wait.
                auto [ec] = co_await stream->async_close(websocket::close_code::normal, use_nothrow);
                if (ec) {
                    logger_->debug("[{}] close handshake ended with: {}", config_.url, ec.message());
                }
                break;
            }
            case ConnectionState::Connecting:
            case ConnectionState::Reconnecting:
                cancel_pending_connect();
                break;
            default:
                break;
        }

        while (loop_running_) {
            co_await loop_done_event_.wait();
        }
    }

    /// Called from the facade destructor: stop everything, drop user callbacks.
    void abort() noexcept {
        stop_requested_ = true;
        state_handler_ = nullptr;
        error_handler_ = nullptr;
        try {
            cancel_pending_connect();
            ping_timer_.cancel();
            if (stream_) {
                beast::get_lowest_layer(*stream_).close();
            }
            inbound_.close();
        } catch (const std::exception& ex) {  // LCOV_EXCL_START - cancelling never fails in practice
            logger_->error("[{}] abort failed: {}", config_.url, ex.what());
        }
    }  // LCOV_EXCL_STOP

    [[nodiscard]] ConnectionState state() const noexcept { return state_; }
    [[nodiscard]] Stats stats() const noexcept { return stats_; }
    [[nodiscard]] const ClientConfig& config() const noexcept { return config_; }
    void on_state_change(WebSocketClient::StateHandler handler) { state_handler_ = std::move(handler); }
    void on_error(WebSocketClient::ErrorHandler handler) { error_handler_ = std::move(handler); }

private:
    // ---- connection loop ---------------------------------------------------

    asio::awaitable<void> run_loop() {
        bool first_attempt = true;
        unsigned reconnect_round = 0;

        while (!stop_requested_) {
            if (!first_attempt) {
                if (!config_.reconnect.enabled) {
                    logger_->info("[{}] reconnect disabled, giving up", config_.url);
                    set_state(ConnectionState::Disconnected);
                    break;
                }
                const auto& max_attempts = config_.reconnect.max_attempts;
                if (max_attempts && reconnect_round >= *max_attempts) {
                    report_error(make_error(ErrorCode::ReconnectExhausted,
                                            "giving up after " + std::to_string(reconnect_round) +
                                                " reconnect attempts, last error: " +
                                                (last_error_ ? last_error_->to_string() : "none")));
                    set_state(ConnectionState::Failed);
                    break;
                }
                ++reconnect_round;
                set_state(ConnectionState::Reconnecting);
                const auto delay = config_.reconnect.delay_for_attempt(reconnect_round);
                logger_->info("[{}] reconnect attempt {} in {} ms", config_.url, reconnect_round,
                              delay.count());
                reconnect_timer_.expires_after(delay);
                co_await reconnect_timer_.async_wait(use_nothrow);
                if (stop_requested_) {
                    break;
                }
            }
            first_attempt = false;

            ++stats_.connect_attempts;
            if (auto connected = co_await connect_once(); !connected) {
                report_error(connected.error());
                continue;
            }

            reconnect_round = 0;
            ++stats_.connections_established;
            logger_->info("[{}] connected", config_.url);
            set_state(ConnectionState::Connected);

            const Error cause = co_await run_session();
            ++stats_.disconnections;
            if (stop_requested_) {
                logger_->info("[{}] connection closed", config_.url);
            } else {
                logger_->info("[{}] disconnected: {}", config_.url, cause.to_string());
                report_error(cause);
            }
        }

        if (stop_requested_) {
            set_state(ConnectionState::Closed);
        }
    }

    void on_loop_finished(const std::exception_ptr& exception) {
        if (exception) {  // LCOV_EXCL_START - defensive, all awaited operations are nothrow
            try {
                std::rethrow_exception(exception);
            } catch (const std::exception& ex) {
                logger_->critical("[{}] connection loop terminated by exception: {}", config_.url, ex.what());
            }
            set_state(ConnectionState::Failed);
        }  // LCOV_EXCL_STOP
        loop_running_ = false;
        stream_.reset();
        inbound_.close();  // buffered messages stay receivable; further receives fail
        loop_done_event_.notify_all();
        connect_event_.notify_all();
    }

    asio::awaitable<Result<void>> connect_once() {
        auto stream = std::make_shared<Stream>(executor_);
        stream_ = stream;

        stream->read_message_max(config_.max_message_size);
        stream->set_option(
            websocket::stream_base::decorator([agent = config_.user_agent](websocket::request_type& request) {
                request.set(beast::http::field::user_agent, agent);
            }));
        stream->control_callback([weak = weak_from_this()](websocket::frame_type kind, beast::string_view) {
            if (auto self = weak.lock()) {
                self->on_control_frame(kind);
            }
        });

        logger_->debug("[{}] resolving {}:{}", config_.url, url_.host, url_.port);
        auto [resolve_ec, endpoints] = co_await resolver_.async_resolve(url_.host, url_.port, use_nothrow);
        if (resolve_ec) {
            co_return connect_failure(ErrorCode::ResolveFailed,
                                      "cannot resolve " + url_.host + ":" + url_.port, resolve_ec);
        }

        auto& tcp_layer = beast::get_lowest_layer(*stream);
        tcp_layer.expires_after(config_.connect_timeout);
        auto [connect_ec, endpoint] = co_await tcp_layer.async_connect(endpoints, use_nothrow);
        if (connect_ec) {
            const auto code =
                connect_ec == beast::error::timeout ? ErrorCode::ConnectTimeout : ErrorCode::ConnectFailed;
            co_return connect_failure(code, "cannot connect to " + url_.host + ":" + url_.port, connect_ec);
        }
        tcp_layer.expires_never();  // the websocket stream manages its own timeouts from here on
        boost::system::error_code ignored;
        tcp_layer.socket().set_option(tcp::no_delay(true), ignored);

        stream->set_option(websocket::stream_base::timeout{
            .handshake_timeout = config_.connect_timeout,
            .idle_timeout = websocket::stream_base::none(),
            .keep_alive_pings = false,
        });

        logger_->debug("[{}] connected to {}, performing handshake", config_.url,
                       endpoint.address().to_string());
        auto [handshake_ec] = co_await stream->async_handshake(url_.host_header, url_.target, use_nothrow);
        if (handshake_ec) {
            const auto code = handshake_ec == beast::error::timeout ? ErrorCode::HandshakeTimeout
                                                                    : ErrorCode::HandshakeFailed;
            co_return connect_failure(code, "websocket handshake with " + url_.host_header + " failed",
                                      handshake_ec);
        }
        co_return Result<void>{};
    }

    std::unexpected<Error> connect_failure(ErrorCode code, std::string message,
                                           boost::system::error_code ec) const {
        if (stop_requested_) {
            return fail(ErrorCode::Cancelled, "connection attempt cancelled", ec);
        }
        return fail(code, std::move(message), ec);
    }

    void cancel_pending_connect() {
        resolver_.cancel();
        reconnect_timer_.cancel();
        if (stream_) {
            beast::get_lowest_layer(*stream_).close();
        }
    }

    // ---- an established session: reader + pinger ---------------------------

    asio::awaitable<Error> run_session() {
        auto stream = stream_;
        session_closing_ = false;
        awaiting_pong_ = false;
        reader_blocked_ = false;
        session_cause_.reset();
        components_running_ = 2;

        auto on_component_done = [self = shared_from_this(), stream](const std::exception_ptr&, Error error) {
            self->on_session_component_finished(stream, std::move(error));
        };
        asio::co_spawn(executor_, read_loop(stream), on_component_done);
        asio::co_spawn(executor_, ping_loop(stream), on_component_done);

        while (components_running_ > 0) {
            co_await session_event_.wait();
        }
        co_return session_cause_.value_or(make_error(ErrorCode::Cancelled, "session ended"));
    }

    void on_session_component_finished(const std::shared_ptr<Stream>& stream, Error error) {
        if (!session_cause_) {
            session_cause_ = std::move(error);
        }
        teardown_session(stream);
        --components_running_;
        session_event_.notify_all();
    }

    void teardown_session(const std::shared_ptr<Stream>& stream) {
        if (session_closing_) {
            return;
        }
        session_closing_ = true;
        ping_timer_.cancel();
        beast::get_lowest_layer(*stream).close();  // aborts every pending operation on the stream
    }

    asio::awaitable<Error> read_loop(std::shared_ptr<Stream> stream) {
        beast::flat_buffer buffer;
        for (;;) {
            auto [ec, bytes] = co_await stream->async_read(buffer, use_nothrow);
            if (ec == websocket::error::closed) {
                co_return make_error(ErrorCode::ConnectionClosed,
                                     "connection closed by peer with code " +
                                         std::to_string(stream->reason().code) + " '" +
                                         std::string(stream->reason().reason) + "'",
                                     ec);
            }
            if (ec == asio::error::operation_aborted) {
                co_return make_error(ErrorCode::Cancelled, "read aborted", ec);
            }
            if (ec) {
                co_return make_error(ErrorCode::ReadFailed, "failed to read message", ec);
            }

            Message message{.payload = beast::buffers_to_string(buffer.data()),
                            .is_text = stream->got_text()};
            buffer.clear();
            ++stats_.messages_received;
            logger_->trace("[{}] received {} message of {} bytes", config_.url,
                           message.is_text ? "text" : "binary", bytes);

            // Apply back-pressure when the application does not keep up. While we are
            // blocked here no pongs can be processed, which the ping loop tolerates.
            reader_blocked_ = true;
            auto [queue_ec] =
                co_await inbound_.async_send(boost::system::error_code{}, std::move(message), use_nothrow);
            reader_blocked_ = false;
            if (queue_ec) {
                co_return make_error(ErrorCode::Cancelled, "receive queue closed", queue_ec);
            }
        }
    }

    asio::awaitable<Error> ping_loop(std::shared_ptr<Stream> stream) {
        const auto cancelled = [] { return make_error(ErrorCode::Cancelled, "ping loop stopped"); };

        if (config_.ping_interval == std::chrono::milliseconds::zero()) {
            ping_timer_.expires_at(asio::steady_timer::time_point::max());
            co_await ping_timer_.async_wait(use_nothrow);
            co_return cancelled();
        }

        std::uint64_t sequence = 0;
        for (;;) {
            ping_timer_.expires_after(config_.ping_interval);
            co_await ping_timer_.async_wait(use_nothrow);
            if (session_closing_) {
                co_return cancelled();
            }

            awaiting_pong_ = true;
            ping_sent_at_ = std::chrono::steady_clock::now();
            const auto payload = "awc-" + std::to_string(++sequence);
            auto [ping_ec] = co_await stream->async_ping(websocket::ping_data{payload}, use_nothrow);
            if (ping_ec) {
                if (session_closing_) {
                    co_return cancelled();
                }
                co_return make_error(ErrorCode::SendFailed, "failed to send ping", ping_ec);
            }
            ++stats_.pings_sent;
            logger_->trace("[{}] ping {} sent", config_.url, payload);

            ping_timer_.expires_after(config_.pong_timeout);
            co_await ping_timer_.async_wait(use_nothrow);  // cut short by a pong
            if (session_closing_) {
                co_return cancelled();
            }
            if (awaiting_pong_ && !reader_blocked_) {
                co_return make_error(
                    ErrorCode::PongTimeout,
                    "no pong received within " + std::to_string(config_.pong_timeout.count()) + " ms");
            }
        }
    }

    void on_control_frame(websocket::frame_type kind) {
        if (kind != websocket::frame_type::pong || !awaiting_pong_) {
            return;
        }
        awaiting_pong_ = false;
        ++stats_.pongs_received;
        const auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - ping_sent_at_);
        stats_.last_ping_rtt = rtt;
        logger_->trace("[{}] pong received, rtt {} ms", config_.url, rtt.count());
        ping_timer_.cancel();  // wake the ping loop so the next interval starts right away
    }

    // ---- bookkeeping -------------------------------------------------------

    void set_state(ConnectionState next) {
        if (next == state_) {
            return;
        }
        const auto previous = std::exchange(state_, next);
        logger_->debug("[{}] state {} -> {}", config_.url, to_string(previous), to_string(next));
        connect_event_.notify_all();
        if (state_handler_) {
            state_handler_(previous, next);
        }
    }

    void report_error(const Error& error) {
        last_error_ = error;
        logger_->warn("[{}] {}", config_.url, error.to_string());
        if (error_handler_) {
            error_handler_(error);
        }
    }

    asio::any_io_executor executor_;
    ClientConfig config_;
    std::shared_ptr<spdlog::logger> logger_;
    Url url_;

    tcp::resolver resolver_;
    asio::steady_timer reconnect_timer_;
    asio::steady_timer ping_timer_;
    AsyncEvent connect_event_;
    AsyncEvent session_event_;
    AsyncEvent loop_done_event_;
    InboundQueue inbound_;
    WriteLock write_lock_;
    std::shared_ptr<Stream> stream_;

    ConnectionState state_ = ConnectionState::Disconnected;
    bool loop_running_ = false;
    bool stop_requested_ = false;
    bool session_closing_ = false;
    bool awaiting_pong_ = false;
    bool reader_blocked_ = false;
    int components_running_ = 0;
    std::optional<Error> session_cause_;
    std::optional<Error> last_error_;
    std::chrono::steady_clock::time_point ping_sent_at_;
    Stats stats_;

    WebSocketClient::StateHandler state_handler_;
    WebSocketClient::ErrorHandler error_handler_;
};

}  // namespace detail

// ---------------------------------------------------------------------------
// WebSocketClient facade
// ---------------------------------------------------------------------------

WebSocketClient::WebSocketClient(boost::asio::io_context& ioc, ClientConfig config)
    : impl_(std::make_shared<detail::ClientImpl>(ioc, std::move(config))) {}

WebSocketClient::~WebSocketClient() {
    impl_->abort();
}

asio::awaitable<Result<void>> WebSocketClient::connect() {
    return impl_->connect();
}

asio::awaitable<Result<void>> WebSocketClient::send(Message message) {
    return impl_->send(std::move(message));
}

asio::awaitable<Result<void>> WebSocketClient::send_text(std::string text) {
    return impl_->send(Message::text(std::move(text)));
}

asio::awaitable<Result<void>> WebSocketClient::send_binary(std::span<const std::byte> data) {
    return impl_->send(Message::binary(data));
}

asio::awaitable<Result<Message>> WebSocketClient::receive() {
    return impl_->receive();
}

asio::awaitable<void> WebSocketClient::close() {
    return impl_->close();
}

ConnectionState WebSocketClient::state() const noexcept {
    return impl_->state();
}

bool WebSocketClient::is_connected() const noexcept {
    return impl_->state() == ConnectionState::Connected;
}

Stats WebSocketClient::stats() const noexcept {
    return impl_->stats();
}

const ClientConfig& WebSocketClient::config() const noexcept {
    return impl_->config();
}

void WebSocketClient::on_state_change(StateHandler handler) {
    impl_->on_state_change(std::move(handler));
}

void WebSocketClient::on_error(ErrorHandler handler) {
    impl_->on_error(std::move(handler));
}

}  // namespace awc
