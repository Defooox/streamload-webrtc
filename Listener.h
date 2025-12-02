#pragma once

#include <boost/beast/core.hpp>
#include <boost/asio/strand.hpp>
#include <memory>

namespace beast = boost::beast;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

class SharedState; 

class Listener : public std::enable_shared_from_this<Listener> {
    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    std::shared_ptr<SharedState> state_;

    void fail(beast::error_code ec, char const* what);
    void do_accept();
    void on_accept(beast::error_code ec, tcp::socket socket);

public:
    Listener(
        net::io_context& ioc,
        tcp::endpoint endpoint,
        std::shared_ptr<SharedState> const& state);

    void run();
};