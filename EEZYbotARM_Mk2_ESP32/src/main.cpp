#include <Arduino.h>
#include <Bluepad32.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// Cấu hình wifi (phải là mạng 2,4 GHz)
const char* WIFI_SSID = "wifi_name";
const char* WIFI_PASSWORD = "wifi_password";

// Cấu hình MQTT
const char* MQTT_HOST = "broker.emqx.io";
const uint16_t MQTT_PORT = 1883;

// Các topic khớp với Base Topic trên web app
const char* TOPIC_ANGLES   = "uit_eezybot_7f3a9c/control/angles";    // Web -> ESP32: góc 4 servo
const char* TOPIC_TEACHING = "uit_eezybot_7f3a9c/control/teaching";  // Web -> ESP32: lệnh Teaching
const char* TOPIC_STATUS   = "uit_eezybot_7f3a9c/status";            // ESP32 -> Web: góc, chế độ, số điểm
const char* TOPIC_ONLINE   = "uit_eezybot_7f3a9c/online";            // ESP32 -> Web: online / offline

WiFiClient net; // Tạo một đường ống mạng vật lý (TCP/IP)
PubSubClient mqtt(net); // Khởi tạo đối tượng MQTT và dùng đường ống mạng net để truyền/nhận dữ liệu

char deviceId[24];

unsigned long lastReconnectAttempt = 0;
unsigned long lastStatusTime = 0;
const unsigned long RECONNECT_INTERVAL = 5000; // Thử kết nối lại broker mỗi 5s
const unsigned long STATUS_INTERVAL = 250;     // Gửi trạng thái tối đa 4 lần/giây

// Biến đại diện cho controller
ControllerPtr myController;

// 1. Biến lưu trữ góc hiện tại của 4 servo (Khởi tạo ở 90 độ - vị trí cân bằng)
float baseAngle = 90.0;
float shoulderAngle = 90.0;
float elbowAngle = 90.0;
float gripperAngle = 90.0;

// 2. Cấu hình thông số điều khiển
const int DEADZONE = 80;     // Lọc nhiễu cho dải -512 đến +511
const float STEP = 1.0;      // Tốc độ xoay (tăng/giảm 1 độ mỗi chu kỳ)

// Cấu hình thời gian gửi UART (Gửi 50 lần/giây = 50Hz) để STM32 không bị ngợp
unsigned long lastUpdateTime = 0;
const int UPDATE_INTERVAL = 20; // 20ms

// Khung gần nhất, dùng để chỉ gửi trạng thái và in debug khi góc thay đổi
uint8_t lastDebugFrame[5] = {0};
uint8_t lastStatusFrame[5] = {0};

// 3. Quyền điều khiển: mặc định thuộc về web, giữ nút B 2s trên tay cầm để chiếm hoặc trả quyền
const unsigned long B_HOLD_MS = 2000;
bool gamepadHasControl = false;
bool bPressed = false;           // Nút B đang được nhấn
bool bHoldHandled = false;       // Lần giữ này đã đổi quyền, phải thả B ra mới đổi tiếp
unsigned long bPressStart = 0;
bool statusDirty = false;        // Có thay đổi cần báo cho web ngoài thay đổi góc

// 4. Teaching: mỗi điểm lưu một tư thế gồm cả 4 servo
enum RobotMode { MODE_MANUAL, MODE_TEACHING, MODE_PLAYBACK };
struct Pose { uint8_t base, shoulder, elbow, gripper; };

const int MAX_POINTS = 50;
Pose points[MAX_POINTS];
int pointCount = 0;
RobotMode robotMode = MODE_MANUAL;   // Không lưu vào flash, để khởi động lại không tự phát lại
Preferences prefs;

// Thông số phát lại, cần căn chỉnh trên tay máy thật
const float PLAYBACK_SPEED_DEG_S = 20.0;       // Tốc độ của khớp phải đi xa nhất
const unsigned long MIN_MOVE_MS = 300;         // Thời gian di chuyển tối thiểu giữa hai điểm
const unsigned long POINT_HOLD_MS = 300;       // Dừng tại điểm khi kẹp không đổi
const unsigned long GRIPPER_SETTLE_MS = 800;   // Chờ kẹp đóng/mở xong rồi mới đi tiếp
const unsigned long MAX_PLAYBACK_STEP_MS = 40; // Thời gian tính tối đa cho một nhịp phát lại

// Phát lại gồm 2 pha: di chuyển 3 khớp đến điểm, rồi đặt góc kẹp và chờ
enum PlaybackPhase { PHASE_MOVING, PHASE_SETTLING };
PlaybackPhase playbackPhase = PHASE_MOVING;
int currentPoint = 0;
unsigned long phaseElapsed = 0;      // Thời gian đã chạy trong pha hiện tại, cộng dồn theo từng nhịp
unsigned long lastPlaybackTick = 0;
unsigned long moveDuration = 0;
unsigned long settleDuration = 0;
float moveStartBase = 0;
float moveStartShoulder = 0;
float moveStartElbow = 0;

const char* controlSourceName() {
    return gamepadHasControl ? "GAMEPAD" : "WEB";
}

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

void setGamepadControl(bool enabled) {
    if (gamepadHasControl == enabled) return;
    gamepadHasControl = enabled;
    statusDirty = true;
    Serial.printf("Quyen dieu khien: %s\n", controlSourceName());
}

// Phát hiện giữ nút B đủ 2s, mỗi lần giữ chỉ đổi quyền một lần
// Đang phát lại thì dừng phát lại, và tay cầm chiếm quyền (kể cả khi mất mạng)
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
    if (!bHoldHandled && millis() - bPressStart >= B_HOLD_MS) {
        bHoldHandled = true;
        if (robotMode == MODE_PLAYBACK) setRobotMode(MODE_MANUAL);
        setGamepadControl(!gamepadHasControl);
    }
}

// Giới hạn hành trình an toàn (Software Limits), dùng chung cho tay cầm và web
// *Lưu ý: Cần căn chỉnh lại các góc này khi lắp thực tế
void applyAngleLimits() {
    baseAngle = constrain(baseAngle, 0, 180);
    shoulderAngle = constrain(shoulderAngle, 45, 135);
    elbowAngle = constrain(elbowAngle, 40, 150);
    gripperAngle = constrain(gripperAngle, 0, 180);
}

// Gửi trạng thái cho web, giữ lại trên broker để web mở sau vẫn biết góc hiện tại
void publishStatus() {
    char buffer[160];
    snprintf(buffer, sizeof(buffer),
             "{\"joint1\":%d,\"joint2\":%d,\"joint3\":%d,\"joint4\":%d,\"mode\":\"%s\",\"points\":%d,\"source\":\"%s\"}",
             (int)baseAngle, (int)shoulderAngle, (int)elbowAngle, (int)gripperAngle,
             robotModeName(), pointCount, controlSourceName());
    mqtt.publish(TOPIC_STATUS, buffer, true);
}

// Chỉ ghi flash khi kết thúc ghi hoặc xóa, không ghi ở mỗi lần lưu điểm để hạn chế hao mòn
void savePoints() {
    bool ok;
    if (pointCount == 0) {
        ok = !prefs.isKey("points") || prefs.remove("points");
    } else {
        size_t length = pointCount * sizeof(Pose);
        ok = prefs.putBytes("points", points, length) == length;
    }

    if (ok) {
        Serial.printf("[TEACHING] Da luu %d diem vao flash\n", pointCount);
    } else {
        Serial.println("[TEACHING] LOI: khong ghi duoc flash, reset se mat cac diem moi");
    }
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

// Bắt đầu đi tới một điểm từ vị trí hiện tại, các khớp cùng đến nơi một lúc
void startMoveToPoint(int index) {
    currentPoint = index;
    const Pose& target = points[currentPoint];
    moveStartBase = baseAngle;
    moveStartShoulder = shoulderAngle;
    moveStartElbow = elbowAngle;

    float maxDelta = max(fabs(target.base - baseAngle),
                         max(fabs(target.shoulder - shoulderAngle), fabs(target.elbow - elbowAngle)));
    moveDuration = max((unsigned long)(maxDelta / PLAYBACK_SPEED_DEG_S * 1000.0), MIN_MOVE_MS);

    playbackPhase = PHASE_MOVING;
    phaseElapsed = 0;
    lastPlaybackTick = millis();
    Serial.printf("[TEACHING] Di toi diem %d/%d\n", currentPoint + 1, pointCount);
}

// Được gọi mỗi 20ms nên không chặn tay cầm, UART và MQTT
void updatePlayback() {
    if (robotMode != MODE_PLAYBACK) return;

    const Pose& target = points[currentPoint];

    // Giới hạn thời gian cộng thêm mỗi nhịp: nếu vòng lặp bị chặn (ví dụ khi kết nối lại MQTT),
    // tay máy đi tiếp từ chỗ đang dừng thay vì nhảy tới vị trí xa hơn
    unsigned long now = millis();
    phaseElapsed += min(now - lastPlaybackTick, MAX_PLAYBACK_STEP_MS);
    lastPlaybackTick = now;

    if (playbackPhase == PHASE_MOVING) {
        float t = min((float)phaseElapsed / moveDuration, 1.0f);
        baseAngle = moveStartBase + (target.base - moveStartBase) * t;
        shoulderAngle = moveStartShoulder + (target.shoulder - moveStartShoulder) * t;
        elbowAngle = moveStartElbow + (target.elbow - moveStartElbow) * t;

        if (t >= 1.0f) {
            // Đến nơi rồi mới đóng/mở kẹp, để kẹp không đóng khi chưa tới vật
            bool gripperChanged = (uint8_t)gripperAngle != target.gripper;
            gripperAngle = target.gripper;
            settleDuration = gripperChanged ? GRIPPER_SETTLE_MS : POINT_HOLD_MS;
            playbackPhase = PHASE_SETTLING;
            phaseElapsed = 0;
        }
        applyAngleLimits();
    } else if (phaseElapsed >= settleDuration) {
        // Hết điểm cuối thì quay về điểm đầu, lặp đến khi có lệnh dừng
        startMoveToPoint((currentPoint + 1) % pointCount);
    }
}

// Xử lý lệnh Teaching từ web, tự kiểm tra điều kiện vì lệnh có thể đến trễ hoặc trùng
void handleTeachingCommand(const char* action) {
    // Lệnh dừng luôn được xử lý trước
    if (strcmp(action, "STOP") == 0) {
        if (robotMode == MODE_PLAYBACK) setRobotMode(MODE_MANUAL);
    } else if (gamepadHasControl) {
        Serial.printf("[TEACHING] Bo qua %s: tay cam dang giu quyen\n", action);
    } else if (strcmp(action, "TEACH_START") == 0) {
        if (robotMode == MODE_MANUAL) setRobotMode(MODE_TEACHING);
    } else if (strcmp(action, "TEACH_STOP") == 0) {
        if (robotMode == MODE_TEACHING) {
            setRobotMode(MODE_MANUAL);
            savePoints();
        }
    } else if (strcmp(action, "ADD_POINT") == 0) {
        if (robotMode != MODE_TEACHING) {
            Serial.println("[TEACHING] Bo qua ADD_POINT: chua bat Record");
        } else if (pointCount >= MAX_POINTS) {
            Serial.println("[TEACHING] Da day diem, khong luu them");
        } else {
            Pose pose = {(uint8_t)baseAngle, (uint8_t)shoulderAngle, (uint8_t)elbowAngle, (uint8_t)gripperAngle};
            // QoS 1 có thể gửi trùng lệnh, nên bỏ qua điểm giống hệt điểm vừa lưu
            if (pointCount > 0 && memcmp(&points[pointCount - 1], &pose, sizeof(Pose)) == 0) {
                Serial.println("[TEACHING] Bo qua diem trung voi diem vua luu");
            } else {
                points[pointCount++] = pose;
                Serial.printf("[TEACHING] Diem %d: %d %d %d %d\n", pointCount,
                              pose.base, pose.shoulder, pose.elbow, pose.gripper);
            }
        }
    } else if (strcmp(action, "RUN") == 0) {
        if (robotMode != MODE_MANUAL) {
            Serial.println("[TEACHING] Bo qua RUN: dang ghi hoac dang phat lai");
        } else if (pointCount == 0) {
            Serial.println("[TEACHING] Bo qua RUN: chua co diem nao");
        } else {
            setRobotMode(MODE_PLAYBACK);
            startMoveToPoint(0);
        }
    } else if (strcmp(action, "CLEAR") == 0) {
        if (robotMode != MODE_PLAYBACK) {
            pointCount = 0;
            savePoints();
        }
    } else {
        Serial.printf("[TEACHING] Lenh khong ro: %s\n", action);
    }

    // Luôn báo lại trạng thái thật, để web sửa lại nếu lệnh bị bỏ qua
    statusDirty = true;
}

// Hàm kết nối và đăng ký để broker tự publish offline vào TOPIC_ONLINE khi bị mất kết nối đột ngột
void connectMqtt() {
    bool ok = mqtt.connect(deviceId, nullptr, nullptr,
                           TOPIC_ONLINE, 1, true, "offline");
    if (!ok) {
        Serial.printf("Ket noi MQTT that bai, rc = %d\n", mqtt.state());
        return;
    }

    mqtt.publish(TOPIC_ONLINE, "online", true);
    mqtt.subscribe(TOPIC_ANGLES, 1);
    mqtt.subscribe(TOPIC_TEACHING, 1);
    publishStatus();
    Serial.println("Da ket noi MQTT broker");
}

// Hàm Callback mỗi khi ESP32 nhận được dữ liệu từ Broker
void onMessage(char* topic, byte* payload, unsigned int length) {
    Serial.printf("[MQTT] %s -> %.*s\n", topic, (int)length, (const char*)payload);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, length);
    if (error) {
        Serial.printf("[MQTT] JSON loi: %s\n", error.c_str());
        return;
    }

    if (strcmp(topic, TOPIC_ANGLES) == 0) {
        // Tay cầm đang giữ quyền hoặc đang phát lại thì bỏ qua lệnh và báo lại góc thật cho web
        if (gamepadHasControl || robotMode == MODE_PLAYBACK) {
            statusDirty = true;
            return;
        }

        // Chỉ cập nhật khớp có trong tin nhắn và đúng kiểu số
        if (doc["joint1"].is<float>()) baseAngle = doc["joint1"].as<float>();
        if (doc["joint2"].is<float>()) shoulderAngle = doc["joint2"].as<float>();
        if (doc["joint3"].is<float>()) elbowAngle = doc["joint3"].as<float>();
        if (doc["joint4"].is<float>()) gripperAngle = doc["joint4"].as<float>();
        applyAngleLimits();
    } else if (strcmp(topic, TOPIC_TEACHING) == 0) {
        handleTeachingCommand(doc["action"] | "");
    }
}

// Hàm callback khi tay cầm kết nối
void onConnectedController(ControllerPtr ctl) {
    if (myController == nullptr) {
        Serial.print(">> Da ket noi: ");
        Serial.println(ctl->getModelName());
        myController = ctl;
    }
}

// Hàm callback khi tay cầm ngắt kết nối
void onDisconnectedController(ControllerPtr ctl) {
    if (myController == ctl) {
        Serial.println("<< Tay cầm đã ngắt kết nối.");
        myController = nullptr;
        // Mất Bluetooth thì trả quyền cho web, tránh web bị khóa mãi
        bPressed = false;
        bHoldHandled = false;
        setGamepadControl(false);
    }
}

void processGamepad(ControllerPtr ctl) {
    // Chưa giữ B 2s để chiếm quyền thì cần gạt và nút A/Y không có tác dụng
    if (!gamepadHasControl) return;

    // A. Trục Left X (Xoay đế)
    int leftX = ctl->axisX();
    if (leftX > DEADZONE) baseAngle += STEP;
    else if (leftX < -DEADZONE) baseAngle -= STEP;

    // B. Trục Left Y (Vai - Thường tay cầm đẩy lên thì giá trị âm, kéo xuống là dương)
    int leftY = ctl->axisY();
    if (leftY < -DEADZONE) shoulderAngle += STEP;      // Đẩy cần lên -> Vươn vai
    else if (leftY > DEADZONE) shoulderAngle -= STEP;  // Kéo cần xuống -> Rút vai

    // C. Trục Right Y (Khuỷu)
    int rightY = ctl->axisRY();
    if (rightY < -DEADZONE) elbowAngle += STEP;
    else if (rightY > DEADZONE) elbowAngle -= STEP;

    // D. Nút Digital (Kẹp gắp)
    if (ctl->a()) gripperAngle = 180;      // Đóng kẹp
    else if (ctl->y()) gripperAngle = 90;  // Mở kẹp

    // E. Giới hạn hành trình an toàn
    applyAngleLimits();
}

void setup() {
    Serial.begin(115200); // Dùng để debug in lên màn hình máy tính

    // Khởi tạo Serial2 để giao tiếp UART với STM32
    // Mặc định ESP32: TX2 = GPIO 17, RX2 = GPIO 16
    Serial2.begin(115200, SERIAL_8N1, 16, 17);

    // Khởi tạo ID dùng làm Client ID trên broker
    uint64_t mac = ESP.getEfuseMac();
    snprintf(deviceId, sizeof(deviceId), "esp32-%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
    Serial.printf("Device ID: %s\n", deviceId);

    // Nạp các điểm Teaching đã lưu, tay máy vẫn ở MANUAL cho đến khi có lệnh Run
    if (prefs.begin("teaching", false)) {
        loadPoints();
    } else {
        Serial.println("[TEACHING] Khong mo duoc flash, cac diem se khong duoc luu");
    }

    // Kết nối WiFi, không chờ ở đây để tay cầm và UART vẫn chạy khi chưa có mạng
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMessage);
    mqtt.setSocketTimeout(3);  // Giảm thời gian chờ broker phản hồi khi kết nối

    // Khởi tạo Bluepad32
    BP32.setup(&onConnectedController, &onDisconnectedController);

    Serial.println("Dang cho tay cam Bluetooth ket noi...");
}

void loop() {
    // Cập nhật trạng thái tay cầm
    BP32.update();

    // MQTT không được làm dừng tay cầm và UART khi mất mạng
    if (mqtt.connected()) {
        mqtt.loop(); // Gửi ping giữ kết nối và nhận gói tin mới, gọi onMessage nếu có
    } else if (WiFi.status() == WL_CONNECTED && millis() - lastReconnectAttempt >= RECONNECT_INTERVAL) {
        lastReconnectAttempt = millis();
        connectMqtt();
    }

    // Tính góc, đóng gói và gửi dữ liệu mỗi 20ms (tốc độ xoay = 1 độ x 50Hz = 50 độ/giây)
    if (millis() - lastUpdateTime >= UPDATE_INTERVAL) {
        lastUpdateTime = millis();

        // Nếu có tay cầm đang kết nối, kiểm tra nút B rồi xử lý tính toán góc
        if (myController != nullptr && myController->isConnected()) {
            updateGamepadControl(myController);
            processGamepad(myController);
        }

        updatePlayback();

        // Khung nhị phân 5 byte: [0xFF][Base][Shoulder][Elbow][Gripper]
        // Góc tối đa là 180 nên 0xFF (255) không bao giờ trùng với dữ liệu
        uint8_t dataFrame[5] = {
            0xFF,
            (uint8_t)baseAngle,
            (uint8_t)shoulderAngle,
            (uint8_t)elbowAngle,
            (uint8_t)gripperAngle
        };

        // In ra Serial Monitor khi góc thay đổi để bạn dễ debug
        if (memcmp(dataFrame, lastDebugFrame, sizeof(dataFrame)) != 0) {
            memcpy(lastDebugFrame, dataFrame, sizeof(dataFrame));
            Serial.printf("Debug: FF %d %d %d %d\n", dataFrame[1], dataFrame[2], dataFrame[3], dataFrame[4]);
        }

        // Gửi qua UART sang STM32
        Serial2.write(dataFrame, sizeof(dataFrame));

        // Báo trạng thái cho web khi góc hoặc quyền điều khiển thay đổi, kể cả khi đổi bằng tay cầm
        if (mqtt.connected() && millis() - lastStatusTime >= STATUS_INTERVAL &&
            (statusDirty || memcmp(dataFrame, lastStatusFrame, sizeof(dataFrame)) != 0)) {
            lastStatusTime = millis();
            statusDirty = false;
            memcpy(lastStatusFrame, dataFrame, sizeof(dataFrame));
            publishStatus();
        }
    }

    // Nhường CPU cho các task khác để tránh watchdog reset
    vTaskDelay(1);
}