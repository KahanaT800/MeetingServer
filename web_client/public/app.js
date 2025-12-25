let currentToken = null;
let currentUsername = null;

function log(message, type = 'info') {
    const consoleDiv = document.getElementById('console-log');
    const time = new Date().toLocaleTimeString();
    let color = '#00ff00'; // info
    if (type === 'error') color = '#ff4444';
    if (type === 'warn') color = '#ffbb33';
    
    consoleDiv.innerHTML += `<div style="color:${color}">[${time}] ${message}</div>`;
    consoleDiv.scrollTop = consoleDiv.scrollHeight;
}

function clearLog() {
    document.getElementById('console-log').innerHTML = '';
}

function updateAuthStatus(token, username) {
    currentToken = token;
    currentUsername = username;
    
    const controls = document.getElementById('meeting-controls');
    const alert = document.getElementById('token-alert');
    
    if (token) {
        controls.style.opacity = '1';
        controls.style.pointerEvents = 'auto';
        alert.className = 'alert alert-success';
        alert.innerHTML = `✅ 已登录: <strong>${username}</strong>`;
        log(`Token saved: ${token.substring(0, 10)}...`);
    } else {
        controls.style.opacity = '0.5';
        controls.style.pointerEvents = 'none';
        alert.className = 'alert alert-warning';
        alert.innerHTML = '请先登录以获取 Token';
    }
}

async function apiCall(endpoint, data) {
    try {
        const res = await fetch(endpoint, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data)
        });
        const json = await res.json();
        
        if (json.error && json.error.code !== 0) {
            log(`❌ Error: ${json.error.message}`, 'error');
            return null;
        }
        return json;
    } catch (e) {
        log(`❌ Network Error: ${e.message}`, 'error');
        return null;
    }
}

// --- Actions ---

async function doRegister() {
    const username = document.getElementById('reg-username').value;
    const password = document.getElementById('reg-password').value;
    const email = document.getElementById('reg-email').value;
    
    if (!username || !password || !email) {
        log('⚠️ 请填写完整注册信息', 'warn');
        return;
    }

    log(`Registering user: ${username}...`);
    const res = await apiCall('/api/register', {
        user_name: username,
        password: password,
        email: email,
        display_name: username
    });

    if (res) {
        log(`✅ 注册成功! User ID: ${res.user.id}`);
        // 自动填充登录框
        document.getElementById('login-username').value = username;
        // 切换到登录标签
        const loginTab = new bootstrap.Tab(document.querySelector('#authTab button[data-bs-target="#login"]'));
        loginTab.show();
    }
}

async function doLogin() {
    const username = document.getElementById('login-username').value;
    const password = document.getElementById('login-password').value;

    log(`Logging in: ${username}...`);
    const res = await apiCall('/api/login', {
        user_name: username,
        password: password
    });

    if (res) {
        log(`✅ 登录成功!`);
        updateAuthStatus(res.session_token, res.user.username);
    }
}

async function createMeeting() {
    const topic = document.getElementById('meeting-topic').value;
    
    log(`Creating meeting: ${topic}...`);
    const res = await apiCall('/api/meeting/create', {
        session_token: currentToken,
        topic: topic
    });

    if (res) {
        const meetingId = res.meeting.meeting_id;
        log(`✅ 会议创建成功! ID: ${meetingId}`);
        document.getElementById('meeting-id').value = meetingId;
    }
}

async function getMeeting() {
    const meetingId = document.getElementById('meeting-id').value;
    if (!meetingId) return log('⚠️ 请输入 Meeting ID', 'warn');

    log(`Querying meeting: ${meetingId}...`);
    const res = await apiCall('/api/meeting/get', {
        session_token: currentToken,
        meeting_id: meetingId
    });

    if (res) {
        log(`📄 会议信息: ${res.meeting.topic} (Organizer: ${res.meeting.organizer_id})`);
    }
}

async function joinMeeting() {
    const meetingId = document.getElementById('meeting-id').value;
    if (!meetingId) return log('⚠️ 请输入 Meeting ID', 'warn');

    log(`Joining meeting: ${meetingId}...`);
    const res = await apiCall('/api/meeting/join', {
        session_token: currentToken,
        meeting_id: meetingId,
        client_info: "Web Client"
    });

    if (res) {
        log(`🚀 加入成功! Server Endpoint: ${res.endpoint.ip}:${res.endpoint.port}`);
    }
}
