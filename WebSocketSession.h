#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/strand.hpp>
#include <memory>
#include <string>
#include <vector>
#include <queue>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

class SharedState;

class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    std::shared_ptr<SharedState> state_;
    std::queue<std::shared_ptr<std::string const>> write_queue_;

    void fail(beast::error_code ec, char const* what);
    void on_accept(beast::error_code ec);
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void on_write(beast::error_code ec, std::size_t bytes_transferred);

public:
    WebSocketSession(tcp::socket&& socket, std::shared_ptr<SharedState> const& state);
    ~WebSocketSession();


    template<class Body, class Allocator>
    void run(http::request<Body, http::basic_fields<Allocator>> req);

    void send(std::shared_ptr<std::string const> const& ss);
};

template<class Body, class Allocator>
void WebSocketSession::run(http::request<Body, http::basic_fields<Allocator>> req) {

    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws_.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& res) {
            res.set(http::field::server, std::string(BOOST_BEAST_VERSION_STRING) + " websocket-server-async");
        }));


    ws_.async_accept(
        req,
        beast::bind_front_handler(
            &WebSocketSession::on_accept,
            shared_from_this()));
}