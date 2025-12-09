#include "HttpServer.h"
#include <boost/beast/version.hpp>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

HttpServer::HttpServer(net::io_context& ioc, tcp::endpoint endpoint,
    std::string upload_path, std::string web_root)
    : acceptor_(net::make_strand(ioc)), upload_path_(std::move(upload_path)),
    web_root_(std::move(web_root)) {

    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(net::socket_base::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(net::socket_base::max_listen_connections);
}

void HttpServer::run() {
    do_accept();
}

void HttpServer::do_accept() {
    acceptor_.async_accept(
        net::make_strand(acceptor_.get_executor()),
        [this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                auto buffer = std::make_shared<beast::flat_buffer>();
                auto req = std::make_shared<http::request<http::string_body>>();

                http::async_read(socket, *buffer, *req,
                    [this, socket = std::move(socket), buffer, req](beast::error_code ec, std::size_t) mutable {
                        if (!ec) {
                            handle_request(std::move(socket), std::move(*req));
                        }
                    });
            }
            do_accept();
        });
}

void HttpServer::handle_request(tcp::socket socket, http::request<http::string_body> req) {
    auto const bad_request = [&]() {
        http::response<http::string_body> res{ http::status::bad_request, req.version() };
        res.set(http::field::content_type, "text/plain");
        res.body() = "Bad request";
        res.prepare_payload();
        return res;
        };

    auto const success_json = [&](const json& j) {
        http::response<http::string_body> res{ http::status::ok, req.version() };
        res.set(http::field::content_type, "application/json");
        res.body() = j.dump();
        res.prepare_payload();
        return res;
        };

    // === РАЗДАЧА СТАТИЧЕСКИХ ФАЙЛОВ (HTML, JS) ===
    if (req.method() == http::verb::get && req.target() == "/") {
        std::ifstream file(web_root_ / "index.html");
        if (!file) {
            http::response<http::string_body> res{ http::status::not_found, req.version() };
            res.body() = "index.html not found";
            res.prepare_payload();
            http::async_write(socket, res, [socket = std::move(socket)](beast::error_code, std::size_t) {});
            return;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        http::response<http::string_body> res{ http::status::ok, req.version() };
        res.set(http::field::content_type, "text/html");
        res.body() = content;
        res.prepare_payload();
        http::async_write(socket, res, [socket = std::move(socket)](beast::error_code, std::size_t) {});
        return;
    }

    if (req.method() == http::verb::get && req.target() == "/client.js") {
        std::ifstream file(web_root_ / "client.js");
        if (!file) {
            http::response<http::string_body> res{ http::status::not_found, req.version() };
            res.body() = "client.js not found";
            res.prepare_payload();
            http::async_write(socket, res, [socket = std::move(socket)](beast::error_code, std::size_t) {});
            return;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        http::response<http::string_body> res{ http::status::ok, req.version() };
        res.set(http::field::content_type, "application/javascript");
        res.body() = content;
        res.prepare_payload();
        http::async_write(socket, res, [socket = std::move(socket)](beast::error_code, std::size_t) {});
        return;
    }

    // === ЗАГРУЗКА ФАЙЛА ===
    if (req.method() != http::verb::post || req.target() != "/upload") {
        http::response<http::string_body> res = bad_request();
        http::async_write(socket, res, [socket = std::move(socket)](beast::error_code, std::size_t) {});
        return;
    }

    // Простейшая обработка multipart (для MVP)
    std::string body = req.body();
    std::string filename = "stream_" + std::to_string(std::time(nullptr)) + ".mp4";

    // Найдем границу multipart (для формы)
    size_t pos = body.find("\r\n\r\n");
    if (pos == std::string::npos) {
        http::response<http::string_body> res{ http::status::bad_request, req.version() };
        res.body() = "Invalid multipart data";
        res.prepare_payload();
        http::async_write(socket, res, [socket = std::move(socket)](beast::error_code, std::size_t) {});
        return;
    }

    // Сохраняем файл (пропуская заголовки multipart)
    std::string full_path = upload_path_ + "/" + filename;
    std::ofstream file(full_path, std::ios::binary);
    if (!file) {
        http::response<http::string_body> res{ http::status::internal_server_error, req.version() };
        res.body() = "Cannot save file";
        res.prepare_payload();
        http::async_write(socket, res, [socket = std::move(socket)](beast::error_code, std::size_t) {});
        return;
    }

    // Записываем только данные файла (после заголовков)
    file.write(body.data() + pos + 4, body.size() - pos - 4);
    file.close();

    std::cout << "📁 File uploaded: " << full_path << std::endl;

    json response;
    response["status"] = "ok";
    response["file_path"] = full_path;

    http::response<http::string_body> res = success_json(response);
    http::async_write(socket, res, [socket = std::move(socket)](beast::error_code, std::size_t) {});
}