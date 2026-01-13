#pragma once

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <memory>

class SharedState;

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using tcp = net::ip::tcp;

class Listener : public std::enable_shared_from_this<Listener> {
public:
    Listener(net::io_context& ioc,
        tcp::endpoint endpoint,
        std::shared_ptr<SharedState> state);

    void run();

private:
    void do_accept();
    void on_accept(beast::error_code ec, tcp::socket socket);

private:
    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    std::shared_ptr<SharedState> state_;
};
