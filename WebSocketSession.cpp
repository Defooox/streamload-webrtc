#include "WebSocketSession.h"
#include "SharedState.h"
#include <iostream>

WebSocketSession::WebSocketSession(tcp::socket&& socket, std::shared_ptr<SharedState> const& state)
    : ws_(std::move(socket)), state_(state) {
}

WebSocketSession::~WebSocketSession() {

    state_->leave(shared_from_this());
}

void WebSocketSession::fail(beast::error_code ec, char const* what) {

    if (ec == net::error::operation_aborted || ec == websocket::error::closed)
        return;
    std::cerr << what << ": " << ec.message() << "\n";
}

void WebSocketSession::on_accept(beast::error_code ec) {
    if (ec)
        return fail(ec, "accept");
    state_->join(shared_from_this());

    do_read();
}

void WebSocketSession::do_read() {

    buffer_.consume(buffer_.size());

    ws_.async_read(
        buffer_,
        beast::bind_front_handler(
            &WebSocketSession::on_read,
            shared_from_this()));
}

void WebSocketSession::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    if (ec)
        return fail(ec, "read");

    state_->send(beast::buffers_to_string(buffer_.data()), shared_from_this());


    do_read();
}

void WebSocketSession::send(std::shared_ptr<std::string const> const& ss) {
    write_queue_.push(ss);


    if (write_queue_.size() > 1)
        return;

    ws_.async_write(
        net::buffer(*write_queue_.front()),
        beast::bind_front_handler(
            &WebSocketSession::on_write,
            shared_from_this()));
}

void WebSocketSession::on_write(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    if (ec)
        return fail(ec, "write");

    write_queue_.pop();


    if (!write_queue_.empty())
        ws_.async_write(
            net::buffer(*write_queue_.front()),
            beast::bind_front_handler(
                &WebSocketSession::on_write,
                shared_from_this()));
}