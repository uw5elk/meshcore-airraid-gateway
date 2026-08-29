#include "AirRaidGateway.h"

#if defined(ESP32) && defined(WITH_AIR_RAID_GATEWAY)

#include "AirRaidGatewayConfig.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Utils.h>
#include <helpers/TxtDataHelpers.h>
#include "ui-new/UITask.h"
#include <time.h>

#define ALERT_POLL_INTERVAL_MS     15000    // жорсткий ліміт alerts.in.ua: 12 запитів/хв
#define ALERT_BACKOFF_MAX_MS      300000    // стеля відступу при 429
#define ALERT_HTTP_TIMEOUT_MS       5000
#define ALERT_WIFI_RETRY_MS        10000
#define ALERT_WIFI_DOWN_IDLE_MS      500    // сон задачі між спробами перепідключення, поки WiFi лежить
#define ALERT_POLL_IDLE_MS           200    // сон задачі між перевірками "ще не час"
#define ALERT_POLL_TASK_STACK      10240    // байти; розмір підібрано за логами uxTaskGetStackHighWaterMark() нижче
#define ALERT_POLL_TASK_CORE           0    // задача драйвера WiFi уже на ядрі 0; ядро 1 лишаємо вільним для loopTask
#define ALERT_STACK_LOG_EVERY_N_POLLS 20    // як рідко писати в MESH_DEBUG запас стека

// Усе, що менше за це значення, означає, що NTP ще не синхронізувався (одразу
// після старту система віддає неправдоподібну епоху) - ніколи не друкуємо
// фальшивий час у повідомленні про тривогу.
#define NTP_READY_EPOCH_THRESHOLD  1700000000UL   // ~2023-11-14 UTC

// Значення CMD_SEND_CHANNEL_TXT_MSG (приватний #define у MyMesh.cpp:8, назовні
// через MyMesh.h не виставлений) - синхронізуємо вручну, бо injectChannelText()
// міняти не можна.
#define CMD_SEND_CHANNEL_TXT_MSG_VAL   3
#define TXT_TYPE_PLAIN_VAL             0

// Індекс 0 - завжди "Public" (його додає MyMesh::begin() при кожному старті).
// Слот 1 займаємо під власний канал тривог.
#define AIR_RAID_CHANNEL_SLOT          1

void AirRaidGateway::begin(MyMesh* mesh, UITask* ui) {
  _mesh = mesh;
  _ui = ui;
  _state = STATE_UNKNOWN;
  _poll_interval_ms = ALERT_POLL_INTERVAL_MS;
  _next_poll_at = millis();  // опитати одразу, щойно підніметься WiFi
  registerChannel();

  // Одноразовий неблокуючий старт із головного потоку. Від цього моменту
  // WiFi.begin()/.disconnect()/.status() викликаються виключно з фонової задачі
  // опитування (див. pollTaskLoop()) - життєвим циклом WiFi-з'єднання володіє
  // рівно один потік.
  WiFi.mode(WIFI_STA);
  WiFi.begin(GW_WIFI_SSID, GW_WIFI_PASS);
  MESH_DEBUG_PRINTLN("AirRaidGateway: connecting to WiFi '%s'...", GW_WIFI_SSID);

  // Київський місцевий час (EET/EEST) для міток часу в повідомленнях про
  // тривогу, через NTP по власному WiFi - незалежно від годинника mesh-мережі.
  // getRTCClock() лишається в UTC, синхронізується з advert-пакетів і надалі
  // використовується тільки для мітки часу в кадрі на дроті.
  configTzTime("EET-2EEST,M3.5.0/1,M10.5.0/1", "pool.ntp.org", "time.google.com");

  if (_poll_task == NULL) {
    _result_queue = xQueueCreate(1, sizeof(PollSnapshot));
    xTaskCreatePinnedToCore(pollTaskTrampoline, "AirRaidPoll", ALERT_POLL_TASK_STACK,
                            this, 1, &_poll_task, ALERT_POLL_TASK_CORE);
  }
}

long AirRaidGateway::secondsSinceLastSuccess() const {
  if (_last_success_at == 0) return -1;
  return (long)((millis() - _last_success_at) / 1000);
}

uint8_t AirRaidGateway::getPollTaskStackPercentFree() const {
  return _poll_task ? (uint8_t)(getPollTaskStackBytesFree() * 100 / ALERT_POLL_TASK_STACK) : 0;
}

void AirRaidGateway::registerChannel() {
  uint8_t psk[16];
  if (!mesh::Utils::fromHex(psk, sizeof(psk), CHANNEL_PSK_HEX)) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: CHANNEL_PSK_HEX must be exactly 32 hex chars - falling back to Public channel");
    _channel_idx = 0;
    return;
  }

  ChannelDetails desired;
  memset(&desired, 0, sizeof(desired));
  StrHelper::strncpy(desired.name, CHANNEL_NAME, sizeof(desired.name));
  memcpy(desired.channel.secret, psk, sizeof(psk));  // решта байтів лишаються 0 -> 128-бітний ключ

  ChannelDetails existing;
  bool already_registered = _mesh->getChannel(AIR_RAID_CHANNEL_SLOT, existing)
    && strncmp(existing.name, desired.name, sizeof(desired.name)) == 0
    && memcmp(existing.channel.secret, desired.channel.secret, sizeof(desired.channel.secret)) == 0;

  if (already_registered) {
    _channel_idx = AIR_RAID_CHANNEL_SLOT;
    return;
  }

  if (_mesh->setChannel(AIR_RAID_CHANNEL_SLOT, desired)) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: registered channel '%s' at idx %d", CHANNEL_NAME, AIR_RAID_CHANNEL_SLOT);
    _channel_idx = AIR_RAID_CHANNEL_SLOT;
  } else {
    MESH_DEBUG_PRINTLN("AirRaidGateway: setChannel(%d) failed - falling back to Public channel", AIR_RAID_CHANNEL_SLOT);
    _channel_idx = 0;
  }
}

// ---- Бік головного потоку: вичитує скриньку і робить усі дії з Mesh/UI ----

void AirRaidGateway::loop() {
  if (_result_queue == NULL) return;

  PollSnapshot snap;
  if (xQueueReceive(_result_queue, &snap, 0) != pdTRUE) return;   // нічого нового - не блокуємось

  _wifi_connected_cached = snap.wifi_connected;
  if (snap.has_http_result) {
    _last_http_code = snap.http_code;
    if (snap.success) _last_success_at = millis();
  }
  if (snap.has_state) handleState(snap.state);
}

void AirRaidGateway::handleState(AlertState new_state) {
  if (_state == STATE_UNKNOWN) {
    _state = new_state;  // мовчки встановлюємо базовий стан, при старті не шлемо нічого
    MESH_DEBUG_PRINTLN("AirRaidGateway: baseline = %s", new_state == STATE_ALERT ? "ALERT" : "CLEAR");
    return;
  }
  if (new_state == _state) return;  // стан не змінився -> нічого не шлемо

  _state = new_state;

  if (_ui != NULL) {
    _ui->wakeDisplay();
    if (new_state == STATE_ALERT) {
      _ui->showAlert("TRYVOGA", 5000);
    } else {
      _ui->showAlert("VIDBIY", 5000);
    }
  }

  char msg[96];
  time_t t = time(nullptr);  // системний час: із NTP, уже київський завдяки configTzTime()
  if (t < NTP_READY_EPOCH_THRESHOLD) {
    // Щойно стартували, NTP ще не синхронізувався - фальшивий час не друкуємо.
    MESH_DEBUG_PRINTLN("AirRaidGateway: NTP not synced yet, sending alert without a timestamp");
    if (new_state == STATE_ALERT) {
      snprintf(msg, sizeof(msg), "\xF0\x9F\x94\xB4 ПОВІТРЯНА ТРИВОГА — %s", REGION_NAME);
    } else {
      snprintf(msg, sizeof(msg), "\xF0\x9F\x9F\xA2 Відбій — %s", REGION_NAME);
    }
  } else {
    struct tm lt;
    localtime_r(&t, &lt);
    if (new_state == STATE_ALERT) {
      snprintf(msg, sizeof(msg), "\xF0\x9F\x94\xB4 ПОВІТРЯНА ТРИВОГА — %s %02d:%02d", REGION_NAME, lt.tm_hour, lt.tm_min);
    } else {
      snprintf(msg, sizeof(msg), "\xF0\x9F\x9F\xA2 Відбій — %s %02d:%02d", REGION_NAME, lt.tm_hour, lt.tm_min);
    }
  }
  sendChannelText(msg);
}

void AirRaidGateway::sendChannelText(const char* text) {
  uint8_t frame[MAX_FRAME_SIZE];
  int i = 0;
  frame[i++] = CMD_SEND_CHANNEL_TXT_MSG_VAL;
  frame[i++] = TXT_TYPE_PLAIN_VAL;
  frame[i++] = _channel_idx;

  uint32_t ts = _mesh->getRTCClock()->getCurrentTime();
  memcpy(&frame[i], &ts, 4);
  i += 4;

  size_t text_len = strlen(text);
  size_t max_text = sizeof(frame) - i;
  if (text_len > max_text) text_len = max_text;
  memcpy(&frame[i], text, text_len);
  i += text_len;

  _mesh->injectChannelText(frame, i);
}

// ---- Бік фонової задачі: тільки WiFi + HTTP, _mesh/_ui не чіпає ніколи ----

void AirRaidGateway::pollTaskTrampoline(void* param) {
  static_cast<AirRaidGateway*>(param)->pollTaskLoop();
}

void AirRaidGateway::pollTaskLoop() {
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      if (millis() - _last_wifi_reconnect_attempt > ALERT_WIFI_RETRY_MS) {
        MESH_DEBUG_PRINTLN("AirRaidGateway: WiFi down, reconnecting...");
        WiFi.disconnect();
        WiFi.begin(GW_WIFI_SSID, GW_WIFI_PASS);
        _last_wifi_reconnect_attempt = millis();
      }
      PollSnapshot snap = { false, STATE_UNKNOWN, false, false, 0, false };
      xQueueOverwrite(_result_queue, &snap);
      vTaskDelay(pdMS_TO_TICKS(ALERT_WIFI_DOWN_IDLE_MS));
      continue;
    }

    if ((long)(millis() - _next_poll_at) < 0) {
      vTaskDelay(pdMS_TO_TICKS(ALERT_POLL_IDLE_MS));
      continue;
    }
    _next_poll_at = millis() + _poll_interval_ms;

    pollOnce();
  }
}

void AirRaidGateway::pollOnce() {
  PollSnapshot snap = { false, STATE_UNKNOWN, true, false, 0, true };

  WiFiClientSecure client;
  client.setInsecure();   // TODO(v2): закріпити/перевіряти сертифікат alerts.in.ua

  HTTPClient http;
  http.setConnectTimeout(ALERT_HTTP_TIMEOUT_MS);
  http.setTimeout(ALERT_HTTP_TIMEOUT_MS);

  static const char* url = "https://api.alerts.in.ua/v1/iot/active_air_raid_alerts.json";
  if (!http.begin(client, url)) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: http.begin() failed");
    snap.http_code = -1;
    xQueueOverwrite(_result_queue, &snap);
    return;
  }
  http.addHeader("Authorization", "Bearer " ALERTS_TOKEN);

  int code = http.GET();
  snap.http_code = code;

  if (code == 200) {
    String body = http.getString();
    _poll_interval_ms = ALERT_POLL_INTERVAL_MS;  // скидаємо відступ, якщо він був
    snap.success = true;

    // Тіло - це рядковий літерал JSON, напр. "   NNN...A...N" - знімаємо лапки навколо
    // (якщо вони є), щоб індекс 0 вмісту збігався з UID 0. Звірено з живими даними:
    // UID 9 (Дніпропетровська обл.) і UID 279 (Кривий Ріг) обидва збіглися з відомим
    // на той момент станом саме за цим 0-based зміщенням.
    int start = 0;
    int content_len = (int)body.length();
    if (content_len >= 2 && body[0] == '"' && body[content_len - 1] == '"') {
      start = 1;
      content_len -= 2;
    }

    if (content_len <= ALERTS_UID) {
      // Обрізана/закоротка/несподівана відповідь - стан НЕ повідомляємо, щоб головний
      // потік не сприйняв це за хибний відбій (чи хибну тривогу).
      MESH_DEBUG_PRINTLN("AirRaidGateway: response too short (%d chars, need > %d) - ignoring, keeping previous state", content_len, ALERTS_UID);
    } else {
      char c = body[start + ALERTS_UID];
      if (c == 'A' || c == 'P') {
        snap.has_state = true;
        snap.state = STATE_ALERT;
      } else if (c == 'N') {
        snap.has_state = true;
        snap.state = STATE_CLEAR;
      } else {
        MESH_DEBUG_PRINTLN("AirRaidGateway: unexpected char '%c' at index %d - ignoring, keeping previous state", c, ALERTS_UID);
      }
    }
  } else if (code == 401) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: HTTP 401 - bad/expired token");
  } else if (code == 429) {
    _poll_interval_ms = min(_poll_interval_ms * 2, (unsigned long)ALERT_BACKOFF_MAX_MS);
    MESH_DEBUG_PRINTLN("AirRaidGateway: HTTP 429 - rate limited, backing off to %lums", _poll_interval_ms);
  } else {
    MESH_DEBUG_PRINTLN("AirRaidGateway: HTTP error %d", code);
  }

  http.end();

  snap.wifi_connected = true;  // сюди потрапляємо лише за WiFi.status() == WL_CONNECTED
  xQueueOverwrite(_result_queue, &snap);

  // Допоміжне для підбору розміру стека: логується рідко, щоб не засмічувати
  // serial кожні 15 с. Коли за реальними показаннями буде обрано безпечний
  // ALERT_POLL_TASK_STACK, це логування (і лічильник) можна прибрати.
  if ((++_poll_count_for_stack_log % ALERT_STACK_LOG_EVERY_N_POLLS) == 1) {
    UBaseType_t words_free = uxTaskGetStackHighWaterMark(NULL);
    MESH_DEBUG_PRINTLN("AirRaidGateway: poll task stack high-water mark = %u bytes free (of %d)",
                        (unsigned)words_free, ALERT_POLL_TASK_STACK);
  }
}

#endif
