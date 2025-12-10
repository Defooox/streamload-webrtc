// SharedState.cpp - с добавленной синхронизацией воспроизведения

#pragma warning(disable: 4566)
#pragma warning(disable: 4146)
#pragma warning(disable: 4068)

#include "SharedState.h"
#include "WebSocketSession.h"
#include "RTCManager.h"
#include <nlohmann/json.hpp>
#include <iostream>

using json = nlohmann::json;

SharedState::SharedState() : rtc_manager_(std::make_unique<RTCManager>()) {
    rtc_manager_->initialize();

    // Запускаем поток для отправки синхронизации
    sync_running_ = true;
    sync_thread_ = std::thread(&SharedState::syncLoop, this);
}

SharedState::~SharedState() {
    sync_running_ = false;
    if (sync_thread_.joinable()) {
        sync_thread_.join();
    }
}

void SharedState::join(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.insert(session);

    std::string client_id = "client_" + std::to_string(reinterpret_cast<uintptr_t>(session.get()));
    session_to_client_id_[session] = client_id;

    std::cout << "[JOIN] Client joined: " << client_id << " (Total: " << sessions_.size() << ")" << std::endl;
}

void SharedState::leave(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = session_to_client_id_.find(session);
    if (it != session_to_client_id_.end()) {
        rtc_manager_->closePeerConnection(it->second);
        session_to_client_id_.erase(it);
    }

    sessions_.erase(session);
    std::cout << "[LEAVE] Client left (Remaining: " << sessions_.size() << ")" << std::endl;
}

void SharedState::send(std::string message, std::shared_ptr<WebSocketSession> sender) {
    try {
        auto j = json::parse(message);
        std::string type = j.value("type", "");

        std::lock_guard<std::mutex> lock(mutex_);

        auto it = session_to_client_id_.find(sender);
        if (it == session_to_client_id_.end()) {
            std::cerr << "[WARN] Session not found in client ID map" << std::endl;
            return;
        }

        std::string client_id = it->second;

        if (type == "start_stream") {
            std::string file_path = j.value("file_path", "");
            std::cout << "[CMD] Start stream command from " << client_id << ": " << file_path << std::endl;

            RTCManager::StreamingConfig config;
            config.video_file_path = file_path;
            config.enable_sync = true;

            rtc_manager_->createPeerConnection(client_id,
                [this, sender](const std::string& msg) {
                    this->sendToSession(sender, msg);
                });

            rtc_manager_->startStreaming(client_id, config);
            return;
        }

        if (type == "play" || type == "pause" || type == "seek") {
          
            auto const ss = std::make_shared<std::string const>(std::move(message));
            for (auto& weak_session : sessions_) {
                if (auto session = weak_session.lock()) {
                    session->send(ss);
                }
            }
        }

        if (type == "stop_stream") {
            std::cout << "[CMD] Stop stream command from " << client_id << std::endl;
            rtc_manager_->stopStreaming(client_id);
            return;
        }

        if (type == "offer") {
            std::cout << "[SIG] Received offer from " << client_id << std::endl;
            std::string sdp = j.value("sdp", "");
            rtc_manager_->handleOffer(client_id, sdp);
        }
        else if (type == "answer") {
            std::cout << "[SIG] Received answer from " << client_id << std::endl;
            std::string sdp = j.value("sdp", "");
            rtc_manager_->handleAnswer(client_id, sdp);
        }
        else if (type == "ice_candidate") {
            std::cout << "[SIG] Received ICE candidate from " << client_id << std::endl;
            std::string candidate = j.value("candidate", "");
            std::string sdpMid = j.value("sdpMid", "");
            int sdpMLineIndex = j.value("sdpMLineIndex", 0);
            rtc_manager_->handleIceCandidate(client_id, candidate, sdpMid, sdpMLineIndex);
        }
        else if (type == "sync_request") {
            // Клиент запрашивает текущее состояние синхронизации
            std::cout << "[SYNC] Sync request from " << client_id << std::endl;
        }
        else {
            // Ретрансляция другим клиентам
            auto const ss = std::make_shared<std::string const>(std::move(message));
            for (auto& weak_session : sessions_) {
                if (auto session = weak_session.lock()) {
                    if (session != sender) {
                        session->send(ss);
                    }
                }
            }
        }
    }
    catch (const json::parse_error& e) {
        std::cerr << "[ERR] JSON parse error: " << e.what() << std::endl;
    }
}

void SharedState::sendToSession(std::shared_ptr<WebSocketSession> session, const std::string& message) {
    auto ss = std::make_shared<std::string const>(message);

    for (auto& weak_session : sessions_) {
        if (auto s = weak_session.lock()) {
            if (s == session) {
                s->send(ss);
                return;
            }
        }
    }
}

void SharedState::syncLoop() {
    std::cout << "[SYNC] Sync loop started" << std::endl;

    while (sync_running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Синхронизация каждые 100ms

        std::lock_guard<std::mutex> lock(mutex_);

        // Отправляем состояние воспроизведения каждому подключенному клиенту
        for (const auto& [session, client_id] : session_to_client_id_) {
            rtc_manager_->sendPlaybackPosition(client_id, 0.0, true);
        }
    }

    std::cout << "[SYNC] Sync loop stopped" << std::endl;
}