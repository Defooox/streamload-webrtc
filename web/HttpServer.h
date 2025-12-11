#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio.hpp>
#include <string>
#include <memory>
#include <filesystem>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class HttpServer : public std::enable_shared_from_this<HttpServer> {
    tcp::acceptor acceptor_;
    std::string upload_path_;
    std::string web_root_; // Changed to string for easier concatenation

    void do_accept();
    void on_accept(beast::error_code ec, tcp::socket socket);

public:
    HttpServer(net::io_context& ioc, tcp::endpoint endpoint,
        std::string upload_path, std::string web_root = "./web");

    void run();
};