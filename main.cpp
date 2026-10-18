#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>

#define WDT_TIMEOUT_SECONDS 4

// --- НАЛАШТУВАННЯ WI-FI ---
const char* WIFI_SSID = "test";
const char* WIFI_PASS = "123456789";

// --- ПРИЗНАЧЕННЯ ПІНІВ ---
const uint8_t PIN_BTN_0      = 0;   // Кнопка 0 (Shift / Заряд / Відхилення будильника)
const uint8_t PIN_BTN_1      = 1;   // Кнопка 1 (Вибір треку / Саймон / Сейф / Wi-Fi)
const uint8_t PIN_BATTERY    = 3;   // АЦП дільника акумулятора (100k + 180k)
const uint8_t PIN_BUZZER     = 7;   // Пасивний п'єзо-базер
const uint8_t PIN_LED_BLUE   = 8;   // Бортовий синій світлодіод плати
const uint8_t PIN_VIBRO      = 10;  // Мосфет мотора

#define LED_ON  LOW
#define LED_OFF HIGH

const float RTC_CALIBRATION_FACTOR = 1.0255f;
const float BATTERY_DIVIDER_RATIO  = 1.597f;

const char* NTP_SERVER      = "pool.ntp.org";
const long  GMT_OFFSET_SEC  = 2 * 3600;
const int   DAYLIGHT_OFFSET = 3600;

WebServer server(80);
Preferences prefs;

int alarmHour = 7;
int alarmMinute = 30;
int rssiThreshold = -82;

enum SysState {
  STATE_CONFIG_WEB,
  STATE_RINGING,
  STATE_MUTED_WALK,
  STATE_GAME_ACTIVE
};

SysState currentState = STATE_CONFIG_WEB;

volatile bool wifiConnected = false;
volatile bool wifiLostPermanently = false;
bool isOfflineMode = false;
int currentRssi = -50;
unsigned long lastRssiCheck = 0;

// ========================================================
// ЧОРНА СКРИНЬКА (RTC_NOINIT_ATTR)
// ========================================================
RTC_NOINIT_ATTR int   rtc_crash_checkpoint;
RTC_NOINIT_ATTR float rtc_vbat_before_crash;
RTC_NOINIT_ATTR char  rtc_last_action[48];
String crashReportWeb = "Дані збоїв відсутні";

void setCheckpoint(int cp, const char* action) {
  rtc_crash_checkpoint = cp;
  strncpy(rtc_last_action, action, sizeof(rtc_last_action) - 1);
  rtc_last_action[sizeof(rtc_last_action) - 1] = '\0';
}

void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiConnected = true;
      wifiLostPermanently = false;
      Serial.println("\n[Wi-Fi] Успішно підключено!");
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiConnected = false;
      wifiLostPermanently = true;
      Serial.println("\n[Wi-Fi] Зв'язок розірвано");
      break;
    default:
      break;
  }
}

float getBatteryVoltage() {
  uint32_t rawMvSum = 0;
  for (int i = 0; i < 16; i++) {
    rawMvSum += analogReadMilliVolts(PIN_BATTERY);
    delayMicroseconds(100);
  }
  float pinMv = (float)rawMvSum / 16.0f;
  return (pinMv * BATTERY_DIVIDER_RATIO) / 1000.0f;
}

int getBatteryPercentage(float voltage) {
  if (voltage >= 4.20f) return 100;
  if (voltage <= 3.30f) return 0;
  int percent = (int)(((voltage - 3.30f) / (4.20f - 3.30f)) * 100.0f);
  return constrain(percent, 0, 100);
}

void speakBatteryLevel() {
  float v = getBatteryVoltage();
  int pct = getBatteryPercentage(v);
  int beeps = 1;
  if (pct > 75) beeps = 4;
  else if (pct > 50) beeps = 3;
  else if (pct > 25) beeps = 2;

  Serial.printf("\n[АКУМУЛЯТОР] %.2fV (%d%%) -> %d біп(и)\n", v, pct, beeps);

  for (int i = 0; i < beeps; i++) {
    digitalWrite(PIN_LED_BLUE, LED_ON);
    digitalWrite(PIN_VIBRO, HIGH);
    tone(PIN_BUZZER, 2200);
    delay(70);
    digitalWrite(PIN_VIBRO, LOW);
    digitalWrite(PIN_LED_BLUE, LED_OFF);
    noTone(PIN_BUZZER);
    delay(120);
  }
}

void playPowerOnJingle() {
  setCheckpoint(100, "Power On Jingle");
  digitalWrite(PIN_VIBRO, HIGH);
  for (int f = 300; f <= 900; f += 120) {
    tone(PIN_BUZZER, f);
    delay(18);
  }
  digitalWrite(PIN_VIBRO, LOW);
  noTone(PIN_BUZZER);
  delay(70);

  const int notes[] = { 659, 831, 988, 1318, 1975 };
  const int durs[]  = {  65,  65,  75,  110,  280 };

  for (int i = 0; i < 5; i++) {
    esp_task_wdt_reset();
    digitalWrite(PIN_LED_BLUE, LED_ON);
    if (i == 1 || i == 3 || i == 4) digitalWrite(PIN_VIBRO, HIGH);

    tone(PIN_BUZZER, notes[i]);
    delay(durs[i]);

    digitalWrite(PIN_VIBRO, LOW);
    digitalWrite(PIN_LED_BLUE, LED_OFF);
    noTone(PIN_BUZZER);
    if (i < 4) delay(25);
  }
}

// ========================================================
// МЕЛОДІЇ ТА ПАСХАЛКИ
// ========================================================

void playRickroll() {
  setCheckpoint(200, "Rickroll");
  #define RR_D4  294
  #define RR_E4  330
  #define RR_FS4 370
  #define RR_G4  392
  #define RR_A4  440
  #define RR_B4  494
  #define RR_C5  523
  #define RR_D5  587
  #define RR_REST 0

  const int notes[] = {
    RR_D4, RR_E4, RR_G4, RR_E4, RR_B4, RR_B4, RR_A4, RR_REST,
    RR_D4, RR_E4, RR_G4, RR_E4, RR_A4, RR_A4, RR_G4, RR_FS4, RR_E4, RR_REST,
    RR_D4, RR_E4, RR_G4, RR_E4, RR_G4, RR_A4, RR_FS4, RR_D4, RR_D4, RR_D4, RR_A4, RR_G4
  };
  const int durs[] = {
    140, 140, 140, 140, 360, 360, 500, 100,
    140, 140, 140, 140, 360, 360, 220, 220, 480, 100,
    140, 140, 140, 140, 280, 240, 280, 360, 140, 140, 320, 650
  };
  const int len = sizeof(notes) / sizeof(notes[0]);

  for (int i = 0; i < len; i++) {
    esp_task_wdt_reset();
    int freq = notes[i];
    int dur = durs[i];

    if (freq != RR_REST) {
      digitalWrite(PIN_LED_BLUE, LED_ON);
      tone(PIN_BUZZER, freq);
      if (freq == RR_B4 || freq == RR_A4 || freq == RR_G4) {
        digitalWrite(PIN_VIBRO, HIGH);
      }
    }

    int soundTime = dur > 35 ? dur - 30 : dur;
    delay(soundTime);

    digitalWrite(PIN_VIBRO, LOW);
    digitalWrite(PIN_LED_BLUE, LED_OFF);
    noTone(PIN_BUZZER);
    delay(30);
  }
}

void playImperialMarch() {
  setCheckpoint(300, "Imperial March");
  #define SW_A3  220
  #define SW_F3  175
  #define SW_C4  262
  #define SW_E4  330
  #define SW_F4  349
  #define SW_GS3 208
  #define SW_A4  440
  #define SW_GS4 415
  #define SW_G4  392
  #define SW_FS4 370
  #define SW_AS3 233
  #define SW_DS4 311
  #define SW_D4  294
  #define SW_CS4 277
  #define SW_B3  247
  #define SW_REST 0

  const int notes[] = {
    SW_A4, SW_A4, SW_A4, SW_F4, SW_C4, SW_A4, SW_F4, SW_C4, SW_A4, SW_REST,
    SW_E4, SW_E4, SW_E4, SW_F4, SW_C4, SW_GS3, SW_F4, SW_C4, SW_A3, SW_REST,
    SW_A4, SW_A3, SW_A3, SW_A4, SW_GS4, SW_G4, SW_FS4, SW_F4, SW_FS4, SW_REST,
    SW_AS3, SW_DS4, SW_D4, SW_CS4, SW_C4, SW_B3, SW_C4, SW_REST,
    SW_F3, SW_GS3, SW_F3, SW_C4, SW_A3, SW_F3, SW_C4, SW_A3
  };
  const int durs[] = {
    450, 450, 450, 320, 140, 450, 320, 140, 600, 150,
    450, 450, 450, 320, 140, 450, 320, 140, 600, 180,
    450, 220, 220, 450, 300, 150, 150, 150, 300, 150,
    250, 400, 300, 150, 150, 150, 300, 150,
    250, 400, 300, 150, 450, 320, 140, 650
  };
  const int len = sizeof(notes) / sizeof(notes[0]);

  for (int i = 0; i < len; i++) {
    esp_task_wdt_reset();
    int freq = notes[i];
    int dur = durs[i];

    if (freq != SW_REST) {
      digitalWrite(PIN_VIBRO, HIGH);
      digitalWrite(PIN_LED_BLUE, LED_ON);
      tone(PIN_BUZZER, freq);
    }

    int soundTime = dur > 40 ? dur - 35 : dur;
    delay(soundTime);

    digitalWrite(PIN_VIBRO, LOW);
    digitalWrite(PIN_LED_BLUE, LED_OFF);
    noTone(PIN_BUZZER);
    delay(35);
  }
}

void playTetris() {
  setCheckpoint(400, "Tetris");
  #define TET_A4 440
  #define TET_B4 494
  #define TET_C5 523
  #define TET_D5 587
  #define TET_E5 659
  #define TET_F5 698
  #define TET_G5 784
  #define TET_A5 880
  #define TET_REST 0

  const int notes[] = {
    TET_E5, TET_B4, TET_C5, TET_D5, TET_C5, TET_B4,
    TET_A4, TET_A4, TET_C5, TET_E5, TET_D5, TET_C5,
    TET_B4, TET_C5, TET_D5, TET_E5,
    TET_C5, TET_A4, TET_A4, TET_REST,
    TET_D5, TET_F5, TET_A5, TET_G5, TET_F5,
    TET_E5, TET_C5, TET_E5, TET_D5, TET_C5,
    TET_B4, TET_B4, TET_C5, TET_D5, TET_E5,
    TET_C5, TET_A4, TET_A4
  };
  const int durs[] = {
    360, 180, 180, 360, 180, 180,
    360, 180, 180, 360, 180, 180,
    540, 180, 360, 360,
    360, 360, 360, 180,
    360, 180, 360, 180, 180,
    540, 180, 360, 180, 180,
    360, 180, 180, 360, 360,
    360, 360, 500
  };
  const int len = sizeof(notes) / sizeof(notes[0]);

  for (int i = 0; i < len; i++) {
    esp_task_wdt_reset();
    int freq = notes[i];
    int dur = durs[i];

    if (freq != TET_REST) {
      digitalWrite(PIN_LED_BLUE, LED_ON);
      tone(PIN_BUZZER, freq);
      if (freq >= TET_E5) digitalWrite(PIN_VIBRO, HIGH);
    }

    int soundTime = dur > 30 ? dur - 25 : dur;
    delay(soundTime);

    digitalWrite(PIN_VIBRO, LOW);
    digitalWrite(PIN_LED_BLUE, LED_OFF);
    noTone(PIN_BUZZER);
    delay(25);
  }
}

void playMegalovania() {
  setCheckpoint(500, "Megalovania");
  Serial.println(">>> [ПАСХАЛКА] Megalovania! <<<");

  #define MG_D4  294
  #define MG_D5  587
  #define MG_A4  440
  #define MG_GS4 415
  #define MG_G4  392
  #define MG_F4  349
  #define MG_C4  262
  #define MG_B3  247
  #define MG_AS3 233

  const int notes[] = {
    MG_D4, MG_D4, MG_D5, MG_A4, MG_GS4, MG_G4, MG_F4, MG_D4, MG_F4, MG_G4,
    MG_C4, MG_C4, MG_D5, MG_A4, MG_GS4, MG_G4, MG_F4, MG_D4, MG_F4, MG_G4,
    MG_B3, MG_B3, MG_D5, MG_A4, MG_GS4, MG_G4, MG_F4, MG_D4, MG_F4, MG_G4,
    MG_AS3, MG_AS3, MG_D5, MG_A4, MG_GS4, MG_G4, MG_F4, MG_D4, MG_F4, MG_G4
  };
  const int durs[] = {
    130, 130, 260, 260, 200, 200, 200, 130, 130, 130,
    130, 130, 260, 260, 200, 200, 200, 130, 130, 130,
    130, 130, 260, 260, 200, 200, 200, 130, 130, 130,
    130, 130, 260, 260, 200, 200, 200, 130, 130, 350
  };
  const int len = sizeof(notes) / sizeof(notes[0]);

  for (int i = 0; i < len; i++) {
    esp_task_wdt_reset();
    int freq = notes[i];
    int dur = durs[i];

    digitalWrite(PIN_LED_BLUE, LED_ON);
    if (freq == MG_D5 || freq <= MG_D4) {
      digitalWrite(PIN_VIBRO, HIGH);
    }

    tone(PIN_BUZZER, freq);
    int soundTime = dur > 25 ? dur - 20 : dur;
    delay(soundTime);

    digitalWrite(PIN_VIBRO, LOW);
    digitalWrite(PIN_LED_BLUE, LED_OFF);
    noTone(PIN_BUZZER);
    delay(20);
  }
}

// ========================================================
// ГРА 1: "САЙМОН" (БЕЗ ДИСПЛЕЯ ТА БЕЗ ЗАВИСАНЬ)
// ========================================================
void playSignal0() {
  digitalWrite(PIN_VIBRO, HIGH);
  tone(PIN_BUZZER, 350);
  delay(140);
  digitalWrite(PIN_VIBRO, LOW);
  noTone(PIN_BUZZER);
}

void playSignal1() {
  digitalWrite(PIN_LED_BLUE, LED_ON);
  tone(PIN_BUZZER, 1400);
  delay(140);
  digitalWrite(PIN_LED_BLUE, LED_OFF);
  noTone(PIN_BUZZER);
}

void playRoundSuccessSound() {
  digitalWrite(PIN_LED_BLUE, LED_ON);
  tone(PIN_BUZZER, 1760); delay(45);
  tone(PIN_BUZZER, 2793); delay(90);
  noTone(PIN_BUZZER);
  digitalWrite(PIN_LED_BLUE, LED_OFF);
}

void runSimonGame() {
  Serial.println("\n=== СТАРТ МІНІ-ГРИ САЙМОН ===");
  currentState = STATE_GAME_ACTIVE;
  setCheckpoint(1, "Simon Active");
  rtc_vbat_before_crash = getBatteryVoltage();

  bool prevWifiWasOn = (WiFi.status() == WL_CONNECTED);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(60);

  for (int f = 600; f <= 1600; f += 200) {
    esp_task_wdt_reset();
    tone(PIN_BUZZER, f); delay(40);
  }
  noTone(PIN_BUZZER);
  delay(500);

  const int MAX_LEVEL = 12;
  uint8_t seq[MAX_LEVEL];
  randomSeed(micros());

  for (int i = 0; i < MAX_LEVEL; i++) seq[i] = random(0, 2);

  int level = 1;
  while (level <= MAX_LEVEL) {
    esp_task_wdt_reset();

    for (int i = 0; i < level; i++) {
      esp_task_wdt_reset();
      if (seq[i] == 0) playSignal0();
      else playSignal1();
      delay(200);
    }

    for (int step = 0; step < level; step++) {
      int pressed = -1;
      unsigned long startWait = millis();

      while (pressed == -1) {
        esp_task_wdt_reset();
        if (digitalRead(PIN_BTN_0) == LOW) pressed = 0;
        if (digitalRead(PIN_BTN_1) == LOW) pressed = 1;

        if (millis() - startWait > 6000) pressed = -2;
        delay(10);
      }

      if (pressed == 0) playSignal0();
      else if (pressed == 1) playSignal1();

      while (digitalRead(PIN_BTN_0) == LOW || digitalRead(PIN_BTN_1) == LOW) {
        esp_task_wdt_reset();
        delay(10);
      }
      delay(80);

      if (pressed != seq[step]) {
        digitalWrite(PIN_VIBRO, HIGH);
        tone(PIN_BUZZER, 300); delay(150);
        tone(PIN_BUZZER, 200); delay(250);
        digitalWrite(PIN_VIBRO, LOW);
        tone(PIN_BUZZER, 120); delay(400);
        noTone(PIN_BUZZER);

        currentState = STATE_CONFIG_WEB;
        if (prevWifiWasOn && !isOfflineMode) {
          WiFi.mode(WIFI_STA);
          WiFi.begin(WIFI_SSID, WIFI_PASS);
        }
        return;
      }
    }

    delay(200);
    playRoundSuccessSound();
    delay(700);
    level++;
  }

  tone(PIN_BUZZER, 523); delay(120);
  tone(PIN_BUZZER, 659); delay(120);
  tone(PIN_BUZZER, 784); delay(120);
  tone(PIN_BUZZER, 1046); delay(400);
  noTone(PIN_BUZZER);

  currentState = STATE_CONFIG_WEB;
  if (prevWifiWasOn && !isOfflineMode) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

// ========================================================
// ГРА 2: "ЗЛАМУВАЧ СЕЙФУ" (HAPTIC SAFE CRACKER)
// ========================================================
void runSafeCrackerGame() {
  Serial.println("\n=== СТАРТ ГРИ: ЗЛАМУВАЧ СЕЙФУ ===");
  currentState = STATE_GAME_ACTIVE;
  setCheckpoint(3, "Safe Cracker Active");

  bool prevWifiWasOn = (WiFi.status() == WL_CONNECTED);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(60);

  // Стартовий звук: зведення пружини замка
  digitalWrite(PIN_VIBRO, HIGH);
  for (int f = 300; f <= 1200; f += 150) {
    tone(PIN_BUZZER, f); delay(30);
  }
  digitalWrite(PIN_VIBRO, LOW);
  noTone(PIN_BUZZER);
  delay(400);

  // Генеруємо 3 секретні комбінації від 2 до 14
  randomSeed(micros());
  int secrets[3];
  secrets[0] = random(2, 15);
  secrets[1] = random(2, 15);
  secrets[2] = random(2, 15);

  int currentTumbler = 0;
  int currentPos = 0;
  int lives = 3;

  unsigned long lastMoveTime = millis();
  bool b0_prev = false;
  bool b1_prev = false;

  while (currentTumbler < 3 && lives > 0) {
    esp_task_wdt_reset();

    bool b0 = (digitalRead(PIN_BTN_0) == LOW);
    bool b1 = (digitalRead(PIN_BTN_1) == LOW);
    bool moved = false;

    // Крутимо вліво
    if (b0 && !b0_prev) {
      currentPos--;
      if (currentPos < 0) currentPos = 16;
      moved = true;
    }
    // Крутимо вправо
    if (b1 && !b1_prev) {
      currentPos++;
      if (currentPos > 16) currentPos = 0;
      moved = true;
    }
    b0_prev = b0;
    b1_prev = b1;

    // Відгук на поворот диска
    if (moved) {
      lastMoveTime = millis();
      int dist = abs(currentPos - secrets[currentTumbler]);

      if (dist == 0) {
        // ТОЧНЕ ВЛУЧАННЯ: Штифт потрапив у паз!
        // Подвійний тактильний клац (Тук-Тук) + металевий дзенькіт
        digitalWrite(PIN_LED_BLUE, LED_ON);
        digitalWrite(PIN_VIBRO, HIGH);
        tone(PIN_BUZZER, 2400);
        delay(35);
        digitalWrite(PIN_VIBRO, LOW);
        noTone(PIN_BUZZER);
        delay(25);
        digitalWrite(PIN_VIBRO, HIGH);
        tone(PIN_BUZZER, 2800);
        delay(35);
        digitalWrite(PIN_VIBRO, LOW);
        digitalWrite(PIN_LED_BLUE, LED_OFF);
        noTone(PIN_BUZZER);
        Serial.printf("[СЕЙФ] Диск #%d: ВЛУЧАННЯ В ПАЗ (%d)!\n", currentTumbler + 1, currentPos);
      } 
      else if (dist <= 2) {
        // Близько (1-2 кроки): дзвінкіший клік
        tone(PIN_BUZZER, 1200); delay(15); noTone(PIN_BUZZER);
      } 
      else {
        // Звичайний зуб шестерні
        tone(PIN_BUZZER, 550); delay(12); noTone(PIN_BUZZER);
      }
    }

    // Якщо гравець завмер на 1.3 секунди — замок робить авто-перевірку позиції
    if (millis() - lastMoveTime > 1300) {
      if (currentPos == secrets[currentTumbler]) {
        // УСПІХ: Штифт зафіксовано в циліндрі!
        Serial.printf("[СЕЙФ] Диск #%d ВІДЧИНЕНО!\n", currentTumbler + 1);
        digitalWrite(PIN_VIBRO, HIGH);
        digitalWrite(PIN_LED_BLUE, LED_ON);
        tone(PIN_BUZZER, 1500); delay(70);
        tone(PIN_BUZZER, 2200); delay(120);
        digitalWrite(PIN_VIBRO, LOW);
        digitalWrite(PIN_LED_BLUE, LED_OFF);
        noTone(PIN_BUZZER);
        delay(300);

        currentTumbler++;
        currentPos = 0; // Скидаємо позицію для наступного диска
        lastMoveTime = millis();
      } else {
        // ПОМИЛКА: Зупинився на хибній позиції
        lives--;
        Serial.printf("[СЕЙФ] Зрив штифта! Залишилось спроб: %d\n", lives);
        tone(PIN_BUZZER, 300); delay(150);
        tone(PIN_BUZZER, 180); delay(250);
        noTone(PIN_BUZZER);
        lastMoveTime = millis(); // Даємо час спробувати знову
      }
    }
    delay(5);
  }

  // Фінал гри
  if (currentTumbler == 3) {
    // ВЕСЬ СЕЙФ ВІДЧИНЕНО!
    Serial.println("[СЕЙФ] ПОВНА ПЕРЕМОГА! СЕЙФ ЗЛАМАНО!");
    digitalWrite(PIN_VIBRO, HIGH);
    tone(PIN_BUZZER, 523); delay(120);
    tone(PIN_BUZZER, 659); delay(120);
    tone(PIN_BUZZER, 784); delay(120);
    tone(PIN_BUZZER, 1046); delay(400);
    digitalWrite(PIN_VIBRO, LOW);
    noTone(PIN_BUZZER);
  } else {
    // СИГНАЛІЗАЦІЯ СЕЙФУ (ПРОГРАШ)
    Serial.println("[СЕЙФ] Спрацювала сигналізація!");
    for (int i = 0; i < 3; i++) {
      digitalWrite(PIN_VIBRO, HIGH);
      digitalWrite(PIN_LED_BLUE, LED_ON);
      tone(PIN_BUZZER, 3500); delay(90);
      digitalWrite(PIN_VIBRO, LOW);
      digitalWrite(PIN_LED_BLUE, LED_OFF);
      tone(PIN_BUZZER, 2200); delay(90);
    }
    noTone(PIN_BUZZER);
  }

  currentState = STATE_CONFIG_WEB;
  if (prevWifiWasOn && !isOfflineMode) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

// --- HTML СТОРІНКА ---
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Smart Alarm</title>
  <style>
    body { font-family: -apple-system, sans-serif; background: #121212; color: #fff; text-align: center; padding: 15px; margin: 0; }
    .card { background: #1e1e1e; border-radius: 14px; padding: 20px; max-width: 380px; margin: 15px auto; box-shadow: 0 4px 20px rgba(0,0,0,0.6); }
    h2 { color: #ff9800; margin-bottom: 5px; }
    h3 { color: #03a9f4; margin: 15px 0 10px 0; font-size: 16px; border-top: 1px solid #333; padding-top: 15px; }
    .stat-row { display: flex; justify-content: space-around; margin: 15px 0; background: #282828; padding: 10px; border-radius: 8px; }
    .stat-val { font-size: 18px; font-weight: bold; color: #4caf50; }
    .progress-bar { background: #444; border-radius: 6px; height: 10px; overflow: hidden; margin: 8px auto; width: 90%; }
    .progress-fill { background: #4caf50; height: 100%; width: %BATTERY_PCT%%; }
    input { width: 85%; padding: 12px; margin: 8px 0; border-radius: 8px; border: 1px solid #444; background: #2a2a2a; color: #fff; font-size: 18px; text-align: center; }
    .btn { width: 90%; background: #ff9800; border: none; color: #000; font-size: 17px; font-weight: bold; padding: 14px; border-radius: 8px; cursor: pointer; margin-top: 15px; }
    .btn-test { background: #03a9f4; margin-top: 10px; }
    .btn-fun { width: 45%; padding: 10px; margin: 4px; font-size: 13px; font-weight: bold; border-radius: 6px; border: 1px solid #555; background: #2c2c2c; color: #eee; cursor: pointer; }
    .fun-grid { display: flex; flex-wrap: wrap; justify-content: center; margin-top: 8px; }
    .info { font-size: 13px; color: #aaa; margin: 5px 0 15px 0; }
    .diag-box { background: #161616; border: 1px solid #333; border-radius: 8px; padding: 10px; margin-top: 10px; text-align: left; font-family: monospace; font-size: 12px; color: #ffb74d; }
  </style>
</head>
<body>
  <div class="card">
    <h2>Розумний Будильник</h2>

    <div class="stat-row">
      <div>
        <div style="font-size: 12px; color: #aaa;">АКУМУЛЯТОР</div>
        <div class="stat-val">%BATTERY_PCT%% <span style="font-size: 13px; color: #888;">(%BATTERY_VOLT%V)</span></div>
      </div>
      <div>
        <div style="font-size: 12px; color: #aaa;">СИГНАЛ WI-FI</div>
        <div class="stat-val" style="color: #ff9800;">%CURRENT_RSSI% dBm</div>
      </div>
    </div>
    <div class="progress-bar"><div class="progress-fill"></div></div>

    <form action="/save" method="POST">
      <label>Час дзвінка:</label><br>
      <input type="time" name="time" value="%TIME_VAL%" required><br>
      
      <label>Поріг вимкнення (dBm):</label><br>
      <input type="number" name="rssi" value="%RSSI_VAL%" min="-95" max="-40" required>
      <div class="info">(-50: біля ліжка, -80...-88: кінець коридору)</div>
      
      <button class="btn" type="submit">Зберегти та Заснути</button>
    </form>

    <form action="/test" method="POST">
      <button class="btn btn-test" type="submit">Тест: Заснути на 15 сек</button>
    </form>

    <h3>🎮 Розваги та Тести</h3>
    <div class="fun-grid">
      <button class="btn-fun" onclick="fetch('/play?t=rick')">🕺 Rickroll</button>
      <button class="btn-fun" onclick="fetch('/play?t=sw')">⚔️ Star Wars</button>
      <button class="btn-fun" onclick="fetch('/play?t=tetris')">🕹️ Тетріс</button>
      <button class="btn-fun" onclick="fetch('/play?t=mega')">💀 Megalovania</button>
    </div>

    <h3>📋 Діагностика / Чорна скринька</h3>
    <div class="diag-box">
      %CRASH_REPORT%
    </div>
  </div>
</body>
</html>
)rawliteral";

void enterSleep(uint64_t seconds) {
  setCheckpoint(90, "Entering Sleep");
  uint64_t calibratedSeconds = (uint64_t)(seconds * RTC_CALIBRATION_FACTOR);

  Serial.printf("\n[DEEP SLEEP] Задано: %llu сек | Калібровано: %llu сек\n", seconds, calibratedSeconds);
  Serial.flush();

  digitalWrite(PIN_VIBRO, LOW);
  digitalWrite(PIN_LED_BLUE, LED_OFF);
  noTone(PIN_BUZZER);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  gpio_pullup_en((gpio_num_t)PIN_BTN_0);
  gpio_pulldown_dis((gpio_num_t)PIN_BTN_0);
  gpio_pullup_en((gpio_num_t)PIN_BTN_1);
  gpio_pulldown_dis((gpio_num_t)PIN_BTN_1);
  esp_deep_sleep_enable_gpio_wakeup((1ULL << PIN_BTN_0) | (1ULL << PIN_BTN_1), ESP_GPIO_WAKEUP_GPIO_LOW);

  esp_sleep_enable_timer_wakeup(calibratedSeconds * 1000000ULL);
  esp_deep_sleep_start();
}

void sleepUntilAlarmTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 3000)) {
    enterSleep(60);
    return;
  }

  long nowSec = timeinfo.tm_hour * 3600 + timeinfo.tm_min * 60 + timeinfo.tm_sec;
  long targetSec = alarmHour * 3600 + alarmMinute * 60;
  long diffSec = targetSec - nowSec;

  if (diffSec <= 10) diffSec += 24 * 3600;

  Serial.printf("Зараз %02d:%02d:%02d. Дзвінок о %02d:%02d. Спати: %ld сек.\n",
                timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                alarmHour, alarmMinute, diffSec);

  enterSleep(diffSec);
}

void startWiFiAndServer() {
  Serial.println("\n[WI-FI] Спроба підключення...");
  digitalWrite(PIN_LED_BLUE, LED_ON);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long startWifiWait = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startWifiWait < 6000)) {
    esp_task_wdt_reset();
    delay(200);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    isOfflineMode = false;
    Serial.print("\nWi-Fi Підключено! IP: http://");
    Serial.println(WiFi.localIP());
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET, NTP_SERVER);
  } else {
    isOfflineMode = true;
    Serial.println("\n[WI-FI] Мережу не знайдено. Автономний режим.");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    digitalWrite(PIN_LED_BLUE, LED_OFF);
  }

  server.on("/", []() {
    float vBat = getBatteryVoltage();
    int pctBat = getBatteryPercentage(vBat);

    String html = HTML_PAGE;
    char timeBuf[6];
    sprintf(timeBuf, "%02d:%02d", alarmHour, alarmMinute);
    html.replace("%TIME_VAL%", timeBuf);
    html.replace("%RSSI_VAL%", String(rssiThreshold));
    html.replace("%CURRENT_RSSI%", String(WiFi.RSSI()));
    html.replace("%BATTERY_PCT%", String(pctBat));
    html.replace("%BATTERY_VOLT%", String(vBat, 2));
    html.replace("%CRASH_REPORT%", crashReportWeb);
    server.send(200, "text/html", html);
  });

  server.on("/save", HTTP_POST, []() {
    if (server.hasArg("time") && server.hasArg("rssi")) {
      String t = server.arg("time");
      alarmHour = t.substring(0, 2).toInt();
      alarmMinute = t.substring(3, 5).toInt();
      rssiThreshold = server.arg("rssi").toInt();

      prefs.begin("alarm", false);
      prefs.putInt("hour", alarmHour);
      prefs.putInt("min", alarmMinute);
      prefs.putInt("rssi", rssiThreshold);
      prefs.end();

      digitalWrite(PIN_LED_BLUE, LED_ON);
      tone(PIN_BUZZER, 2000); delay(100);
      tone(PIN_BUZZER, 3000); delay(150);
      noTone(PIN_BUZZER);
      digitalWrite(PIN_LED_BLUE, LED_OFF);

      server.send(200, "text/html", "<h1>Збережено!</h1><p>Будильник о " + t + "</p>");
      delay(1000);
      sleepUntilAlarmTime();
    }
  });

  server.on("/test", HTTP_POST, []() {
    server.send(200, "text/html", "<h1>Тест: сон 15 сек</h1>");
    delay(1000);
    enterSleep(15);
  });

  server.on("/play", HTTP_GET, []() {
    if (server.hasArg("t")) {
      String track = server.arg("t");
      server.send(200, "text/plain", "OK");

      if (track == "rick") playRickroll();
      else if (track == "sw") playImperialMarch();
      else if (track == "tetris") playTetris();
      else if (track == "mega") playMegalovania();
    } else {
      server.send(400, "text/plain", "Bad Request");
    }
  });

  if (!isOfflineMode) server.begin();
  currentState = STATE_CONFIG_WEB;
}

void toggleWifi() {
  if (!isOfflineMode && wifiConnected) {
    Serial.println("\n[WI-FI] Вимикаємо Wi-Fi (Офлайн-режим)");
    server.stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    isOfflineMode = true;
    wifiConnected = false;

    digitalWrite(PIN_VIBRO, HIGH);
    tone(PIN_BUZZER, 1200); delay(80);
    digitalWrite(PIN_VIBRO, LOW);
    tone(PIN_BUZZER, 600);  delay(100);
    tone(PIN_BUZZER, 250);  delay(180);
    noTone(PIN_BUZZER);

    for (int i = 0; i < 3; i++) {
      digitalWrite(PIN_LED_BLUE, LED_ON); delay(50);
      digitalWrite(PIN_LED_BLUE, LED_OFF); delay(50);
    }
  } else {
    Serial.println("\n[WI-FI] Вмикаємо Wi-Fi");
    isOfflineMode = false;

    digitalWrite(PIN_LED_BLUE, LED_ON);
    digitalWrite(PIN_VIBRO, HIGH);
    tone(PIN_BUZZER, 784); delay(80);
    digitalWrite(PIN_VIBRO, LOW);
    tone(PIN_BUZZER, 1046); delay(80);
    tone(PIN_BUZZER, 1568); delay(180);
    noTone(PIN_BUZZER);

    startWiFiAndServer();
  }
}

void playAlarmSound(unsigned long now) {
  static unsigned long lastStep = 0;
  static uint8_t phase = 0;

  if (phase == 0 && now - lastStep >= 120) {
    lastStep = now;
    phase = 1;
    digitalWrite(PIN_VIBRO, HIGH);
    digitalWrite(PIN_LED_BLUE, LED_ON);
    tone(PIN_BUZZER, 3800);
  } 
  else if (phase == 1 && now - lastStep >= 240) {
    lastStep = now;
    phase = 0;
    digitalWrite(PIN_VIBRO, LOW);
    digitalWrite(PIN_LED_BLUE, LED_OFF);
    tone(PIN_BUZZER, 2500);
  }
}

bool isFarEnough() {
  if (isOfflineMode || wifiLostPermanently || !wifiConnected) return true;
  if (currentRssi <= rssiThreshold && currentRssi != 0) return true;
  return false;
}

void triggerSuccessDismiss() {
  Serial.println(">>> УСПІХ! Будильник вимкнено! <<<");
  digitalWrite(PIN_VIBRO, LOW);
  noTone(PIN_BUZZER);

  digitalWrite(PIN_LED_BLUE, LED_ON);
  tone(PIN_BUZZER, 1760); delay(100);
  tone(PIN_BUZZER, 2200); delay(120);
  tone(PIN_BUZZER, 2793); delay(300);
  noTone(PIN_BUZZER);
  digitalWrite(PIN_LED_BLUE, LED_OFF);

  sleepUntilAlarmTime();
}

void triggerRefusal() {
  Serial.println(">>> ЗАБЛИЗЬКО! Йди далі! <<<");
  digitalWrite(PIN_VIBRO, LOW);
  for (int i = 0; i < 2; i++) {
    digitalWrite(PIN_LED_BLUE, LED_ON);
    tone(PIN_BUZZER, 350); delay(80);
    digitalWrite(PIN_LED_BLUE, LED_OFF);
    noTone(PIN_BUZZER); delay(40);
  }
  currentState = STATE_RINGING;
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_BTN_0, INPUT_PULLUP);
  pinMode(PIN_BTN_1, INPUT_PULLUP);
  pinMode(PIN_BATTERY, INPUT);
  pinMode(PIN_VIBRO, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);

  digitalWrite(PIN_VIBRO, LOW);
  digitalWrite(PIN_LED_BLUE, LED_OFF);
  noTone(PIN_BUZZER);

  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = WDT_TIMEOUT_SECONDS * 1000,
    .idle_core_mask = (1 << 0),
    .trigger_panic = true
  };
  esp_task_wdt_reconfigure(&twdt_config);
  esp_task_wdt_add(NULL);

  esp_reset_reason_t rst_reason = esp_reset_reason();
  const char* reasonStr = "Звичайний запуск";

  switch (rst_reason) {
    case ESP_RST_POWERON:   reasonStr = "Холодне підключення (Power On)"; break;
    case ESP_RST_SW:        reasonStr = "Програмний ребут"; break;
    case ESP_RST_PANIC:     reasonStr = "КРАШ: Exception / Panic"; break;
    case ESP_RST_INT_WDT:   reasonStr = "КРАШ: Interrupt Watchdog"; break;
    case ESP_RST_TASK_WDT:  reasonStr = "КРАШ: Task Watchdog"; break;
    case ESP_RST_DEEPSLEEP: reasonStr = "Вихід із Deep Sleep"; break;
    case ESP_RST_BROWNOUT:  reasonStr = "КРАШ: BROWNOUT (Просідання живлення!)"; break;
    default:                reasonStr = "Інша причина"; break;
  }

  char buf[256];
  snprintf(buf, sizeof(buf), 
           "Статус: %s<br>Остання дія: [%d] %s<br>VBat перед крашем: %.2fV",
           reasonStr, rtc_crash_checkpoint, rtc_last_action, rtc_vbat_before_crash);
  crashReportWeb = String(buf);
  Serial.printf("\n[DIAGNOSTICS] %s | Checkpoint: %d | Action: %s\n", 
                reasonStr, rtc_crash_checkpoint, rtc_last_action);

  prefs.begin("alarm", true);
  alarmHour = prefs.getInt("hour", 7);
  alarmMinute = prefs.getInt("min", 30);
  rssiThreshold = prefs.getInt("rssi", -82);
  prefs.end();

  WiFi.onEvent(WiFiEvent);

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("\n>>> [ТАЙМЕР] ПІДЙОМ! ТРИВОГА! <<<");
    currentState = STATE_RINGING;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.setTxPower(WIFI_POWER_11dBm);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    delay(200);
    return;
  }

  if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO) {
    Serial.println("\n[КНОПКА] Прокидання. Перевірка утримання обох кнопок...");

    unsigned long waitStart = millis();
    while (millis() - waitStart < 400) {
      if (digitalRead(PIN_BTN_0) == LOW && digitalRead(PIN_BTN_1) == LOW) break;
      delay(10);
    }

    bool bothHeld = true;
    for (int i = 0; i < 8; i++) {
      esp_task_wdt_reset();
      if (digitalRead(PIN_BTN_0) != LOW || digitalRead(PIN_BTN_1) != LOW) {
        bothHeld = false;
        digitalWrite(PIN_VIBRO, LOW);
        digitalWrite(PIN_LED_BLUE, LED_OFF);
        noTone(PIN_BUZZER);
        break;
      }

      int vibroTime = 25 + (i * 6);
      int toneTime  = 45;

      digitalWrite(PIN_LED_BLUE, LED_ON);
      digitalWrite(PIN_VIBRO, HIGH);
      tone(PIN_BUZZER, 1800 + (i * 200));

      if (vibroTime <= toneTime) {
        delay(vibroTime);
        digitalWrite(PIN_VIBRO, LOW);
        delay(toneTime - vibroTime);
        noTone(PIN_BUZZER);
      } else {
        delay(toneTime);
        noTone(PIN_BUZZER);
        delay(vibroTime - toneTime);
        digitalWrite(PIN_VIBRO, LOW);
      }

      digitalWrite(PIN_LED_BLUE, LED_OFF);
      delay(55);
    }

    if (bothHeld) {
      digitalWrite(PIN_VIBRO, HIGH);
      digitalWrite(PIN_LED_BLUE, LED_ON);
      tone(PIN_BUZZER, 1318); delay(100);
      digitalWrite(PIN_VIBRO, LOW);
      tone(PIN_BUZZER, 1760); delay(120);
      tone(PIN_BUZZER, 2637); delay(250);
      noTone(PIN_BUZZER);
      digitalWrite(PIN_LED_BLUE, LED_OFF);

      startWiFiAndServer();
      return;
    } else {
      digitalWrite(PIN_VIBRO, LOW);
      tone(PIN_BUZZER, 800); delay(80);
      tone(PIN_BUZZER, 400); delay(120);
      noTone(PIN_BUZZER);

      sleepUntilAlarmTime();
      return;
    }
  }

  playPowerOnJingle();
  startWiFiAndServer();
}

void loop() {
  esp_task_wdt_reset();
  unsigned long now = millis();

  // 1. ГОЛОВНИЙ РЕЖИМ (ВЕБ АБО ОФЛАЙН)
  if (currentState == STATE_CONFIG_WEB) {
    if (!isOfflineMode && wifiConnected) {
      server.handleClient();
    }

    static unsigned long lastBlink = 0;
    static bool ledState = false;
    if (!isOfflineMode && wifiConnected) {
      if (now - lastBlink > 1500) {
        lastBlink = now;
        ledState = !ledState;
        digitalWrite(PIN_LED_BLUE, ledState ? LED_ON : LED_OFF);
      }
    } else {
      digitalWrite(PIN_LED_BLUE, LED_OFF);
    }

    // ========================================================
    // ДВИГУН КНОПОК
    // ========================================================
    bool b0 = (digitalRead(PIN_BTN_0) == LOW);
    bool b1 = (digitalRead(PIN_BTN_1) == LOW);

    static bool b0_prev = false;
    static bool b1_prev = false;
    static unsigned long b0_press_time = 0;
    static unsigned long b1_press_time = 0;

    static int shift_count = 0;
    static bool shift_mode_active = false;

    static int b1_clicks = 0;
    static unsigned long b1_last_click_time = 0;
    static bool b1_long_press_handled = false;

    // --- А. ЗАТИСКАННЯ SHIFT (КНОПКА 0) ---
    if (b0 && !b0_prev) {
      b0_press_time = now;
      shift_count = 0;
      shift_mode_active = true;
    }

    // Поки Кнопка 0 затиснута — кліки Кнопки 1 перемикають трек
    if (b0 && shift_mode_active) {
      if (b1 && !b1_prev) {
        shift_count++;
        digitalWrite(PIN_VIBRO, HIGH);
        digitalWrite(PIN_LED_BLUE, LED_ON);
        tone(PIN_BUZZER, 700 + (shift_count * 300));
        delay(35);
        digitalWrite(PIN_VIBRO, LOW);
        digitalWrite(PIN_LED_BLUE, LED_OFF);
        noTone(PIN_BUZZER);
        Serial.printf("[SHIFT] Обрано трек: %d\n", shift_count);
      }
    }

    // Відпускання Кнопки 0 -> Запуск вибраного треку або озвучка заряду
    if (!b0 && b0_prev) {
      shift_mode_active = false;
      unsigned long press_dur = now - b0_press_time;

      if (shift_count == 0) {
        if (press_dur < 500) {
          speakBatteryLevel();
        }
      } else {
        Serial.printf("[SHIFT] Запуск треку #%d\n", shift_count);
        if (shift_count == 1)      playRickroll();
        else if (shift_count == 2) playImperialMarch();
        else if (shift_count == 3) playTetris();
        else                       playMegalovania();
      }
      shift_count = 0;
    }
    b0_prev = b0;

    // --- Б. НЕЗАЛЕЖНА КНОПКА 1 (КОЛИ КНОПКА 0 ВІДПУЩЕНА) ---
    if (!b0 && !shift_mode_active) {
      if (b1 && !b1_prev) {
        b1_press_time = now;
        b1_long_press_handled = false;
      }

      // Затискання Кнопки 1 > 1.2 с -> Гра Саймон
      if (b1 && !b1_long_press_handled) {
        if (now - b1_press_time > 1200) {
          b1_long_press_handled = true;
          b1_clicks = 0;
          runSimonGame();
        }
      }

      // Відпускання Кнопки 1
      if (!b1 && b1_prev) {
        if (!b1_long_press_handled && (now - b1_press_time < 450)) {
          b1_clicks++;
          b1_last_click_time = now;
        }
      }

      // Обробка серії кліків Кнопки 1 (таймаут 320 мс)
      if (b1_clicks > 0 && (now - b1_last_click_time > 320)) {
        if (b1_clicks == 1) {
          // 1 клік -> Короткий тактильний вібротест
          digitalWrite(PIN_VIBRO, HIGH);
          digitalWrite(PIN_LED_BLUE, LED_ON);
          delay(100);
          digitalWrite(PIN_VIBRO, LOW);
          digitalWrite(PIN_LED_BLUE, LED_OFF);
        } else if (b1_clicks == 2) {
          // 2 кліки -> ГРА 2: ЗЛАМУВАЧ СЕЙФУ
          runSafeCrackerGame();
        } else if (b1_clicks >= 3) {
          // 3 кліки -> ПЕРЕМИКАННЯ WI-FI (ON / OFF)
          toggleWifi();
        }
        b1_clicks = 0;
      }
    }
    b1_prev = b1;

    delay(2);
    return;
  }

  // Фонове зчитування RSSI
  if (wifiConnected && (now - lastRssiCheck > 400)) {
    lastRssiCheck = now;
    currentRssi = WiFi.RSSI();
  }

  int btn0 = digitalRead(PIN_BTN_0);

  // 2. РЕЖИМ ДЗВІНКА
  if (currentState == STATE_RINGING) {
    playAlarmSound(now);

    if (btn0 == LOW) {
      digitalWrite(PIN_VIBRO, LOW);
      digitalWrite(PIN_LED_BLUE, LED_OFF);
      noTone(PIN_BUZZER);
      Serial.println("\n[ТИХИЙ РЕЖИМ] Іди в кінець коридору...");
      currentState = STATE_MUTED_WALK;
      delay(150);
      return;
    }
  }

  // 3. ТИХИЙ РЕЖИМ ХОДЬБИ
  else if (currentState == STATE_MUTED_WALK) {
    static unsigned long lastTick = 0;
    if (now - lastTick > 600) {
      lastTick = now;
      digitalWrite(PIN_LED_BLUE, LED_ON);
      tone(PIN_BUZZER, 1200); delay(20);
      noTone(PIN_BUZZER);
      digitalWrite(PIN_LED_BLUE, LED_OFF);

      Serial.printf("Статус: %s | RSSI: %d dBm (Ціль <= %d)\n",
                    (isOfflineMode || wifiLostPermanently) ? "ОБІРВАНО" : "Підключено",
                    currentRssi, rssiThreshold);
    }

    if (btn0 == HIGH) {
      Serial.println("Кнопку відпущено!");
      if (isFarEnough()) {
        triggerSuccessDismiss();
      } else {
        triggerRefusal();
      }
    }
  }

  delay(2);
}
