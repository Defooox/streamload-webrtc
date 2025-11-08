#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <iostream>
#include <thread>
#include <set>
#include <mutex>
#include <nlohmann/json.hpp> // Для JSON обработки

using tcp = boost::asio::ip::tcp;
namespace websocket = boost::beast::websocket;
using json = nlohmann::json;

std::set<websocket::stream<tcp::socket>*> clients;
std::mutex clients_mutex;

void broadcast(const std::string& message, websocket::stream<tcp::socket>* sender) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    for (auto* client : clients) {
        if (client != sender) {
            boost::beast::flat_buffer buffer;
            buffer.commit(boost::asio::buffer_copy(buffer.prepare(message.size()), boost::asio::buffer(message)));
            client->text(true);
            client->write(buffer.data());
        }
    }
}

void do_session(tcp::socket socket) {
    websocket::stream<tcp::socket> ws{std::move(socket)}; 
    try {
        ws.accept();

        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            clients.insert(&ws);
        }

        std::cout << "[INFO] New client connected\n";

        for (;;) {
            boost::beast::flat_buffer buffer;
            ws.read(buffer);
            std::string message = boost::beast::buffers_to_string(buffer.data());

            try {
                auto j = json::parse(message);
                std::cout << "[RECEIVED] " << j.dump() << std::endl;
                broadcast(message, &ws);
            } catch (...) {
                std::cerr << "[WARN] Received invalid JSON: " << message << std::endl;
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Session: " << e.what() << std::endl;
    }

    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        clients.erase(&ws);
    }
}


int main() {
    try {
        boost::asio::io_context ioc{1};
        tcp::acceptor acceptor{ioc, tcp::endpoint(tcp::v4(), 8080)};
        std::cout << "[INFO] WebSocket server running on port 8080\n";

        for (;;) {
            tcp::socket socket{ioc};
            acceptor.accept(socket);

            std::thread([sock = std::move(socket)]() mutable {
                do_session(std::move(sock));
            }).detach();
        }
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << std::endl;
    }
}
