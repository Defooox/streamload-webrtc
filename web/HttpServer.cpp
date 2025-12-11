// HttpServer.cpp - Fixed for large file uploads
#include "HttpServer.h"
#include <boost/beast/version.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class HttpSession : public std::enable_shared_from_this<HttpSession> {
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    std::unique_ptr<http::request_parser<http::string_body>> parser_;
    http::request<http::string_body> req_;
    std::shared_ptr<void> res_;
    std::string upload_path_;
    std::string web_root_;

public:
    HttpSession(
        tcp::socket&& socket,
        std::string upload_path,
        std::string web_root)
        : stream_(std::move(socket))
        , upload_path_(std::move(upload_path))
        , web_root_(std::move(web_root))
    {
    }

    void run() {
        net::dispatch(stream_.get_executor(),
            beast::bind_front_handler(
                &HttpSession::do_read,
                shared_from_this()));
    }

    void do_read() {
        // Create new parser with increased body limit
        parser_ = std::make_unique<http::request_parser<http::string_body>>();
        parser_->body_limit(500 * 1024 * 1024); // 500MB limit

        stream_.expires_after(std::chrono::seconds(300)); // 5 minutes for large files
        buffer_.clear();

        http::async_read(stream_, buffer_, *parser_,
            beast::bind_front_handler(
                &HttpSession::on_read,
                shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);

        if (ec == http::error::end_of_stream)
            return do_close();

        if (ec) {
            std::cerr << "[HTTP ERR] Read error: " << ec.message() << std::endl;
            return fail(ec, "read");
        }

        // Extract request from parser
        req_ = parser_->release();
        parser_.reset(); // Free parser memory

        handle_request();
    }

    void handle_request() {
        auto const bad_request = [&](beast::string_view why) {
            auto res = std::make_shared<http::response<http::string_body>>(
                http::status::bad_request, req_.version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::content_type, "text/plain");
            res->set(http::field::access_control_allow_origin, "*");
            res->keep_alive(req_.keep_alive());
            res->body() = std::string(why);
            res->prepare_payload();
            return res;
            };

        // Handle CORS preflight
        if (req_.method() == http::verb::options) {
            auto res = std::make_shared<http::response<http::string_body>>(
                http::status::ok, req_.version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::access_control_allow_origin, "*");
            res->set(http::field::access_control_allow_methods, "GET, POST, OPTIONS");
            res->set(http::field::access_control_allow_headers, "Content-Type");
            res->keep_alive(req_.keep_alive());
            res->prepare_payload();
            return send_response(res);
        }

        // Serve index.html
        if (req_.method() == http::verb::get &&
            (req_.target() == "/" || req_.target() == "/index.html")) {
            std::string path = web_root_ + "/index.html";
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                std::cerr << "[HTTP] index.html not found at: " << path << std::endl;
                return send_response(bad_request("index.html not found"));
            }

            std::string content((std::istreambuf_iterator<char>(file)),
                std::istreambuf_iterator<char>());
            auto res = std::make_shared<http::response<http::string_body>>(
                http::status::ok, req_.version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::content_type, "text/html; charset=utf-8");
            res->set(http::field::access_control_allow_origin, "*");
            res->keep_alive(req_.keep_alive());
            res->body() = content;
            res->prepare_payload();
            return send_response(res);
        }

        // Serve client.js
        if (req_.method() == http::verb::get && req_.target() == "/client.js") {
            std::string path = web_root_ + "/client.js";
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                std::cerr << "[HTTP] client.js not found at: " << path << std::endl;
                return send_response(bad_request("client.js not found"));
            }

            std::string content((std::istreambuf_iterator<char>(file)),
                std::istreambuf_iterator<char>());
            auto res = std::make_shared<http::response<http::string_body>>(
                http::status::ok, req_.version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::content_type, "application/javascript; charset=utf-8");
            res->set(http::field::access_control_allow_origin, "*");
            res->keep_alive(req_.keep_alive());
            res->body() = content;
            res->prepare_payload();
            return send_response(res);
        }

        // Handle file upload
        if (req_.method() == http::verb::post && req_.target() == "/upload") {
            std::cout << "[HTTP] ========== UPLOAD REQUEST ==========" << std::endl;
            std::cout << "[HTTP] Content-Length: " << req_[http::field::content_length] << std::endl;
            std::cout << "[HTTP] Content-Type: " << req_[http::field::content_type] << std::endl;
            std::cout << "[HTTP] Body size: " << req_.body().size() << " bytes" << std::endl;

            if (req_.body().empty()) {
                std::cerr << "[HTTP] ERROR: Empty upload body" << std::endl;
                return send_response(bad_request("Empty upload"));
            }

            std::string body = req_.body();
            std::string content_type = std::string(req_[http::field::content_type]);

            // Extract boundary from Content-Type
            std::string boundary;
            size_t boundary_pos = content_type.find("boundary=");
            if (boundary_pos != std::string::npos) {
                boundary = content_type.substr(boundary_pos + 9);
                boundary = "--" + boundary;
                std::cout << "[HTTP] Boundary: " << boundary << std::endl;
            }

            // Parse multipart data
            size_t data_start = 0;
            size_t data_end = body.size();

            if (!boundary.empty()) {
                // Find first boundary
                size_t first_boundary = body.find(boundary);
                if (first_boundary != std::string::npos) {
                    // Find the end of headers (double CRLF)
                    size_t header_end = body.find("\r\n\r\n", first_boundary);
                    if (header_end != std::string::npos) {
                        data_start = header_end + 4;
                    }
                }

                // Find closing boundary
                std::string closing_boundary = boundary + "--";
                size_t last_boundary = body.rfind(closing_boundary);
                if (last_boundary != std::string::npos && last_boundary > data_start) {
                    data_end = last_boundary;
                }
                else {
                    size_t last_regular = body.rfind("\r\n" + boundary);
                    if (last_regular != std::string::npos && last_regular > data_start) {
                        data_end = last_regular;
                    }
                }
            }

            size_t data_length = data_end - data_start;
            std::cout << "[HTTP] Data range: " << data_start << " - " << data_end << std::endl;
            std::cout << "[HTTP] Data length: " << data_length << " bytes" << std::endl;

            if (data_length == 0 || data_length > 500 * 1024 * 1024) {
                std::cerr << "[HTTP] ERROR: Invalid data length: " << data_length << std::endl;
                return send_response(bad_request("Invalid file size"));
            }

            // Generate filename
            std::string filename = "stream_" + std::to_string(std::time(nullptr)) + ".mp4";
            std::string full_path = upload_path_ + "/" + filename;

            std::cout << "[HTTP] Saving to: " << full_path << std::endl;

            // Write file
            std::ofstream file(full_path, std::ios::binary);
            if (!file) {
                std::cerr << "[HTTP] ERROR: Cannot create file: " << full_path << std::endl;
                auto res = std::make_shared<http::response<http::string_body>>(
                    http::status::internal_server_error, req_.version());
                res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
                res->set(http::field::access_control_allow_origin, "*");
                res->body() = "Cannot save file";
                res->prepare_payload();
                return send_response(res);
            }

            file.write(body.data() + data_start, data_length);
            file.close();

            std::cout << "[HTTP] SUCCESS File saved: " << full_path
                << " (" << data_length << " bytes)" << std::endl;
            std::cout << "[HTTP] ======================================" << std::endl;

            // Send success response
            json response_json;
            response_json["status"] = "ok";
            response_json["file_path"] = full_path;
            response_json["size"] = data_length;

            auto res = std::make_shared<http::response<http::string_body>>(
                http::status::ok, req_.version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::content_type, "application/json");
            res->set(http::field::access_control_allow_origin, "*");
            res->keep_alive(req_.keep_alive());
            res->body() = response_json.dump();
            res->prepare_payload();
            return send_response(res);
        }

        std::cerr << "[HTTP] Unknown request: " << req_.method_string()
            << " " << req_.target() << std::endl;
        return send_response(bad_request("Unknown request"));
    }

    template<class Body, class Fields>
    void send_response(std::shared_ptr<http::response<Body, Fields>> res) {
        res_ = res;

        http::async_write(stream_, *res,
            beast::bind_front_handler(
                &HttpSession::on_write,
                shared_from_this(),
                res->need_eof()));
    }

    void on_write(bool close, beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);

        if (ec) {
            std::cerr << "[HTTP] Write error: " << ec.message() << std::endl;
            return fail(ec, "write");
        }

        if (close) {
            return do_close();
        }

        res_ = nullptr;
        do_read();
    }

    void do_close() {
        beast::error_code ec;
        stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
    }

    void fail(beast::error_code ec, char const* what) {
        if (ec == net::error::operation_aborted) return;
        std::cerr << "[HTTP ERR] " << what << ": " << ec.message() << "\n";
    }
};

// HttpServer implementation
HttpServer::HttpServer(net::io_context& ioc, tcp::endpoint endpoint,
    std::string upload_path, std::string web_root)
    : acceptor_(net::make_strand(ioc))
    , upload_path_(std::move(upload_path))
    , web_root_(std::move(web_root))
{
    beast::error_code ec;

    acceptor_.open(endpoint.protocol(), ec);
    if (ec) {
        std::cerr << "[HTTP] Open error: " << ec.message() << std::endl;
        return;
    }

    acceptor_.set_option(net::socket_base::reuse_address(true), ec);
    if (ec) {
        std::cerr << "[HTTP] Set option error: " << ec.message() << std::endl;
    }

    acceptor_.bind(endpoint, ec);
    if (ec) {
        std::cerr << "[HTTP] Bind error: " << ec.message() << std::endl;
        return;
    }

    acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
        std::cerr << "[HTTP] Listen error: " << ec.message() << std::endl;
        return;
    }

    std::cout << "[HTTP] Server listening on " << endpoint << std::endl;
}

void HttpServer::run() {
    do_accept();
}

void HttpServer::do_accept() {
    acceptor_.async_accept(
        net::make_strand(acceptor_.get_executor()),
        beast::bind_front_handler(
            &HttpServer::on_accept,
            shared_from_this()));
}

void HttpServer::on_accept(beast::error_code ec, tcp::socket socket) {
    if (ec) {
        std::cerr << "[HTTP] Accept error: " << ec.message() << "\n";
    }
    else {
        std::cout << "[HTTP] New connection accepted" << std::endl;
        std::make_shared<HttpSession>(
            std::move(socket),
            upload_path_,
            web_root_
        )->run();
    }

    do_accept();
}