#include "SharedState.h"
#include "WebSocketSession.h"

SharedState::SharedState() {}

void SharedState::join(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.insert(session);
}

void SharedState::leave(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(session);
}


void SharedState::send(std::string message, std::shared_ptr<WebSocketSession> ignore_session) {

    std::vector<std::weak_ptr<WebSocketSession>> v;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        v.reserve(sessions_.size());
        for (auto const& wp : sessions_)
            v.push_back(wp);
    }

    for (auto const& wp : v) {
        if (auto sp = wp.lock()) {
            if (sp != ignore_session) {
                // Передаем владение сообщением в сессию
                sp->send(std::make_shared<std::string const>(message));
            }
        }
    }
}