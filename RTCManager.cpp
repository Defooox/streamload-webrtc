
#pragma warning(disable: 4146)
#pragma warning(disable: 4068)
#pragma warning(disable: 4566)

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


extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/error.h>
}

using json = nlohmann::json;



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

    auto video_source = webrtc::make_ref_counted<FileVideoTrackSource>(config.video_file_path);
    video_sources_[clientId] = video_source;

    webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track =
        peer_connection_factory_->CreateVideoTrack(video_source, "video_stream");

    auto result = it->second.peer_connection->AddTrack(video_track, { "video_stream" });
    if (!result.ok()) {
        std::cerr << "[ERR] Failed to add video track: " << result.error().message() << std::endl;
        return;
    }

 
    if (config.enable_sync) {
        webrtc::DataChannelInit data_channel_config;
        data_channel_config.ordered = true;

        auto data_channel = it->second.peer_connection->CreateDataChannel("sync", &data_channel_config);
        it->second.data_channel = data_channel;
        it->second.data_channel_observer = new DataChannelObserver(clientId);
        data_channel->RegisterObserver(it->second.data_channel_observer);

        std::cout << "[OK] DataChannel created for sync" << std::endl;
    }

    it->second.is_streaming = true;
    it->second.video_source = video_source; 
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
            conn_it->second.data_channel->UnregisterObserver();
            conn_it->second.data_channel->Close();
            conn_it->second.data_channel = nullptr;
        }
        if (conn_it->second.data_channel_observer) {
            delete conn_it->second.data_channel_observer;
            conn_it->second.data_channel_observer = nullptr;
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
    stopStreaming(clientId);

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
    AVFormatContext* format_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* frame_rgb = nullptr; 
    AVPacket* packet = nullptr;
    SwsContext* sws_ctx = nullptr;
    int video_stream_idx = -1;

   
    if (avformat_open_input(&format_ctx, file_path_.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "[ERR] Cannot open video file: " << file_path_ << std::endl;
        return;
    }

    if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
        std::cerr << "[ERR] Cannot find stream info" << std::endl;
        avformat_close_input(&format_ctx);
        return;
    }


    for (unsigned i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }

    if (video_stream_idx == -1) {
        std::cerr << "[ERR] No video stream found" << std::endl;
        avformat_close_input(&format_ctx);
        return;
    }


    AVCodecParameters* codec_params = format_ctx->streams[video_stream_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec) {
        std::cerr << "[ERR] Codec not found" << std::endl;
        avformat_close_input(&format_ctx);
        return;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, codec_params);

    codec_ctx->thread_count = 0;

    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << "[ERR] Cannot open codec" << std::endl;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return;
    }

    frame = av_frame_alloc();
    packet = av_packet_alloc();

    int width = codec_ctx->width;
    int height = codec_ctx->height;

   
    sws_ctx = sws_getContext(
        width, height, codec_ctx->pix_fmt,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

 
    double fps = av_q2d(format_ctx->streams[video_stream_idx]->avg_frame_rate);
    if (fps < 1.0) fps = av_q2d(format_ctx->streams[video_stream_idx]->r_frame_rate);
    if (fps < 1.0 || std::isnan(fps)) {
        fps = 30.0;
        std::cout << "[WARN] FPS not detected, defaulting to 30.0" << std::endl;
    }

    auto frame_duration = std::chrono::microseconds(static_cast<int64_t>(1000000.0 / fps));
    std::cout << "[VIDEO] Playing: " << width << "x" << height << " @ " << fps << " FPS" << std::endl;

    current_time_ = 0.0;
    is_playing_ = true;

    while (running_) {

        auto start_time = std::chrono::steady_clock::now();
        int64_t frame_count = 0;

        av_seek_frame(format_ctx, video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codec_ctx); 

        std::cout << "[VIDEO] Starting/Looping stream..." << std::endl;

     
        while (running_ && av_read_frame(format_ctx, packet) >= 0) {
            if (packet->stream_index == video_stream_idx) {


                int send_ret = avcodec_send_packet(codec_ctx, packet);
                if (send_ret < 0) {
                    std::cerr << "[ERR] Error sending packet to decoder: " << send_ret << std::endl;
                }
                else {

                    while (true) {
                        int receive_ret = avcodec_receive_frame(codec_ctx, frame);

                        if (receive_ret == AVERROR(EAGAIN) || receive_ret == AVERROR_EOF) {
                            break; 
                        }
                        else if (receive_ret < 0) {

                            char err_buf[AV_ERROR_MAX_STRING_SIZE];
                            av_strerror(receive_ret, err_buf, AV_ERROR_MAX_STRING_SIZE);
                            std::cerr << "[ERR] Decoder error: " << err_buf << " (" << receive_ret << ")" << std::endl;
                            break;
                        }

          
                        std::cout << "[VIDEO-FRAME] Decoded and processed frame #" << frame_count << " (TS: " << frame->pts << ")" << std::endl;

                        webrtc::scoped_refptr<webrtc::I420Buffer> i420_buffer =
                            webrtc::I420Buffer::Create(width, height);

      
                        uint8_t* dest[3] = {
                            i420_buffer->MutableDataY(),
                            i420_buffer->MutableDataU(),
                            i420_buffer->MutableDataV()
                        };
                        int dest_stride[3] = {
                            i420_buffer->StrideY(),
                            i420_buffer->StrideU(),
                            i420_buffer->StrideV()
                        };

                        sws_scale(sws_ctx, frame->data, frame->linesize, 0, height, dest, dest_stride);

                        int64_t timestamp_us = frame_count * 1000000 / static_cast<int64_t>(fps);

                        webrtc::VideoFrame video_frame = webrtc::VideoFrame::Builder()
                            .set_video_frame_buffer(i420_buffer)
                            .set_timestamp_us(timestamp_us)
                            .set_rotation(webrtc::kVideoRotation_0)
                            .build();

                        OnFrame(video_frame);

                        current_time_ = static_cast<double>(timestamp_us) / 1000000.0;
                        frame_count++;

                      
                        auto target_time = start_time + frame_duration * frame_count;
                        std::this_thread::sleep_until(target_time);
                    }
                }
            }
            av_packet_unref(packet);
        }
        if (!running_) break;
    }

    // 6. Очистка ресурсов
    av_packet_free(&packet);
    av_frame_free(&frame);
    sws_freeContext(sws_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&format_ctx);

    std::cout << "[VIDEO] Playback finished completely" << std::endl;
}

double RTCManager::FileVideoTrackSource::getCurrentTime() const {
    return current_time_.load();
}

bool RTCManager::FileVideoTrackSource::isPlaying() const {
    return is_playing_.load();
}