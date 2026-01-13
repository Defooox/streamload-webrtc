#include "HttpServer.h"

#include <boost/beast/version.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>


using json = nlohmann::json;

namespace {

    std::string sanitize_basename(std::string name) {

        auto pos1 = name.find_last_of('/');
        auto pos2 = name.find_last_of('\\');
        auto pos = (pos1 == std::string::npos) ? pos2 : (pos2 == std::string::npos ? pos1 : std::max(pos1, pos2));
        if (pos != std::string::npos) name = name.substr(pos + 1);

     
        for (char& c : name) {
            const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-';
            if (!ok) c = '_';
        }


        bool any_alnum = false;
        for (char c : name) if (std::isalnum(static_cast<unsigned char>(c))) { any_alnum = true; break; }
        if (!any_alnum) name = "upload.bin";

        return name;
    }

    std::string random_suffix() {
        static thread_local std::mt19937_64 rng{ std::random_device{}() };
        std::uniform_int_distribution<uint64_t> dist;
        std::ostringstream oss;
        oss << std::hex << dist(rng);
        return oss.str();
    }

} 

class HttpSession : public std::enable_shared_from_this<HttpSession> {
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;


    std::unique_ptr<http::request_parser<http::buffer_body>> parser_;
    http::request<http::buffer_body> req_;
    std::shared_ptr<void> res_;

    std::string upload_path_;
    std::string web_root_;

    std::ofstream upload_file_;
    std::string upload_full_path_;
    std::size_t upload_bytes_written_{ 0 };
    std::array<char, 64 * 1024> body_buf_{};

public:
    HttpSession(tcp::socket&& socket, std::string upload_path, std::string web_root)
        : stream_(std::move(socket))
        , upload_path_(std::move(upload_path))
        , web_root_(std::move(web_root))
    {
    }

    void run() {
        net::dispatch(stream_.get_executor(),
            beast::bind_front_handler(&HttpSession::do_read, shared_from_this()));
    }

private:
    void do_read() {
        parser_ = std::make_unique<http::request_parser<http::buffer_body>>();
 
        parser_->body_limit(500 * 1024 * 1024);

        stream_.expires_after(std::chrono::seconds(300));
        buffer_.clear();

   
        http::async_read_header(stream_, buffer_, *parser_,
            beast::bind_front_handler(&HttpSession::on_read_header, shared_from_this()));
    }

    void on_read_header(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);

        if (ec == http::error::end_of_stream)
            return do_close();

        if (ec) {
            std::cerr << "[HTTP ERR] Read header error: " << ec.message() << std::endl;
            return fail(ec, "read_header");
        }


        req_ = parser_->get();

    
        const beast::string_view target = req_.target();
        const bool is_upload_raw = target.size() >= 11 && target.substr(0, 11) == "/upload_raw";

        if (req_.method() == http::verb::put && is_upload_raw) {
            return begin_streaming_upload();
        }


        handle_request_no_body();
    }

    void begin_streaming_upload() {
        std::cout << "[HTTP] ========== UPLOAD_RAW REQUEST ==========" << std::endl;
        std::cout << "[HTTP] Method: " << req_.method_string() << std::endl;
        std::cout << "[HTTP] Target: " << req_.target() << std::endl;
        std::cout << "[HTTP] Content-Length: " << req_[http::field::content_length] << std::endl;
        std::cout << "[HTTP] Content-Type: " << req_[http::field::content_type] << std::endl;

 
        try {
            std::filesystem::create_directories(upload_path_);
        }
        catch (...) {
    
        }

     
        std::string client_name;
        auto it = req_.find("X-Filename");
        if (it != req_.end()) {
            client_name = sanitize_basename(std::string(it->value()));
        }

    
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

        std::string ext = ".bin";
        if (!client_name.empty()) {
            auto dot = client_name.find_last_of('.');
            if (dot != std::string::npos && dot + 1 < client_name.size()) {
                ext = client_name.substr(dot);
                if (ext.size() > 16) ext = ".bin";
            }
        }

        std::string filename = "stream_" + std::to_string(ms) + "_" + random_suffix() + ext;
        upload_full_path_ = upload_path_ + "/" + filename;

        upload_file_.open(upload_full_path_, std::ios::binary);
        upload_bytes_written_ = 0;

        if (!upload_file_) {
            std::cerr << "[HTTP] ERROR: Cannot create file: " << upload_full_path_ << std::endl;
            return send_simple_error(http::status::internal_server_error, "Cannot save file");
        }

    
        do_read_upload_body();
    }

    void do_read_upload_body() {
        auto& preq = parser_->get();         
        preq.body().data = body_buf_.data();
        preq.body().size = body_buf_.size();

        http::async_read_some(stream_, buffer_, *parser_,
            beast::bind_front_handler(&HttpSession::on_read_upload_body, shared_from_this()));
    }

    void on_read_upload_body(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);

        if (ec == http::error::need_buffer) {
      
            return do_read_upload_body();
        }

        if (ec == http::error::end_of_stream) {
        
            std::cerr << "[HTTP] Client closed stream during upload" << std::endl;
            upload_file_.close();
            return do_close();
        }

        if (ec) {
            std::cerr << "[HTTP ERR] Upload read error: " << ec.message() << std::endl;
            upload_file_.close();
            return fail(ec, "upload_read");
        }

        auto& preq = parser_->get();
        const std::size_t remaining = preq.body().size;
        const std::size_t bytes_in_chunk = body_buf_.size() - remaining;

        if (bytes_in_chunk > 0) {
            upload_file_.write(body_buf_.data(), static_cast<std::streamsize>(bytes_in_chunk));
            upload_bytes_written_ += bytes_in_chunk;
        }

        if (parser_->is_done()) {
            upload_file_.close();
            std::cout << "[HTTP] SUCCESS File saved: " << upload_full_path_
                << " (" << upload_bytes_written_ << " bytes)" << std::endl;
            std::cout << "[HTTP] ======================================" << std::endl;

            json response_json;
            response_json["status"] = "ok";
            response_json["file_path"] = upload_full_path_;
            response_json["size"] = upload_bytes_written_;

            auto res = std::make_shared<http::response<http::string_body>>(
                http::status::ok, req_.version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::content_type, "application/json");
            res->set(http::field::access_control_allow_origin, "*");
            res->keep_alive(parser_->get().keep_alive());
            res->body() = response_json.dump();
            res->prepare_payload();
            return send_response(res);
        }

        do_read_upload_body();
    }

    void handle_request_no_body() {
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

        if (req_.method() == http::verb::options) {
            auto res = std::make_shared<http::response<http::string_body>>(
                http::status::ok, req_.version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::access_control_allow_origin, "*");
            res->set(http::field::access_control_allow_methods, "GET, PUT, POST, OPTIONS");
            res->set(http::field::access_control_allow_headers, "Content-Type, X-Filename");
            res->keep_alive(req_.keep_alive());
            res->prepare_payload();
            return send_response(res);
        }

        if (req_.method() == http::verb::get && req_.target() == "/favicon.ico") {
            auto res = std::make_shared<http::response<http::string_body>>(
                http::status::no_content, req_.version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::access_control_allow_origin, "*");
            res->keep_alive(req_.keep_alive());
            res->prepare_payload();
            return send_response(res);
        }

        if (req_.method() == http::verb::get && (req_.target() == "/" || req_.target() == "/index.html")) {
            std::string path = web_root_ + "/index.html";
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                std::cerr << "[HTTP] index.html not found at: " << path << std::endl;
                return send_response(bad_request("index.html not found"));
            }

            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            auto res = std::make_shared<http::response<http::string_body>>(
                http::status::ok, req_.version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::content_type, "text/html; charset=utf-8");
            res->set(http::field::access_control_allow_origin, "*");
            res->set(http::field::access_control_allow_methods, "GET, PUT, OPTIONS");
            res->set(http::field::access_control_allow_headers, "*");
            res->set("Content-Security-Policy",
                "default-src * 'unsafe-inline' 'unsafe-eval' data: blob:; "
                "script-src * 'unsafe-inline' 'unsafe-eval'; "
                "connect-src * ws: wss:; "
                "style-src * 'unsafe-inline';");
            res->set("X-Content-Type-Options", "nosniff");
            res->set("X-Frame-Options", "SAMEORIGIN");
            res->keep_alive(req_.keep_alive());
            res->body() = content;
            res->prepare_payload();
            return send_response(res);
        }

        if (req_.method() == http::verb::get && req_.target() == "/client.js") {
            std::string path = web_root_ + "/client.js";
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                std::cerr << "[HTTP] client.js not found at: " << path << std::endl;
                return send_response(bad_request("client.js not found"));
            }

            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            auto res = std::make_shared<http::response<http::string_body>>(
                http::status::ok, req_.version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::content_type, "application/javascript; charset=utf-8");
            res->set(http::field::access_control_allow_origin, "*");
            res->set(http::field::access_control_allow_methods, "GET, PUT, OPTIONS");
            res->set(http::field::access_control_allow_headers, "*");
            res->set("X-Content-Type-Options", "nosniff");
            res->set("Cache-Control", "no-cache, no-store, must-revalidate");
            res->keep_alive(req_.keep_alive());
            res->body() = content;
            res->prepare_payload();
            return send_response(res);
        }


        if (req_.method() == http::verb::post && req_.target() == "/upload") {
            json j;
            j["status"] = "error";
            j["message"] = "Deprecated. Use PUT /upload_raw with raw file body.";

            auto res = std::make_shared<http::response<http::string_body>>(
                http::status::gone, req_.version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::content_type, "application/json");
            res->set(http::field::access_control_allow_origin, "*");
            res->keep_alive(req_.keep_alive());
            res->body() = j.dump();
            res->prepare_payload();
            return send_response(res);
        }

        std::cerr << "[HTTP] Unknown request: " << req_.method_string()
            << " " << req_.target() << std::endl;
        return send_response(bad_request("Unknown request"));
    }

    void send_simple_error(http::status st, std::string msg) {
        auto res = std::make_shared<http::response<http::string_body>>(st, req_.version());
        res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res->set(http::field::content_type, "text/plain");
        res->set(http::field::access_control_allow_origin, "*");
        res->keep_alive(req_.keep_alive());
        res->body() = std::move(msg);
        res->prepare_payload();
        send_response(res);
    }

    template<class Body, class Fields>
    void send_response(std::shared_ptr<http::response<Body, Fields>> res) {
        res_ = res;
        http::async_write(stream_, *res,
            beast::bind_front_handler(&HttpSession::on_write, shared_from_this(), res->need_eof()));
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
        beast::bind_front_handler(&HttpServer::on_accept, shared_from_this()));
}

void HttpServer::on_accept(beast::error_code ec, tcp::socket socket) {
    if (ec) {
        std::cerr << "[HTTP] Accept error: " << ec.message() << "\n";
    }
    else {
        std::cout << "[HTTP] New connection accepted" << std::endl;
        std::make_shared<HttpSession>(std::move(socket), upload_path_, web_root_)->run();
    }

    do_accept();
}
