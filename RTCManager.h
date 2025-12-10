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

class WebSocketSession;

class RTCManager {
public:
    using OnMessageCallback = std::function<void(const std::string&)>;

    struct StreamingConfig {
        std::string video_file_path;
        bool enable_sync = true;
    };

    RTCManager();
    ~RTCManager();

    void initialize();
    void createPeerConnection(const std::string& clientId, OnMessageCallback callback);
    void startStreaming(const std::string& clientId, const StreamingConfig& config);
    void stopStreaming(const std::string& clientId);
    void handleOffer(const std::string& clientId, const std::string& sdp);
    void handleAnswer(const std::string& clientId, const std::string& sdp);
    void handleIceCandidate(const std::string& clientId, const std::string& candidate,
        const std::string& sdpMid, int sdpMLineIndex);
    void closePeerConnection(const std::string& clientId);
    void sendPlaybackPosition(const std::string& clientId, double currentTime, bool isPlaying);

private:
    class PeerConnectionObserver;
    class CreateSessionDescriptionObserver;
    class SetSessionDescriptionObserver;
    class DataChannelObserver;


    class FileVideoTrackSource : public webrtc::AdaptedVideoTrackSource {
    public:
        FileVideoTrackSource();
        explicit FileVideoTrackSource(const std::string& file_path);

        void Start();
        void Stop();


        bool is_screencast() const override { return false; }
        absl::optional<bool> needs_denoising() const override { return false; }

      
        SourceState state() const override {
            return running_ ? kLive : kEnded;
        }

        bool remote() const override {
            return false;
        }

    protected:

        ~FileVideoTrackSource() override;

    private:
        std::string file_path_;
        std::thread capture_thread_;
        std::atomic<bool> running_{ false };
        void CaptureLoop();
    };

    struct PeerConnectionContext {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection;
        webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel;
        webrtc::scoped_refptr<PeerConnectionObserver> observer;
        OnMessageCallback callback;
        bool is_streaming = false;
    };

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> peer_connection_factory_;
    std::map<std::string, PeerConnectionContext> peer_connections_;
    std::map<std::string, webrtc::scoped_refptr<FileVideoTrackSource>> video_sources_;
    std::unique_ptr<webrtc::Thread> signaling_thread_;
    std::unique_ptr<webrtc::Thread> worker_thread_;
};