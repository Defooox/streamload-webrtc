#include "WebSocketSession.h"
#include "SharedState.h"

#include <iostream>

WebSocketSession::WebSocketSession(tcp::socket socket,
    std::shared_ptr<SharedState> state,
    net::io_context& ioc)
    : ws_(std::move(socket))
    , state_(std::move(state))
    , strand_(net::make_strand(ioc))
{
}

void WebSocketSession::run(http::request<http::string_body> req) {
    // Таймауты и настройки для WS
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws_.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& res) {
            res.set(http::field::server, "RTCServer");
        }));

    //  Лимит размера входящих сообщений
    ws_.read_message_max(kMaxIncomingMessageBytes);

    ws_.async_accept(
        req,
        net::bind_executor(
            strand_,
            beast::bind_front_handler(&WebSocketSession::on_accept, shared_from_this())
        )
    );
}

void WebSocketSession::on_accept(beast::error_code ec) {
    if (ec) {
        std::cerr << "[WS] accept error: " << ec.message() << "\n";
        return;
    }

    if (state_) state_->join(shared_from_this());
    do_read();
}

void WebSocketSession::do_read() {
    ws_.async_read(
        buffer_,
        net::bind_executor(
            strand_,
            beast::bind_front_handler(&WebSocketSession::on_read, shared_from_this())
        )
    );
}

void WebSocketSession::on_read(beast::error_code ec, std::size_t) {
    if (ec == websocket::error::closed) {
        leave_state_once();
        return;
    }

    if (ec) {
        std::cerr << "[WS] read error: " << ec.message() << "\n";
        leave_state_once();
        return;
    }

    std::string msg = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());

    // КРИТИЧНО: сразу продолжаем чтение, чтобы не терять ICE/answer во время тяжёлой обработки
    do_read();

    // Обрабатываем сообщение вне strand (иначе start_stream блокирует прием новых сообщений)
    auto self = shared_from_this();
    net::post(
        ws_.get_executor(),
        [self, msg = std::move(msg)]() mutable {
            if (self->state_) {
                self->state_->send(std::move(msg), self);
            }
        }
    );
}


void WebSocketSession::send(std::shared_ptr<std::string const> const& msg) {
    net::post(
        strand_,
        [self = shared_from_this(), msg]() {
            if (self->closing_) return;

            //  Ограничиваем исходящую очередь
            if (self->write_queue_.size() >= kMaxWriteQueue) {
                std::cerr << "[WS] write queue overflow (" << self->write_queue_.size()
                    << "), closing session\n";
                self->do_close(websocket::close_reason(websocket::close_code::try_again_later));
                return;
            }

            const bool writing = !self->write_queue_.empty();
            self->write_queue_.push_back(msg);
            if (!writing) self->do_write();
        }
    );
}

void WebSocketSession::send(std::string msg) {
    send(std::make_shared<std::string const>(std::move(msg)));
}

void WebSocketSession::do_write() {
    if (closing_ || write_queue_.empty()) return;

    ws_.text(true);
    ws_.async_write(
        net::buffer(*write_queue_.front()),
        net::bind_executor(
            strand_,
            beast::bind_front_handler(&WebSocketSession::on_write, shared_from_this())
        )
    );
}

void WebSocketSession::on_write(beast::error_code ec, std::size_t) {
    if (ec) {
        std::cerr << "[WS] write error: " << ec.message() << "\n";
        leave_state_once();
        return;
    }

    if (!write_queue_.empty()) write_queue_.pop_front();
    if (!write_queue_.empty()) do_write();
}

void WebSocketSession::close() {
    net::post(strand_, [self = shared_from_this()] {
        self->do_close(websocket::close_reason(websocket::close_code::normal));
        });
}

void WebSocketSession::do_close(websocket::close_reason reason) {
    if (closing_) return;
    closing_ = true;

    // Очередь больше не нужна
    write_queue_.clear();

    ws_.async_close(
        reason,
        net::bind_executor(
            strand_,
            beast::bind_front_handler(&WebSocketSession::on_close, shared_from_this())
        )
    );
}

void WebSocketSession::on_close(beast::error_code ec) {
    if (ec) {
        // Нормально видеть operation_aborted если сокет уже умер
        std::cerr << "[WS] close error: " << ec.message() << "\n";
    }
    leave_state_once();
}

void WebSocketSession::leave_state_once() {
    if (state_) {
        state_->leave(shared_from_this());
        state_.reset();
    }
}
