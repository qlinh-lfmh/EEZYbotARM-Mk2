#include <Arduino.h>
#include <Bluepad32.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ================== WiFi và MQTT ==================
const char* WIFI_SSID = "tai";   // Chỉ dùng được mạng 2,4 GHz
const char* WIFI_PASSWORD = "10122006";

const char* MQTT_HOST = "broker.emqx.io";
const uint16_t MQTT_PORT = 1883;

// Các topic phải khớp với BASE_TOPIC trên web
const char* TOPIC_ANGLES   = "uit_eezybot_7f3a9c/control/angles";    // Web -> ESP32: góc servo
const char* TOPIC_TEACHING = "uit_eezybot_7f3a9c/control/teaching";  // Web -> ESP32: lệnh Teaching
const char* TOPIC_STATUS   = "uit_eezybot_7f3a9c/status";            // ESP32 -> Web: góc, chế độ, số điểm
const char* TOPIC_ONLINE   = "uit_eezybot_7f3a9c/online";            // ESP32 -> Web: online / offline

const unsigned long RECONNECT_INTERVAL = 5000;  // Thử kết nối lại broker mỗi 5 s
const unsigned long STATUS_INTERVAL = 100;      // Báo trạng thái cho web tối đa 10 lần/giây, để thanh trượt chạy theo tay cầm mượt hơn

WiFiClient net;
PubSubClient mqtt(net);
char deviceId[24];
unsigned long lastReconnectAttempt = 0;
unsigned long lastStatusTime = 0;
bool statusDirty = false;   // Chế độ, quyền điều khiển hoặc số điểm vừa đổi, cần báo cho web

// ================== Servo ==================
enum Joint { BASE, SHOULDER, ELBOW, GRIPPER, JOINT_COUNT };
const char* JOINT_KEYS[JOINT_COUNT] = { "joint1", "joint2", "joint3", "joint4" };

// Giới hạn hành trình (đã khảo sát trên tay máy thật để servo không tì vào khung).
// Toàn bộ hệ thống (web, tay cầm, Teaching, status) dùng GÓC KHỚP THẬT của tay máy,
// chỉ khi gửi cho STM32 mới đổi sang góc servo theo GEAR_RATIO.
// Web tự lấy giới hạn này qua status, nhưng nên sửa cả DEFAULT_LIMITS trong app.js cho khớp
// Đế 60–120 (góc thật sau bánh răng), vai 0–40, khuỷu 0–80, kẹp 90 (mở) – 180 (đóng)
const float JOINT_MIN[JOINT_COUNT] = {  60,   0,   0,  90 };
const float JOINT_MAX[JOINT_COUNT] = { 120,  40,  80, 180 };

// Tỉ số truyền bánh răng (servo quay bao nhiêu độ thì khớp quay 1 độ), quanh tâm 90 độ:
//   góc servo = 90 + GEAR_RATIO x (góc khớp - 90)
// Đế: đo được servo 0..180 làm đế quay 60..120, tức tỉ số 3
const float GEAR_RATIO[JOINT_COUNT] = { 3, 1, 1, 1 };

uint8_t jointToServo(int joint, float jointAngle) {
    float servo = 90 + GEAR_RATIO[joint] * (jointAngle - 90);
    return (uint8_t)lroundf(constrain(servo, 0, 180));
}

const TickType_t UPDATE_INTERVAL = pdMS_TO_TICKS(20);  // Task chuyển động chạy đúng mỗi 20 ms (50 Hz)

// Chuyển động có tăng tốc/giảm tốc để tay máy không bị giật khi bắt đầu chạy và khi dừng.
// Đơn vị là độ SERVO trong một nhịp 20 ms. Khớp có bánh răng quay chậm hơn servo GEAR_RATIO lần
const float MAX_SPEED = 2.0;   // Tốc độ tối đa: 2 độ/nhịp = servo 100 độ/giây (đế thật ≈ 33 độ/giây)
const float ACCEL = 0.3;       // Mỗi nhịp tăng/giảm tốc 0,3 độ/nhịp: từ đứng yên lên tối đa mất ~0,13 s,
                               // từ tối đa đến dừng hẳn đi thêm ~6,7 độ servo

// Số độ servo ứng với 1 độ khớp
float servoScale(int joint) {
    return fabsf(GEAR_RATIO[joint]);
}

const unsigned long JOG_TIMEOUT_MS = 100;  // Lệnh cần gạt quá cũ (loop bị chặn) thì dừng, tránh tay máy tự chạy tiếp

// Vị trí Home (góc khớp thật), tay máy về đây khi bật nguồn. Phải khớp HOME_ANGLES trên web (nút Home)
#define HOME_POSE { 90, 0, 0, 90 }
const float HOME[JOINT_COUNT] = HOME_POSE;

// Các biến dưới đây dùng chung giữa loop() và task chuyển động, phải đọc/ghi trong motionMux
portMUX_TYPE motionMux = portMUX_INITIALIZER_UNLOCKED;
float angle[JOINT_COUNT]  = HOME_POSE;   // Góc đang gửi cho STM32 (chỉ task chuyển động ghi)
float target[JOINT_COUNT] = HOME_POSE;   // Góc servo cần đi tới
float velocity[JOINT_COUNT] = { 0, 0, 0, 0 };   // Vận tốc hiện tại, độ servo/nhịp, có dấu
int movingJoint = -1;                            // Servo đang chạy, -1 là tất cả đứng yên
int jogJoint = -1;                               // Servo cần gạt đang đẩy, -1 là không gạt
int jogActiveJoint = -1;                         // Servo task chuyển động đang cho chạy theo cần gạt
int jogDirection = 0;                            // +1 tăng góc, -1 giảm góc
unsigned long jogTime = 0;                       // Lần cuối loop() cập nhật lệnh cần gạt
uint8_t sentFrame[5] = { 0xFF };                  // Khung vừa gửi cho STM32, để loop() in debug

struct Pose {
    uint8_t angle[JOINT_COUNT];  // Đế, vai, khuỷu, kẹp
};

void setTarget(int joint, float value) {
    float v = constrain(value, JOINT_MIN[joint], JOINT_MAX[joint]);
    portENTER_CRITICAL(&motionMux);
    target[joint] = v;
    portEXIT_CRITICAL(&motionMux);
}

void setJog(int joint, int direction) {
    portENTER_CRITICAL(&motionMux);
    jogJoint = joint;
    jogDirection = direction;
    jogTime = millis();
    portEXIT_CRITICAL(&motionMux);
}

// Dừng mọi servo ngay tại vị trí hiện tại (dùng cho STOP và khi đổi quyền điều khiển)
void holdCurrentPose() {
    portENTER_CRITICAL(&motionMux);
    jogJoint = -1;
    jogActiveJoint = -1;
    for (int j = 0; j < JOINT_COUNT; j++) {
        target[j] = angle[j];
        velocity[j] = 0;
    }
    portEXIT_CRITICAL(&motionMux);
}

bool isMoving() {
    bool moving = false;
    portENTER_CRITICAL(&motionMux);
    for (int j = 0; j < JOINT_COUNT; j++) {
        if (angle[j] != target[j] || velocity[j] != 0) moving = true;
    }
    portEXIT_CRITICAL(&motionMux);
    return moving;
}

Pose targetPose() {
    Pose pose;
    portENTER_CRITICAL(&motionMux);
    for (int j = 0; j < JOINT_COUNT; j++) pose.angle[j] = lroundf(target[j]);
    portEXIT_CRITICAL(&motionMux);
    return pose;
}

Pose currentPose() {
    Pose pose;
    portENTER_CRITICAL(&motionMux);
    for (int j = 0; j < JOINT_COUNT; j++) pose.angle[j] = lroundf(angle[j]);
    portEXIT_CRITICAL(&motionMux);
    return pose;
}

// Cần gạt: khi đang gạt, đích luôn nằm trước góc hiện tại một đoạn vừa đủ để servo tăng tốc lên tối đa.
// Thả cần thì đích được đặt đúng chỗ servo kịp giảm tốc dừng êm (~6,7 độ servo khi đang chạy tối đa).
// Chạy trong task chuyển động nên tốc độ luôn đều kể cả khi loop() bị chậm. Gọi trong motionMux
void applyJog(unsigned long now) {
    if (jogJoint >= 0 && now - jogTime > JOG_TIMEOUT_MS) jogJoint = -1;  // loop() bị chặn, coi như thả cần

    if (jogJoint < 0) {
        if (jogActiveJoint >= 0) {
            int j = jogActiveJoint;
            float v = velocity[j];
            float brake = v * fabsf(v) / (2 * ACCEL);  // Quãng đường giảm tốc tới 0, có dấu, độ servo
            target[j] = constrain(angle[j] + brake / servoScale(j), JOINT_MIN[j], JOINT_MAX[j]);
            jogActiveJoint = -1;
        }
        return;
    }

    if (movingJoint >= 0 && movingJoint != jogJoint) return;  // Đợi servo khác chạy xong
    int j = jogJoint;
    jogActiveJoint = j;
    float v = fabsf(velocity[j]);
    // Đích phải nằm XA HƠN hẳn bước kế tiếp (tối đa v + ACCEL), nếu không servo tưởng đã tới đích,
    // dừng lại và không bao giờ tăng tốc được. Cộng 2·ACCEL để luôn dư ra
    float lookAhead = v * v / (2 * ACCEL) + v + 2 * ACCEL;
    target[j] = constrain(angle[j] + jogDirection * lookAhead / servoScale(j), JOINT_MIN[j], JOINT_MAX[j]);
}

// Mỗi lúc chỉ một servo chạy: servo đang chạy tới đích xong mới tới lượt servo kế tiếp,
// theo thứ tự đế, vai, khuỷu, kẹp. Servo tăng tốc dần, gần tới đích thì giảm tốc rồi dừng êm. Gọi trong motionMux
void updateMotion() {
    if (movingJoint < 0 || (angle[movingJoint] == target[movingJoint] && velocity[movingJoint] == 0)) {
        movingJoint = -1;
        for (int j = 0; j < JOINT_COUNT; j++) {
            if (angle[j] != target[j]) {
                movingJoint = j;
                break;
            }
        }
        if (movingJoint < 0) return;
    }

    int j = movingJoint;
    float scale = servoScale(j);
    float remaining = (target[j] - angle[j]) * scale;  // Quãng đường còn lại, độ servo, có dấu
    float dir = (remaining > 0) ? 1 : -1;

    // Vận tốc mong muốn: tối đa MAX_SPEED, nhưng gần đích thì nhỏ dần để kịp dừng êm (v² = 2·a·s)
    float wanted = dir * max(ACCEL, min(MAX_SPEED, sqrtf(2 * ACCEL * fabsf(remaining))));
    float v = velocity[j];
    if (wanted > v) v = min(v + ACCEL, wanted);
    else v = max(v - ACCEL, wanted);

    // Bước này tới hoặc vượt đích thì đặt đúng đích và dừng
    if (fabsf(remaining) < 0.01f || (v * remaining > 0 && fabsf(v) >= fabsf(remaining))) {
        angle[j] = target[j];
        velocity[j] = 0;
        return;
    }

    angle[j] += v / scale;
    velocity[j] = v;

    // Đang chạy ngược khỏi đích để giảm tốc (vì đích vừa bị đổi) thì không được vượt giới hạn
    if (angle[j] < JOINT_MIN[j] || angle[j] > JOINT_MAX[j]) {
        angle[j] = constrain(angle[j], JOINT_MIN[j], JOINT_MAX[j]);
        velocity[j] = 0;
    }
}

// Task chuyển động: chạy riêng, đúng nhịp 20 ms, không bị WiFi/MQTT/Bluetooth trong loop() làm khựng.
// Mỗi nhịp: áp lệnh cần gạt, đưa servo đi thêm một bước, rồi gửi khung cho STM32
// Khung 5 byte: [0xFF][đế][vai][khuỷu][kẹp] chứa GÓC SERVO (đã đổi từ góc khớp qua tỉ số truyền),
// góc tối đa 180 nên không trùng với 0xFF
void motionTask(void*) {
    TickType_t lastWake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&lastWake, UPDATE_INTERVAL);
        unsigned long now = millis();

        uint8_t frame[5] = { 0xFF };
        portENTER_CRITICAL(&motionMux);
        applyJog(now);
        updateMotion();
        for (int j = 0; j < JOINT_COUNT; j++) frame[j + 1] = jointToServo(j, angle[j]);
        memcpy(sentFrame, frame, sizeof(frame));
        portEXIT_CRITICAL(&motionMux);

        Serial2.write(frame, sizeof(frame));
    }
}

// In góc ra Serial khi thay đổi, làm trong loop() để việc in không làm chậm task chuyển động
void printAnglesIfChanged() {
    static uint8_t lastPrinted[5];
    uint8_t frame[5];
    float baseJoint;
    portENTER_CRITICAL(&motionMux);
    memcpy(frame, sentFrame, sizeof(frame));
    baseJoint = angle[BASE];
    portEXIT_CRITICAL(&motionMux);

    if (memcmp(frame, lastPrinted, sizeof(frame)) != 0) {
        memcpy(lastPrinted, frame, sizeof(frame));
        Serial.printf("Servo: %d %d %d %d | Goc de that: %.1f\n",
                      frame[1], frame[2], frame[3], frame[4], baseJoint);
    }
}

// ================== Chế độ và Teaching ==================
enum RobotMode { MODE_MANUAL, MODE_TEACHING, MODE_PLAYBACK };
RobotMode robotMode = MODE_MANUAL;  // Không lưu vào flash, để khởi động lại không tự phát lại

const int MAX_POINTS = 50;
Pose points[MAX_POINTS];
int pointCount = 0;
Preferences prefs;

const unsigned long SERVO_GAP_MS = 750;    // Nghỉ giữa hai lần servo chạy liên tiếp khi phát lại
const unsigned long CYCLE_END_MS = 1000;   // Chạy hết điểm cuối thì nghỉ bấy lâu rồi mới về điểm ban đầu
int playbackIndex = 0;              // Điểm đang đi tới
unsigned long arrivedTime = 0;      // Lúc servo vừa chạy xong dừng lại
bool playbackFirstMove = false;     // Lần chạy đầu tiên sau khi bấm Run thì không cần chờ
bool playbackCycleEnd = false;      // Vừa xong điểm cuối, lần chạy kế tiếp (về điểm đầu) phải chờ CYCLE_END_MS

const char* robotModeName() {
    if (robotMode == MODE_TEACHING) return "TEACHING";
    if (robotMode == MODE_PLAYBACK) return "PLAYBACK";
    return "MANUAL";
}

void setRobotMode(RobotMode mode) {
    if (robotMode == mode) return;
    robotMode = mode;
    statusDirty = true;
    Serial.printf("Che do: %s\n", robotModeName());
}

// Chỉ ghi flash khi kết thúc Record hoặc xóa điểm, để hạn chế hao mòn flash
void savePoints() {
    bool ok;
    if (pointCount == 0) {
        ok = !prefs.isKey("points") || prefs.remove("points");
    } else {
        size_t length = pointCount * sizeof(Pose);
        ok = prefs.putBytes("points", points, length) == length;
    }

    if (ok) Serial.printf("[TEACHING] Da luu %d diem vao flash\n", pointCount);
    else Serial.println("[TEACHING] LOI: khong ghi duoc flash");
}

void loadPoints() {
    if (!prefs.isKey("points")) return;

    size_t length = prefs.getBytesLength("points");
    if (length == 0 || length % sizeof(Pose) != 0 || length > sizeof(points) ||
        prefs.getBytes("points", points, length) != length) {
        Serial.println("[TEACHING] Du lieu trong flash khong hop le, bo qua");
        return;
    }

    pointCount = length / sizeof(Pose);
    Serial.printf("[TEACHING] Da nap %d diem tu flash\n", pointCount);
}

// Lưu góc đích chứ không lưu góc servo đang đi qua, nên điểm được lưu luôn là vị trí cuối cùng của thanh trượt
void addPoint() {
    Pose pose = targetPose();

    // Bỏ qua điểm trùng điểm vừa lưu, ví dụ khi lệnh QoS 1 bị gửi lặp
    if (pointCount > 0 && memcmp(&points[pointCount - 1], &pose, sizeof(Pose)) == 0) return;

    if (pointCount >= MAX_POINTS) {
        Serial.println("[TEACHING] Da day diem, khong luu them");
        return;
    }

    points[pointCount++] = pose;
    Serial.printf("[TEACHING] Diem %d: %d %d %d %d\n", pointCount,
                  pose.angle[BASE], pose.angle[SHOULDER], pose.angle[ELBOW], pose.angle[GRIPPER]);
}

// Bật Record: xóa bài cũ và lưu tư thế hiện tại làm điểm ban đầu.
// Sau đó mỗi lần nhấn Lưu điểm (ADD_POINT) sẽ lưu thêm một điểm
void startTeaching() {
    pointCount = 0;
    addPoint();
    setRobotMode(MODE_TEACHING);
}

void stopTeaching() {
    setRobotMode(MODE_MANUAL);
    savePoints();
}

void startPlayback() {
    holdCurrentPose();  // Bỏ lệnh cần gạt hoặc góc web còn sót trước khi phát lại
    playbackIndex = 0;
    playbackFirstMove = true;
    playbackCycleEnd = false;
    setRobotMode(MODE_PLAYBACK);
    Serial.printf("[PLAYBACK] Bat dau, di toi diem 1/%d\n", pointCount);
}

void stopPlayback() {
    holdCurrentPose();
    setRobotMode(MODE_MANUAL);
}

// Phát lại từng servo một: tìm servo đầu tiên (đế, vai, khuỷu, kẹp) chưa ở đúng góc của điểm đang đi,
// cho nó chạy, chạy xong nghỉ 0,75 s rồi mới tới servo kế tiếp. Servo nào không đổi góc thì bỏ qua.
// Xong mọi servo của một điểm thì sang điểm sau. Xong điểm cuối thì nghỉ 1 s rồi về điểm ban đầu,
// lặp lại cho đến khi có lệnh STOP
void updatePlayback() {
    if (robotMode != MODE_PLAYBACK) return;

    unsigned long now = millis();
    if (isMoving()) {
        arrivedTime = now;  // Servo còn chạy, tính thời gian nghỉ từ lúc nó dừng
        return;
    }

    // Tìm servo kế tiếp cần chạy tới điểm hiện tại. So với góc đã kẹp theo giới hạn,
    // để điểm nằm ngoài giới hạn không làm phát lại đứng mãi ở một servo
    const Pose& point = points[playbackIndex];
    int joint = -1;
    float value = 0;
    portENTER_CRITICAL(&motionMux);
    for (int j = 0; j < JOINT_COUNT; j++) {
        float want = constrain((float)point.angle[j], JOINT_MIN[j], JOINT_MAX[j]);
        if (angle[j] != want) {
            joint = j;
            value = want;
            break;
        }
    }
    portEXIT_CRITICAL(&motionMux);

    if (joint < 0) {
        // Đã tới điểm này, sang điểm kế tiếp. Hết điểm cuối thì quay về điểm ban đầu
        playbackIndex++;
        if (playbackIndex >= pointCount) {
            playbackIndex = 0;
            playbackCycleEnd = true;
            Serial.println("[PLAYBACK] Xong mot vong, nghi 1 s roi ve diem ban dau");
        } else {
            Serial.printf("[PLAYBACK] Di toi diem %d/%d\n", playbackIndex + 1, pointCount);
        }
        return;
    }

    unsigned long wait = playbackFirstMove ? 0 : (playbackCycleEnd ? CYCLE_END_MS : SERVO_GAP_MS);
    if (now - arrivedTime < wait) return;

    playbackFirstMove = false;
    playbackCycleEnd = false;
    setTarget(joint, value);
}

// ================== Tay cầm ==================
const int DEADZONE = 150;              // Cần gạt có giá trị -512..511, trong khoảng ±150 coi như ở giữa
const unsigned long B_HOLD_MS = 2000;  // Giữ B 2 s để chiếm hoặc trả quyền điều khiển

ControllerPtr myController = nullptr;
bool gamepadHasControl = false;  // Mặc định web giữ quyền
bool bPressed = false;
bool bHoldHandled = false;       // Lần giữ này đã đổi quyền, phải thả B ra mới đổi tiếp
unsigned long bPressStart = 0;
int stickJoint = -1;             // Servo đang được cần gạt điều khiển, -1 là chưa gạt

const char* controlSourceName() {
    return gamepadHasControl ? "GAMEPAD" : "WEB";
}

void setGamepadControl(bool enabled) {
    if (gamepadHasControl == enabled) return;
    gamepadHasControl = enabled;
    stickJoint = -1;
    setJog(-1, 0);
    statusDirty = true;
    Serial.printf("Quyen dieu khien: %s\n", controlSourceName());
}

// Khi tay cầm chiếm quyền thì web không gửi được lệnh nữa, nên phải tự kết thúc
// Record và dừng phát lại, đồng thời bỏ các góc web gửi mà servo chưa chạy tới
void giveControlToGamepad() {
    if (robotMode == MODE_TEACHING) stopTeaching();
    if (robotMode == MODE_PLAYBACK) stopPlayback();
    holdCurrentPose();
    setGamepadControl(true);
}

void updateGamepadControl(ControllerPtr ctl) {
    if (!ctl->b()) {
        bPressed = false;
        bHoldHandled = false;
        return;
    }
    if (!bPressed) {
        bPressed = true;
        bPressStart = millis();
    }
    if (bHoldHandled || millis() - bPressStart < B_HOLD_MS) return;

    bHoldHandled = true;
    if (gamepadHasControl) setGamepadControl(false);
    else giveControlToGamepad();
}

void processGamepad(ControllerPtr ctl) {
    // Mỗi khớp một trục cần gạt, giá trị dương là tăng góc:
    // - Cần trái:  X → đế (sang phải tăng góc), Y → vai (đẩy lên tăng góc)
    // - Cần phải:  Y → khuỷu (đẩy lên tăng góc), X → kẹp (sang phải đóng kẹp, sang trái mở kẹp)
    int axis[JOINT_COUNT];
    axis[BASE] = ctl->axisX();
    axis[SHOULDER] = -ctl->axisY();
    axis[ELBOW] = -ctl->axisRY();
    axis[GRIPPER] = ctl->axisRX();

    // Thả cần về giữa thì bỏ khóa
    if (stickJoint >= 0 && abs(axis[stickJoint]) < DEADZONE) stickJoint = -1;

    // Chỉ khóa vào trục bị gạt mạnh nhất và giữ khóa đến khi thả cần,
    // nên gạt lệch chéo cũng không kéo theo servo khác
    if (stickJoint < 0) {
        int strongest = DEADZONE;
        for (int j = 0; j < JOINT_COUNT; j++) {
            if (abs(axis[j]) >= strongest) {
                strongest = abs(axis[j]);
                stickJoint = j;
            }
        }
    }

    // Chỉ báo khớp và chiều cho task chuyển động, task tự cho servo tăng tốc lên 100 độ/giây
    if (stickJoint >= 0) setJog(stickJoint, (axis[stickJoint] > 0) ? 1 : -1);
    else setJog(-1, 0);
}

void onConnectedController(ControllerPtr ctl) {
    if (myController != nullptr) return;
    myController = ctl;
    Serial.printf(">> Da ket noi tay cam: %s\n", ctl->getModelName().c_str());
}

void onDisconnectedController(ControllerPtr ctl) {
    if (myController != ctl) return;
    myController = nullptr;
    bPressed = false;
    bHoldHandled = false;
    stickJoint = -1;
    setGamepadControl(false);  // Trả quyền cho web để web không bị khóa
    Serial.println("<< Tay cam da ngat ket noi");
}

// ================== MQTT ==================
// Góc báo cho web:
// - Web đang điều khiển: báo góc ĐÍCH, để thanh trượt không bị kéo ngược khi servo còn đang chạy hoặc chờ tới lượt
// - Tay cầm hoặc phát lại: báo góc THẬT đang chạy, vì góc đích của cần gạt luôn nằm trước servo một đoạn
Pose statusPose() {
    return (gamepadHasControl || robotMode == MODE_PLAYBACK) ? currentPose() : targetPose();
}

void publishStatus() {
    Pose pose = statusPose();
    char buffer[224];  // Gói MQTT mặc định tối đa 256 byte gồm cả topic, payload này dài nhất ~160 byte
    // Gửi kèm giới hạn góc để web tự đặt min/max cho thanh trượt theo đúng JOINT_MIN/JOINT_MAX
    snprintf(buffer, sizeof(buffer),
             "{\"joint1\":%d,\"joint2\":%d,\"joint3\":%d,\"joint4\":%d,\"mode\":\"%s\",\"points\":%d,\"source\":\"%s\","
             "\"min\":[%d,%d,%d,%d],\"max\":[%d,%d,%d,%d]}",
             pose.angle[BASE], pose.angle[SHOULDER], pose.angle[ELBOW], pose.angle[GRIPPER],
             robotModeName(), pointCount, controlSourceName(),
             (int)JOINT_MIN[BASE], (int)JOINT_MIN[SHOULDER], (int)JOINT_MIN[ELBOW], (int)JOINT_MIN[GRIPPER],
             (int)JOINT_MAX[BASE], (int)JOINT_MAX[SHOULDER], (int)JOINT_MAX[ELBOW], (int)JOINT_MAX[GRIPPER]);
    mqtt.publish(TOPIC_STATUS, buffer, true);  // retain để web mở sau vẫn biết trạng thái
}

void publishStatusIfChanged() {
    static Pose lastPose;
    Pose pose = statusPose();
    bool changed = statusDirty || memcmp(&pose, &lastPose, sizeof(Pose)) != 0;
    if (!changed || !mqtt.connected() || millis() - lastStatusTime < STATUS_INTERVAL) return;

    lastStatusTime = millis();
    lastPose = pose;
    statusDirty = false;
    publishStatus();
}

// Lệnh có thể đến trễ hoặc bị lặp, nên mỗi lệnh đều tự kiểm tra lại điều kiện
void handleTeachingCommand(const char* action) {
    if (strcmp(action, "STOP") == 0) {
        if (robotMode == MODE_PLAYBACK) stopPlayback();
    } else if (gamepadHasControl) {
        Serial.printf("[TEACHING] Bo qua %s: tay cam dang giu quyen\n", action);
    } else if (strcmp(action, "TEACH_START") == 0) {
        if (robotMode == MODE_MANUAL) startTeaching();
    } else if (strcmp(action, "TEACH_STOP") == 0) {
        if (robotMode == MODE_TEACHING) stopTeaching();
    } else if (strcmp(action, "ADD_POINT") == 0) {
        if (robotMode == MODE_TEACHING) addPoint();
    } else if (strcmp(action, "RUN") == 0) {
        // Cần ít nhất điểm ban đầu và một điểm đã lưu thì mới có chuyển động để phát lại
        if (robotMode == MODE_MANUAL && pointCount >= 2) startPlayback();
    } else if (strcmp(action, "CLEAR") == 0) {
        if (robotMode == MODE_MANUAL) {
            pointCount = 0;
            savePoints();
        }
    } else {
        Serial.printf("[TEACHING] Lenh khong ro: %s\n", action);
    }

    // Luôn báo lại trạng thái thật, để web sửa lại nếu lệnh bị bỏ qua
    statusDirty = true;
}

void onMessage(char* topic, byte* payload, unsigned int length) {
    Serial.printf("[MQTT] %s -> %.*s\n", topic, (int)length, (const char*)payload);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, length);
    if (error) {
        Serial.printf("[MQTT] JSON loi: %s\n", error.c_str());
        return;
    }

    if (strcmp(topic, TOPIC_ANGLES) == 0) {
        // Tay cầm đang giữ quyền hoặc đang phát lại thì bỏ qua, và báo lại góc thật cho web
        if (gamepadHasControl || robotMode == MODE_PLAYBACK) {
            statusDirty = true;
            return;
        }
        // Web chỉ gửi các khớp vừa đổi, khớp nào không có trong tin nhắn thì giữ nguyên
        for (int j = 0; j < JOINT_COUNT; j++) {
            if (doc[JOINT_KEYS[j]].is<float>()) setTarget(j, doc[JOINT_KEYS[j]].as<float>());
        }
    } else if (strcmp(topic, TOPIC_TEACHING) == 0) {
        handleTeachingCommand(doc["action"] | "");
    }
}

// Đăng ký LWT để broker tự báo "offline" khi ESP32 mất kết nối đột ngột
void connectMqtt() {
    if (!mqtt.connect(deviceId, nullptr, nullptr, TOPIC_ONLINE, 1, true, "offline")) {
        Serial.printf("Ket noi MQTT that bai, rc = %d\n", mqtt.state());
        return;
    }

    mqtt.publish(TOPIC_ONLINE, "online", true);
    mqtt.subscribe(TOPIC_ANGLES, 1);
    mqtt.subscribe(TOPIC_TEACHING, 1);
    publishStatus();
    Serial.println("Da ket noi MQTT broker");
}

// ================== Chương trình chính ==================
void setup() {
    Serial.begin(115200);                      // Debug lên máy tính
    Serial2.begin(115200, SERIAL_8N1, 16, 17);  // UART sang STM32: RX2 = GPIO 16, TX2 = GPIO 17

    // Client ID lấy từ địa chỉ MAC để không trùng với thiết bị khác trên broker
    uint64_t mac = ESP.getEfuseMac();
    snprintf(deviceId, sizeof(deviceId), "esp32-%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
    Serial.printf("Device ID: %s\n", deviceId);

    if (prefs.begin("teaching", false)) {
        loadPoints();
    } else {
        Serial.println("[TEACHING] Khong mo duoc flash, cac diem se khong duoc luu");
    }

    // Không chờ WiFi ở đây, để tay cầm và UART vẫn chạy khi chưa có mạng
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMessage);
    mqtt.setSocketTimeout(3);  // Không để loop bị chặn quá lâu khi broker không phản hồi

    BP32.setup(&onConnectedController, &onDisconnectedController);
    Serial.println("Dang cho tay cam Bluetooth ket noi...");

    for (int j = 0; j < JOINT_COUNT; j++) sentFrame[j + 1] = jointToServo(j, HOME[j]);

    // Task chuyển động chạy trên core 1 (WiFi và Bluetooth chạy trên core 0),
    // ưu tiên cao hơn loop() để luôn gửi góc cho STM32 đúng nhịp
    xTaskCreatePinnedToCore(motionTask, "motion", 4096, nullptr, 2, nullptr, 1);
}

// loop() lo phần "chậm": đọc tay cầm, nhận lệnh MQTT, phát lại, báo trạng thái.
// Chỉ đặt góc đích, còn việc chạy servo do task chuyển động làm
void loop() {
    BP32.update();

    if (mqtt.connected()) {
        mqtt.loop();  // Nhận tin nhắn mới và gửi ping giữ kết nối
    } else if (WiFi.status() == WL_CONNECTED && millis() - lastReconnectAttempt >= RECONNECT_INTERVAL) {
        lastReconnectAttempt = millis();
        connectMqtt();
    }

    // Đọc tay cầm mỗi vòng lặp để lệnh cần gạt luôn mới
    if (myController != nullptr && myController->isConnected()) {
        updateGamepadControl(myController);
        if (gamepadHasControl) processGamepad(myController);
    }

    updatePlayback();
    publishStatusIfChanged();
    printAnglesIfChanged();

    vTaskDelay(1);  // Nhường CPU cho các task khác để tránh watchdog reset
}