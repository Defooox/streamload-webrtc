#pragma warning(disable: 4146)
#pragma warning(disable: 4068)
#pragma warning(disable: 4566)

#include "SharedState.h"
#include "WebSocketSession.h"
#include "RTCManager.h"

#include <nlohmann/json.hpp>
#include <iostream>
#include <vector>
#include <chrono>

using json = nlohmann::json;

SharedState::SharedState() : rtc_manager_(std::make_unique<RTCManager>()) {
    std::cout << "[STATE] Initializing SharedState..." << std::endl;
    rtc_manager_->initialize();
    sync_running_ = true;
    sync_thread_ = std::thread(&SharedState::syncLoop, this);
    std::cout << "[STATE] SharedState initialized" << std::endl;
}

SharedState::~SharedState() {
    std::cout << "[STATE] Shutting down SharedState..." << std::endl;
    sync_running_ = false;
    if (sync_thread_.joinable()) sync_thread_.join();
    std::cout << "[STATE] SharedState shut down" << std::endl;
}

void SharedState::join(std::shared_ptr<WebSocketSession> session) {
    std::string client_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        sessions_.insert(session);

   
        client_id = "client_" + std::to_string(reinterpret_cast<uintptr_t>(session.get()));

    
        session_to_client_id_[std::weak_ptr<WebSocketSession>(session)] = client_id;

        std::cout << "[STATE] ======================================" << std::endl;
        std::cout << "[STATE] Client joined: " << client_id << std::endl;
        std::cout << "[STATE] Total clients: " << sessions_.size() << std::endl;
        std::cout << "[STATE] ======================================" << std::endl;
    }


    {
        std::lock_guard<std::mutex> rtc_lock(rtc_mutex_);

    
        std::weak_ptr<WebSocketSession> weak_session = session;

        rtc_manager_->createPeerConnection(client_id, [this, weak_session](const std::string& msg) {
            if (auto s = weak_session.lock()) {
                this->sendToSession(s, msg);
            }
            else {
                std::cerr << "[STATE] Session expired, cannot send message!" << std::endl;
            }
            });
    }
}

void SharedState::leave(std::shared_ptr<WebSocketSession> session) {
    std::string client_id;

    {
        std::lock_guard<std::mutex> lock(mutex_);

    
        auto it = session_to_client_id_.find(std::weak_ptr<WebSocketSession>(session));
        if (it != session_to_client_id_.end()) {
            client_id = it->second;
            session_to_client_id_.erase(it);
        }

        sessions_.erase(std::weak_ptr<WebSocketSession>(session));

        std::cout << "[STATE] Remaining clients: " << sessions_.size() << std::endl;
    }

    if (!client_id.empty()) {
        std::cout << "[STATE] Client leaving: " << client_id << std::endl;
        std::lock_guard<std::mutex> rtc_lock(rtc_mutex_);
        rtc_manager_->closePeerConnection(client_id);
    }
}

void SharedState::send(std::string message, std::shared_ptr<WebSocketSession> sender) {
    std::cout << "\n[STATE] ========================================" << std::endl;
    std::cout << "[STATE] Received message: " << message << std::endl;
    std::cout << "[STATE] ========================================" << std::endl;

    try {
        auto j = json::parse(message);
        std::string type = j.value("type", "");

        std::cout << "[STATE] Message type: " << type << std::endl;

        std::string client_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto it = session_to_client_id_.find(std::weak_ptr<WebSocketSession>(sender));
            if (it == session_to_client_id_.end()) {
                std::cerr << "[STATE] ❌ ERROR: Sender session not found!" << std::endl;
                return;
            }
            client_id = it->second;
        }

        std::cout << "[STATE] Client ID: " << client_id << std::endl;

        if (type == "start_stream") {
            std::string file_path = j.value("file_path", "");

            std::cout << "\n[STATE] ========================================" << std::endl;
            std::cout << "[STATE] START_STREAM REQUEST" << std::endl;
            std::cout << "[STATE] Client: " << client_id << std::endl;
            std::cout << "[STATE] File: " << file_path << std::endl;
            std::cout << "[STATE] ========================================\n" << std::endl;

            if (file_path.empty()) {
                std::cerr << "[STATE] ERROR: Empty file path!" << std::endl;
                return;
            }

            RTCManager::StreamingConfig config;
            config.video_file_path = file_path;
            config.enable_sync = true;
            config.loop = true;

            std::cout << "[STATE] Calling rtc_manager_->startGlobalStream()..." << std::endl;
            { std::lock_guard<std::mutex> rtc_lock(rtc_mutex_); rtc_manager_->startGlobalStream(config); }
            std::cout << "[STATE] startGlobalStream() completed\n" << std::endl;
        }
        else if (type == "stop_stream") {
            std::cout << "[STATE] STOP_STREAM REQUEST from " << client_id << std::endl;
            { std::lock_guard<std::mutex> rtc_lock(rtc_mutex_); rtc_manager_->stopGlobalStream(); }
            std::cout << "[STATE] Stream stopped\n" << std::endl;
        }
        else if (type == "offer") {
            std::string sdp = j.value("sdp", "");
            std::cout << "[STATE] OFFER from " << client_id << " (SDP length: " << sdp.length() << ")" << std::endl;
            { std::lock_guard<std::mutex> rtc_lock(rtc_mutex_); rtc_manager_->handleOffer(client_id, sdp); }
        }
        else if (type == "answer") {
            std::string sdp = j.value("sdp", "");
            std::cout << "[STATE] ANSWER from " << client_id << " (SDP length: " << sdp.length() << ")" << std::endl;
            { std::lock_guard<std::mutex> rtc_lock(rtc_mutex_); rtc_manager_->handleAnswer(client_id, sdp); }
        }
        else if (type == "ice_candidate") {
            std::string candidate = j.value("candidate", "");
            std::string sdpMid = j.value("sdpMid", "");
            int sdpMLineIndex = j.value("sdpMLineIndex", 0);
            std::cout << "[STATE] ICE_CANDIDATE from " << client_id << std::endl;
            { std::lock_guard<std::mutex> rtc_lock(rtc_mutex_); rtc_manager_->handleIceCandidate(client_id, candidate, sdpMid, sdpMLineIndex); }
        }
        else {
            std::cerr << "[STATE] Unknown message type: " << type << std::endl;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[STATE] ERROR parsing message: " << e.what() << std::endl;
        std::cerr << "[STATE] Message was: " << message << std::endl;
    }
}

void SharedState::sendToSession(std::shared_ptr<WebSocketSession> session, const std::string& message) {
    try {
        auto j = json::parse(message);
        std::string type = j.value("type", "");
        std::cout << "[STATE] Sending to client: " << type << std::endl;
    }
    catch (...) {
        std::cout << "[STATE] Sending to client (non-JSON)" << std::endl;
    }

    
    session->send(std::make_shared<std::string const>(message));
}

void SharedState::syncLoop() {
    std::cout << "[STATE] Sync loop started" << std::endl;

    while (sync_running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));


        bool streaming = false;
        double time = 0.0;
        {
            std::lock_guard<std::mutex> rtc_lock(rtc_mutex_);
            streaming = rtc_manager_->isStreaming();
            if (streaming) time = rtc_manager_->getCurrentPlaybackTime();
        }
        if (!streaming) continue;

      
        std::vector<std::string> client_ids;
        client_ids.reserve(64);

        {
            std::lock_guard<std::mutex> lock(mutex_);

            for (auto it = session_to_client_id_.begin(); it != session_to_client_id_.end();) {
                if (it->first.expired()) {
                    it = session_to_client_id_.erase(it);
                    continue;
                }
                client_ids.push_back(it->second);
                ++it;
            }


            for (auto it = sessions_.begin(); it != sessions_.end();) {
                if (it->expired()) it = sessions_.erase(it);
                else ++it;
            }
        }

        if (client_ids.empty()) continue;

        {
            std::lock_guard<std::mutex> rtc_lock(rtc_mutex_);
            for (const auto& client_id : client_ids) {
                rtc_manager_->sendPlaybackPosition(client_id, time, true);
            }
        }
    }

    std::cout << "[STATE] Sync loop stopped" << std::endl;
}
