#pragma once

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/http.hpp>

#include <deque>
#include <memory>
#include <string>

class SharedState;

namespace net = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace http = beast::http;

using tcp = net::ip::tcp;

class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
public:
    // Signaling JSON должен быть маленький; лимит защищает от memory DoS
    static constexpr std::size_t kMaxIncomingMessageBytes = 64 * 1024; // 64 KB

    // Ограничиваем исходящую очередь, чтобы медленный клиент не раздувал RAM
    static constexpr std::size_t kMaxWriteQueue = 256;

    WebSocketSession(tcp::socket socket,
        std::shared_ptr<SharedState> state,
        net::io_context& ioc);

    void run(http::request<http::string_body> req);

    void send(std::shared_ptr<std::string const> const& msg);
    void send(std::string msg);

    void close();

private:
    void on_accept(beast::error_code ec);

    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);

    void do_write();
    void on_write(beast::error_code ec, std::size_t bytes_transferred);

    void do_close(websocket::close_reason reason);
    void on_close(beast::error_code ec);
    void leave_state_once();

private:
    websocket::stream<tcp::socket> ws_;
    std::shared_ptr<SharedState> state_;

    beast::flat_buffer buffer_;

    net::strand<net::io_context::executor_type> strand_;
    std::deque<std::shared_ptr<std::string const>> write_queue_;

    bool closing_{ false };
};
