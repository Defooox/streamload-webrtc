const video = document.getElementById("video");
const logEl = document.getElementById("log");

const wsDot = document.getElementById("wsDot");
const rtcDot = document.getElementById("rtcDot");
const wsStatus = document.getElementById("wsStatus");
const rtcStatus = document.getElementById("rtcStatus");

const connectBtn = document.getElementById("connectBtn");
const disconnectBtn = document.getElementById("disconnectBtn");
const startBtn = document.getElementById("startBtn");
const stopBtn = document.getElementById("stopBtn");

const fileInput = document.getElementById("fileInput");
const uploadBtn = document.getElementById("uploadBtn");
const fileSelect = document.getElementById("fileSelect");

const progressBar = document.getElementById("progressBar");
const uploadInfo = document.getElementById("uploadInfo");

let ws = null;
let pc = null;
let dataChannel = null;
let remoteStream = null;


let pendingRemoteCandidates = [];

function log(msg) {
    const line = `[${new Date().toLocaleTimeString()}] ${msg}`;
    console.log(line);
    if (logEl) {
        logEl.textContent += line + "\n";
        logEl.scrollTop = logEl.scrollHeight;
    }
}

function setWsState(state) {
    if (wsStatus) wsStatus.textContent = state;
    if (wsDot) wsDot.className = "dot " + (state === "connected" ? "ok" : (state === "error" ? "bad" : ""));
}

function setRtcState(state) {
    if (rtcStatus) rtcStatus.textContent = state;
    if (rtcDot) rtcDot.className = "dot " + (state === "connected" ? "ok" : (state === "error" ? "bad" : ""));
}

function wsUrl() {
    const proto = location.protocol === "https:" ? "wss" : "ws";
    return `${proto}://${location.hostname}:8080`;
}


function connectWS() {
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
        log("WS уже подключается/подключён.");
        return;
    }

    setWsState("connecting");
    ws = new WebSocket(wsUrl());

    ws.onopen = () => {
        setWsState("connected");
        log("WS connected: " + wsUrl());
    };

    ws.onmessage = async (ev) => {
        let msg;
        try { msg = JSON.parse(ev.data); }
        catch { log("WS non-JSON: " + ev.data); return; }

        try {
            await handleSignal(msg);
        } catch (e) {
            log("handleSignal error: " + (e?.message || e));
            console.error(e);
        }
    };

    ws.onclose = () => {
        setWsState("disconnected");
        log("WS closed");
    };

    ws.onerror = (e) => {
        setWsState("error");
        log("WS error");
        console.error(e);
    };
}

function disconnectWS() {
    try { if (ws) ws.close(); } catch { }
    ws = null;
    setWsState("disconnected");
}


function normalizeCandidateString(s) {
    if (!s) return s;
  
    if (s.startsWith("a=candidate:")) return s.slice(2); 
    if (s.startsWith("a=end-of-candidates")) return "";
    return s;
}

async function flushPendingRemoteCandidates() {
    if (!pc || !pc.remoteDescription) return;
    if (!pendingRemoteCandidates.length) return;

    log("Flushing remote ICE: " + pendingRemoteCandidates.length);
    const list = pendingRemoteCandidates;
    pendingRemoteCandidates = [];

    for (const ice of list) {
        try {
            await pc.addIceCandidate(ice);
            log("ICE <= added (flushed)");
        } catch (e) {
            log("ICE add error (flushed): " + (e?.message || e));
        }
    }
}

function createPeerConnection() {
    if (pc) {
        try { pc.close(); } catch { }
        pc = null;
    }

    setRtcState("connecting");
    pendingRemoteCandidates = [];

    remoteStream = new MediaStream();
    video.srcObject = null;
    video.muted = true;    
    video.autoplay = true;
    video.playsInline = true;

    pc = new RTCPeerConnection({
        iceServers: [
            { urls: ["stun:stun.l.google.com:19302", "stun:stun1.l.google.com:19302"] },
        ]
    });

    pc.ontrack = (ev) => {
        log(`ontrack: kind=${ev.track.kind}, streams=${ev.streams ? ev.streams.length : 0}`);

        if (ev.streams && ev.streams[0]) {
            video.srcObject = ev.streams[0];
        } else {
            remoteStream.addTrack(ev.track);
            video.srcObject = remoteStream;
        }
        video.muted = true;
        const p = video.play();
        if (p && typeof p.catch === "function") {
            p.catch(() => log("Видео не стартануло автоматически (autoplay). Кликни по странице/нажми Play."));
        }
    };

    pc.onicecandidate = (ev) => {
        if (!ev.candidate) {
            log("ICE gathering complete (browser)");
            return;
        }
        if (!ws || ws.readyState !== WebSocket.OPEN) return;

        const payload = {
            type: "ice_candidate",
            candidate: ev.candidate.candidate,
            sdpMid: ev.candidate.sdpMid ?? "0",
            sdpMLineIndex: ev.candidate.sdpMLineIndex ?? 0
        };

        log("ICE -> server: " + payload.candidate.slice(0, 60) + "...");
        ws.send(JSON.stringify(payload));
    };

    pc.onicegatheringstatechange = () => {
        log("ICE gathering state: " + pc.iceGatheringState);
    };

    pc.oniceconnectionstatechange = () => {
        log("ICE connection state: " + pc.iceConnectionState);
    };

    pc.onconnectionstatechange = () => {
        log("PC connection state: " + pc.connectionState);
        if (pc.connectionState === "connected") setRtcState("connected");
        if (pc.connectionState === "failed" || pc.connectionState === "disconnected") setRtcState("error");
        if (pc.connectionState === "closed") setRtcState("idle");
    };

    pc.ondatachannel = (ev) => {
        dataChannel = ev.channel;
        log("DataChannel received: " + dataChannel.label);
        dataChannel.onopen = () => log("DC open: " + dataChannel.label);
        dataChannel.onclose = () => log("DC closed: " + dataChannel.label);
        dataChannel.onmessage = (e) => log("DC msg: " + e.data);
    };
}


async function handleSignal(msg) {
    switch (msg.type) {
        case "offer": {
            log("SIG offer (sdp len=" + (msg.sdp ? msg.sdp.length : 0) + ")");
            if (!pc) createPeerConnection();

            await pc.setRemoteDescription({ type: "offer", sdp: msg.sdp });
            await flushPendingRemoteCandidates();

            const answer = await pc.createAnswer();
            await pc.setLocalDescription(answer);

         
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({ type: "answer", sdp: pc.localDescription.sdp }));
            }

            
            await flushPendingRemoteCandidates();
            break;
        }

        case "ice_candidate": {
            const candStr = normalizeCandidateString(msg.candidate);
            if (!candStr) return;

            const ice = new RTCIceCandidate({
                candidate: candStr,
                sdpMid: msg.sdpMid || "0",
                sdpMLineIndex: msg.sdpMLineIndex ?? 0
            });

         
            if (!pc || !pc.remoteDescription) {
                pendingRemoteCandidates.push(ice);
                log("ICE <= queued (pc not ready)");
                return;
            }

            try {
                await pc.addIceCandidate(ice);
                log("ICE <= added");
            } catch (e) {
                log("ICE add error: " + (e?.message || e));
            }
            break;
        }

        case "answer": {
           
            log("SIG answer");
            if (!pc) return;
            await pc.setRemoteDescription({ type: "answer", sdp: msg.sdp });
            await flushPendingRemoteCandidates();
            break;
        }

        default:
            log("SIG unknown type: " + msg.type);
    }
}

function startStream() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        log("Нужно сначала Connect (WS).");
        return;
    }
    if (!pc) createPeerConnection();

    const filePath = fileSelect.value;
    if (!filePath) {
        log("Выбери файл из списка (после upload).");
        return;
    }

    ws.send(JSON.stringify({ type: "start_stream", file_path: filePath }));
    log("CMD start_stream file_path=" + filePath);
}

function stopStream() {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ type: "stop_stream" }));
        log("CMD stop_stream");
    }

    if (pc) { try { pc.close(); } catch { } pc = null; }
    setRtcState("idle");
    video.srcObject = null;
    remoteStream = null;
    pendingRemoteCandidates = [];
}

function addFileOption(filePath) {
    const label = (filePath || "").split(/[\\/]/).pop();

    for (const opt of fileSelect.options) {
        if (opt.value === filePath) {
            fileSelect.value = filePath;
            return;
        }
    }

    const opt = document.createElement("option");
    opt.value = filePath;
    opt.textContent = label;
    fileSelect.appendChild(opt);
    fileSelect.value = filePath;
}

function uploadFile() {
    const file = fileInput.files && fileInput.files[0] ? fileInput.files[0] : null;
    if (!file) { uploadInfo.textContent = "Файл не выбран."; return; }

    progressBar.style.width = "0%";
    uploadInfo.textContent = `Загрузка: ${file.name} (${Math.round(file.size / 1024 / 1024)} MB)`;

    const xhr = new XMLHttpRequest();
    xhr.open("PUT", "/upload_raw");
    xhr.setRequestHeader("X-Filename", file.name);
    xhr.setRequestHeader("Content-Type", "application/octet-stream");

    xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) {
            const pct = Math.round((e.loaded / e.total) * 100);
            progressBar.style.width = pct + "%";
        }
    };

    xhr.onload = () => {
        if (xhr.status >= 200 && xhr.status < 300) {
            progressBar.style.width = "100%";
            try {
                const data = JSON.parse(xhr.responseText);
                const filePath = data.file_path;
                uploadInfo.textContent = "OK. Сохранено: " + filePath;
                if (filePath) addFileOption(filePath);
            } catch {
                uploadInfo.textContent = "Upload OK, но ответ не JSON.";
            }
        } else {
            uploadInfo.textContent = `Ошибка upload: HTTP ${xhr.status}`;
        }
    };

    xhr.onerror = () => { uploadInfo.textContent = "Ошибка сети при upload."; };

    xhr.send(file); 
}


connectBtn.onclick = () => connectWS();
disconnectBtn.onclick = () => { disconnectWS(); stopStream(); };

startBtn.onclick = () => startStream();
stopBtn.onclick = () => stopStream();

uploadBtn.onclick = () => uploadFile();

fileInput.onchange = () => {
    const f = fileInput.files && fileInput.files[0] ? fileInput.files[0] : null;
    uploadInfo.textContent = f ? `Готов к загрузке: ${f.name}` : "Файл не выбран.";
};

setWsState("disconnected");
setRtcState("idle");
