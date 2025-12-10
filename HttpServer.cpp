#include "HttpServer.h"
#include <boost/beast/version.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ============================================================================
// HttpSession: Handles an individual connection safely
// ============================================================================
class HttpSession : public std::enable_shared_from_this<HttpSession> {
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
    std::shared_ptr<void> res_; // Keeps the response alive during async_write
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
        // We need to be executing within a strand to perform async operations safely
        net::dispatch(stream_.get_executor(),
            beast::bind_front_handler(
                &HttpSession::do_read,
                shared_from_this()));
    }

    void do_read() {
        // Make the request empty before reading,
        // otherwise the operation behavior is undefined.
        req_ = {};
        stream_.expires_after(std::chrono::seconds(30));

        http::async_read(stream_, buffer_, req_,
            beast::bind_front_handler(
                &HttpSession::on_read,
                shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);

        // This means they closed the connection
        if (ec == http::error::end_of_stream)
            return do_close();

        if (ec)
            return fail(ec, "read");

        handle_request();
    }

    void handle_request() {
        // Helper to create a bad request response
        auto const bad_request = [&](beast::string_view why) {
            auto res = std::make_shared<http::response<http::string_body>>(http::status::bad_request, req_.version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::content_type, "text/plain");
            res->keep_alive(req_.keep_alive());
            res->body() = std::string(why);
            res->prepare_payload();
            return res;
            };

        // --- 1. Serve index.html ---
        if (req_.method() == http::verb::get && (req_.target() == "/" || req_.target() == "/index.html")) {
            std::string path = web_root_ + "/index.html";
            std::ifstream file(path);
            if (!file) {
                return send_response(bad_request("index.html not found in " + web_root_));
            }

            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            auto res = std::make_shared<http::response<http::string_body>>(http::status::ok, req_.version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::content_type, "text/html");
            res->keep_alive(req_.keep_alive());
            res->body() = content;
            res->prepare_payload();
            return send_response(res);
        }

        // --- 2. Serve client.js ---
        if (req_.method() == http::verb::get && req_.target() == "/client.js") {
            std::string path = web_root_ + "/client.js";
            std::ifstream file(path);
            if (!file) {
                return send_response(bad_request("client.js not found"));
            }

            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            auto res = std::make_shared<http::response<http::string_body>>(http::status::ok, req_.version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::content_type, "application/javascript");
            res->keep_alive(req_.keep_alive());
            res->body() = content;
            res->prepare_payload();
            return send_response(res);
        }

        // --- 3. Handle Upload ---
        if (req_.method() == http::verb::post && req_.target() == "/upload") {
            std::string body = req_.body();
            std::string filename = "stream_" + std::to_string(std::time(nullptr)) + ".mp4";

            // Simple multipart parsing (as per your original code)
            size_t pos = body.find("\r\n\r\n");
            if (pos == std::string::npos) {
                return send_response(bad_request("Invalid multipart data"));
            }

            std::string full_path = upload_path_ + "/" + filename;
            std::ofstream file(full_path, std::ios::binary);
            if (!file) {
                auto res = std::make_shared<http::response<http::string_body>>(http::status::internal_server_error, req_.version());
                res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
                res->body() = "Cannot save file";
                res->prepare_payload();
                return send_response(res);
            }

            // Write the file content (skipping the header)
            file.write(body.data() + pos + 4, body.size() - pos - 4);
            file.close();

            std::cout << "[HTTP] File uploaded: " << full_path << std::endl;

            json response_json;
            response_json["status"] = "ok";
            response_json["file_path"] = full_path;

            auto res = std::make_shared<http::response<http::string_body>>(http::status::ok, req_.version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::content_type, "application/json");
            res->keep_alive(req_.keep_alive());
            res->body() = response_json.dump();
            res->prepare_payload();
            return send_response(res);
        }

        return send_response(bad_request("Unknown request"));
    }

    // Generic function to send any response type
    template<class Body, class Fields>
    void send_response(std::shared_ptr<http::response<Body, Fields>> res) {
        // IMPORTANT: We store the response in res_ to keep it alive
        // until async_write completes.
        res_ = res;

        http::async_write(stream_, *res,
            beast::bind_front_handler(
                &HttpSession::on_write,
                shared_from_this(),
                res->need_eof()));
    }

    void on_write(bool close, beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);

        if (ec) return fail(ec, "write");

        if (close) {
            return do_close();
        }

        // Clear the response to free memory and read another request
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

// ============================================================================
// HttpServer Implementation
// ============================================================================

HttpServer::HttpServer(net::io_context& ioc, tcp::endpoint endpoint,
    std::string upload_path, std::string web_root)
    : acceptor_(net::make_strand(ioc))
    , upload_path_(std::move(upload_path))
    , web_root_(std::move(web_root))
{
    beast::error_code ec;

    acceptor_.open(endpoint.protocol(), ec);
    if (ec) {
        std::cerr << "Open error: " << ec.message() << std::endl;
        return;
    }

    acceptor_.set_option(net::socket_base::reuse_address(true), ec);
    acceptor_.bind(endpoint, ec);
    if (ec) {
        std::cerr << "Bind error: " << ec.message() << std::endl;
        return;
    }

    acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
        std::cerr << "Listen error: " << ec.message() << std::endl;
        return;
    }
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
        std::cerr << "accept: " << ec.message() << "\n";
    }
    else {
        // Create the Session and run it
        std::make_shared<HttpSession>(
            std::move(socket),
            upload_path_,
            web_root_
        )->run();
    }

    // Accept next connection
    do_accept();
}