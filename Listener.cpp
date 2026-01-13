#include "Listener.h"
#include "WebSocketSession.h"
#include "SharedState.h"

#include <boost/beast/websocket.hpp>
#include <iostream>

namespace websocket = boost::beast::websocket;

Listener::Listener(net::io_context& ioc,
    tcp::endpoint endpoint,
    std::shared_ptr<SharedState> state)
    : ioc_(ioc)
    , acceptor_(net::make_strand(ioc))
    , state_(std::move(state))
{
    beast::error_code ec;

    acceptor_.open(endpoint.protocol(), ec);
    if (ec) throw beast::system_error(ec);

    acceptor_.set_option(net::socket_base::reuse_address(true), ec);
    if (ec) throw beast::system_error(ec);

    acceptor_.bind(endpoint, ec);
    if (ec) throw beast::system_error(ec);

    acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if (ec) throw beast::system_error(ec);
}

void Listener::run() {
    do_accept();
}

void Listener::do_accept() {
    acceptor_.async_accept(
        net::make_strand(ioc_),
        beast::bind_front_handler(&Listener::on_accept, shared_from_this())
    );
}

void Listener::on_accept(beast::error_code ec, tcp::socket socket) {
    if (ec) {
        std::cerr << "[WS] accept error: " << ec.message() << "\n";
        do_accept();
        return;
    }


    do_accept();


    auto sp_socket = std::make_shared<tcp::socket>(std::move(socket));
    auto buffer = std::make_shared<beast::flat_buffer>();
    auto req = std::make_shared<http::request<http::string_body>>();

    http::async_read(
        *sp_socket,
        *buffer,
        *req,
        [this, sp_socket, buffer, req](beast::error_code read_ec, std::size_t) mutable {
            if (read_ec) {
                std::cerr << "[WS] http read error: " << read_ec.message() << "\n";
                beast::error_code ignore;
                sp_socket->shutdown(tcp::socket::shutdown_both, ignore);
                sp_socket->close(ignore);
                return;
            }

            if (!websocket::is_upgrade(*req)) {
     
                http::response<http::string_body> res{ http::status::upgrade_required, req->version() };
                res.set(http::field::server, "RTCServer");
                res.set(http::field::content_type, "text/plain; charset=utf-8");
                res.keep_alive(false);
                res.body() = "Upgrade Required: use WebSocket endpoint.";
                res.prepare_payload();

                http::async_write(
                    *sp_socket,
                    res,
                    [sp_socket](beast::error_code, std::size_t) {
                        beast::error_code ignore;
                        sp_socket->shutdown(tcp::socket::shutdown_both, ignore);
                        sp_socket->close(ignore);
                    }
                );
                return;
            }

   
            auto session = std::make_shared<WebSocketSession>(std::move(*sp_socket), state_, ioc_);
            session->run(std::move(*req));
        }
    );
}
