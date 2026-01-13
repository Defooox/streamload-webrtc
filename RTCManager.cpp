#pragma warning(disable: 4146)
#pragma warning(disable: 4068)
#pragma warning(disable: 4566)

#include "RTCManager.h"
#include <rtc_base/ref_counted_object.h>
#include <rtc_base/ref_count.h>
#include <rtc_base/ssl_adapter.h>
#include <rtc_base/thread.h>
#include <rtc_base/time_utils.h> 
#include <api/rtc_event_log/rtc_event_log_factory.h>
#include <api/task_queue/default_task_queue_factory.h>
#include <api/jsep.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>
#include <api/video/i420_buffer.h>
#include <media/base/adapted_video_track_source.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/error.h>
}

using json = nlohmann::json;

static const std::string STREAM_ID = "video_stream_0";

class LocalSetSessionDescriptionObserver : public webrtc::SetLocalDescriptionObserverInterface {
public:
    static webrtc::scoped_refptr<webrtc::SetLocalDescriptionObserverInterface> Create() {
        return webrtc::make_ref_counted<LocalSetSessionDescriptionObserver>();
    }

    void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
        if (!error.ok()) {
            std::cerr << "[ERR] SetLocalDescription failed: " << error.message() << std::endl;
        }
        else {
            std::cout << "[INFO] SetLocalDescription succeeded" << std::endl;
        }
    }
};

class LocalSetRemoteDescriptionObserver : public webrtc::SetRemoteDescriptionObserverInterface {
public:
    void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
        if (!error.ok()) {
            std::cerr << "[ERR] SetRemoteDescription failed: " << error.message() << std::endl;
        }
        else {
            std::cout << "[INFO] SetRemoteDescription succeeded" << std::endl;
        }
    }
};

class RTCManager::DataChannelObserver : public webrtc::DataChannelObserver {
public:
    explicit DataChannelObserver(const std::string& id) : client_id_(id) {}
    void OnStateChange() override {
        std::cout << "[DC] State changed for " << client_id_ << std::endl;
    }
    void OnMessage(const webrtc::DataBuffer& buffer) override {
        std::cout << "[DC] Message received from " << client_id_ << std::endl;
    }
private:
    std::string client_id_;
};

class RTCManager::PeerConnectionObserver : public webrtc::PeerConnectionObserver, public webrtc::RefCountInterface {
public:
    explicit PeerConnectionObserver(const std::string& id, OnMessageCallback cb)
        : client_id_(id), callback_(cb) {
    }

    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override {
        const char* state_names[] = { "stable", "have-local-offer", "have-local-pranswer",
                                      "have-remote-offer", "have-remote-pranswer", "closed" };
        std::cout << "[RTC] Signaling state " << client_id_ << ": "
            << state_names[static_cast<int>(new_state)] << std::endl;
    }

    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override {
        const char* states[] = { "new", "gathering", "complete" };
        std::cout << "[ICE] Gathering state " << client_id_ << ": "
            << states[static_cast<int>(new_state)] << std::endl;

        if (new_state == webrtc::PeerConnectionInterface::kIceGatheringComplete) {
            std::cout << "[ICE] ✓ All candidates gathered for " << client_id_ << std::endl;
        }
    }

    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
        std::string sdp;
        candidate->ToString(&sdp);

        std::cout << "\n[ICE] ========================================" << std::endl;
        std::cout << "[ICE] New candidate for " << client_id_ << std::endl;

        std::string protocol = candidate->candidate().protocol();
        std::cout << "[ICE]   Protocol: " << protocol << std::endl;

        const auto& addr = candidate->candidate().address();
        std::cout << "[ICE]   Address: " << addr.ipaddr().ToString()
            << ":" << addr.port() << std::endl;

        std::cout << "[ICE] → Sending to client" << std::endl;
        std::cout << "[ICE] ========================================\n" << std::endl;

        json msg = {
            {"type", "ice_candidate"},
            {"candidate", sdp},
            {"sdpMid", candidate->sdp_mid()},
            {"sdpMLineIndex", candidate->sdp_mline_index()}
        };

        callback_(msg.dump());
    }

    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override {
        std::cout << "[DC] Data channel created for " << client_id_ << std::endl;
    }

    void OnRenegotiationNeeded() override {
        std::cout << "[RTC] Renegotiation needed for " << client_id_ << std::endl;
    }

    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override {
        const char* state_str[] = {
            "new", "checking", "connected", "completed",
            "failed", "disconnected", "closed"
        };

        std::cout << "\n[ICE] ========================================" << std::endl;
        std::cout << "[ICE] Connection state for " << client_id_ << std::endl;
        std::cout << "[ICE] State: " << state_str[static_cast<int>(new_state)] << std::endl;
        std::cout << "[ICE] ========================================\n" << std::endl;

        switch (new_state) {
        case webrtc::PeerConnectionInterface::kIceConnectionNew:
            std::cout << "[ICE] 🔵 New ICE connection" << std::endl;
            break;

        case webrtc::PeerConnectionInterface::kIceConnectionChecking:
            std::cout << "[ICE] 🔄 Checking ICE candidates..." << std::endl;
            std::cout << "[ICE] Trying to establish connectivity..." << std::endl;
            break;

        case webrtc::PeerConnectionInterface::kIceConnectionConnected:
            std::cout << "[ICE] ✓✓✓✓✓✓✓✓✓✓✓✓✓✓✓✓✓✓✓✓" << std::endl;
            std::cout << "[ICE] ✓✓✓ ICE CONNECTED ✓✓✓" << std::endl;
            std::cout << "[ICE] ✓✓✓✓✓✓✓✓✓✓✓✓✓✓✓✓✓✓✓✓" << std::endl;
            std::cout << "[ICE] Media can now flow!" << std::endl;
            break;

        case webrtc::PeerConnectionInterface::kIceConnectionCompleted:
            std::cout << "[ICE] ✓✓✓ ICE COMPLETED ✓✓✓" << std::endl;
            break;

        case webrtc::PeerConnectionInterface::kIceConnectionFailed:
            std::cout << "[ICE] ✗✗✗✗✗✗✗✗✗✗✗✗✗✗✗✗✗✗✗✗" << std::endl;
            std::cout << "[ICE] ✗✗✗ ICE FAILED ✗✗✗" << std::endl;
            std::cout << "[ICE] ✗✗✗✗✗✗✗✗✗✗✗✗✗✗✗✗✗✗✗✗" << std::endl;
            std::cout << "[ICE] Check: firewall, NAT, TURN server" << std::endl;
            break;

        case webrtc::PeerConnectionInterface::kIceConnectionDisconnected:
            std::cout << "[ICE] ⚠️ ICE disconnected" << std::endl;
            break;

        case webrtc::PeerConnectionInterface::kIceConnectionClosed:
            std::cout << "[ICE] 🔴 ICE closed" << std::endl;
            break;
        }

        std::cout << std::endl;
    }

    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) override {
        const char* states[] = { "new", "connecting", "connected", "disconnected", "failed", "closed" };
        std::cout << "[PC] Connection state " << client_id_ << ": "
            << states[static_cast<int>(new_state)] << std::endl;

        if (new_state == webrtc::PeerConnectionInterface::PeerConnectionState::kConnected) {
            std::cout << "[PC] ✓✓✓ PEER CONNECTION ESTABLISHED ✓✓✓" << std::endl;
        }
    }

private:
    std::string client_id_;
    OnMessageCallback callback_;
};

class RTCManager::CreateSessionDescriptionObserver : public webrtc::CreateSessionDescriptionObserver {
public:
    CreateSessionDescriptionObserver(const std::string& id, OnMessageCallback cb, webrtc::PeerConnectionInterface* pc)
        : client_id_(id), callback_(cb), pc_(pc) {
    }

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {

        std::unique_ptr<webrtc::SessionDescriptionInterface> local_desc(desc);

        const auto sdp_type = local_desc->GetType();
        const std::string type_str = webrtc::SdpTypeToString(sdp_type);

        std::cout << "[RTC] CreateSessionDescription SUCCESS for " << client_id_
            << " (type: " << type_str << ")" << std::endl;

        std::string sdp;
        local_desc->ToString(&sdp);

        if (pc_) {
            std::cout << "[RTC] Setting local description..." << std::endl;
            pc_->SetLocalDescription(
                std::move(local_desc),
                LocalSetSessionDescriptionObserver::Create()
            );
        }
        else {
            local_desc.reset();
        }

        std::cout << "[RTC] Sending " << type_str
            << " to " << client_id_ << std::endl;

        json msg = {
            {"type", type_str},
            {"sdp", sdp}
        };

        callback_(msg.dump());
    }

    void OnFailure(webrtc::RTCError error) override {
        std::cerr << "[ERR] Create SDP failed for " << client_id_
            << ": " << error.message() << std::endl;
    }

private:
    std::string client_id_;
    OnMessageCallback callback_;
    webrtc::PeerConnectionInterface* pc_;
};

RTCManager::RTCManager() {}

RTCManager::~RTCManager() {
 
    stopGlobalStream();


    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(pc_mutex_);
        ids.reserve(peer_connections_.size());
        for (const auto& [id, _] : peer_connections_) {
            (void)_;
            ids.push_back(id);
        }
    }
    for (const auto& id : ids) {
        closePeerConnection(id);
    }

    peer_connection_factory_ = nullptr;

    if (signaling_thread_) signaling_thread_->Stop();
    if (worker_thread_) worker_thread_->Stop();
    if (network_thread_) network_thread_->Stop();
}

class RTCManager::RemoteDescriptionObserver
    : public webrtc::SetRemoteDescriptionObserverInterface {
public:
    RemoteDescriptionObserver(RTCManager* mgr, std::string clientId, bool isOffer)
        : mgr_(mgr), clientId_(std::move(clientId)), isOffer_(isOffer) {
    }

    void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
        if (!error.ok()) {
            std::cerr << "[ERR] SetRemoteDescription failed for " << clientId_
                << ": " << error.message() << "\n";
            return;
        }
        if (isOffer_) mgr_->onRemoteOfferSet(clientId_);
        else          mgr_->onRemoteAnswerSet(clientId_);
    }

private:
    RTCManager* mgr_;
    std::string clientId_;
    bool isOffer_;
};

void RTCManager::initialize() {
    std::cout << "[RTC] Initializing WebRTC..." << std::endl;
    webrtc::InitializeSSL();

    network_thread_ = webrtc::Thread::CreateWithSocketServer();
    network_thread_->SetName("NetworkThread", nullptr);
    network_thread_->Start();


    signaling_thread_ = webrtc::Thread::Create();
    signaling_thread_->SetName("SignalingThread", nullptr);
    signaling_thread_->Start();

    worker_thread_ = webrtc::Thread::Create();
    worker_thread_->SetName("WorkerThread", nullptr);
    worker_thread_->Start();

    peer_connection_factory_ = webrtc::CreatePeerConnectionFactory(
        network_thread_.get(),
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
        throw std::runtime_error("Failed to create PC Factory");
    }

    std::cout << "[RTC] WebRTC initialized successfully" << std::endl;
}

void RTCManager::createPeerConnection(const std::string& clientId, OnMessageCallback callback) {
    std::cout << "\n[RTC] ========================================" << std::endl;
    std::cout << "[RTC] Creating PeerConnection for " << clientId << std::endl;
    std::cout << "[RTC] ========================================" << std::endl;

    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

    
    std::cout << "[ICE] Configuring STUN servers..." << std::endl;

    webrtc::PeerConnectionInterface::IceServer stun1;
    stun1.uri = "stun:stun.l.google.com:19302";
    config.servers.push_back(stun1);
    std::cout << "[ICE]   ✓ " << stun1.uri << std::endl;

    webrtc::PeerConnectionInterface::IceServer stun2;
    stun2.uri = "stun:stun1.l.google.com:19302";
    config.servers.push_back(stun2);
    std::cout << "[ICE]   ✓ " << stun2.uri << std::endl;

    
    std::cout << "[ICE] Configuring TURN servers..." << std::endl;

    webrtc::PeerConnectionInterface::IceServer turn1;
    turn1.uri = "turn:numb.viagenie.ca";
    turn1.username = "webrtc@live.com";
    turn1.password = "muazkh";
    config.servers.push_back(turn1);
    std::cout << "[ICE]   ✓ " << turn1.uri << " (primary)" << std::endl;

    webrtc::PeerConnectionInterface::IceServer turn2;
    turn2.uri = "turn:openrelay.metered.ca:80";
    turn2.username = "openrelayproject";
    turn2.password = "openrelayproject";
    config.servers.push_back(turn2);
    std::cout << "[ICE]   ✓ " << turn2.uri << " (backup)" << std::endl;

    webrtc::PeerConnectionInterface::IceServer turn3;
    turn3.uri = "turn:openrelay.metered.ca:443?transport=tcp";
    turn3.username = "openrelayproject";
    turn3.password = "openrelayproject";
    config.servers.push_back(turn3);
    std::cout << "[ICE]   ✓ " << turn3.uri << " (TCP)" << std::endl;


    config.tcp_candidate_policy = webrtc::PeerConnectionInterface::kTcpCandidatePolicyDisabled;
    config.continual_gathering_policy = webrtc::PeerConnectionInterface::GATHER_CONTINUALLY;
    config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
    config.rtcp_mux_policy = webrtc::PeerConnectionInterface::kRtcpMuxPolicyRequire;
    config.type = webrtc::PeerConnectionInterface::kAll;

    std::cout << "[ICE] TCP candidates: DISABLED" << std::endl;
    std::cout << "[ICE] Continual gathering: ENABLED" << std::endl;
    std::cout << "[RTC] ========================================\n" << std::endl;

    auto observer = new webrtc::RefCountedObject<PeerConnectionObserver>(clientId, callback);
    webrtc::PeerConnectionDependencies deps(observer);

    auto result = peer_connection_factory_->CreatePeerConnectionOrError(config, std::move(deps));
    if (!result.ok()) {
        std::cerr << "[ERR] ✗ CreatePeerConnection failed: " << result.error().message() << std::endl;
        return;
    }

    PeerConnectionContext context;
    context.peer_connection = result.value();
    context.observer = observer;
    context.callback = callback;

    webrtc::DataChannelInit dc_config;
    dc_config.ordered = true;

    auto dc = context.peer_connection->CreateDataChannel("sync", &dc_config);
    if (dc) {
        context.data_channel = dc;
        context.data_channel_observer = new DataChannelObserver(clientId);
        dc->RegisterObserver(context.data_channel_observer);
        std::cout << "[DC]  DataChannel 'sync' created for " << clientId << std::endl;
    }
    else {
        context.data_channel = nullptr;
        context.data_channel_observer = nullptr;
        std::cerr << "[DC]  CreateDataChannel returned null for " << clientId << std::endl;
    }

 
    {
        std::lock_guard<std::mutex> lock(pc_mutex_);
        peer_connections_[clientId] = std::move(context);
    }

    std::cout << "[RTC] ✓ PeerConnection created for " << clientId << std::endl;


    if (global_video_source_) {
        std::cout << "[RTC] Stream is active -> attaching track + creating offer for " << clientId << std::endl;

       
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_ref;
        OnMessageCallback cb_ref;

        {
            std::lock_guard<std::mutex> lk(pc_mutex_);
            auto pit = peer_connections_.find(clientId);
            if (pit == peer_connections_.end()) return;
            pc_ref = pit->second.peer_connection;
            cb_ref = pit->second.callback;

    
            if (pit->second.video_track) {
                std::cout << "[RTC] Video track already exists for " << clientId << std::endl;
                return;
            }
        }

        if (pc_ref->signaling_state() != webrtc::PeerConnectionInterface::SignalingState::kStable) {
            std::cout << "[RTC] ⚠️ Not stable, skipping offer for now: " << clientId << std::endl;
            return;
        }


        auto video_track = peer_connection_factory_->CreateVideoTrack(global_video_source_, "video_label");
        auto add_res = pc_ref->AddTrack(video_track, { STREAM_ID });
        if (!add_res.ok()) {
            std::cerr << "[ERR] ✗ AddTrack failed for " << clientId << ": " << add_res.error().message() << std::endl;
            return;
        }

        {
            std::lock_guard<std::mutex> lk(pc_mutex_);
            auto pit = peer_connections_.find(clientId);
            if (pit != peer_connections_.end()) {
                pit->second.video_track = video_track;
            }
        }

        std::cout << "[RTC] ✓ Track added, creating offer for " << clientId << std::endl;

        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        options.offer_to_receive_video = false;
        options.offer_to_receive_audio = false;

        pc_ref->CreateOffer(
            new webrtc::RefCountedObject<CreateSessionDescriptionObserver>(clientId, cb_ref, pc_ref.get()),
            options
        );
    }
    else {
        std::cout << "[RTC] Waiting for server offer/start_stream...\n" << std::endl;
    }
}
    


double RTCManager::getCurrentPlaybackTime() const {
    if (global_video_source_) {
        return global_video_source_->getCurrentTime();
    }
    return 0.0;
}

bool RTCManager::isStreaming() const {
    return global_video_source_ && global_video_source_->isPlaying();
}

void RTCManager::stopGlobalStream() {
    std::cout << "[STREAM] Stopping global stream..." << std::endl;

    if (global_video_source_) {
        global_video_source_->Stop();
        global_video_source_ = nullptr;
        std::cout << "[STREAM] Video source stopped" << std::endl;
    }

    std::lock_guard<std::mutex> lock(pc_mutex_);
    for (auto& [id, ctx] : peer_connections_) {
        if (ctx.video_track) {
            auto senders = ctx.peer_connection->GetSenders();
            for (const auto& sender : senders) {
                if (sender->track() && sender->track()->kind() == "video") {
                    ctx.peer_connection->RemoveTrackOrError(sender);
                    std::cout << "[STREAM] Track removed from " << id << std::endl;
                }
            }
            ctx.video_track = nullptr;
        }
    }

    std::cout << "[STREAM] Global stream stopped" << std::endl;
}

void RTCManager::handleOffer(const std::string& clientId, const std::string& sdp) {
    std::cout << "\n[RTC] ========================================" << std::endl;
    std::cout << "[RTC] Handling Offer from " << clientId << std::endl;
    std::cout << "[RTC] ========================================" << std::endl;

    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> desc =
        webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &error);

    if (!desc) {
        std::cerr << "[ERR] Failed to parse offer: " << error.description << std::endl;
        return;
    }

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_ref;
    bool has_video_track = false;

    {
        std::lock_guard<std::mutex> lock(pc_mutex_);
        auto it = peer_connections_.find(clientId);
        if (it == peer_connections_.end()) {
            std::cerr << "[ERR] No peer connection for " << clientId << std::endl;
            return;
        }

        pc_ref = it->second.peer_connection;
        has_video_track = (it->second.video_track != nullptr);

        auto current_state = pc_ref->signaling_state();
        std::cout << "[RTC] Current signaling state: " << static_cast<int>(current_state) << std::endl;
    }


    if (global_video_source_ && !has_video_track) {
        std::cout << "[RTC] Adding video track BEFORE setting remote description..." << std::endl;

        auto video_track = peer_connection_factory_->CreateVideoTrack(global_video_source_, "video_label");
        auto sender_res = pc_ref->AddTrack(video_track, { STREAM_ID });

        if (sender_res.ok()) {
            std::cout << "[RTC] ✓ Video track added successfully" << std::endl;

          
            std::lock_guard<std::mutex> lock(pc_mutex_);
            auto it = peer_connections_.find(clientId);
            if (it != peer_connections_.end() && !it->second.video_track) {
                it->second.video_track = video_track;
            }
        }
        else {
            std::cerr << "[ERR] ✗ Failed to add track: " << sender_res.error().message() << std::endl;
        }
    }
    else if (!global_video_source_) {
        std::cout << "[RTC] ⚠️ WARNING: No active video source, Answer will have no video!" << std::endl;
    }
    else {
        std::cout << "[RTC] Video track already exists" << std::endl;
    }

  
    std::cout << "[RTC] Setting remote description (offer)..." << std::endl;
    auto obs = webrtc::make_ref_counted<RTCManager::RemoteDescriptionObserver>(this, clientId, true);
    pc_ref->SetRemoteDescription(std::move(desc), obs);

    std::cout << "[RTC] ========================================\n" << std::endl;
}


void RTCManager::handleAnswer(const std::string& clientId, const std::string& sdp) {
    std::cout << "[RTC] Handling Answer from " << clientId << std::endl;


    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> desc =
        webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp, &error);

    if (!desc) {
        std::cerr << "[ERR] Failed to parse answer: " << error.description << std::endl;
        return;
    }


    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_ref;
    {
        std::lock_guard<std::mutex> lock(pc_mutex_);
        auto it = peer_connections_.find(clientId);
        if (it == peer_connections_.end()) {
            std::cerr << "[ERR] No peer connection for " << clientId << std::endl;
            return;
        }
        pc_ref = it->second.peer_connection;
    }


    std::cout << "[RTC] Setting remote description (answer)..." << std::endl;
    auto obs = webrtc::make_ref_counted<RTCManager::RemoteDescriptionObserver>(this, clientId, false);
    pc_ref->SetRemoteDescription(std::move(desc), obs);

    std::cout << "[RTC] Answer processed successfully" << std::endl;
}


void RTCManager::startGlobalStream(const StreamingConfig& config) {
    std::cout << "[RTC] Starting global stream..." << std::endl;

    if (global_video_source_) {
        std::cerr << "[RTC] Stream already running, stopping first..." << std::endl;
        stopGlobalStream();
    }

    global_video_source_ = new webrtc::RefCountedObject<RTCManager::FileVideoTrackSource>(
        config.video_file_path,
        config.loop
    );
    global_video_source_->Start();

    std::cout << "[RTC] Waiting for video source to initialize..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    struct Target {
        std::string clientId;
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        OnMessageCallback cb;
        bool has_track = false;
    };

    std::vector<Target> targets;
    targets.reserve(32);

    {
        std::lock_guard<std::mutex> lock(pc_mutex_);

        if (peer_connections_.empty()) {
            std::cout << "[RTC] No clients connected yet, stream ready." << std::endl;
            return;
        }

        for (auto& [clientId, ctx] : peer_connections_) {
            if (!ctx.peer_connection) continue;

            Target t;
            t.clientId = clientId;
            t.pc = ctx.peer_connection;
            t.cb = ctx.callback;
            t.has_track = (ctx.video_track != nullptr);
            targets.push_back(std::move(t));
        }
    }


    for (auto& t : targets) {
        if (!t.pc) continue;

        auto state = t.pc->signaling_state();
        if (state != webrtc::PeerConnectionInterface::SignalingState::kStable) {
            std::cout << "[RTC] Skip " << t.clientId
                << " because signaling_state != stable (" << static_cast<int>(state) << ")"
                << std::endl;
            continue;
        }

        if (!t.has_track) {
            auto video_track =
                peer_connection_factory_->CreateVideoTrack(global_video_source_, "video_label");

            auto sender_res = t.pc->AddTrack(video_track, { STREAM_ID });
            if (!sender_res.ok()) {
                std::cerr << "[RTC] AddTrack failed for " << t.clientId
                    << ": " << sender_res.error().message() << std::endl;
                continue;
            }


            {
                std::lock_guard<std::mutex> lock(pc_mutex_);
                auto it = peer_connections_.find(t.clientId);
                if (it != peer_connections_.end() && !it->second.video_track) {
                    it->second.video_track = video_track;
                }
            }
        }

        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        options.offer_to_receive_audio = false;
        options.offer_to_receive_video = false;

        t.pc->CreateOffer(
            new webrtc::RefCountedObject<CreateSessionDescriptionObserver>(t.clientId, t.cb, t.pc.get()),
            options
        );
    }

    std::cout << "[RTC] Global stream negotiation started." << std::endl;
}

void RTCManager::handleIceCandidate(const std::string& clientId,
    const std::string& candidate,
    const std::string& sdpMid,
    int sdpMLineIndex) {

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;

    {
        std::lock_guard<std::mutex> lock(pc_mutex_);
        auto it = peer_connections_.find(clientId);
        if (it == peer_connections_.end()) return;

        auto& ctx = it->second;


        if (!ctx.remote_description_set) {
            ctx.pending_ice.push_back({ candidate, sdpMid, sdpMLineIndex });
            std::cout << "[ICE] Buffered candidate for " << clientId
                << " (pending=" << ctx.pending_ice.size() << ")" << std::endl;
            return;
        }

        pc = ctx.peer_connection;
    }


    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidateInterface> cand(
        webrtc::CreateIceCandidate(sdpMid, sdpMLineIndex, candidate, &error));

    if (!cand) {
        std::cerr << "[ERR] Failed to create ICE candidate: " << error.description << std::endl;
        return;
    }

    if (!pc->AddIceCandidate(cand.get())) {
        std::cerr << "[ERR] AddIceCandidate failed for " << clientId << std::endl;
    }
    else {
        std::cout << "[ICE] Candidate added for " << clientId << std::endl;
    }
}


void RTCManager::closePeerConnection(const std::string& clientId) {
    std::cout << "[RTC] Closing PeerConnection for " << clientId << std::endl;

    std::lock_guard<std::mutex> lock(pc_mutex_);
    auto it = peer_connections_.find(clientId);
    if (it == peer_connections_.end()) return;

    auto& ctx = it->second;

    if (ctx.data_channel) {
        if (ctx.data_channel_observer) {
            ctx.data_channel->UnregisterObserver();
            delete ctx.data_channel_observer;
            ctx.data_channel_observer = nullptr;
        }
        ctx.data_channel->Close();
        ctx.data_channel = nullptr;
    }
    else {

        if (ctx.data_channel_observer) {
            delete ctx.data_channel_observer;
            ctx.data_channel_observer = nullptr;
        }
    }

    ctx.video_track = nullptr;

    if (ctx.peer_connection) {
        ctx.peer_connection->Close();
        ctx.peer_connection = nullptr;
    }

    ctx.pending_ice.clear();
    ctx.remote_description_set = false;

    peer_connections_.erase(it);
    std::cout << "[RTC] PeerConnection closed for " << clientId << std::endl;
}

void RTCManager::sendPlaybackPosition(const std::string& clientId, double currentTime, bool isPlaying) {
    std::lock_guard<std::mutex> lock(pc_mutex_);
    auto it = peer_connections_.find(clientId);
    if (it == peer_connections_.end()) return;

    if (it->second.data_channel && it->second.data_channel->state() == webrtc::DataChannelInterface::kOpen) {
        json msg = { {"type", "sync"}, {"currentTime", currentTime}, {"isPlaying", isPlaying} };
        webrtc::DataBuffer buffer(msg.dump());
        it->second.data_channel->Send(buffer);
    }
}

RTCManager::FileVideoTrackSource::FileVideoTrackSource(const std::string& file_path, bool loop)
    : file_path_(file_path), should_loop_(loop) {
    std::cout << "[VIDEO] FileVideoTrackSource created for: " << file_path << std::endl;
}

RTCManager::FileVideoTrackSource::~FileVideoTrackSource() {
    std::cout << "[VIDEO] FileVideoTrackSource destroyed" << std::endl;
    Stop();
}

void RTCManager::FileVideoTrackSource::Start() {
    if (running_) {
        std::cout << "[VIDEO] ⚠️ Already running" << std::endl;
        return;
    }

    std::cout << "[VIDEO] Starting capture thread..." << std::endl;
    running_ = true;
    capture_thread_ = std::thread(&FileVideoTrackSource::CaptureLoop, this);
}

void RTCManager::FileVideoTrackSource::Stop() {
    if (!running_) return;

    std::cout << "[VIDEO] Stopping capture thread..." << std::endl;
    running_ = false;
    if (capture_thread_.joinable()) capture_thread_.join();
    std::cout << "[VIDEO] Capture thread stopped" << std::endl;
}

double RTCManager::FileVideoTrackSource::getCurrentTime() const {
    return current_time_.load();
}

bool RTCManager::FileVideoTrackSource::isPlaying() const {
    return is_playing_.load();
}

void RTCManager::FileVideoTrackSource::CaptureLoop() {
    std::cout << "\n[VIDEO] ========================================" << std::endl;
    std::cout << "[VIDEO] Capture loop started" << std::endl;
    std::cout << "[VIDEO] File: " << file_path_ << std::endl;
    std::cout << "[VIDEO] ========================================\n" << std::endl;

    AVFormatContext* format_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* sws_ctx = nullptr;

    std::cout << "[VIDEO] Opening file..." << std::endl;
    if (avformat_open_input(&format_ctx, file_path_.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "[ERR] Failed to open file: " << file_path_ << std::endl;
        is_playing_ = false;
        return;
    }
    std::cout << "[VIDEO] ✓ File opened" << std::endl;

    if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
        std::cerr << "[ERR] Failed to find stream info" << std::endl;
        avformat_close_input(&format_ctx);
        is_playing_ = false;
        return;
    }
    std::cout << "[VIDEO] ✓ Stream info found" << std::endl;

    int video_stream_idx = -1;
    for (unsigned i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }

    if (video_stream_idx == -1) {
        std::cerr << "[ERR] No video stream found" << std::endl;
        avformat_close_input(&format_ctx);
        is_playing_ = false;
        return;
    }
    std::cout << "[VIDEO] ✓ Video stream found at index " << video_stream_idx << std::endl;

    AVCodecParameters* codec_params = format_ctx->streams[video_stream_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec) {
        std::cerr << "[ERR] Codec not found" << std::endl;
        avformat_close_input(&format_ctx);
        is_playing_ = false;
        return;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, codec_params);

    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << "[ERR] Could not open codec" << std::endl;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        is_playing_ = false;
        return;
    }

    frame = av_frame_alloc();
    packet = av_packet_alloc();

    int width = codec_ctx->width;
    int height = codec_ctx->height;

    std::cout << "[VIDEO] Video properties: " << width << "x" << height << std::endl;

    sws_ctx = sws_getContext(width, height, codec_ctx->pix_fmt,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    double fps = av_q2d(format_ctx->streams[video_stream_idx]->avg_frame_rate);
    if (fps < 1.0 || fps > 120.0) fps = 30.0;

    std::cout << "[VIDEO] FPS: " << fps << std::endl;

    int64_t frame_delay_us = static_cast<int64_t>(1000000.0 / fps);

    is_playing_ = true;

    auto playback_start_time = std::chrono::steady_clock::now();
    int64_t frame_count = 0;

    std::cout << "[VIDEO] 🎬 Starting frame loop...\n" << std::endl;

    while (running_) {
        int ret = av_read_frame(format_ctx, packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF && should_loop_) {
                std::cout << "[VIDEO] 🔄 Looping video..." << std::endl;
                av_seek_frame(format_ctx, video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(codec_ctx);
                playback_start_time = std::chrono::steady_clock::now();
                frame_count = 0;
                continue;
            }
            else {
                std::cout << "[VIDEO] End of file reached" << std::endl;
                break;
            }
        }

        if (packet->stream_index == video_stream_idx) {
            ret = avcodec_send_packet(codec_ctx, packet);
            if (ret < 0) {
                std::cerr << "[ERR] Error sending packet to decoder" << std::endl;
                av_packet_unref(packet);
                continue;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(codec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) {
                    std::cerr << "[ERR] Error receiving frame from decoder" << std::endl;
                    break;
                }

                auto expected_time = playback_start_time + std::chrono::microseconds(frame_count * frame_delay_us);
                auto now = std::chrono::steady_clock::now();

                if (expected_time > now) {
                    std::this_thread::sleep_for(expected_time - now);
                }

                webrtc::scoped_refptr<webrtc::I420Buffer> i420_buffer = webrtc::I420Buffer::Create(width, height);
                uint8_t* dest[3] = { i420_buffer->MutableDataY(), i420_buffer->MutableDataU(), i420_buffer->MutableDataV() };
                int dest_stride[3] = { i420_buffer->StrideY(), i420_buffer->StrideU(), i420_buffer->StrideV() };

                sws_scale(sws_ctx, frame->data, frame->linesize, 0, height, dest, dest_stride);

                int64_t timestamp_us = frame_count * 1000000 / static_cast<int64_t>(fps);
                webrtc::VideoFrame video_frame = webrtc::VideoFrame::Builder()
                    .set_video_frame_buffer(i420_buffer)
                    .set_timestamp_us(timestamp_us)
                    .set_rotation(webrtc::kVideoRotation_0)
                    .build();

                OnFrame(video_frame);

                current_time_ = static_cast<double>(frame_count) / fps;
                frame_count++;

                if (frame_count % 150 == 0) {
                    std::cout << "[VIDEO] 📹 Frames: " << frame_count
                        << " | Time: " << current_time_.load() << "s" << std::endl;
                }
            }
        }
        av_packet_unref(packet);
    }

    is_playing_ = false;

    std::cout << "\n[VIDEO] Cleaning up..." << std::endl;
    av_frame_free(&frame);
    av_packet_free(&packet);
    sws_freeContext(sws_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&format_ctx);
    std::cout << "[VIDEO] ✓ Cleanup complete\n" << std::endl;
}

void RTCManager::flushPendingIce(
    const std::string& clientId,
    const webrtc::scoped_refptr<webrtc::PeerConnectionInterface>& pc,
    std::vector<PendingIceCandidate>&& pending
) {
    if (!pc) return;
    if (pending.empty()) return;

    std::cout << "[ICE] Flushing " << pending.size()
        << " buffered candidate(s) for " << clientId << std::endl;

    for (const auto& p : pending) {
        webrtc::SdpParseError error;
        std::unique_ptr<webrtc::IceCandidateInterface> cand(
            webrtc::CreateIceCandidate(p.sdp_mid, p.sdp_mline_index, p.candidate, &error));

        if (!cand) {
            std::cerr << "[ERR] Failed to parse buffered ICE: " << error.description << std::endl;
            continue;
        }

        if (!pc->AddIceCandidate(cand.get())) {
            std::cerr << "[ERR] AddIceCandidate (buffered) failed for " << clientId << std::endl;
        }
    }
}

void RTCManager::onRemoteOfferSet(const std::string& clientId) {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    OnMessageCallback cb;
    std::vector<PendingIceCandidate> pending;

    {
        std::lock_guard<std::mutex> lock(pc_mutex_);
        auto it = peer_connections_.find(clientId);
        if (it == peer_connections_.end()) return;

        auto& ctx = it->second;
        ctx.remote_description_set = true;

        pc = ctx.peer_connection;
        cb = ctx.callback;

        pending = std::move(ctx.pending_ice);
        ctx.pending_ice.clear();
    }

    flushPendingIce(clientId, pc, std::move(pending));

    std::cout << "[RTC] Remote offer set. Creating answer for " << clientId << std::endl;

    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    options.offer_to_receive_audio = false;
    options.offer_to_receive_video = false;

    pc->CreateAnswer(
        new webrtc::RefCountedObject<CreateSessionDescriptionObserver>(clientId, cb, pc.get()),
        options
    );
}

void RTCManager::onRemoteAnswerSet(const std::string& clientId) {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    std::vector<PendingIceCandidate> pending;

    {
        std::lock_guard<std::mutex> lock(pc_mutex_);
        auto it = peer_connections_.find(clientId);
        if (it == peer_connections_.end()) return;

        auto& ctx = it->second;
        ctx.remote_description_set = true;

        pc = ctx.peer_connection;

        pending = std::move(ctx.pending_ice);
        ctx.pending_ice.clear();
    }

    flushPendingIce(clientId, pc, std::move(pending));
}
