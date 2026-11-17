(function () {
  'use strict';

  // --- CẤU HÌNH ---
  const BASE_TOPIC = 'uit_eezybot_7f3a9c';    // Phải khớp với các TOPIC_... trên ESP32
  const JOINTS = [1, 2, 3, 4];                // 1: Đế, 2: Vai, 3: Khuỷu, 4: Kẹp
  const HOME_ANGLES = [90, 0, 0, 90];         // Phải khớp góc khởi tạo trên ESP32: đế 90, vai 0, khuỷu 0, kẹp 90 (mở)
  // Giới hạn mặc định khi chưa nhận status, lấy theo JOINT_MIN/JOINT_MAX trên ESP32.
  // Khi ESP32 gửi status (có "min"/"max"), web sẽ tự cập nhật theo giá trị thật
  // Đế là góc quay thật sau bánh răng (60–120), không phải góc servo. Kẹp 90 là mở, 180 là đóng
  const DEFAULT_LIMITS = { 1: [60, 120], 2: [0, 40], 3: [0, 80], 4: [90, 180] };
  const PUBLISH_INTERVAL_MS = 50;             // Gửi góc tối đa 20 lần/giây khi kéo thanh trượt
  const LOCAL_CONTROL_HOLD_MS = 350;          // Bỏ qua góc từ ESP32 ngay sau khi người dùng thao tác
  const ECHO_TIMEOUT_MS = 2000;               // Chờ tối đa bấy lâu để ESP32 xác nhận góc vừa gửi
  const MAX_LOG_ENTRIES = 200;

  // --- TRẠNG THÁI ---
  const state = {
    client: null,
    connection: 'disconnected',               // 'disconnected' | 'connecting' | 'connected'
    robotOnline: false,
    mode: 'MANUAL',                           // 'MANUAL' | 'TEACHING' | 'PLAYBACK'
    points: 0,
    source: 'WEB',                            // Bên giữ quyền điều khiển: 'WEB' | 'GAMEPAD'
    angles: { 1: 90, 2: 0, 3: 0, 4: 90 },
    limits: { 1: [...DEFAULT_LIMITS[1]], 2: [...DEFAULT_LIMITS[2]], 3: [...DEFAULT_LIMITS[3]], 4: [...DEFAULT_LIMITS[4]] },
    lastLocalControlTime: 0,
    pendingJoints: new Set(),                 // Các khớp đã đổi nhưng chưa gửi
    awaitingEcho: {},                         // Khớp đã gửi nhưng ESP32 chưa báo lại đúng góc đó: { khớp: { value, until } }
    draggingJoint: null,                      // Thanh trượt đang được giữ chuột/ngón tay, không để status ghi đè
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
  function clampAngle(joint, value) {
    const [min, max] = state.limits[joint];
    return Math.max(min, Math.min(max, Math.round(value)));
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
    el.btnHome.disabled = !canControl;

    el.btnRecord.disabled = !canControl;
    el.btnRecord.textContent = teaching ? 'Record On' : 'Record Off';
    el.btnRecord.classList.toggle('recording', teaching);
    el.btnAddPoint.disabled = !(canControl && teaching);
    el.btnClearPoints.disabled = !(canControl && !teaching);
    el.btnRun.disabled = !(canControl && !teaching && state.points >= 2);  // Điểm ban đầu + ít nhất 1 điểm đã lưu
    el.btnRun.textContent = playback ? 'Running...' : 'Run';
    el.btnStop.disabled = !connected;           // Lệnh Dừng luôn dùng được khi còn kết nối broker
    el.teachPointBadge.textContent = `${state.points} điểm lưu`;
  }

  // Đặt min/max cho thanh trượt, ô số và nhãn hiển thị của một khớp
  function setJointLimits(joint, min, max) {
    state.limits[joint] = [min, max];
    const slider = $(`slider${joint}`);
    const input = $(`input${joint}`);
    if (slider && input) {  // Kẹp không có thanh trượt
      slider.min = input.min = min;
      slider.max = input.max = max;
      $(`min${joint}`).textContent = `${min}°`;
      $(`max${joint}`).textContent = `${max}°`;
    }
    setAngle(joint, state.angles[joint]);  // Kẹp lại góc đang hiển thị vào khoảng mới
  }

  // Nhận giới hạn từ status của ESP32, bỏ qua nếu dữ liệu không hợp lệ
  function applyLimitsFromStatus(mins, maxs) {
    if (!Array.isArray(mins) || !Array.isArray(maxs)) return;
    JOINTS.forEach((n, i) => {
      const min = mins[i];
      const max = maxs[i];
      const valid = Number.isInteger(min) && Number.isInteger(max) && min >= 0 && max <= 180 && min <= max;
      if (!valid) return;
      const [curMin, curMax] = state.limits[n];
      if (min !== curMin || max !== curMax) {
        setJointLimits(n, min, max);
        addLog(`Giới hạn khớp ${n}: ${min}° – ${max}°`, 'info');
      }
    });
  }

  function setAngle(joint, value) {
    const angle = clampAngle(joint, value);
    state.angles[joint] = angle;
    const slider = $(`slider${joint}`);
    if (slider) slider.value = angle;
    // Không đổi ô số khi người dùng đang gõ vào đó
    const input = $(`input${joint}`);
    if (input && document.activeElement !== input) input.value = angle;
  }

  // --- GỬI LỆNH MQTT ---
  // quiet = true: không ghi log, dùng cho các gói gửi liên tục khi đang kéo thanh trượt,
  // vì ghi log 20 lần/giây làm giao diện bị khựng
  function publish(subTopic, payload, qos, quiet) {
    if (!state.client || state.connection !== 'connected') {
      addLog('Chưa kết nối broker, lệnh không được gửi.', 'error');
      return;
    }
    const topic = `${BASE_TOPIC}/${subTopic}`;
    const message = JSON.stringify(payload);
    state.client.publish(topic, message, { qos: qos, retain: false }, (err) => {
      if (err) addLog(`Lỗi gửi tới ${topic}: ${err.message}`, 'error');
    });
    if (!quiet) addLog(`[PUB] ${topic} -> ${message}`, 'pub');
  }

  // Chỉ gửi các khớp vừa đổi, để không ghi đè góc đích của khớp đang chờ tới lượt chạy trên ESP32
  function sendPendingJoints(qos) {
    if (state.pendingJoints.size === 0) return;
    const payload = {};
    const until = performance.now() + ECHO_TIMEOUT_MS;
    state.pendingJoints.forEach((n) => {
      payload[`joint${n}`] = state.angles[n];
      state.awaitingEcho[n] = { value: state.angles[n], until: until };
    });
    state.pendingJoints.clear();
    publish('control/angles', payload, qos, qos === 0);  // Chỉ ghi log góc cuối (QoS 1)
  }

  function cancelScheduledSend() {
    if (state.publishTimer) {
      clearTimeout(state.publishTimer);
      state.publishTimer = null;
    }
  }

  // Khi đang kéo: gom thay đổi và gửi góc mới nhất theo chu kỳ, cho phép mất gói (QoS 0)
  function scheduleSendJoint(joint) {
    state.lastLocalControlTime = performance.now();
    state.pendingJoints.add(joint);
    if (state.publishTimer) return;
    state.publishTimer = setTimeout(() => {
      state.publishTimer = null;
      sendPendingJoints(0);
    }, PUBLISH_INTERVAL_MS);
  }

  // Gửi ngay các góc đang chờ và đảm bảo gói đến nơi (QoS 1)
  function flushPendingJoints() {
    cancelScheduledSend();
    sendPendingJoints(1);
  }

  // Khi thao tác xong: gửi ngay góc cuối cùng
  function sendJointsNow(joints) {
    state.lastLocalControlTime = performance.now();
    joints.forEach((n) => state.pendingJoints.add(n));
    flushPendingJoints();
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
    applyLimitsFromStatus(data.min, data.max);

    // Web không giữ quyền (đang phát lại hoặc tay cầm điều khiển): thanh trượt chạy theo ESP32
    const now = performance.now();
    const webInControl = state.mode !== 'PLAYBACK' && !isGamepadActive();
    const recentlyTouched = now - state.lastLocalControlTime < LOCAL_CONTROL_HOLD_MS;
    if (!webInControl) state.awaitingEcho = {};

    JOINTS.forEach((n) => {
      const value = data[`joint${n}`];
      if (typeof value !== 'number' || !Number.isFinite(value)) return;

      // Thanh trượt đang bị giữ hoặc ô số đang được gõ thì tuyệt đối không ghi đè, tránh giật dưới tay người dùng
      if (state.draggingJoint === n || document.activeElement === $(`input${n}`)) return;

      if (webInControl) {
        // Vừa thao tác xong thì giữ nguyên thanh trượt
        if (recentlyTouched) return;
        // Status đi qua broker có thể về trễ: chỉ nhận lại góc của khớp vừa gửi khi ESP32 đã báo
        // đúng góc đó, để thanh trượt không bị kéo về góc cũ rồi nhảy lại
        const echo = state.awaitingEcho[n];
        if (echo) {
          if (value !== echo.value && now < echo.until) return;
          delete state.awaitingEcho[n];  // Đã xác nhận, hoặc quá lâu thì tin ESP32 (ví dụ góc bị giới hạn lại)
        }
      }
      setAngle(n, value);
    });
    updateRobotUI();
  }

  function handleMessage(topic, payload) {
    const message = payload.toString();

    if (topic === `${BASE_TOPIC}/status`) {
      // Status đến tới 10 lần/giây, chỉ ghi log khi chế độ, quyền điều khiển hoặc số điểm thay đổi
      const before = `${state.mode}|${state.source}|${state.points}`;
      handleStatus(message);
      if (`${state.mode}|${state.source}|${state.points}` !== before) addLog(`[SUB] ${topic} -> ${message}`, 'sub');
    } else if (topic === `${BASE_TOPIC}/online`) {
      addLog(`[SUB] ${topic} -> ${message}`, 'sub');
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
    cancelScheduledSend();
    state.pendingJoints.clear();
    state.awaitingEcho = {};
    client.end(true);
    addLog('Đã ngắt kết nối MQTT.');
    updateConnectionUI();
  }

  // --- GẮN SỰ KIỆN ---
  JOINTS.forEach((n) => {
    const slider = $(`slider${n}`);
    const input = $(`input${n}`);

    // Đánh dấu thanh trượt đang được giữ, để status từ ESP32 không kéo nó đi khi người dùng dừng tay giữa chừng
    slider.addEventListener('pointerdown', () => {
      state.draggingJoint = n;
    });

    slider.addEventListener('input', () => {
      state.draggingJoint = state.draggingJoint ?? n;  // Bàn phím (phím mũi tên) không có pointerdown
      setAngle(n, Number(slider.value));
      scheduleSendJoint(n);
    });
    slider.addEventListener('change', () => sendJointsNow([n]));

    // Chỉ gửi khi nhập xong (Enter hoặc rời ô) để servo không chạy theo từng chữ số đang gõ
    input.addEventListener('change', () => {
      const value = parseFloat(input.value);
      if (Number.isNaN(value)) {
        input.value = state.angles[n];
        return;
      }
      setAngle(n, value);
      input.value = state.angles[n];
      sendJointsNow([n]);
    });
  });

  // Thả chuột/ngón tay ở bất kỳ đâu (kể cả ngoài thanh trượt) thì kết thúc kéo và gửi ngay góc cuối,
  // để không bị kẹt trạng thái đang kéo khi con trỏ ra khỏi thanh trượt lúc thả
  function endDrag() {
    const n = state.draggingJoint;
    if (n === null) return;
    state.draggingJoint = null;
    if (state.pendingJoints.size > 0) flushPendingJoints();
    state.lastLocalControlTime = performance.now();
  }
  window.addEventListener('pointerup', endDrag);
  window.addEventListener('pointercancel', endDrag);
  window.addEventListener('blur', endDrag);
  JOINTS.forEach((n) => $(`slider${n}`).addEventListener('keyup', endDrag));

  el.btnHome.addEventListener('click', () => {
    JOINTS.forEach((n, i) => setAngle(n, HOME_ANGLES[i]));
    sendJointsNow(JOINTS);
    addLog('Đưa tay máy về vị trí Home.');
  });

  // Record On lưu tư thế hiện tại làm điểm ban đầu, Lưu điểm lưu vị trí cuối của các thanh trượt.
  // Luôn gửi nốt góc đang chờ trước, để ESP32 lưu đúng vị trí cuối của thanh trượt
  el.btnRecord.addEventListener('click', () => {
    const start = state.mode !== 'TEACHING';
    flushPendingJoints();
    state.mode = start ? 'TEACHING' : 'MANUAL';
    sendTeachingCommand(start ? 'TEACH_START' : 'TEACH_STOP');
    updateRobotUI();
  });

  el.btnAddPoint.addEventListener('click', () => {
    flushPendingJoints();
    sendTeachingCommand('ADD_POINT');
  });

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
  JOINTS.forEach((n) => setJointLimits(n, state.limits[n][0], state.limits[n][1]));
  updateConnectionUI();
})();
