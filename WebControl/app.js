(function () {
  'use strict';

  // --- CẤU HÌNH ---
  const BASE_TOPIC = 'uit_eezybot_7f3a9c';    // Phải khớp với các TOPIC_... trên ESP32
  const JOINTS = [1, 2, 3, 4];                // 1: Đế, 2: Vai, 3: Khuỷu, 4: Kẹp
  const HOME_ANGLES = [90, 90, 90, 90];
  const GRIPPER_RELEASE_ANGLE = 90;           // Phải khớp với góc mở kẹp trên ESP32
  const GRIPPER_GRAB_ANGLE = 180;             // Phải khớp với góc đóng kẹp trên ESP32
  const PUBLISH_INTERVAL_MS = 50;             // Gửi góc tối đa 20 lần/giây khi kéo thanh trượt
  const LOCAL_CONTROL_HOLD_MS = 350;          // Bỏ qua góc từ ESP32 ngay sau khi người dùng thao tác
  const MAX_LOG_ENTRIES = 200;

  // --- TRẠNG THÁI ---
  const state = {
    client: null,
    connection: 'disconnected',               // 'disconnected' | 'connecting' | 'connected'
    robotOnline: false,
    mode: 'MANUAL',                           // 'MANUAL' | 'TEACHING' | 'PLAYBACK'
    points: 0,
    source: 'WEB',                            // Bên giữ quyền điều khiển: 'WEB' | 'GAMEPAD'
    angles: { 1: 90, 2: 90, 3: 90, 4: 90 },
    lastLocalControlTime: 0,
    publishTimer: null,
    logCount: 0
  };

  // --- PHẦN TỬ GIAO DIỆN ---
  const $ = (id) => document.getElementById(id);
  const el = {
    btnConnect: $('btnConnect'),
    brokerUrl: $('brokerUrl'),
    statusDot: $('statusDot'),
    statusText: $('statusText'),
    robotDot: $('robotDot'),
    robotText: $('robotText'),
    btnRelease: $('btnRelease'),
    btnGrab: $('btnGrab'),
    btnRecord: $('btnRecord'),
    btnRun: $('btnRun'),
    btnStop: $('btnStop'),
    btnAddPoint: $('btnAddPoint'),
    btnClearPoints: $('btnClearPoints'),
    teachPointBadge: $('teachPointBadge'),
    btnHome: $('btnHome'),
    logHeader: $('logHeaderToggle'),
    logChevron: $('logChevron'),
    logBody: $('logBody'),
    logConsole: $('logConsole'),
    logCounter: $('logCounter'),
    btnClearLog: $('btnClearLog')
  };

  // --- TIỆN ÍCH ---
  function clampAngle(value) {
    return Math.max(0, Math.min(180, Math.round(value)));
  }

  function generateClientId() {
    return 'robot_arm_web_' + Math.random().toString(16).substring(2, 10);
  }

  function addLog(text, type) {
    state.logCount++;
    el.logCounter.textContent = `(${state.logCount} messages)`;

    const now = new Date();
    const time = now.toTimeString().split(' ')[0] + '.' + String(now.getMilliseconds()).padStart(3, '0');
    const entry = document.createElement('div');
    entry.className = `log-entry ${type || 'system'}`;
    entry.textContent = `[${time}] ${text}`;
    el.logConsole.appendChild(entry);

    if (el.logConsole.children.length > MAX_LOG_ENTRIES) {
      el.logConsole.removeChild(el.logConsole.firstChild);
    }
    el.logBody.scrollTop = el.logBody.scrollHeight;
  }

  // Trạng thái giữ lại trên broker có thể đã cũ, nên chỉ tin khi ESP32 đang online
  function isGamepadActive() {
    return state.robotOnline && state.source === 'GAMEPAD';
  }

  // --- CẬP NHẬT GIAO DIỆN ---
  function updateConnectionUI() {
    const labels = { disconnected: 'Disconnected', connecting: 'Connecting...', connected: 'Connected' };
    el.statusDot.className = `status-dot ${state.connection}`;
    el.statusText.textContent = labels[state.connection];
    el.btnConnect.textContent = state.client ? (state.connection === 'connected' ? 'Disconnect' : 'Cancel') : 'Connect';
    el.btnConnect.classList.toggle('connected', state.connection === 'connected');
    el.brokerUrl.disabled = Boolean(state.client);
    updateRobotUI();
  }

  function updateRobotUI() {
    const connected = state.connection === 'connected';
    const playback = state.mode === 'PLAYBACK';
    const teaching = state.mode === 'TEACHING';
    const gamepadActive = isGamepadActive();
    const canControl = connected && !playback && !gamepadActive;

    el.robotDot.className = `status-dot ${state.robotOnline ? 'connected' : 'disconnected'}`;
    el.robotText.textContent = `ESP32: ${state.robotOnline ? 'Online' : 'Offline'}${gamepadActive ? ' · Tay cầm đang điều khiển' : ''}`;

    JOINTS.forEach((n) => {
      $(`slider${n}`).disabled = !canControl;
      $(`input${n}`).disabled = !canControl;
    });
    el.btnHome.disabled = el.btnRelease.disabled = el.btnGrab.disabled = !canControl;

    el.btnRecord.disabled = !canControl;
    el.btnRecord.textContent = teaching ? 'Record On' : 'Record Off';
    el.btnRecord.classList.toggle('recording', teaching);
    el.btnAddPoint.disabled = !(canControl && teaching);
    el.btnClearPoints.disabled = !canControl;
    el.btnRun.disabled = !(canControl && !teaching);
    el.btnRun.textContent = playback ? 'Running...' : 'Run';
    el.btnStop.disabled = !connected;           // Lệnh Dừng luôn dùng được khi còn kết nối broker
    el.teachPointBadge.textContent = `${state.points} điểm lưu`;
  }

  function updateGripperUI() {
    el.btnRelease.classList.toggle('active', state.angles[4] === GRIPPER_RELEASE_ANGLE);
    el.btnGrab.classList.toggle('active', state.angles[4] === GRIPPER_GRAB_ANGLE);
  }

  function setAngle(joint, value) {
    const angle = clampAngle(value);
    state.angles[joint] = angle;
    $(`slider${joint}`).value = angle;
    // Không đổi ô số khi người dùng đang gõ vào đó
    const input = $(`input${joint}`);
    if (document.activeElement !== input) input.value = angle;
    if (joint === 4) updateGripperUI();
  }

  // --- GỬI LỆNH MQTT ---
  function publish(subTopic, payload, qos) {
    if (!state.client || state.connection !== 'connected') {
      addLog('Chưa kết nối broker, lệnh không được gửi.', 'error');
      return;
    }
    const topic = `${BASE_TOPIC}/${subTopic}`;
    const message = JSON.stringify(payload);
    state.client.publish(topic, message, { qos: qos, retain: false }, (err) => {
      if (err) addLog(`Lỗi gửi tới ${topic}: ${err.message}`, 'error');
    });
    addLog(`[PUB] ${topic} -> ${message}`, 'pub');
  }

  function sendAngles(qos) {
    publish('control/angles', {
      joint1: state.angles[1],
      joint2: state.angles[2],
      joint3: state.angles[3],
      joint4: state.angles[4]
    }, qos);
  }

  // Khi đang kéo: gom thay đổi và gửi góc mới nhất theo chu kỳ, cho phép mất gói (QoS 0)
  function scheduleSendAngles() {
    state.lastLocalControlTime = performance.now();
    if (state.publishTimer) return;
    state.publishTimer = setTimeout(() => {
      state.publishTimer = null;
      sendAngles(0);
    }, PUBLISH_INTERVAL_MS);
  }

  // Khi thao tác xong: gửi ngay góc cuối cùng và đảm bảo gói đến nơi (QoS 1)
  function sendAnglesNow() {
    state.lastLocalControlTime = performance.now();
    if (state.publishTimer) {
      clearTimeout(state.publishTimer);
      state.publishTimer = null;
    }
    sendAngles(1);
  }

  function sendTeachingCommand(action) {
    publish('control/teaching', { action: action }, 1);
  }

  // --- NHẬN PHẢN HỒI TỪ ESP32 ---
  function handleStatus(message) {
    let data;
    try {
      data = JSON.parse(message);
    } catch (e) {
      addLog('Bỏ qua trạng thái không phải JSON.', 'error');
      return;
    }

    if (['MANUAL', 'TEACHING', 'PLAYBACK'].includes(data.mode)) state.mode = data.mode;
    if (Number.isInteger(data.points) && data.points >= 0) state.points = data.points;
    if (['WEB', 'GAMEPAD'].includes(data.source)) state.source = data.source;

    // Lúc người dùng vừa thao tác thì giữ góc trên giao diện để thanh trượt không bị giật ngược,
    // trừ khi web đang không có quyền điều khiển
    const recentlyTouched = performance.now() - state.lastLocalControlTime < LOCAL_CONTROL_HOLD_MS;
    if (state.mode === 'PLAYBACK' || isGamepadActive() || !recentlyTouched) {
      JOINTS.forEach((n) => {
        const value = data[`joint${n}`];
        if (typeof value === 'number' && Number.isFinite(value)) setAngle(n, value);
      });
    }
    updateRobotUI();
  }

  function handleMessage(topic, payload) {
    const message = payload.toString();
    addLog(`[SUB] ${topic} -> ${message}`, 'sub');

    if (topic === `${BASE_TOPIC}/status`) {
      handleStatus(message);
    } else if (topic === `${BASE_TOPIC}/online`) {
      state.robotOnline = message === 'online';
      updateRobotUI();
    }
  }

  // --- KẾT NỐI BROKER ---
  function connect() {
    const brokerUrl = el.brokerUrl.value.trim();
    if (!brokerUrl) {
      addLog('Vui lòng nhập WebSocket URL của broker.', 'error');
      return;
    }

    const clientId = generateClientId();

    let client;
    try {
      client = mqtt.connect(brokerUrl, {
        clientId: clientId,
        connectTimeout: 7000,
        reconnectPeriod: 3000
      });
    } catch (err) {
      addLog(`Không tạo được kết nối MQTT: ${err.message}`, 'error');
      return;
    }

    state.client = client;
    state.connection = 'connecting';
    addLog(`Đang kết nối tới ${brokerUrl} (Client ID: ${clientId})...`);
    updateConnectionUI();

    // Không gửi góc khi vừa kết nối, để tay máy không tự giật đến góc đang hiện trên web
    client.on('connect', () => {
      if (client !== state.client) return;
      state.connection = 'connected';
      addLog(`Đã kết nối tới ${brokerUrl}`, 'info');
      client.subscribe([`${BASE_TOPIC}/status`, `${BASE_TOPIC}/online`], { qos: 1 }, (err) => {
        if (err) addLog(`Lỗi đăng ký topic: ${err.message}`, 'error');
      });
      updateConnectionUI();
    });

    client.on('message', (topic, payload) => {
      if (client === state.client) handleMessage(topic, payload);
    });

    client.on('error', (err) => {
      if (client === state.client) addLog(`Lỗi MQTT: ${err.message}`, 'error');
    });

    // Thư viện tự kết nối lại nên vẫn giữ client, tránh tạo client thứ hai trùng Client ID
    client.on('close', () => {
      if (client !== state.client) return;
      if (state.connection === 'connected') addLog('Mất kết nối với broker, đang thử lại...', 'error');
      state.connection = 'connecting';
      state.robotOnline = false;
      updateConnectionUI();
    });
  }

  function disconnect() {
    const client = state.client;
    state.client = null;
    state.connection = 'disconnected';
    state.robotOnline = false;
    state.mode = 'MANUAL';
    if (state.publishTimer) {
      clearTimeout(state.publishTimer);
      state.publishTimer = null;
    }
    client.end(true);
    addLog('Đã ngắt kết nối MQTT.');
    updateConnectionUI();
  }

  // --- GẮN SỰ KIỆN ---
  JOINTS.forEach((n) => {
    const slider = $(`slider${n}`);
    const input = $(`input${n}`);

    slider.addEventListener('input', () => {
      setAngle(n, Number(slider.value));
      scheduleSendAngles();
    });
    slider.addEventListener('change', sendAnglesNow);

    // Chỉ gửi khi nhập xong (Enter hoặc rời ô) để servo không chạy theo từng chữ số đang gõ
    input.addEventListener('change', () => {
      const value = parseFloat(input.value);
      if (Number.isNaN(value)) {
        input.value = state.angles[n];
        return;
      }
      setAngle(n, value);
      input.value = state.angles[n];
      sendAnglesNow();
    });
  });

  el.btnHome.addEventListener('click', () => {
    JOINTS.forEach((n, i) => setAngle(n, HOME_ANGLES[i]));
    sendAnglesNow();
    addLog('Đưa tay máy về vị trí Home.');
  });

  el.btnRelease.addEventListener('click', () => {
    setAngle(4, GRIPPER_RELEASE_ANGLE);
    sendAnglesNow();
  });

  el.btnGrab.addEventListener('click', () => {
    setAngle(4, GRIPPER_GRAB_ANGLE);
    sendAnglesNow();
  });

  el.btnRecord.addEventListener('click', () => {
    const start = state.mode !== 'TEACHING';
    state.mode = start ? 'TEACHING' : 'MANUAL';
    sendTeachingCommand(start ? 'TEACH_START' : 'TEACH_STOP');
    updateRobotUI();
  });

  el.btnAddPoint.addEventListener('click', () => sendTeachingCommand('ADD_POINT'));
  el.btnRun.addEventListener('click', () => sendTeachingCommand('RUN'));
  el.btnStop.addEventListener('click', () => sendTeachingCommand('STOP'));

  el.btnClearPoints.addEventListener('click', () => {
    if (confirm('Bạn có chắc muốn xóa toàn bộ điểm Teaching đã lưu trên ESP32?')) {
      sendTeachingCommand('CLEAR');
    }
  });

  el.btnConnect.addEventListener('click', () => {
    if (state.client) disconnect();
    else connect();
  });

  el.logHeader.addEventListener('click', () => {
    el.logBody.classList.toggle('collapsed');
    el.logChevron.style.transform = el.logBody.classList.contains('collapsed') ? 'rotate(-90deg)' : 'rotate(0deg)';
  });

  el.btnClearLog.addEventListener('click', (e) => {
    e.stopPropagation();
    el.logConsole.textContent = '';
    state.logCount = 0;
    el.logCounter.textContent = '(0 messages)';
  });

  // --- KHỞI TẠO ---
  updateGripperUI();
  updateConnectionUI();
})();
