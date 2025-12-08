
#pragma warning(disable: 4146)
#pragma warning(disable: 4068) 

#include "RTCManager.h"
#include <rtc_base/ref_counted_object.h>
#include <rtc_base/ssl_adapter.h>
#include <rtc_base/thread.h>
#include <api/rtc_event_log/rtc_event_log_factory.h>
#include <api/task_queue/default_task_queue_factory.h>
#include <api/jsep.h>
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;


class LocalSetRemoteDescriptionObserver : public webrtc::SetRemoteDescriptionObserverInterface {
public:
    void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
        if (error.ok()) {
            std::cout << "SetRemoteDescription succeeded" << std::endl;
        }
        else {
            std::cerr << "SetRemoteDescription failed: " << error.message() << std::endl;
        }
    }
};


RTCManager::RTCManager() {
}

RTCManager::~RTCManager() {
    peer_connections_.clear();
    peer_connection_factory_ = nullptr;

    if (signaling_thread_) {
        signaling_thread_->Stop();
    }
    if (worker_thread_) {
        worker_thread_->Stop();
    }
}

void RTCManager::initialize() {
    webrtc::InitializeSSL();

    signaling_thread_ = webrtc::Thread::Create();
    signaling_thread_->SetName("SignalingThread", nullptr);
    signaling_thread_->Start();

    worker_thread_ = webrtc::Thread::Create();
    worker_thread_->SetName("WorkerThread", nullptr);
    worker_thread_->Start();

    peer_connection_factory_ = webrtc::CreatePeerConnectionFactory(
        worker_thread_.get(),
        worker_thread_.get(),
        signaling_thread_.get(),
        nullptr,
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        webrtc::CreateBuiltinVideoEncoderFactory(),
        webrtc::CreateBuiltinVideoDecoderFactory(),
        nullptr,
        nullptr
    );

    if (!peer_connection_factory_) {
        std::cerr << "Failed to create PeerConnectionFactory" << std::endl;
    }
}

void RTCManager::createPeerConnection(const std::string& clientId, OnMessageCallback callback) {
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

    webrtc::PeerConnectionInterface::IceServer stun_server;
    stun_server.uri = "stun:stun.l.google.com:19302";
    config.servers.push_back(stun_server);

    auto observer = std::make_unique<PeerConnectionObserver>(clientId, callback);
    webrtc::PeerConnectionDependencies dependencies(observer.get());

    auto peer_connection_result = peer_connection_factory_->CreatePeerConnectionOrError(
        config, std::move(dependencies));

    if (peer_connection_result.ok()) {
        PeerConnectionContext context;
        context.peer_connection = peer_connection_result.value();
        context.observer = std::move(observer);
        context.callback = callback;

        peer_connections_[clientId] = std::move(context);
        std::cout << "PeerConnection created for client: " << clientId << std::endl;
    }
    else {
        std::cerr << "Failed to create PeerConnection: " << peer_connection_result.error().message() << std::endl;
    }
}

void RTCManager::handleOffer(const std::string& clientId, const std::string& sdp) {
    auto it = peer_connections_.find(clientId);
    if (it == peer_connections_.end()) {
        std::cerr << "PeerConnection not found for client: " << clientId << std::endl;
        return;
    }

    webrtc::SdpParseError error;
    auto session_desc = webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &error);

    if (!session_desc) {
        std::cerr << "Failed to parse offer SDP: " << error.description << std::endl;
        return;
    }


    auto set_observer = webrtc::make_ref_counted<LocalSetRemoteDescriptionObserver>();

    it->second.peer_connection->SetRemoteDescription(
        std::move(session_desc),
        set_observer
    );

    auto create_observer = webrtc::make_ref_counted<CreateSessionDescriptionObserver>(clientId, it->second.callback);
    it->second.peer_connection->CreateAnswer(create_observer.get(), webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
}

void RTCManager::handleAnswer(const std::string& clientId, const std::string& sdp) {
    auto it = peer_connections_.find(clientId);
    if (it == peer_connections_.end()) {
        std::cerr << "PeerConnection not found for client: " << clientId << std::endl;
        return;
    }

    webrtc::SdpParseError error;
    auto session_desc = webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp, &error);

    if (!session_desc) {
        std::cerr << "Failed to parse answer SDP: " << error.description << std::endl;
        return;
    }


    auto set_observer = webrtc::make_ref_counted<LocalSetRemoteDescriptionObserver>();

    it->second.peer_connection->SetRemoteDescription(
        std::move(session_desc),
        set_observer
    );
}

void RTCManager::handleIceCandidate(const std::string& clientId, const std::string& candidate,
    const std::string& sdpMid, int sdpMLineIndex) {
    auto it = peer_connections_.find(clientId);
    if (it == peer_connections_.end()) {
        std::cerr << "PeerConnection not found for client: " << clientId << std::endl;
        return;
    }

    webrtc::SdpParseError error;

    std::unique_ptr<webrtc::IceCandidateInterface> candidate_ptr(
        webrtc::CreateIceCandidate(sdpMid, sdpMLineIndex, candidate, &error)
    );

    if (!candidate_ptr) {
        std::cerr << "Failed to parse ICE candidate: " << error.description << std::endl;
        return;
    }


    if (!it->second.peer_connection->AddIceCandidate(candidate_ptr.get())) {
        std::cerr << "Failed to add ICE candidate for client: " << clientId << std::endl;
    }
}

void RTCManager::closePeerConnection(const std::string& clientId) {
    auto it = peer_connections_.find(clientId);
    if (it != peer_connections_.end()) {
        it->second.peer_connection->Close();
        peer_connections_.erase(it);
        std::cout << "PeerConnection closed for client: " << clientId << std::endl;
    }
}


RTCManager::PeerConnectionObserver::PeerConnectionObserver(const std::string& id, OnMessageCallback cb)
    : client_id_(id), callback_(cb) {
}

void RTCManager::PeerConnectionObserver::OnSignalingChange(
    webrtc::PeerConnectionInterface::SignalingState new_state) {
}

void RTCManager::PeerConnectionObserver::OnIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState new_state) {
}

void RTCManager::PeerConnectionObserver::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
    std::string sdp;
    candidate->ToString(&sdp);

    json msg;
    msg["type"] = "ice_candidate";
    msg["candidate"] = sdp;
    msg["sdpMid"] = candidate->sdp_mid();
    msg["sdpMLineIndex"] = candidate->sdp_mline_index();

    callback_(msg.dump());
}

void RTCManager::PeerConnectionObserver::OnDataChannel(
    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
    std::cout << "Data channel created: " << channel->label() << std::endl;
}


RTCManager::CreateSessionDescriptionObserver::CreateSessionDescriptionObserver(
    const std::string& id, OnMessageCallback cb)
    : client_id_(id), callback_(cb) {
}

void RTCManager::CreateSessionDescriptionObserver::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
    std::string sdp;
    desc->ToString(&sdp);

    json msg;
    msg["type"] = webrtc::SdpTypeToString(desc->GetType());
    msg["sdp"] = sdp;

    callback_(msg.dump());
}

void RTCManager::CreateSessionDescriptionObserver::OnFailure(webrtc::RTCError error) {
    std::cerr << "Failed to create session description: " << error.message() << std::endl;
}


void RTCManager::SetSessionDescriptionObserver::OnSuccess() {
    std::cout << "SetRemoteDescription succeeded (Legacy)" << std::endl;
}

void RTCManager::SetSessionDescriptionObserver::OnFailure(webrtc::RTCError error) {
    std::cerr << "SetRemoteDescription failed (Legacy): " << error.message() << std::endl;
}