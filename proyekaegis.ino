#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>   // wajib versi 7.x
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <TinyGPS++.h>

// ======================= KONFIGURASI (EDIT BAGIAN INI) =======================
#define WIFI_SSID     "anderground"
#define WIFI_PASS     "psycheXXX"

#define MQTT_HOST     "192.168.137.1"   // IP laptop yang menjalankan Mosquitto
#define MQTT_PORT     1883

#define TOPIC_TELEMETRY "aegis/telemetry"
#define TOPIC_ALERT     "aegis/alert"
#define TOPIC_STATUS    "aegis/status"    // "online" / "offline" (via Last Will)
// =============================================================================

#define BUZZER_PIN   25
#define LED_MERAH    26
#define BUTTON_PIN   27
#define LED_HIJAU    32

// --- GPS u-blox NEO-6M (UART2 ESP32) ---
#define GPS_RX_PIN   16   // ESP32 RX2  <- TX modul GPS
#define GPS_TX_PIN   17   // ESP32 TX2  -> RX modul GPS
#define GPS_BAUD     9600

Adafruit_MPU6050 mpu;
TinyGPSPlus gps;
HardwareSerial gpsSerial(2);

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

enum SystemState { SIAGA, COUNTDOWN, DARURAT };
SystemState currentState = SIAGA;

unsigned long countdownStartTime = 0;
const unsigned long COUNTDOWN_DURATION = 10000; // 10 detik

unsigned long lastPrintTime = 0;
unsigned long cancelGracePeriodUntil = 0;

// Variabel kedip alarm merah non-blocking
unsigned long lastBlinkTime = 0;
bool blinkState = false;

// Variabel LED Hijau Hemat Daya (Heartbeat Strobe)
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 2000;
const unsigned long FLASH_DURATION = 80;

// Variabel pemantau GPS
unsigned long lastGpsWarn = 0;

// Variabel jaringan
const unsigned long TELEMETRY_INTERVAL = 1000;
const unsigned long MQTT_RETRY_INTERVAL = 5000;
unsigned long lastTelemetry = 0;
unsigned long lastMqttAttempt = 0;
bool wifiWasConnected = false;
char clientId[24];

// Alert darurat yang tertunda jika MQTT sedang putus
char pendingAlert[320];
bool hasPendingAlert = false;

// ------------------------------ Helper ---------------------------------------

const char* stateName() {
  switch (currentState) {
    case SIAGA:     return "SIAGA";
    case COUNTDOWN: return "COUNTDOWN";
    case DARURAT:   return "DARURAT";
  }
  return "?";
}

String gpsStatusText() {
  if (gps.charsProcessed() < 10) {
    return "GPS: tidak ada data (cek kabel TX/RX)";
  }
  if (!gps.location.isValid()) {
    return "GPS: mencari satelit (" + String(gps.satellites.isValid() ? gps.satellites.value() : 0) + " sat)";
  }
  return "GPS: FIX (" + String(gps.satellites.value()) + " sat)";
}

String netStatusText() {
  if (WiFi.status() != WL_CONNECTED) return "WiFi: OFF";
  if (!mqtt.connected()) return "WiFi: OK | MQTT: OFF";
  return "WiFi: OK | MQTT: OK";
}

// ------------------------------ MQTT -----------------------------------------

void connectMqtt() {
  Serial.printf("[MQTT] Menyambung ke %s:%d ...\n", MQTT_HOST, MQTT_PORT);
  // Last Will: jika ESP32 putus mendadak, broker menerbitkan "offline" (retained)
  bool ok = mqtt.connect(clientId, NULL, NULL, TOPIC_STATUS, 0, true, "offline");
  if (ok) {
    Serial.println("[MQTT] Terhubung.");
    mqtt.publish(TOPIC_STATUS, "online", true);

    if (hasPendingAlert) {
      if (mqtt.publish(TOPIC_ALERT, pendingAlert)) {
        Serial.println("[MQTT] Alert darurat tertunda berhasil dikirim.");
        hasPendingAlert = false;
      }
    }
  } else {
    Serial.printf("[MQTT] Gagal, rc=%d. Coba lagi dalam %lu detik.\n", mqtt.state(), MQTT_RETRY_INTERVAL / 1000);
  }
}

// Menjaga koneksi tanpa memblokir logika keselamatan
void maintainConnections() {
  bool wifiNow = (WiFi.status() == WL_CONNECTED);
  if (wifiNow && !wifiWasConnected) {
    Serial.printf("[WiFi] Terhubung. IP ESP32: %s\n", WiFi.localIP().toString().c_str());
  } else if (!wifiNow && wifiWasConnected) {
    Serial.println("[WiFi] Terputus. Mencoba menyambung ulang otomatis...");
  }
  wifiWasConnected = wifiNow;

  if (!wifiNow) return;

  // Jangan coba reconnect saat COUNTDOWN agar kedip alarm & tombol batal tidak tersendat
  if (!mqtt.connected() && currentState != COUNTDOWN &&
      millis() - lastMqttAttempt >= MQTT_RETRY_INTERVAL) {
    lastMqttAttempt = millis();
    connectMqtt();
  }
}

void publishTelemetry(float a_total, float roll) {
  if (!mqtt.connected()) return;

  JsonDocument doc;
  doc["a_total"] = round(a_total * 100.0) / 100.0;
  doc["roll"]    = round(roll * 10.0) / 10.0;
  doc["state"]   = stateName();
  doc["rssi"]    = WiFi.RSSI();
  doc["sat"]     = gps.satellites.isValid() ? gps.satellites.value() : 0;
  doc["fix"]     = gps.location.isValid();
  if (gps.location.isValid()) {
    doc["lat"] = gps.location.lat();
    doc["lng"] = gps.location.lng();
  }

  char buf[256];
  serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(TOPIC_TELEMETRY, buf);
}

// event: "countdown" | "emergency" | "cancelled" | "reset"
void publishAlert(const char* event, float a_total, float roll) {
  JsonDocument doc;
  doc["event"]   = event;
  doc["a_total"] = round(a_total * 100.0) / 100.0;
  doc["roll"]    = round(roll * 10.0) / 10.0;
  doc["uptime_ms"] = millis();
  doc["fix"]     = gps.location.isValid();
  if (gps.location.isValid()) {
    doc["lat"] = gps.location.lat();
    doc["lng"] = gps.location.lng();
    char link[80];
    snprintf(link, sizeof(link), "https://maps.google.com/?q=%.6f,%.6f",
             gps.location.lat(), gps.location.lng());
    doc["maps"] = link;
  }

  char buf[320];
  serializeJson(doc, buf, sizeof(buf));

  if (mqtt.connected() && mqtt.publish(TOPIC_ALERT, buf)) {
    Serial.printf("[MQTT] Alert '%s' terkirim.\n", event);
  } else if (strcmp(event, "emergency") == 0) {
    // Alert darurat tidak boleh hilang: simpan dan kirim saat MQTT tersambung lagi
    strlcpy(pendingAlert, buf, sizeof(pendingAlert));
    hasPendingAlert = true;
    Serial.println("[MQTT] OFFLINE: alert darurat ditunda, dikirim begitu tersambung.");
  } else {
    Serial.printf("[MQTT] OFFLINE: alert '%s' dilewati.\n", event);
  }
}

// ------------------------------ Setup ----------------------------------------

void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_MERAH, OUTPUT);
  pinMode(LED_HIJAU, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  digitalWrite(LED_HIJAU, LOW);
  digitalWrite(LED_MERAH, LOW);
  noTone(BUZZER_PIN);

  // GPS NEO-6M
  gpsSerial.setRxBufferSize(1024);
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[OK] UART2 GPS NEO-6M dimulai (9600 baud).");

  // MPU6050
  Wire.begin(21, 22);
  if (!mpu.begin()) {
    Serial.println("[ERROR] Sensor MPU6050 tidak terdeteksi!");
  } else {
    Serial.println("[OK] Sensor MPU6050 Terhubung!");
    mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
  }

  // WiFi (non-blocking: sistem keselamatan tetap jalan walau WiFi belum siap)
  snprintf(clientId, sizeof(clientId), "aegis-%06X", (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF));
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WiFi] Menyambung ke '%s' di latar belakang...\n", WIFI_SSID);

  // MQTT
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(2);
  mqtt.setKeepAlive(15);

  Serial.println("\n==============================================");
  Serial.println("  SISTEM AEGIS - MODE HEMAT DAYA + MQTT       ");
  Serial.println("==============================================");
}

// ------------------------------ Loop -----------------------------------------

void loop() {
  // Baca data NMEA dari GPS
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  if (millis() > 5000 && gps.charsProcessed() < 10 && millis() - lastGpsWarn >= 5000) {
    lastGpsWarn = millis();
    Serial.println("[WARNING] Tidak ada data dari GPS. Periksa wiring: TX GPS -> GPIO16, RX GPS -> GPIO17, VCC, GND.");
  }

  // Jaringan
  maintainConnections();
  mqtt.loop();

  // Baca MPU6050
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  float ax_g = a.acceleration.x / 9.80665;
  float ay_g = a.acceleration.y / 9.80665;
  float az_g = a.acceleration.z / 9.80665;
  float a_total = sqrt(pow(ax_g, 2) + pow(ay_g, 2) + pow(az_g, 2));
  float roll = atan2(ay_g, az_g) * 180.0 / PI;

  bool buttonPressed = (digitalRead(BUTTON_PIN) == LOW);

  // Kirim telemetri ke Node-RED tiap 1 detik (di semua state)
  if (millis() - lastTelemetry >= TELEMETRY_INTERVAL) {
    lastTelemetry = millis();
    publishTelemetry(a_total, roll);
  }

  switch (currentState) {
    case SIAGA:
      if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL) {
        lastHeartbeat = millis();
        digitalWrite(LED_HIJAU, HIGH);
      } else if (millis() - lastHeartbeat >= FLASH_DURATION) {
        digitalWrite(LED_HIJAU, LOW);
      }

      digitalWrite(LED_MERAH, LOW);
      noTone(BUZZER_PIN);

      if (millis() - lastPrintTime >= 1000) {
        lastPrintTime = millis();
        String gpsInfo = gpsStatusText();
        String netInfo = netStatusText();
        if (millis() < cancelGracePeriodUntil) {
          int sisa = (cancelGracePeriodUntil - millis()) / 1000 + 1;
          Serial.printf("[SIAGA - JEDA %ds] a_total: %.2fg | Roll: %.1f° | %s | %s\n",
                        sisa, a_total, roll, gpsInfo.c_str(), netInfo.c_str());
        } else {
          Serial.printf("[SIAGA] a_total: %.2fg | Roll: %.1f° | %s | %s\n",
                        a_total, roll, gpsInfo.c_str(), netInfo.c_str());
        }
      }

      if (millis() >= cancelGracePeriodUntil) {
        if (a_total >= 3.0 || fabs(roll) > 60.0) {
          Serial.println("\n----------------------------------------------");
          Serial.println("[!] BENTURAN / MOTOR REBAH TERDETEKSI!");
          Serial.printf("[!] a_total: %.2fg | Roll: %.1f deg\n", a_total, roll);
          Serial.println("[!] Masuk Fase COUNTDOWN. Tekan Tombol Biru untuk Batal!");
          Serial.println("----------------------------------------------");
          currentState = COUNTDOWN;
          countdownStartTime = millis();
          digitalWrite(LED_HIJAU, LOW);
          publishAlert("countdown", a_total, roll);
        }
      }
      break;

    case COUNTDOWN:
      digitalWrite(LED_HIJAU, LOW);

      if (buttonPressed) {
        Serial.println("\n==============================================");
        Serial.println(">>> [BATAL] TOMBOL PEMBATAL BERHASIL DITEKAN! <<<");
        Serial.println(">>> Sistem langsung kembali ke SIAGA.");
        Serial.println("==============================================\n");

        noTone(BUZZER_PIN);
        digitalWrite(LED_MERAH, LOW);

        currentState = SIAGA;
        cancelGracePeriodUntil = millis() + 5000;
        publishAlert("cancelled", a_total, roll);
        delay(200);
        return;
      }

      if (millis() - lastBlinkTime >= 200) {
        lastBlinkTime = millis();
        blinkState = !blinkState;
        digitalWrite(LED_MERAH, blinkState);
        if (blinkState) {
          tone(BUZZER_PIN, 1200);
        } else {
          noTone(BUZZER_PIN);
        }
      }

      if (millis() - countdownStartTime >= COUNTDOWN_DURATION) {
        Serial.println("\n==============================================");
        Serial.println("[DARURAT] WAKTU HABIS! Bantuan Tidak Dibatalkan.");
        Serial.println("[DARURAT] Mengirimkan Sinyal SOS ke Node-RED via MQTT...");
        if (gps.location.isValid()) {
          Serial.printf("[DARURAT] Koordinat TKP : Lat %.6f, Lng %.6f\n", gps.location.lat(), gps.location.lng());
          Serial.printf("[DARURAT] Link Maps: https://maps.google.com/?q=%.6f,%.6f\n", gps.location.lat(), gps.location.lng());
          Serial.printf("[DARURAT] Usia data GPS: %lu ms | Satelit: %u\n", gps.location.age(), gps.satellites.value());
        } else {
          Serial.println("[DARURAT] Koordinat belum tersedia (GPS belum fix).");
        }
        Serial.println("==============================================\n");
        currentState = DARURAT;
        publishAlert("emergency", a_total, roll);
      }
      break;

    case DARURAT:
      tone(BUZZER_PIN, 1000);
      digitalWrite(LED_MERAH, HIGH);
      digitalWrite(LED_HIJAU, LOW);

      if (buttonPressed) {
        Serial.println("\n[RESET] Sistem di-reset manual kembali ke SIAGA.");
        noTone(BUZZER_PIN);
        digitalWrite(LED_MERAH, LOW);
        cancelGracePeriodUntil = millis() + 5000;
        currentState = SIAGA;
        publishAlert("reset", a_total, roll);
        delay(300);
      }
      break;
  }
}
