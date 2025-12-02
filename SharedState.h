#pragma once

#include <memory>
#include <mutex>
#include <set>
#include <string>

class WebSocketSession;

class SharedState {
    std::mutex mutex_;

    std::set<std::weak_ptr<WebSocketSession>, std::owner_less<std::weak_ptr<WebSocketSession>>> sessions_;

public:
    SharedState();

    void join(std::shared_ptr<WebSocketSession> session);
    void leave(std::shared_ptr<WebSocketSession> session);
    void send(std::string message, std::shared_ptr<WebSocketSession> ignore_session);
};