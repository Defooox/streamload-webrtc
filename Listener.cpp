#include "Listener.h"
#include "WebSocketSession.h"
#include "SharedState.h"
#include <iostream>

namespace http = boost::beast::http;

Listener::Listener(
    net::io_context& ioc,
    tcp::endpoint endpoint,
    std::shared_ptr<SharedState> const& state)
    : ioc_(ioc), acceptor_(net::make_strand(ioc)), state_(state)
{
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(net::socket_base::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(net::socket_base::max_listen_connections);

}

void Listener::run() {
    do_accept();
}

void Listener::fail(beast::error_code ec, char const* what) {

    if (ec == net::error::operation_aborted)
        return;
    std::cerr << what << ": " << ec.message() << "\n";
}

void Listener::do_accept() {
    acceptor_.async_accept(
        net::make_strand(ioc_),
        beast::bind_front_handler(
            &Listener::on_accept,
            shared_from_this()));
}

void Listener::on_accept(beast::error_code ec, tcp::socket socket) {
    if (ec)
        return fail(ec, "accept");

    auto stream = std::make_shared<beast::tcp_stream>(std::move(socket));
    auto buffer = std::make_shared<beast::flat_buffer>();
    auto req = std::make_shared<http::request<http::string_body>>();

    http::async_read(*stream, *buffer, *req,
        [this, stream, buffer, req](beast::error_code ec, std::size_t) {
            if (ec)
                return fail(ec, "read_http");

            if (websocket::is_upgrade(*req)) {

                std::make_shared<WebSocketSession>(stream->release_socket(), state_)->run(*req);
            }
        });

    do_accept();
}