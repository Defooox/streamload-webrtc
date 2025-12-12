#pragma once

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <map>
#include <functional>
#include <thread>
#include <atomic>

class WebSocketSession;
class RTCManager;

class SharedState {
    std::mutex mutex_;
    std::set<std::weak_ptr<WebSocketSession>, std::owner_less<std::weak_ptr<WebSocketSession>>> sessions_;
    std::map<std::shared_ptr<WebSocketSession>, std::string> session_to_client_id_;
    std::unique_ptr<RTCManager> rtc_manager_;


    std::thread sync_thread_;
    std::atomic<bool> sync_running_{ false };

    void sendToSession(std::shared_ptr<WebSocketSession> session, const std::string& message);
    void syncLoop(); 

public:
    SharedState();
    ~SharedState();

    void join(std::shared_ptr<WebSocketSession> session);
    void leave(std::shared_ptr<WebSocketSession> session);
    void send(std::string message, std::shared_ptr<WebSocketSession> sender);
};