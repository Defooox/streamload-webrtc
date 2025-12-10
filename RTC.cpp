#include "SharedState.h"
#include "Listener.h"
#include "HttpServer.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <iostream>
#include <vector>
#include <thread>
#include <filesystem>

namespace net = boost::asio;
using tcp = net::ip::tcp;

int main() {
    try {
        const auto address = net::ip::make_address("0.0.0.0");
        const unsigned short ws_port = 8080;  
        const unsigned short http_port = 8081; 
        const int threads = 4;

 
        std::cout << "WebRTC Sync Streaming Server\n";
        std::cout << "WebSocket (signaling): ws://" << address << ":" << ws_port << "\n";
        std::cout << "HTTP (UI & upload):    http://" << address << ":" << http_port << "\n";


        net::io_context ioc{ threads };
        auto state = std::make_shared<SharedState>();

        std::make_shared<Listener>(ioc, tcp::endpoint{ address, ws_port }, state)->run();


        std::filesystem::create_directories("./uploads");
        std::make_shared<HttpServer>(ioc, tcp::endpoint{ address, http_port }, "./uploads", "./web")->run();


        net::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&ioc](boost::system::error_code const&, int) {
            std::cout << "\nStopping server..." << std::endl;
            ioc.stop();
            });

        std::vector<std::thread> workers;
        workers.reserve(threads - 1);
        for (int i = threads - 1; i > 0; --i) {
            workers.emplace_back([&ioc]() {
                ioc.run();
                });
        }

        ioc.run();

        for (auto& t : workers) {
            t.join();
        }

        std::cout << "Server stopped cleanly." << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << " Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}