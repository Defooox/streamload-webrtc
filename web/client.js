
const WS_PORT = 8080;
const HTTP_PORT = 8081;
const WS_URL = `ws://localhost:${WS_PORT}`;
const HTTP_URL = `http://localhost:${HTTP_PORT}`;


const fileInput = document.getElementById('fileInput');
const uploadBtn = document.getElementById('uploadBtn');
const uploadStatus = document.getElementById('uploadStatus');
const startStreamBtn = document.getElementById('startStreamBtn');
const stopStreamBtn = document.getElementById('stopStreamBtn');
const streamStatus = document.getElementById('streamStatus');
const syncStatus = document.getElementById('syncStatus');
const remoteVideo = document.getElementById('remoteVideo');


const ws = new WebSocket(WS_URL);
let pc = null;
let dataChannel = null;


ws.onopen = () => {
    console.log('WebSocket подключен');
    setStatus('uploadStatus', 'success', '✓ Подключение установлено');
};

ws.onerror = (error) => {
    console.error('WebSocket ошибка:', error);
    setStatus('uploadStatus', 'error', '❌ Ошибка WebSocket соединения');
};

ws.onclose = () => {
    console.log(' WebSocket закрыт');
    setStatus('uploadStatus', 'info', '🔌 Соединение закрыто');
};

uploadBtn.onclick = async () => {
    const file = fileInput.files[0];
    if (!file) {
        setStatus('uploadStatus', 'error', 'Выберите файл!');
        return;
    }

    setStatus('uploadStatus', 'info', ' Загрузка файла...');
    uploadBtn.disabled = true;

    const formData = new FormData();
    formData.append('file', file);

    try {
        const response = await fetch(`${HTTP_URL}/upload`, {
            method: 'POST',
            body: formData
        });
        
        const result = await response.json();
        
        if (result.status === 'ok') {
            window.uploadedFilePath = result.file_path;
            setStatus('uploadStatus', 'success', 
                ` Файл загружен: <code>${file.name}</code>`);
            

            document.getElementById('streamSection').style.display = 'block';
            streamStatus.textContent = `Файл: ${file.name} (${(file.size/1024/1024).toFixed(2)} MB)`;
        } else {
            throw new Error(result.error || 'Unknown error');
        }
    } catch (e) {
        console.error('Upload error:', e);
        setStatus('uploadStatus', 'error', `Ошибка: ${e.message}`);
    } finally {
        uploadBtn.disabled = false;
    }
};


startStreamBtn.onclick = async () => {
    if (!window.uploadedFilePath) {
        setStatus('streamStatus', 'error', ' Файл не загружен');
        return;
    }

    setStatus('syncStatus', 'info', 'Инициализация WebRTC...');
    startStreamBtn.disabled = true;


    pc = new RTCPeerConnection({
        iceServers: [{ urls: 'stun:stun.l.google.com:19302' }]
    });

  
    pc.ontrack = (event) => {
        remoteVideo.srcObject = event.streams[0];
        setStatus('syncStatus', 'success', '▶️ Получаем видео...');
        document.getElementById('playerSection').style.display = 'block';
    };


    dataChannel = pc.createDataChannel('sync', { ordered: true });
    
    dataChannel.onopen = () => {
        setStatus('syncStatus', 'success', ' Синхронизация активна');
        console.log('DataChannel opened');
    };

    dataChannel.onmessage = (e) => {
        const sync = JSON.parse(e.data);
        if (sync.type === 'sync') {
            syncVideo(sync);
        }
    };

 
    ws.send(JSON.stringify({
        type: 'start_stream',
        file_path: window.uploadedFilePath
    }));

    
    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    ws.send(JSON.stringify({ type: 'offer', sdp: offer.sdp }));


    pc.onicecandidate = (e) => {
        if (e.candidate) {
            ws.send(JSON.stringify({
                type: 'ice_candidate',
                candidate: e.candidate.candidate,
                sdpMid: e.candidate.sdpMid,
                sdpMLineIndex: e.candidate.sdpMLineIndex
            }));
        }
    };


    pc.onconnectionstatechange = () => {
        console.log('Connection state:', pc.connectionState);
        if (pc.connectionState === 'connected') {
            setStatus('syncStatus', 'success', ' Соединение установлено');
        }
    };

    stopStreamBtn.disabled = false;
};


stopStreamBtn.onclick = () => {
    if (pc) {
        pc.close();
        pc = null;
    }
    
    ws.send(JSON.stringify({ type: 'stop_stream' }));
    
    startStreamBtn.disabled = false;
    stopStreamBtn.disabled = true;
    
    remoteVideo.srcObject = null;
    document.getElementById('playerSection').style.display = 'none';
    setStatus('syncStatus', 'info', 'Стрим остановлен');
};

ws.onmessage = async (e) => {
    const msg = JSON.parse(e.data);
    
    if (msg.type === 'answer') {
        await pc.setRemoteDescription(new RTCSessionDescription(msg));
    } else if (msg.type === 'ice_candidate') {
        await pc.addIceCandidate(new RTCIceCandidate({
            candidate: msg.candidate,
            sdpMid: msg.sdpMid,
            sdpMLineIndex: msg.sdpMLineIndex
        }));
    }
};


function syncVideo(sync) {
    const timeDiff = Math.abs(remoteVideo.currentTime - sync.currentTime);
    
    if (timeDiff > 0.5) {
        remoteVideo.currentTime = sync.currentTime;
        console.log('🔄 Sync time:', sync.currentTime.toFixed(2));
    }

    if (sync.isPlaying && remoteVideo.paused) {
        remoteVideo.play();
    } else if (!sync.isPlaying && !remoteVideo.paused) {
        remoteVideo.pause();
    }
}
function setStatus(elementId, type, message) {
    const el = document.getElementById(elementId);
    el.className = type;
    el.innerHTML = message;
}

setInterval(() => {
    if (dataChannel && dataChannel.readyState === 'open') {

    }
}, 100);