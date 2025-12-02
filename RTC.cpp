#include "Listener.h"
#include "SharedState.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <iostream>
#include <vector>
#include <thread>

namespace net = boost::asio;

int main() {
    try {
        // Настройки сервера
        auto const address = net::ip::make_address("0.0.0.0"); // Слушаем на всех интерфейсах
        unsigned short const port = 8080; // Стандартный порт для Web-приложений
        int const threads = 4; // Количество потоков для обработки

        std::cout << "Starting signaling server on " << address << ":" << port << "..." << std::endl;

        net::io_context ioc{ threads };

        // Создаем общее состояние (хранилище сессий)
        auto state = std::make_shared<SharedState>();

        // Создаем и запускаем "слушателя"
        std::make_shared<Listener>(ioc, tcp::endpoint{ address, port }, state)->run();

        // Обработка сигналов (Ctrl+C) для корректной остановки
        net::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&ioc](boost::system::error_code const&, int) {
            std::cout << "\nStopping server..." << std::endl;
            ioc.stop();
            });

        // Запускаем io_context в пуле потоков
        std::vector<std::thread> v;
        v.reserve(threads - 1);
        for (auto i = threads - 1; i > 0; --i) {
            v.emplace_back([&ioc] {
                ioc.run();
                });
        }

        // Главный поток тоже участвует в работе
        ioc.run();

        // Ждем завершения всех потоков
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