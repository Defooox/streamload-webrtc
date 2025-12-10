 
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
 
        req_ = {};
        stream_.expires_after(std::chrono::seconds(30));

        http::async_read(stream_, buffer_, req_,
            beast::bind_front_handler(
                &HttpSession::on_read,
                shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);
 
        if (ec == http::error::end_of_stream)
            return do_close();

        if (ec)
            return fail(ec, "read");

        handle_request();
    }

    void handle_request() {
        
        auto const bad_request = [&](beast::string_view why) {
            auto res = std::make_shared<http::response<http::string_body>>(http::status::bad_request, req_.version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::content_type, "text/plain");
            res->set(http::field::access_control_allow_origin, "*");
            res->keep_alive(req_.keep_alive());
            res->body() = std::string(why);
            res->prepare_payload();
            return res;
            };

   
        if (req_.method() == http::verb::options) {
            auto res = std::make_shared<http::response<http::string_body>>(http::status::ok, req_.version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::access_control_allow_origin, "*");
            res->set(http::field::access_control_allow_methods, "GET, POST, OPTIONS");
            res->set(http::field::access_control_allow_headers, "Content-Type");
            res->keep_alive(req_.keep_alive());
            res->prepare_payload();
            return send_response(res);
        }

 
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
            res->set(http::field::access_control_allow_origin, "*");
            res->keep_alive(req_.keep_alive());
            res->body() = content;
            res->prepare_payload();
            return send_response(res);
        }

 
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
            res->set(http::field::access_control_allow_origin, "*");
            res->keep_alive(req_.keep_alive());
            res->body() = content;
            res->prepare_payload();
            return send_response(res);
        }

        // --- 3. Handle Upload (FIXED) ---
        if (req_.method() == http::verb::post && req_.target() == "/upload") {
            std::cout << "[HTTP] Upload request received, body size: " << req_.body().size() << std::endl;

            std::string body = req_.body();
            std::string filename = "stream_" + std::to_string(std::time(nullptr)) + ".mp4";

        
            auto content_type = req_[http::field::content_type];
            std::cout << "[HTTP] Content-Type: " << content_type << std::endl;

      
            size_t header_end = body.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                std::cout << "[HTTP] Warning: No multipart header found, treating as raw data" << std::endl;
                header_end = 0;  
            }
            else {
                header_end += 4;  
            }
             
            size_t data_end = body.size();
            size_t boundary_pos = body.rfind("\r\n--");
            if (boundary_pos != std::string::npos && boundary_pos > header_end) {
                data_end = boundary_pos;
            }

            size_t data_length = data_end - header_end;
            std::cout << "[HTTP] Data length: " << data_length << " bytes" << std::endl;

            std::string full_path = upload_path_ + "/" + filename;
            std::ofstream file(full_path, std::ios::binary);
            if (!file) {
                std::cerr << "[HTTP] Cannot create file: " << full_path << std::endl;
                auto res = std::make_shared<http::response<http::string_body>>(http::status::internal_server_error, req_.version());
                res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
                res->set(http::field::access_control_allow_origin, "*");
                res->body() = "Cannot save file";
                res->prepare_payload();
                return send_response(res);
            }

 
            file.write(body.data() + header_end, data_length);
            file.close();

            std::cout << "[HTTP] File uploaded: " << full_path << " (" << data_length << " bytes)" << std::endl;

            json response_json;
            response_json["status"] = "ok";
            response_json["file_path"] = full_path;
            response_json["size"] = data_length;

            auto res = std::make_shared<http::response<http::string_body>>(http::status::ok, req_.version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::content_type, "application/json");
            res->set(http::field::access_control_allow_origin, "*");
            res->keep_alive(req_.keep_alive());
            res->body() = response_json.dump();
            res->prepare_payload();
            return send_response(res);
        }

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

        if (ec) return fail(ec, "write");

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
 
        std::make_shared<HttpSession>(
            std::move(socket),
            upload_path_,
            web_root_
        )->run();
    }

 
    do_accept();
}