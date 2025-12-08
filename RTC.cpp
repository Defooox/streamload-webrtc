#include "SharedState.h"
#include "Listener.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <iostream>
#include <vector>
#include <thread>

namespace net = boost::asio;

int main() {
    try {
        
        auto const address = net::ip::make_address("0.0.0.0"); 
        unsigned short const port = 8080; 
        int const threads = 4; 

        std::cout << "Starting signaling server on " << address << ":" << port << "..." << std::endl;

        net::io_context ioc{ threads };

        auto state = std::make_shared<SharedState>();

       
        std::make_shared<Listener>(ioc, tcp::endpoint{ address, port }, state)->run();

       
        net::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&ioc](boost::system::error_code const&, int) {
            std::cout << "\nStopping server..." << std::endl;
            ioc.stop();
            });

       
        std::vector<std::thread> v;
        v.reserve(threads - 1);
        for (auto i = threads - 1; i > 0; --i) {
            v.emplace_back([&ioc] {
                ioc.run();
                });
        }

       
        ioc.run();

        for (auto& t : v)
            t.join();

        std::cout << "Server stopped cleanly." << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}