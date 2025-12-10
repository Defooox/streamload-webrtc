#pragma warning(disable: 4146)
#pragma warning(disable: 4068)
#pragma warning(disable: 4566) // Отключаем предупреждения о Unicode символах

#include "RTCManager.h"
#include <rtc_base/ref_counted_object.h>
#include <rtc_base/ref_count.h>
#include <rtc_base/ssl_adapter.h>
#include <rtc_base/thread.h>
#include <api/rtc_event_log/rtc_event_log_factory.h>
#include <api/task_queue/default_task_queue_factory.h>
#include <api/jsep.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>
#include <api/video/i420_buffer.h>
#include <rtc_base/time_utils.h>
#include <media/base/adapted_video_track_source.h>

using json = nlohmann::json;

// === OBSERVER КЛАССЫ ===

class LocalSetRemoteDescriptionObserver : public webrtc::SetRemoteDescriptionObserverInterface {
public:
    void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
        if (error.ok()) {
            std::cout << "[OK] SetRemoteDescription succeeded" << std::endl;
        }
        else {
            std::cerr << "[ERR] SetRemoteDescription failed: " << error.message() << std::endl;
        }
    }

protected:
    ~LocalSetRemoteDescriptionObserver() override = default;
};

class RTCManager::DataChannelObserver : public webrtc::DataChannelObserver {
public:
    explicit DataChannelObserver(const std::string& id) : client_id_(id) {}

    void OnStateChange() override {
        std::cout << "[DC] DataChannel state changed for " << client_id_ << std::endl;
    }

    void OnMessage(const webrtc::DataBuffer& buffer) override {
        std::string message(reinterpret_cast<const char*>(buffer.data.data()), buffer.data.size());
        std::cout << "[MSG] Received sync message from " << client_id_ << ": " << message << std::endl;
    }

private:
    std::string client_id_;
};

// КРИТИЧНО: Observer должен быть ref-counted объектом
class RTCManager::PeerConnectionObserver : public webrtc::PeerConnectionObserver,
    public webrtc::RefCountInterface {
public:
    static webrtc::scoped_refptr<PeerConnectionObserver> Create(
        const std::string& id, OnMessageCallback cb) {
        return webrtc::make_ref_counted<PeerConnectionObserver>(id, cb);
    }

    PeerConnectionObserver(const std::string& id, OnMessageCallback cb)
        : client_id_(id), callback_(cb) {
    }

    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override {
        std::cout << "[SIG] Signaling state changed: " << static_cast<int>(new_state) << std::endl;
    }

    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override {
        std::cout << "[ICE] ICE gathering state changed: " << static_cast<int>(new_state) << std::endl;
    }

    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
        std::string sdp;
        candidate->ToString(&sdp);

        json msg;
        msg["type"] = "ice_candidate";
        msg["candidate"] = sdp;
        msg["sdpMid"] = candidate->sdp_mid();
        msg["sdpMLineIndex"] = candidate->sdp_mline_index();

        callback_(msg.dump());
        std::cout << "[ICE] ICE candidate generated" << std::endl;
    }

    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override {
        std::cout << "[DC] Data channel created: " << channel->label() << std::endl;
    }

    void OnRenegotiationNeeded() override {
        std::cout << "[RTC] Renegotiation needed" << std::endl;
    }

    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override {
        std::cout << "[ICE] ICE connection state: " << static_cast<int>(new_state) << std::endl;
    }

protected:
    ~PeerConnectionObserver() override = default;

private:
    std::string client_id_;
    OnMessageCallback callback_;
};

class RTCManager::CreateSessionDescriptionObserver : public webrtc::CreateSessionDescriptionObserver {
public:
    CreateSessionDescriptionObserver(const std::string& id, OnMessageCallback cb)
        : client_id_(id), callback_(cb) {
    }

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
        std::string sdp;
        desc->ToString(&sdp);

        json msg;
        msg["type"] = webrtc::SdpTypeToString(desc->GetType());
        msg["sdp"] = sdp;

        callback_(msg.dump());
        std::cout << "[OK] Session description created: " << webrtc::SdpTypeToString(desc->GetType()) << std::endl;
    }

    void OnFailure(webrtc::RTCError error) override {
        std::cerr << "[ERR] Failed to create session description: " << error.message() << std::endl;
    }

protected:
    ~CreateSessionDescriptionObserver() override = default;

private:
    std::string client_id_;
    OnMessageCallback callback_;
};

class RTCManager::SetSessionDescriptionObserver : public webrtc::SetSessionDescriptionObserver {
public:
    void OnSuccess() override {
        std::cout << "[OK] SetRemoteDescription succeeded (Legacy)" << std::endl;
    }

    void OnFailure(webrtc::RTCError error) override {
        std::cerr << "[ERR] SetRemoteDescription failed (Legacy): " << error.message() << std::endl;
    }

protected:
    ~SetSessionDescriptionObserver() override = default;
};

RTCManager::RTCManager() {
}

RTCManager::~RTCManager() {
    peer_connections_.clear();
    video_sources_.clear();
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

    std::cout << "Initializing WebRTC threads and factory..." << std::endl;

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
        std::cerr << "[ERR] Failed to create PeerConnectionFactory" << std::endl;
        throw std::runtime_error("PC Factory creation failed");
    }

    std::cout << "[OK] WebRTC initialized" << std::endl;
}

void RTCManager::createPeerConnection(const std::string& clientId, OnMessageCallback callback) {
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

    webrtc::PeerConnectionInterface::IceServer stun_server;
    stun_server.uri = "stun:stun.l.google.com:19302";
    config.servers.push_back(stun_server);

    auto observer = webrtc::make_ref_counted<PeerConnectionObserver>(clientId, callback);
    webrtc::PeerConnectionDependencies dependencies(observer.get());

    auto peer_connection_result = peer_connection_factory_->CreatePeerConnectionOrError(
        config, std::move(dependencies));

    if (peer_connection_result.ok()) {
        PeerConnectionContext context;
        context.peer_connection = peer_connection_result.value();
        context.observer = observer;
        context.callback = callback;

        peer_connections_[clientId] = std::move(context);
        std::cout << "[OK] PeerConnection created for client: " << clientId << std::endl;
    }
    else {
        std::cerr << "[ERR] Failed to create PeerConnection: "
            << peer_connection_result.error().message() << std::endl;
    }
}

void RTCManager::startStreaming(const std::string& clientId, const StreamingConfig& config) {
    auto it = peer_connections_.find(clientId);
    if (it == peer_connections_.end()) {
        std::cerr << "[ERR] PeerConnection not found for streaming" << std::endl;
        return;
    }

    if (it->second.is_streaming) {
        std::cerr << "[WARN] Already streaming for client: " << clientId << std::endl;
        return;
    }

    std::cout << "[START] Starting stream for " << clientId << " from: " << config.video_file_path << std::endl;

    // КРИТИЧНО: Используем rtc::make_ref_counted вместо webrtc::make_ref_counted
    auto video_source = webrtc::make_ref_counted<FileVideoTrackSource>(config.video_file_path);
    video_sources_[clientId] = video_source;

    // Создаём видеотрек (передаём scoped_refptr напрямую)
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track =
        peer_connection_factory_->CreateVideoTrack(video_source, "video_stream");

    // Добавляем трек в PeerConnection
    auto result = it->second.peer_connection->AddTrack(video_track, { "video_stream" });
    if (!result.ok()) {
        std::cerr << "[ERR] Failed to add video track: " << result.error().message() << std::endl;
        return;
    }

    // Создаем DataChannel для синхронизации
    if (config.enable_sync) {
        webrtc::DataChannelInit data_channel_config;
        data_channel_config.ordered = true;

        auto data_channel = it->second.peer_connection->CreateDataChannel("sync", &data_channel_config);
        it->second.data_channel = data_channel;

        data_channel->RegisterObserver(new DataChannelObserver(clientId));
        std::cout << "[OK] DataChannel created for sync" << std::endl;
    }

    it->second.is_streaming = true;
    video_source->Start();

    std::cout << "[OK] Streaming active for: " << clientId << std::endl;
}

void RTCManager::stopStreaming(const std::string& clientId) {
    auto source_it = video_sources_.find(clientId);
    if (source_it != video_sources_.end()) {
        source_it->second->Stop();
        video_sources_.erase(source_it);
        std::cout << "[STOP] Stopped video source for: " << clientId << std::endl;
    }

    auto conn_it = peer_connections_.find(clientId);
    if (conn_it != peer_connections_.end()) {
        conn_it->second.is_streaming = false;
        if (conn_it->second.data_channel) {
            conn_it->second.data_channel->Close();
            conn_it->second.data_channel = nullptr;
        }
    }
}

void RTCManager::handleOffer(const std::string& clientId, const std::string& sdp) {
    auto it = peer_connections_.find(clientId);
    if (it == peer_connections_.end()) {
        std::cerr << "[ERR] PeerConnection not found for client: " << clientId << std::endl;
        return;
    }

    webrtc::SdpParseError error;
    auto session_desc = webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &error);

    if (!session_desc) {
        std::cerr << "[ERR] Failed to parse offer SDP: " << error.description << std::endl;
        return;
    }

    auto set_observer = webrtc::make_ref_counted<LocalSetRemoteDescriptionObserver>();
    it->second.peer_connection->SetRemoteDescription(std::move(session_desc), set_observer);


    auto create_observer = webrtc::make_ref_counted<CreateSessionDescriptionObserver>(clientId, it->second.callback);
    it->second.peer_connection->CreateAnswer(create_observer.get(), webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
}

void RTCManager::handleAnswer(const std::string& clientId, const std::string& sdp) {
    auto it = peer_connections_.find(clientId);
    if (it == peer_connections_.end()) {
        std::cerr << "[ERR] PeerConnection not found for client: " << clientId << std::endl;
        return;
    }

    webrtc::SdpParseError error;
    auto session_desc = webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp, &error);

    if (!session_desc) {
        std::cerr << "[ERR] Failed to parse answer SDP: " << error.description << std::endl;
        return;
    }

    auto set_observer = webrtc::make_ref_counted<LocalSetRemoteDescriptionObserver>();
    it->second.peer_connection->SetRemoteDescription(std::move(session_desc), set_observer);
}

void RTCManager::handleIceCandidate(const std::string& clientId, const std::string& candidate,
    const std::string& sdpMid, int sdpMLineIndex) {
    auto it = peer_connections_.find(clientId);
    if (it == peer_connections_.end()) {
        std::cerr << "[ERR] PeerConnection not found for client: " << clientId << std::endl;
        return;
    }

    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidateInterface> candidate_ptr(
        webrtc::CreateIceCandidate(sdpMid, sdpMLineIndex, candidate, &error)
    );

    if (!candidate_ptr) {
        std::cerr << "[ERR] Failed to parse ICE candidate: " << error.description << std::endl;
        return;
    }

    if (!it->second.peer_connection->AddIceCandidate(candidate_ptr.get())) {
        std::cerr << "[ERR] Failed to add ICE candidate for client: " << clientId << std::endl;
    }
}

void RTCManager::closePeerConnection(const std::string& clientId) {
    auto it = peer_connections_.find(clientId);
    if (it != peer_connections_.end()) {
        it->second.peer_connection->Close();
        peer_connections_.erase(it);
        std::cout << "[OK] PeerConnection closed for client: " << clientId << std::endl;
    }
}

void RTCManager::sendPlaybackPosition(const std::string& clientId, double currentTime, bool isPlaying) {
    auto it = peer_connections_.find(clientId);
    if (it == peer_connections_.end() || !it->second.data_channel ||
        it->second.data_channel->state() != webrtc::DataChannelInterface::kOpen) {
        return;
    }

    json sync_msg;
    sync_msg["type"] = "sync";
    sync_msg["currentTime"] = currentTime;
    sync_msg["isPlaying"] = isPlaying;
    sync_msg["timestamp"] = std::chrono::system_clock::now().time_since_epoch().count();

    std::string msg_str = sync_msg.dump();
    webrtc::DataBuffer buffer(msg_str);

    if (!it->second.data_channel->Send(buffer)) {
        std::cerr << "[WARN] Failed to send sync data to: " << clientId << std::endl;
    }
}

RTCManager::FileVideoTrackSource::FileVideoTrackSource()
    : webrtc::AdaptedVideoTrackSource() {
}

RTCManager::FileVideoTrackSource::FileVideoTrackSource(const std::string& file_path)
    : webrtc::AdaptedVideoTrackSource(), file_path_(file_path) {
}

RTCManager::FileVideoTrackSource::~FileVideoTrackSource() {
    Stop();
}

void RTCManager::FileVideoTrackSource::Start() {
    if (running_) return;
    running_ = true;
    capture_thread_ = std::thread(&FileVideoTrackSource::CaptureLoop, this);
    std::cout << "[VIDEO] Video capture thread started for: " << file_path_ << std::endl;
}

void RTCManager::FileVideoTrackSource::Stop() {
    if (!running_) return;
    running_ = false;
    if (capture_thread_.joinable()) {
        capture_thread_.join();
        std::cout << "[VIDEO] Video capture thread stopped" << std::endl;
    }
}

void RTCManager::FileVideoTrackSource::CaptureLoop() {
  
    while (running_) {
    
        webrtc::scoped_refptr<webrtc::I420Buffer> buffer =
            webrtc::I420Buffer::Create(640, 480);

        memset(buffer->MutableDataY(), 0, buffer->StrideY() * buffer->height());
        memset(buffer->MutableDataU(), 128, buffer->StrideU() * (buffer->height() / 2));
        memset(buffer->MutableDataV(), 128, buffer->StrideV() * (buffer->height() / 2));

        webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(buffer)
            .set_timestamp_rtp(static_cast<uint32_t>(webrtc::TimeMicros() * 90 / 1000)) 
            .build();

        OnFrame(frame);

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}