#pragma once

#include <api/peer_connection_interface.h>
#include <api/create_peerconnection_factory.h>
#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <memory>
#include <string>
#include <functional>

class WebSocketSession;

class RTCManager {
public:
    using OnMessageCallback = std::function<void(const std::string&)>;

    RTCManager();
    ~RTCManager();

    void initialize();
    void createPeerConnection(const std::string& clientId, OnMessageCallback callback);
    void handleOffer(const std::string& clientId, const std::string& sdp);
    void handleAnswer(const std::string& clientId, const std::string& sdp);
    void handleIceCandidate(const std::string& clientId, const std::string& candidate,
        const std::string& sdpMid, int sdpMLineIndex);
    void closePeerConnection(const std::string& clientId);

private:
    class PeerConnectionObserver : public webrtc::PeerConnectionObserver {
    public:
        PeerConnectionObserver(const std::string& id, OnMessageCallback cb);

        void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override;
        void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
        void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
        void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;
        void OnRenegotiationNeeded() override {}
        void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override {}

    private:
        std::string client_id_;
        OnMessageCallback callback_;
    };

    class CreateSessionDescriptionObserver : public webrtc::CreateSessionDescriptionObserver {
    public:
        CreateSessionDescriptionObserver(const std::string& id, OnMessageCallback cb);

        void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
        void OnFailure(webrtc::RTCError error) override;

    private:
        std::string client_id_;
        OnMessageCallback callback_;
    };

    class SetSessionDescriptionObserver : public webrtc::SetSessionDescriptionObserver {
    public:
        SetSessionDescriptionObserver() = default;
        void OnSuccess() override;
        void OnFailure(webrtc::RTCError error) override;
    };

    struct PeerConnectionContext {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection;
        std::unique_ptr<PeerConnectionObserver> observer;
        OnMessageCallback callback;
    };

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> peer_connection_factory_;
    std::map<std::string, PeerConnectionContext> peer_connections_;
    std::unique_ptr<webrtc::Thread> signaling_thread_;
    std::unique_ptr<webrtc::Thread> worker_thread_;
};