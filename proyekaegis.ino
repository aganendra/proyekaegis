#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <TinyGPS++.h>

#define BUZZER_PIN   25
#define LED_MERAH    26
#define BUTTON_PIN   27
#define LED_HIJAU    32

Adafruit_MPU6050 mpu;
TinyGPSPlus gps;

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
const unsigned long HEARTBEAT_INTERVAL = 2000; // Siklus total 2000 ms (2 detik)
const unsigned long FLASH_DURATION = 80;       // Hanya menyala 80 ms (hemat daya 96%)

void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_MERAH, OUTPUT);
  pinMode(LED_HIJAU, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  digitalWrite(LED_HIJAU, LOW);
  digitalWrite(LED_MERAH, LOW);
  noTone(BUZZER_PIN);

  Wire.begin(21, 22);
  if (!mpu.begin()) {
    Serial.println("[ERROR] Sensor MPU6050 tidak terdeteksi!");
  } else {
    Serial.println("[OK] Sensor MPU6050 Terhubung!");
    mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
  }

  Serial.println("\n==============================================");
  Serial.println("  SISTEM AEGIS - MODE HEMAT DAYA (HEARTBEAT)  ");
  Serial.println("==============================================");
}

void loop() {
  // Simulasi feed GPS
  const char *nmeaData = "$GPRMC,083559.00,A,0746.2234,S,11024.4567,E,0.0,0.0,051126,,,A*7C\r\n";
  for (int i = 0; nmeaData[i] != '\0'; i++) gps.encode(nmeaData[i]);

  // Baca MPU6050
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  float ax_g = a.acceleration.x / 9.80665;
  float ay_g = a.acceleration.y / 9.80665;
  float az_g = a.acceleration.z / 9.80665;
  float a_total = sqrt(pow(ax_g, 2) + pow(ay_g, 2) + pow(az_g, 2));
  float roll = atan2(ay_g, az_g) * 180.0 / PI;

  // Baca tombol pembatal secara kontinu
  bool buttonPressed = (digitalRead(BUTTON_PIN) == LOW);

  switch (currentState) {
    case SIAGA:
      // 1. Logika LED Hijau Hemat Daya (Menyala 80ms setiap 2 detik)
      if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL) {
        lastHeartbeat = millis();
        digitalWrite(LED_HIJAU, HIGH);
      } else if (millis() - lastHeartbeat >= FLASH_DURATION) {
        digitalWrite(LED_HIJAU, LOW);
      }

      digitalWrite(LED_MERAH, LOW);
      noTone(BUZZER_PIN);

      // 2. Cetak telemetri SIAGA tiap 1 detik
      if (millis() - lastPrintTime >= 1000) {
        lastPrintTime = millis();
        if (millis() < cancelGracePeriodUntil) {
          int sisa = (cancelGracePeriodUntil - millis()) / 1000 + 1;
          Serial.printf("[SIAGA - JEDA %ds] a_total: %.2fg | Roll: %.1f° | Status: Abaikan trigger\n", sisa, a_total, roll);
        } else {
          Serial.printf("[SIAGA] a_total: %.2fg | Roll: %.1f° | Status: Normal & Proteksi Aktif\n", a_total, roll);
        }
      }

      // 3. Evaluasi pemicu benturan / kemiringan
      if (millis() >= cancelGracePeriodUntil) {
        if (a_total >= 3.0 || abs(roll) > 60.0) {
          Serial.println("\n----------------------------------------------");
          Serial.println("[!] BENTURAN / MOTOR REBAH TERDETEKSI!");
          Serial.printf("[!] a_total: %.2fg | Roll: %.1f deg\n", a_total, roll);
          Serial.println("[!] Masuk Fase COUNTDOWN. Tekan Tombol Biru untuk Batal!");
          Serial.println("----------------------------------------------");
          currentState = COUNTDOWN;
          countdownStartTime = millis();
          digitalWrite(LED_HIJAU, LOW);
        }
      }
      break;

    case COUNTDOWN:
      digitalWrite(LED_HIJAU, LOW);

      // Cek tombol pembatal
      if (buttonPressed) {
        Serial.println("\n==============================================");
        Serial.println(">>> [BATAL] TOMBOL PEMBATAL BERHASIL DITEKAN! <<<");
        Serial.println(">>> Sistem langsung kembali ke SIAGA.");
        Serial.println("==============================================\n");

        noTone(BUZZER_PIN);
        digitalWrite(LED_MERAH, LOW);

        currentState = SIAGA;
        cancelGracePeriodUntil = millis() + 5000; // Toleransi jeda 5 detik
        delay(200); // Debounce singkat
        return;
      }

      // Alarm berkedip non-blocking setiap 200ms
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

      // Countdown selesai tanpa dibatalkan
      if (millis() - countdownStartTime >= COUNTDOWN_DURATION) {
        Serial.println("\n==============================================");
        Serial.println("[DARURAT] WAKTU HABIS! Bantuan Tidak Dibatalkan.");
        Serial.println("[DARURAT] Mengirimkan Sinyal SOS via Telegram & Firebase...");
        if (gps.location.isValid()) {
          Serial.printf("[DARURAT] Koordinat TKP : Lat %.6f, Lng %.6f\n", gps.location.lat(), gps.location.lng());
          Serial.printf("[DARURAT] Link Maps: https://maps.google.com/?q=%.6f,%.6f\n", gps.location.lat(), gps.location.lng());
        }
        Serial.println("==============================================\n");
        currentState = DARURAT;
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
        delay(300);
      }
      break;
  }
}