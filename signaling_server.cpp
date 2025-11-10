#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/smart_ptr/make_shared.hpp>
#include <boost/enable_shared_from_this.hpp>

#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <set>
#include <map>
#include <mutex>
#include <thread>
#include <stdexcept>

namespace beast = boost::beast;
namespace http = boost::beast::http; 
namespace websocket = boost::beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;



class websocket_session; 

/**
 * @brief Управляет всеми комнатами и сессиями в них
 */
class room_manager {
    std::map<std::string, std::set<websocket_session*>> rooms_;
    std::mutex mutex_;

public:
    void join(const std::string& room_id, websocket_session* session);
    void leave(const std::string& room_id, websocket_session* session);
    void broadcast(const std::string& room_id, const std::string& message, websocket_session* sender);
};



/**
 * @brief Обрабатывает одну WebSocket сессию.
 */
class websocket_session : public boost::enable_shared_from_this<websocket_session>
{
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    std::string room_id_;
    room_manager& room_manager_;
    std::vector<boost::shared_ptr<std::string const>> send_queue_;

public:
    explicit websocket_session(tcp::socket&& socket, room_manager& manager, std::string room_id)
        : ws_(std::move(socket))
        , room_id_(std::move(room_id))
        , room_manager_(manager)
    {
    }

    ~websocket_session() {
        room_manager_.leave(room_id_, this);
    }

    template<class Body, class Allocator>
    void run(http::request<Body, http::basic_fields<Allocator>> req)
    {
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

        ws_.async_accept(
            req,
            beast::bind_front_handler(&websocket_session::on_accept, shared_from_this()));
    }

    void on_accept(beast::error_code ec);
    void send(boost::shared_ptr<std::string const> const& ss);

private:
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void on_send(boost::shared_ptr<std::string const> const& ss);
    void do_write();
    void on_write(beast::error_code ec, std::size_t bytes_transferred);
};



void room_manager::join(const std::string& room_id, websocket_session* session) {
    std::lock_guard<std::mutex> lock(mutex_);
    rooms_[room_id].insert(session);
    std::cout << "[ROOM] Session joined room '" << room_id << "'. Total: " << rooms_[room_id].size() << std::endl;
}

void room_manager::leave(const std::string& room_id, websocket_session* session) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rooms_.count(room_id)) {
        rooms_[room_id].erase(session);
        std::cout << "[ROOM] Session left room '" << room_id << "'. Total: " << rooms_[room_id].size() << std::endl;
        if (rooms_[room_id].empty()) {
            rooms_.erase(room_id);
        }
    }
}

void room_manager::broadcast(const std::string& room_id, const std::string& message, websocket_session* sender) {
    auto const ss = boost::make_shared<std::string const>(message);

    std::vector<websocket_session*> sessions_to_send;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!rooms_.count(room_id)) {
            return;
        }
        for (auto* session : rooms_.at(room_id)) {
            if (session != sender) {
                sessions_to_send.push_back(session);
            }
        }
    }

    for (auto* session : sessions_to_send) {
        session->send(ss);
    }
}


void websocket_session::on_accept(beast::error_code ec)
{
    if(ec) { 
        std::cerr << "[ERROR] Accept: " << ec.message() << std::endl;
        return;
    }

    room_manager_.join(room_id_, this);
    do_read(); 
}

void websocket_session::do_read()
{
    ws_.async_read(
        buffer_,
        beast::bind_front_handler(&websocket_session::on_read, shared_from_this()));
}

void websocket_session::on_read(beast::error_code ec, std::size_t bytes_transferred)
{
    boost::ignore_unused(bytes_transferred);

    if(ec == websocket::error::closed)
        return;

    if(ec) {
        std::cerr << "[ERROR] Read: " << ec.message() << std::endl;
        return;
    }

    std::string message = beast::buffers_to_string(buffer_.data());
    std::cout << "[RECV " << room_id_ << "] " << message << std::endl;
    
    room_manager_.broadcast(room_id_, message, this);

    buffer_.consume(buffer_.size());
    do_read();
}

void websocket_session::send(boost::shared_ptr<std::string const> const& ss)
{
    net::post(
        ws_.get_executor(),
        beast::bind_front_handler(
            &websocket_session::on_send,
            shared_from_this(),
            ss));
}

void websocket_session::on_send(boost::shared_ptr<std::string const> const& ss)
{
    send_queue_.push_back(ss);

    if (send_queue_.size() > 1)
        return;

    do_write();
}

void websocket_session::do_write()
{
    ws_.text(true);
    ws_.async_write(
        net::buffer(*send_queue_.front()),
        beast::bind_front_handler(&websocket_session::on_write, shared_from_this()));
}

void websocket_session::on_write(beast::error_code ec, std::size_t bytes_transferred)
{
    boost::ignore_unused(bytes_transferred);

    if(ec) {
        std::cerr << "[ERROR] Write: " << ec.message() << std::endl;
        return;
    }

    send_queue_.erase(send_queue_.begin());

    if(!send_queue_.empty())
        do_write();
}


/**
 * @brief Принимает HTTP соединения и передает их в http_session.
 */
class http_session : public boost::enable_shared_from_this<http_session>
{
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
    room_manager& room_manager_;

public:
    http_session(tcp::socket&& socket, room_manager& manager)
        : stream_(std::move(socket))
        , room_manager_(manager)
    {
    }

    void run()
    {
        net::dispatch(stream_.get_executor(),
            beast::bind_front_handler(&http_session::do_read, shared_from_this()));
    }

private:
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void send_not_found(beast::string_view target);
    void on_write(bool keep_alive, beast::error_code ec, std::size_t bytes_transferred);
    void do_close();
};


void http_session::do_read()
{
    req_ = {}; 

    http::async_read(stream_, buffer_, req_, 
        beast::bind_front_handler(&http_session::on_read, shared_from_this()));
}

void http_session::on_read(beast::error_code ec, std::size_t bytes_transferred)
{
    boost::ignore_unused(bytes_transferred);

    if(ec == http::error::end_of_stream) 
        return do_close();

    if(ec) {
        std::cerr << "[ERROR] HTTP Read: " << ec.message() << std::endl;
        return;
    }

    if(websocket::is_upgrade(req_))
    {
        std::string room_id = std::string(req_.target());
        
        boost::make_shared<websocket_session>(
            stream_.release_socket(),
            room_manager_,
            std::move(room_id)
        )->run(std::move(req_));
    }
    else
    {
        send_not_found(req_.target());
    }
}

void http_session::send_not_found(beast::string_view target)
{
    http::response<http::string_body> res{http::status::not_found, req_.version()};
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "text/html");
    res.keep_alive(req_.keep_alive());
    res.body() = "WebSocket upgrade required at " + std::string(target);
    res.prepare_payload();

    http::async_write(stream_, res,
        beast::bind_front_handler(&http_session::on_write, shared_from_this(), res.keep_alive()));
}

void http_session::on_write(bool keep_alive, beast::error_code ec, std::size_t bytes_transferred)
{
    boost::ignore_unused(bytes_transferred);
    if(ec) {
        std::cerr << "[ERROR] HTTP Write: " << ec.message() << std::endl;
        return;
    }
    if(!keep_alive)
        return do_close();
    
    do_read();
}

void http_session::do_close()
{
    beast::error_code ec;
    stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
}


/**
 * @brief Принимает входящие TCP соединения
 */
class listener : public boost::enable_shared_from_this<listener>
{
    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    room_manager& room_manager_;
    tcp::endpoint endpoint_; 

public:
    listener(net::io_context& ioc, tcp::endpoint endpoint, room_manager& manager)
        : ioc_(ioc)
        , acceptor_(net::make_strand(ioc))
        , room_manager_(manager)
        , endpoint_(endpoint)
    {
    }

    void run(); 

private:
    void do_accept()
    {
        acceptor_.async_accept(
            net::make_strand(ioc_),
            beast::bind_front_handler(&listener::on_accept, shared_from_this()));
    }

    void on_accept(beast::error_code ec, tcp::socket socket)
    {
        if(ec) {
            std::cerr << "[ERROR] Accept: " << ec.message() << std::endl;
            return;
        }
        else
        {
            boost::make_shared<http_session>(
                std::move(socket),
                room_manager_
            )->run();
        }

        do_accept();
    }
};


void listener::run() 
{
    beast::error_code ec;
    
    acceptor_.open(endpoint_.protocol(), ec);
    if(ec) throw std::runtime_error("[FATAL] Open: " + ec.message());
    
    acceptor_.set_option(net::socket_base::reuse_address(true), ec);
    if(ec) throw std::runtime_error("[FATAL] SetOption: " + ec.message());

    acceptor_.bind(endpoint_, ec);
    if(ec) throw std::runtime_error("[FATAL] Bind: " + ec.message());

    acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if(ec) throw std::runtime_error("[FATAL] Listen: " + ec.message());
    
    do_accept();
}


int main()
{
    try {
        auto const address = net::ip::make_address("0.0.0.0");
        auto const port = static_cast<unsigned short>(8080);
        auto const threads = 4;

        net::io_context ioc{threads};
        
        room_manager manager;

        boost::shared_ptr<listener> server = boost::make_shared<listener>(
            ioc,
            tcp::endpoint{address, port},
            manager
        );
        
        server->run(); 

        std::cout << "[INFO] WebSocket server running on " << address << ":" << port << std::endl;

        std::vector<std::thread> v;
        v.reserve(threads - 1);
        for(auto i = threads - 1; i > 0; --i)
            v.emplace_back(
                [&ioc]
                {
                    ioc.run();
                });
        ioc.run();
        
        for (auto& t : v) t.join();

    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
