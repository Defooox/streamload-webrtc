#pragma once

#include <api/peer_connection_interface.h>
#include <api/create_peerconnection_factory.h>
#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <media/base/adapted_video_track_source.h>
#include <api/media_stream_interface.h>
#include <api/scoped_refptr.h>
#include <rtc_base/ref_counted_object.h>
#include <pc/video_track_source.h>
#include <absl/types/optional.h>

#include <memory>
#include <string>
#include <functional>
#include <map>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>

class RTCManager {
public:
    using OnMessageCallback = std::function<void(const std::string&)>;

    struct StreamingConfig {
        std::string video_file_path;
        bool enable_sync = true;
        bool loop = true;
    };

    RTCManager();
    ~RTCManager();

    void initialize();
    void createPeerConnection(const std::string& clientId, OnMessageCallback callback);
    void startGlobalStream(const StreamingConfig& config);
    void stopGlobalStream();
    void handleOffer(const std::string& clientId, const std::string& sdp);
    void handleAnswer(const std::string& clientId, const std::string& sdp);
    void handleIceCandidate(const std::string& clientId, const std::string& candidate,
        const std::string& sdpMid, int sdpMLineIndex);
    void closePeerConnection(const std::string& clientId);
    void sendPlaybackPosition(const std::string& clientId, double currentTime, bool isPlaying);
    double getCurrentPlaybackTime() const;
    bool isStreaming() const;

private:
    struct PendingIceCandidate; // <-- добавить forward

    void onRemoteOfferSet(const std::string& clientId);
    void onRemoteAnswerSet(const std::string& clientId);

    // NEW: flushPendingIce больше не читает peer_connections_
    void flushPendingIce(
        const std::string& clientId,
        const webrtc::scoped_refptr<webrtc::PeerConnectionInterface>& pc,
        std::vector<PendingIceCandidate>&& pending
    );

class PeerConnectionObserver;
    class CreateSessionDescriptionObserver;
    class DataChannelObserver;
    class RemoteDescriptionObserver;

    class FileVideoTrackSource : public webrtc::AdaptedVideoTrackSource {
    public:
        FileVideoTrackSource(const std::string& file_path, bool loop);
        virtual ~FileVideoTrackSource();

        void Start();
        void Stop();
        double getCurrentTime() const;
        bool isPlaying() const;

        bool is_screencast() const override { return false; }
        absl::optional<bool> needs_denoising() const override { return false; }
        SourceState state() const override { return kLive; }
        bool remote() const override { return false; }

    private:
        std::string file_path_;
        std::thread capture_thread_;
        std::atomic<bool> running_{ false };
        bool should_loop_;
        std::atomic<double> current_time_{ 0.0 };
        std::atomic<bool> is_playing_{ false };

        void CaptureLoop();
    };

    
    struct PendingIceCandidate {
        std::string candidate;
        std::string sdp_mid;
        int sdp_mline_index = 0;
    };

    struct PeerConnectionContext {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection;
        webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel;
        PeerConnectionObserver* observer = nullptr;
        webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track;
        DataChannelObserver* data_channel_observer = nullptr;
        OnMessageCallback callback;
        bool needs_offer = false;
            bool remote_description_set = false;
        std::vector<PendingIceCandidate> pending_ice;
};

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> peer_connection_factory_;
    std::map<std::string, PeerConnectionContext> peer_connections_;
    std::mutex pc_mutex_;

    webrtc::scoped_refptr<FileVideoTrackSource> global_video_source_;

    std::unique_ptr<webrtc::Thread> signaling_thread_;
    std::unique_ptr<webrtc::Thread> worker_thread_;
    std::unique_ptr<webrtc::Thread> network_thread_;
};