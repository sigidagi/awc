#include "test_server.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <array>

namespace awc::test {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

namespace {

constexpr auto use_nothrow = asio::as_tuple(asio::use_awaitable);

struct Session {
    explicit Session(tcp::socket socket)
        : ws(std::move(socket)), hold_timer(ws.get_executor(), asio::steady_timer::time_point::max()) {}

    websocket::stream<beast::tcp_stream> ws;
    asio::steady_timer hold_timer;
    bool handshake_done = false;

    /// Parks the session until `drop()` is called, without touching the socket.
    asio::awaitable<void> hold() { co_await hold_timer.async_wait(use_nothrow); }

    /// Reads and discards raw bytes until the peer goes away: WebSocket frames
    /// (pings, close) are swallowed without any reaction.
    asio::awaitable<void> discard_incoming() {
        std::array<char, 4096> sink{};
        for (;;) {
            auto [ec, bytes] = co_await ws.next_layer().async_read_some(asio::buffer(sink), use_nothrow);
            if (ec) {
                co_return;
            }
        }
    }

    void drop() {
        hold_timer.cancel();
        beast::get_lowest_layer(ws).close();
    }
};

}  // namespace

struct TestServer::State : std::enable_shared_from_this<TestServer::State> {
    State(asio::io_context& ioc, Behaviour initial) : acceptor(ioc), behaviour(initial) {}

    tcp::acceptor acceptor;
    std::uint16_t port = 0;
    Behaviour behaviour;
    std::vector<std::shared_ptr<Session>> sessions;
    std::size_t sessions_accepted = 0;
    std::size_t messages_received = 0;
    std::vector<std::string> user_agents;

    asio::awaitable<void> accept_loop() {
        for (;;) {
            auto [ec, socket] = co_await acceptor.async_accept(use_nothrow);
            if (ec) {
                co_return;
            }
            ++sessions_accepted;
            auto session = std::make_shared<Session>(std::move(socket));
            sessions.push_back(session);
            asio::co_spawn(acceptor.get_executor(), run_session(session, behaviour),
                           [self = shared_from_this(), session](const std::exception_ptr&) {
                               std::erase(self->sessions, session);
                           });
        }
    }

    asio::awaitable<void> run_session(std::shared_ptr<Session> session, Behaviour mode) {
        auto& ws = session->ws;

        if (mode == Behaviour::SilentTcp) {
            co_await session->discard_incoming();
            co_return;
        }

        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        auto [read_ec, read_bytes] = co_await http::async_read(ws.next_layer(), buffer, request, use_nothrow);
        if (read_ec) {
            co_return;
        }
        user_agents.emplace_back(request[http::field::user_agent]);

        if (mode == Behaviour::RejectHandshake) {
            http::response<http::string_body> response{http::status::forbidden, request.version()};
            response.body() = "go away";
            response.prepare_payload();
            co_await http::async_write(ws.next_layer(), response, use_nothrow);
            session->drop();
            co_return;
        }

        auto [accept_ec] = co_await ws.async_accept(request, use_nothrow);
        if (accept_ec) {
            co_return;
        }
        session->handshake_done = true;

        switch (mode) {
            case Behaviour::DropAfterHandshake:
                session->drop();
                co_return;
            case Behaviour::CloseAfterHandshake:
                co_await ws.async_close(websocket::close_reason{websocket::close_code::going_away, "bye"},
                                        use_nothrow);
                co_return;
            case Behaviour::NeverRead:
                co_await session->hold();
                co_return;
            case Behaviour::IgnoreFrames:
                co_await session->discard_incoming();
                co_return;
            default:
                break;
        }

        for (;;) {
            auto [ec, bytes] = co_await ws.async_read(buffer, use_nothrow);
            if (ec) {
                co_return;
            }
            ++messages_received;
            ws.text(ws.got_text());
            auto [write_ec, written] = co_await ws.async_write(buffer.data(), use_nothrow);
            buffer.clear();
            if (write_ec) {
                co_return;
            }
        }
    }
};

TestServer::TestServer(asio::io_context& ioc, Behaviour behaviour)
    : state_(std::make_shared<State>(ioc, behaviour)) {
    start();
}

TestServer::~TestServer() {
    try {
        stop();
    } catch (...) {  // NOLINT(bugprone-empty-catch) - best effort cleanup
    }
}

void TestServer::start() {
    auto& acceptor = state_->acceptor;
    if (acceptor.is_open()) {
        return;
    }
    const tcp::endpoint endpoint{asio::ip::make_address("127.0.0.1"), state_->port};
    acceptor.open(endpoint.protocol());
    acceptor.set_option(asio::socket_base::reuse_address(true));
    acceptor.bind(endpoint);
    acceptor.listen();
    state_->port = acceptor.local_endpoint().port();
    asio::co_spawn(acceptor.get_executor(), state_->accept_loop(), asio::detached);
}

void TestServer::stop() {
    if (state_->acceptor.is_open()) {
        state_->acceptor.close();
    }
    drop_all();
}

std::uint16_t TestServer::port() const noexcept {
    return state_->port;
}

std::string TestServer::url(std::string_view target) const {
    return "ws://127.0.0.1:" + std::to_string(port()) + std::string(target);
}

void TestServer::set_behaviour(Behaviour behaviour) noexcept {
    state_->behaviour = behaviour;
}

std::size_t TestServer::sessions_accepted() const noexcept {
    return state_->sessions_accepted;
}

std::size_t TestServer::open_sessions() const noexcept {
    return state_->sessions.size();
}

std::size_t TestServer::messages_received() const noexcept {
    return state_->messages_received;
}

const std::vector<std::string>& TestServer::user_agents() const noexcept {
    return state_->user_agents;
}

void TestServer::drop_all() {
    for (const auto& session : std::vector(state_->sessions)) {
        session->drop();
    }
}

asio::awaitable<void> TestServer::broadcast(Message message) {
    for (const auto& session : std::vector(state_->sessions)) {
        if (!session->handshake_done) {
            continue;
        }
        session->ws.text(message.is_text);
        co_await session->ws.async_write(asio::buffer(message.payload), use_nothrow);
    }
}

}  // namespace awc::test
