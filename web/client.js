class WebRTCClient {
    constructor() {
        this.ws = null;
        this.pc = null;
        this.dataChannel = null;
        this.isConnected = false;
        this.currentFilePath = null;
        this.uploadedFiles = [];
        this.syncEnabled = true;
        this.localTimeUpdate = false;

        this.init();
    }

    init() {
        this.setupElements();
        this.setupEventListeners();
        this.connectWebSocket();
        this.log('Client initialized', 'success');
    }

    setupElements() {
        this.statusEl = document.getElementById('connectionStatus');
        this.viewerCountEl = document.getElementById('viewerCount');
        this.syncIndicatorEl = document.getElementById('syncIndicator');

        this.videoPlayer = document.getElementById('videoPlayer');
        this.videoPlaceholder = document.getElementById('videoPlaceholder');

  
        this.playBtn = document.getElementById('playBtn');
        this.pauseBtn = document.getElementById('pauseBtn');
        this.stopBtn = document.getElementById('stopBtn');


        this.uploadArea = document.getElementById('uploadArea');
        this.fileInput = document.getElementById('fileInput');
        this.fileList = document.getElementById('fileList');
        this.uploadProgress = document.getElementById('uploadProgress');
        this.uploadProgressFill = document.getElementById('uploadProgressFill');


        this.progressBar = document.getElementById('progressBar');
        this.progressFill = document.getElementById('progressFill');
        this.timeDisplay = document.getElementById('timeDisplay');

   
        this.logContainer = document.getElementById('logContainer');
    }

    setupEventListeners() {
        this.uploadArea.addEventListener('click', () => this.fileInput.click());
        this.fileInput.addEventListener('change', (e) => this.handleFileSelect(e));

 
        this.uploadArea.addEventListener('dragover', (e) => {
            e.preventDefault();
            this.uploadArea.classList.add('dragover');
        });

        this.uploadArea.addEventListener('dragleave', () => {
            this.uploadArea.classList.remove('dragover');
        });

        this.uploadArea.addEventListener('drop', (e) => {
            e.preventDefault();
            this.uploadArea.classList.remove('dragover');
            if (e.dataTransfer.files.length) {
                this.uploadFile(e.dataTransfer.files[0]);
            }
        });


        this.playBtn.addEventListener('click', () => this.play());
        this.pauseBtn.addEventListener('click', () => this.pause());
        this.stopBtn.addEventListener('click', () => this.stop());


        this.progressBar.addEventListener('click', (e) => this.seek(e));

        this.videoPlayer.addEventListener('timeupdate', () => this.onVideoTimeUpdate());
        this.videoPlayer.addEventListener('play', () => this.onVideoPlay());
        this.videoPlayer.addEventListener('pause', () => this.onVideoPause());
        this.videoPlayer.addEventListener('loadedmetadata', () => this.onVideoLoaded());
    }

    connectWebSocket() {
        this.log('Connecting to signaling server...', 'info');
        this.updateStatus('connecting');

        const host = location.hostname;
        const protocol = (location.protocol === 'https:') ? 'wss://' : 'ws://';
        const wsURL = protocol + host + ':8080';

        this.log(`Attempting connection to: ${wsURL}`, 'info');

        try {
            this.ws = new WebSocket(wsURL);
        } catch (e) {
            this.log(`Failed to create WebSocket: ${e.message}`, 'error');
            this.updateStatus('disconnected');
            setTimeout(() => this.connectWebSocket(), 5000);
            return;
        }


        this.ws.onopen = () => {
            this.log('WebSocket connected successfully', 'success');
            this.isConnected = true;
            this.updateStatus('connected');
        };

        this.ws.onmessage = (event) => {
            this.handleSignalingMessage(JSON.parse(event.data));
        };

        this.ws.onerror = (error) => {

            this.log('WebSocket connection error (check port 8080 and firewall)', 'error');
        };

        this.ws.onclose = (event) => {
    
            let reason = (event.code === 1006) ? "Connection failed (Firewall/Port 8080 blocked?)" : "Disconnected";
            this.log(`WebSocket closed: ${reason}`, 'warning');
            this.isConnected = false;
            this.updateStatus('disconnected');

    
            setTimeout(() => this.connectWebSocket(), 3000);
        };
    }

    async handleSignalingMessage(message) {
        try {
            switch (message.type) {
                case 'offer':
                    await this.handleOffer(message.sdp);
                    break;

                case 'answer':
                    await this.handleAnswer(message.sdp);
                    break;

                case 'ice_candidate':
                    await this.handleIceCandidate(message);
                    break;

                case 'sync':
                    this.handleSync(message);
                    break;

                default:
                    this.log('Unknown message type: ' + message.type, 'warning');
            }
        } catch (error) {
            this.log('Error handling message: ' + error.message, 'error');
        }
    }

    createPeerConnection() {
        const config = {
            iceServers: [
                { urls: 'stun:stun.l.google.com:19302' },
                { urls: 'stun:stun1.l.google.com:19302' }
            ]
        };

        this.pc = new RTCPeerConnection(config);
        this.log('PeerConnection created', 'info');

   
        this.pc.onicecandidate = (event) => {
            if (event.candidate) {
                this.sendSignaling({
                    type: 'ice_candidate',
                    candidate: event.candidate.candidate,
                    sdpMid: event.candidate.sdpMid,
                    sdpMLineIndex: event.candidate.sdpMLineIndex
                });
                this.log('ICE candidate sent', 'info');
            }
        };


        this.pc.onconnectionstatechange = () => {
            this.log('Connection state: ' + this.pc.connectionState, 'info');

            if (this.pc.connectionState === 'connected') {
                this.syncIndicatorEl.style.display = 'inline-flex';
            } else if (this.pc.connectionState === 'disconnected' ||
                this.pc.connectionState === 'failed') {
                this.syncIndicatorEl.style.display = 'none';
            }
        };


        this.pc.ontrack = (event) => {
            this.log('Received video track', 'success');
            this.videoPlayer.srcObject = event.streams[0];
            this.videoPlayer.style.display = 'block';
            this.videoPlaceholder.style.display = 'none';
            this.enableControls();

      
            this.videoPlayer.play().catch(e => {
                this.log(`Autoplay blocked: ${e.name}. Please click the Play button.`, 'warning');
            });
        };

        this.pc.ondatachannel = (event) => {
            this.dataChannel = event.channel;
            this.setupDataChannel();
            this.log('DataChannel received', 'success');
        };
    }

    setupDataChannel() {
        this.dataChannel.onopen = () => {
            this.log('DataChannel opened', 'success');
        };

        this.dataChannel.onmessage = (event) => {
            const message = JSON.parse(event.data);
            if (message.type === 'sync') {
                this.applySyncData(message);
            }
        };

        this.dataChannel.onclose = () => {
            this.log('DataChannel closed', 'warning');
        };
    }

    async handleOffer(sdp) {
        if (!this.pc) {
            this.createPeerConnection();
        }

        this.log('Received offer, creating answer...', 'info');

        await this.pc.setRemoteDescription({ type: 'offer', sdp });
        const answer = await this.pc.createAnswer();
        await this.pc.setLocalDescription(answer);

        this.sendSignaling({
            type: 'answer',
            sdp: answer.sdp
        });

        this.log('Answer sent', 'success');
    }

    async handleAnswer(sdp) {
        if (!this.pc) {
            this.log('No PeerConnection for answer', 'error');
            return;
        }

        await this.pc.setRemoteDescription({ type: 'answer', sdp });
        this.log('Answer processed', 'success');
    }

    async handleIceCandidate(message) {
        if (!this.pc) {
            this.log('No PeerConnection for ICE candidate', 'error');
            return;
        }

        try {
            await this.pc.addIceCandidate({
                candidate: message.candidate,
                sdpMid: message.sdpMid,
                sdpMLineIndex: message.sdpMLineIndex
            });
            this.log('ICE candidate added', 'info');
        } catch (error) {
            this.log('Error adding ICE candidate: ' + error.message, 'error');
        }
    }

    handleSync(message) {
        if (!this.syncEnabled || this.localTimeUpdate) return;

        const { currentTime, isPlaying } = message;


        if (Math.abs(this.videoPlayer.currentTime - currentTime) > 1.0) {
            this.log(`Syncing time: ${currentTime.toFixed(2)}s`, 'info');
            this.videoPlayer.currentTime = currentTime;
        }

        if (isPlaying && this.videoPlayer.paused) {
            this.videoPlayer.play().catch(e => this.log('Play error: ' + e, 'error'));
        } else if (!isPlaying && !this.videoPlayer.paused) {
            this.videoPlayer.pause();
        }
    }

    applySyncData(message) {
        this.handleSync(message);
    }

    sendSignaling(message) {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify(message));
        }
    }

    sendSyncUpdate() {
        if (this.dataChannel && this.dataChannel.readyState === 'open') {
            const syncData = {
                type: 'sync',
                currentTime: this.videoPlayer.currentTime,
                isPlaying: !this.videoPlayer.paused,
                timestamp: Date.now()
            };
            this.dataChannel.send(JSON.stringify(syncData));
        }
    }

    async handleFileSelect(event) {
        const file = event.target.files[0];
        if (file) {
            await this.uploadFile(file);
        }
    }

    async uploadFile(file) {
        if (!file.type.startsWith('video/')) {
            this.log('Please select a video file', 'error');
            return;
        }

        this.log(`Uploading: ${file.name} (${this.formatFileSize(file.size)})`, 'info');
        this.uploadProgress.classList.add('active');

        const formData = new FormData();
        formData.append('video', file);

        try {
            const xhr = new XMLHttpRequest();

            xhr.upload.onprogress = (event) => {
                if (event.lengthComputable) {
                    const percent = Math.round((event.loaded / event.total) * 100);
                    this.uploadProgressFill.style.width = percent + '%';
                    this.uploadProgressFill.textContent = percent + '%';
                }
            };

            xhr.onload = () => {
                if (xhr.status === 200) {
                    try {
                        const response = JSON.parse(xhr.responseText);
                        this.log(`Upload complete: ${response.file_path} (${this.formatFileSize(response.size)})`, 'success');
                        this.uploadedFiles.push({
                            name: file.name,
                            size: response.size || file.size,
                            path: response.file_path
                        });
                        this.updateFileList();
                        this.uploadProgress.classList.remove('active');
                        this.uploadProgressFill.style.width = '0%';
                        this.uploadProgressFill.textContent = '0%';
                    } catch (e) {
                        this.log('Upload response parse error: ' + e.message, 'error');
                        this.uploadProgress.classList.remove('active');
                    }
                } else {
                    this.log(`Upload failed: ${xhr.status} ${xhr.statusText}`, 'error');
                    this.uploadProgress.classList.remove('active');
                }
            };

            xhr.onerror = (e) => {
                this.log('Upload network error - check if server is running on port 8081', 'error');
                this.uploadProgress.classList.remove('active');
                console.error('XHR Error:', e);
            };

            xhr.ontimeout = () => {
                this.log('Upload timeout', 'error');
                this.uploadProgress.classList.remove('active');
            };

            xhr.open('POST', 'http://localhost:8081/upload');
            xhr.timeout = 120000; 
            xhr.send(formData);

        } catch (error) {
            this.log('Upload error: ' + error.message, 'error');
            this.uploadProgress.classList.remove('active');
            console.error('Upload exception:', error);
        }
    }

    updateFileList() {
        if (this.uploadedFiles.length === 0) {
            this.fileList.innerHTML = '<p style="text-align: center; color: #999; padding: 20px;">No videos uploaded yet</p>';
            return;
        }

        this.fileList.innerHTML = this.uploadedFiles.map((file, index) => `
            <div class="file-item" data-index="${index}" data-path="${file.path}">
                <div class="file-name">${file.name}</div>
                <div class="file-size">${this.formatFileSize(file.size)}</div>
            </div>
        `).join('');

 
        this.fileList.querySelectorAll('.file-item').forEach(item => {
            item.addEventListener('click', () => {
                const path = item.getAttribute('data-path');
                this.selectVideo(path);

 
                this.fileList.querySelectorAll('.file-item').forEach(i => i.classList.remove('active'));
                item.classList.add('active');
            });
        });
    }

    async selectVideo(filePath) {
        this.log(`Starting stream: ${filePath}`, 'info');
        this.currentFilePath = filePath;

        if (!this.pc) {
            this.createPeerConnection();
        }

        this.pc.addTransceiver('video', { direction: 'recvonly' });

    
        const offer = await this.pc.createOffer();
        await this.pc.setLocalDescription(offer);

        this.sendSignaling({
            type: 'start_stream',
            file_path: filePath
        });

        this.sendSignaling({
            type: 'offer',
            sdp: offer.sdp
        });

        this.log('Offer sent, waiting for stream...', 'info');
    }

    play() {
        this.localTimeUpdate = true;
        this.videoPlayer.play()
            .then(() => {
                this.log('Playing', 'info');
                this.sendSyncUpdate();
                setTimeout(() => this.localTimeUpdate = false, 100);
            })
            .catch(e => this.log('Play error: ' + e, 'error'));
    }

    pause() {
        this.localTimeUpdate = true;
        this.videoPlayer.pause();
        this.log('Paused', 'info');
        this.sendSyncUpdate();
        setTimeout(() => this.localTimeUpdate = false, 100);
    }

    stop() {
        this.videoPlayer.pause();
        this.videoPlayer.currentTime = 0;
        this.log('Stopped', 'info');

    
        this.sendSignaling({
            type: 'stop_stream'
        });

     
        if (this.pc) {
            this.pc.close();
            this.pc = null;
        }

        this.videoPlayer.srcObject = null;
        this.videoPlayer.style.display = 'none';
        this.videoPlaceholder.style.display = 'block';
        this.disableControls();
        this.syncIndicatorEl.style.display = 'none';
    }

    seek(event) {
        const rect = this.progressBar.getBoundingClientRect();
        const percent = (event.clientX - rect.left) / rect.width;
        const newTime = percent * this.videoPlayer.duration;

        this.localTimeUpdate = true;
        this.videoPlayer.currentTime = newTime;
        this.log(`Seek to: ${this.formatTime(newTime)}`, 'info');
        this.sendSyncUpdate();
        setTimeout(() => this.localTimeUpdate = false, 100);
    }

    onVideoTimeUpdate() {
        if (!this.videoPlayer.duration) return;

        const percent = (this.videoPlayer.currentTime / this.videoPlayer.duration) * 100;
        this.progressFill.style.width = percent + '%';

        const current = this.formatTime(this.videoPlayer.currentTime);
        const duration = this.formatTime(this.videoPlayer.duration);
        this.timeDisplay.textContent = `${current} / ${duration}`;
    }

    onVideoPlay() {
        if (!this.localTimeUpdate) {
            this.sendSyncUpdate();
        }
    }

    onVideoPause() {
        if (!this.localTimeUpdate) {
            this.sendSyncUpdate();
        }
    }

    onVideoLoaded() {
        this.log(`Video loaded: ${this.videoPlayer.videoWidth}x${this.videoPlayer.videoHeight}`, 'success');
    }

    enableControls() {
        this.playBtn.disabled = false;
        this.pauseBtn.disabled = false;
        this.stopBtn.disabled = false;
    }

    disableControls() {
        this.playBtn.disabled = true;
        this.pauseBtn.disabled = true;
        this.stopBtn.disabled = true;
    }

    updateStatus(status) {
        this.statusEl.className = 'status ' + status;
        this.statusEl.textContent = status.charAt(0).toUpperCase() + status.slice(1);
    }

    log(message, type = 'info') {
        const timestamp = new Date().toLocaleTimeString();
        const entry = document.createElement('div');
        entry.className = `log-entry ${type}`;
        entry.textContent = `[${timestamp}] ${message}`;

        this.logContainer.appendChild(entry);
        this.logContainer.scrollTop = this.logContainer.scrollHeight;


        while (this.logContainer.children.length > 100) {
            this.logContainer.removeChild(this.logContainer.firstChild);
        }
    }

    formatTime(seconds) {
        const mins = Math.floor(seconds / 60);
        const secs = Math.floor(seconds % 60);
        return `${mins.toString().padStart(2, '0')}:${secs.toString().padStart(2, '0')}`;
    }

    formatFileSize(bytes) {
        if (bytes < 1024) return bytes + ' B';
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
        if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
        return (bytes / (1024 * 1024 * 1024)).toFixed(1) + ' GB';
    }
}


window.addEventListener('DOMContentLoaded', () => {
    new WebRTCClient();
});