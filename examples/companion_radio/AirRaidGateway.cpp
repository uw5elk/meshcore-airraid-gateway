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
#define ALERT_POLL_TASK_STACK      12288    // байти; розмір підібрано за логами uxTaskGetStackHighWaterMark() нижче
                                            // (було 10240; +2 КБ запасу на додаткові кадри детального запиту)
#define ALERT_POLL_TASK_CORE           0    // задача драйвера WiFi уже на ядрі 0; ядро 1 лишаємо вільним для loopTask
#define ALERT_STACK_LOG_EVERY_N_POLLS 20    // як рідко писати в MESH_DEBUG запас стека

// ---- Детальний запит про загрози (спрацьовує лише на переході CLEAR->ALERT) ----
#define ALERT_DETAIL_HTTP_TIMEOUT_MS  6000  // підключення + читання; обмежує додаткову затримку тривоги
#define ALERT_DETAIL_DEADLINE_MS      8000  // жорсткий ліміт часу на потокове сканування
#define ALERT_DETAIL_MAX_BYTES    (64*1024) // жорсткий ліміт прочитаних байт, якщо документ розростеться
#define MAX_THREATS                     8   // на один запис; надлишок відкидаємо
#define THREAT_TYPE_MAX                28   // "strategic_aircraft_activity" = 27 символів + NUL
#define JSON_TOKEN_MAX                 40   // найдовший рядок, який нас цікавить; довші обрізаються
#define JSON_KEY_MAX                   24

// Запис, який нам потрібен ("Криворізький район"), і обласний запис, на який
// відкочуємось, якщо першого немає. Звіряємо з рядковим значенням "location_uid".
#define DETAIL_TARGET_UID   "46"
#define DETAIL_FALLBACK_UID "9"

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
  _level = ALERT_LEVEL_UNKNOWN;
  _threat_list[0] = 0;
  _task_state = STATE_UNKNOWN;
  _detail_pending = false;
  memset(&_pending_snap, 0, sizeof(_pending_snap));
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
  if (snap.has_state) handleState(snap);
}

void AirRaidGateway::handleState(const PollSnapshot& snap) {
  AlertState new_state = snap.state;

  if (_state == STATE_UNKNOWN) {
    _state = new_state;  // мовчки встановлюємо базовий стан, при старті не шлемо нічого
    MESH_DEBUG_PRINTLN("AirRaidGateway: baseline = %s", new_state == STATE_ALERT ? "ALERT" : "CLEAR");
    return;
  }
  if (new_state == _state) return;  // стан не змінився -> нічого не шлемо

  _state = new_state;

  if (new_state == STATE_ALERT && snap.has_details) {
    _level = snap.level;
    StrHelper::strncpy(_threat_list, snap.threat_list, sizeof(_threat_list));
  } else {
    // Відбій або тривога, для якої детальний запит не вдався - показувати нічого.
    _level = ALERT_LEVEL_UNKNOWN;
    _threat_list[0] = 0;
  }

  if (_ui != NULL) {
    _ui->wakeDisplay();
    if (new_state == STATE_ALERT) {
      _ui->showAlert("TRYVOGA", 5000);
    } else {
      _ui->showAlert("VIDBIY", 5000);
    }
  }

  // " HH:MM" або "", поки NTP не синхронізувався (щойно після старту система
  // віддає неправдоподібну епоху - фальшивий час не друкуємо).
  char when[8];
  when[0] = 0;
  time_t t = time(nullptr);  // системний час: із NTP, уже київський завдяки configTzTime()
  if (t < NTP_READY_EPOCH_THRESHOLD) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: NTP not synced yet, sending alert without a timestamp");
  } else {
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(when, sizeof(when), " %02d:%02d", lt.tm_hour, lt.tm_min);
  }

  char msg[160];
  if (new_state != STATE_ALERT) {
    snprintf(msg, sizeof(msg), "\xF0\x9F\x9F\xA2 Відбій — %s%s", REGION_NAME, when);
  } else if (_level == ALERT_LEVEL_UNKNOWN) {
    // Придатних деталей немає - звичайне повідомлення, точно як раніше.
    snprintf(msg, sizeof(msg), "\xF0\x9F\x94\xB4 ПОВІТРЯНА ТРИВОГА — %s%s", REGION_NAME, when);
  } else {
    const char* emoji = (_level == ALERT_LEVEL_YELLOW) ? "\xF0\x9F\x9F\xA1"   // U+1F7E1
                                                       : "\xF0\x9F\x94\xB4";  // U+1F534
    if (_threat_list[0]) {
      snprintf(msg, sizeof(msg), "%s ТРИВОГА — %s%s (%s)", emoji, REGION_NAME, when, _threat_list);
    } else {
      // Рівень відомий, але загроз не перелічено - порожні дужки прибираємо.
      snprintf(msg, sizeof(msg), "%s ТРИВОГА — %s%s", emoji, REGION_NAME, when);
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

// ---- Потоковий сканер JSON для /v1/alerts/active.json ----
//
// Той документ важить десятки КБ - для getString() це забагато. Натомість
// віддаємо Stream у HTTPClient::writeToStream(), який згодовує нам тіло
// шматками по ~1,4 КБ (зі свого буфера в купі) і, що важливо, сам розбирає
// chunked transfer-encoding - якби ми читали getStreamPtr() напряму, всередині
// даних лишались би шістнадцяткові заголовки чанків. Завантаження обриваємо
// "коротким записом", щойно знайдено потрібний запис; HTTPClient тоді здається
// з HTTPC_ERROR_STREAM_WRITE, що ми й вважаємо успіхом.
//
// Жоден запис не буферизується цілком. Побайтовий токенізатор вихоплює три
// потрібні поля у невеликий скретч по ходу читання і фіксує його, коли
// надходить закривна дужка запису - тож запис із багатьма загрозами нічого
// переповнити не може. Увесь стан - ~600 Б у .bss, поза стеком задачі.
struct AlertRecord {
  AlertLevel level;
  uint8_t threat_count;
  char threats[MAX_THREATS][THREAT_TYPE_MAX];
};

class AlertJsonScanner : public Stream {
public:
  void begin(unsigned long deadline_at) {
    _deadline_at = deadline_at;
    _total = 0;
    _abort = false;
    _depth = 0;
    _in_string = _escaped = _pending_string = false;
    _tok_len = 0;
    _key[0] = 0;
    _in_record = false;
    _have_primary = _have_fallback = false;
    resetScratch();
  }

  // NULL, якщо не знайшлося ні цільового запису, ні запасного.
  const AlertRecord* result() const {
    if (_have_primary) return &_primary;
    if (_have_fallback) return &_fallback;
    return NULL;
  }
  bool foundPrimary() const { return _have_primary; }
  uint32_t bytesScanned() const { return _total; }

  // Обв'язка Print/Stream. Справжню роботу робить лише write(); це приймач, не джерело.
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, size_t len) override {
    if (_abort) return 0;   // вже впорались - короткий запис каже HTTPClient зупинитись
    _total += len;
    for (size_t i = 0; i < len; i++) {
      feed((char)buf[i]);
      if (_abort) return 0;
    }
    if (_total > ALERT_DETAIL_MAX_BYTES || (long)(millis() - _deadline_at) >= 0) {
      _abort = true;
      return 0;
    }
    return len;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

private:
  void resetScratch() {
    _uid[0] = 0;
    _scratch.level = ALERT_LEVEL_UNKNOWN;
    _scratch.threat_count = 0;
  }

  void appendTok(char c) {
    if (_tok_len < JSON_TOKEN_MAX - 1) _tok[_tok_len++] = c;   // довші рядки обрізаються, а не переповнюють
  }

  void addThreat(const char* type) {
    if (_scratch.threat_count >= MAX_THREATS) return;
    for (uint8_t i = 0; i < _scratch.threat_count; i++) {      // дешеве усунення дублів за сирою назвою
      if (strcmp(_scratch.threats[i], type) == 0) return;
    }
    StrHelper::strncpy(_scratch.threats[_scratch.threat_count], type, THREAT_TYPE_MAX);
    _scratch.threat_count++;
  }

  // Завершений рядок, який виявився значенням (а не ключем).
  void onValue(const char* val) {
    if (!_in_record) return;
    if (strcmp(_key, "location_uid") == 0) {
      StrHelper::strncpy(_uid, val, sizeof(_uid));
    } else if (strcmp(_key, "alert_level") == 0) {
      if (strcmp(val, "red") == 0) _scratch.level = ALERT_LEVEL_RED;
      else if (strcmp(val, "yellow") == 0) _scratch.level = ALERT_LEVEL_YELLOW;
      // будь-що інше (зокрема JSON null, який сюди не доходить) лишає UNKNOWN
    } else if (strcmp(_key, "threat_type") == 0 || strcmp(_key, "threats") == 0) {
      // Терпить обидві форми: [{"threat_type":"drones"}] і плаский ["drones"].
      // У плоскому випадку _key лишається "threats" на весь масив.
      addThreat(val);
    }
  }

  void closeRecord() {
    _in_record = false;
    if (strcmp(_uid, DETAIL_TARGET_UID) == 0) {
      _primary = _scratch;
      _have_primary = true;
      _abort = true;   // отримали те, по що прийшли - обриваємо завантаження
    } else if (!_have_fallback && strcmp(_uid, DETAIL_FALLBACK_UID) == 0) {
      _fallback = _scratch;
      _have_fallback = true;   // скануємо далі; цільовий запис ще може трапитись
    }
    resetScratch();
  }

  void feed(char c) {
    if (_in_string) {
      if (_escaped) { _escaped = false; appendTok(c); }
      else if (c == '\\') _escaped = true;
      else if (c == '"') { _in_string = false; _tok[_tok_len] = 0; _pending_string = true; }
      else appendTok(c);
      return;
    }

    // Рядок можна класифікувати лише за тим, що йде після нього: ':' робить його
    // ключем, будь-що інше - значенням. Вирішуємо це тут, а далі провалюємось
    // нижче, щоб поточний символ усе одно пройшов звичайну структурну обробку.
    if (_pending_string) {
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return;   // ще не визначились
      _pending_string = false;
      if (c == ':') {
        StrHelper::strncpy(_key, _tok, sizeof(_key));
        return;
      }
      onValue(_tok);
    }

    switch (c) {
      case '"':
        _in_string = true;
        _tok_len = 0;
        break;
      case '{':
        // Найперша дужка - це корінь документа, а не запис. Будь-який інший обʼєкт,
        // що відкривається, поки ми ще не всередині запису, запис і починає - це
        // працює і для {"alerts":[...]}, і для голого масиву [...] на верхньому рівні.
        if (!_in_record && _depth >= 1) {
          _in_record = true;
          _record_depth = _depth;
          resetScratch();
        }
        _depth++;
        break;
      case '[':
        _depth++;
        break;
      case '}':
      case ']':
        if (_depth > 0) _depth--;
        if (_in_record && _depth == _record_depth) closeRecord();
        break;
      default:
        break;
    }
  }

  unsigned long _deadline_at = 0;
  uint32_t _total = 0;
  bool _abort = false;

  uint16_t _depth = 0, _record_depth = 0;
  bool _in_string = false, _escaped = false, _pending_string = false, _in_record = false;
  uint8_t _tok_len = 0;
  char _tok[JSON_TOKEN_MAX];
  char _key[JSON_KEY_MAX];
  char _uid[12];

  AlertRecord _scratch, _primary, _fallback;
  bool _have_primary = false, _have_fallback = false;
};

static AlertJsonScanner alert_scanner;   // .bss, а не стек задачі

// threat_type з alerts.in.ua -> коротка українська назва. Незнайомі типи стають
// "невідомо", тож поповнення API ніколи не дасть порожнього чи англомовного запису.
static const struct { const char* raw; const char* uk; } THREAT_NAMES[] = {
  { "drones",                      "дрони" },
  { "unspecified_missiles",        "ракети" },
  { "ballistic_missiles",          "балістика" },
  { "cruise_missiles",             "крилаті" },
  { "mig31k_departure",            "МіГ-31К" },
  { "guided_aerial_bombs",         "КАБи" },
  { "tactic_aircraft_activity",    "авіація" },
  { "strategic_aircraft_activity", "стратег. авіація" },
  { "air_defense",                 "ППО" },
  { "unknown",                     "невідомо" },
};

static const char* translateThreat(const char* raw) {
  for (size_t i = 0; i < sizeof(THREAT_NAMES) / sizeof(THREAT_NAMES[0]); i++) {
    if (strcmp(raw, THREAT_NAMES[i].raw) == 0) return THREAT_NAMES[i].uk;
  }
  return "невідомо";
}

// Складає "дрони, ракети" в out, усуваючи дублі за *перекладеною* назвою (кілька
// сирих типів можуть злитись у "невідомо"). Назва, що не влазить, пропускається
// цілком - ніколи не ріжеться - тож результат не може обірватись усередині
// UTF-8 послідовності й стати кракозябрами в застосунку. Саме пропуск, а не
// зупинка, дає шанс коротшій наступній назві все ж потрапити в список.
static void renderThreatList(const AlertRecord& rec, char* out, size_t out_sz) {
  out[0] = 0;
  size_t used = 0;
  const char* picked[MAX_THREATS];
  uint8_t n_picked = 0;

  for (uint8_t i = 0; i < rec.threat_count; i++) {
    const char* uk = translateThreat(rec.threats[i]);

    bool dup = false;
    for (uint8_t j = 0; j < n_picked; j++) {
      if (strcmp(picked[j], uk) == 0) { dup = true; break; }
    }
    if (dup) continue;

    size_t sep = used ? 2 : 0;
    size_t need = sep + strlen(uk);
    if (used + need + 1 > out_sz) continue;   // цілком не влазить -> пропускаємо повністю

    if (sep) { out[used++] = ','; out[used++] = ' '; }
    strcpy(&out[used], uk);
    used += strlen(uk);
    picked[n_picked++] = uk;
  }
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
      // WiFi відпав між опитуванням і детальним запитом. Тривога все одно мусить
      // піти, тож відпускаємо її зараз без деталей, а не тримаємо до повернення WiFi.
      if (_detail_pending) {
        MESH_DEBUG_PRINTLN("AirRaidGateway: WiFi lost before detail fetch - sending plain alert");
        _detail_pending = false;
        _pending_snap.wifi_connected = false;
        xQueueOverwrite(_result_queue, &_pending_snap);
      } else {
        // Звичайний знімок "WiFi лежить" кладемо лише тоді, коли немає притриманої
        // тривоги - скринька вміщує рівно один запис, тож обидва не помістяться.
        PollSnapshot snap;
        memset(&snap, 0, sizeof(snap));
        xQueueOverwrite(_result_queue, &snap);
      }
      vTaskDelay(pdMS_TO_TICKS(ALERT_WIFI_DOWN_IDLE_MS));
      continue;
    }

    // Деталізація виконується окремим кадром, уже після того, як pollOnce()
    // повернувся і його стекові WiFiClientSecure/HTTPClient знищено - дві
    // TLS-сесії не мають жити одночасно на цій купі.
    if (_detail_pending) {
      fetchDetails();
      _detail_pending = false;
      xQueueOverwrite(_result_queue, &_pending_snap);
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
  PollSnapshot snap;
  memset(&snap, 0, sizeof(snap));
  snap.has_http_result = true;
  snap.wifi_connected = true;

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

  // Допоміжне для підбору розміру стека: логується рідко, щоб не засмічувати
  // serial кожні 15 с. Коли за реальними показаннями буде обрано безпечний
  // ALERT_POLL_TASK_STACK, це логування (і лічильник) можна прибрати.
  if ((++_poll_count_for_stack_log % ALERT_STACK_LOG_EVERY_N_POLLS) == 1) {
    UBaseType_t words_free = uxTaskGetStackHighWaterMark(NULL);
    MESH_DEBUG_PRINTLN("AirRaidGateway: poll task stack high-water mark = %u bytes free (of %d)",
                        (unsigned)words_free, ALERT_POLL_TASK_STACK);
  }

  // Детальний запит заслуговує лише перехід CLEAR->ALERT. Притримуємо знімок на
  // один кадр, щоб тривога понесла з собою список загроз; усе інше йде одразу.
  // Після старту _task_state дорівнює UNKNOWN, тож тихий baseline запиту не
  // спричиняє ніколи.
  if (snap.has_state) {
    bool transition_to_alert = (snap.state == STATE_ALERT && _task_state == STATE_CLEAR);
    _task_state = snap.state;
    if (transition_to_alert) {
      _pending_snap = snap;
      _detail_pending = true;
      return;   // покладе pollTaskLoop() після того, як відпрацює fetchDetails()
    }
  }

  xQueueOverwrite(_result_queue, &snap);
}

// Один додатковий запит, який робиться лише на переході CLEAR->ALERT, щоб внести
// список загроз у повідомлення. Усі шляхи відмови тут нефатальні за задумом:
// has_details лишається false, і handleState() відкочується на звичайне
// повідомлення. Тривога виходить завжди - деталі це бонус, а не передумова.
void AirRaidGateway::fetchDetails() {
  _pending_snap.has_details = false;
  _pending_snap.level = ALERT_LEVEL_UNKNOWN;
  _pending_snap.threat_list[0] = 0;

  WiFiClientSecure client;
  client.setInsecure();   // TODO(v2): pin/verify alerts.in.ua cert

  HTTPClient http;
  http.setConnectTimeout(ALERT_DETAIL_HTTP_TIMEOUT_MS);
  http.setTimeout(ALERT_DETAIL_HTTP_TIMEOUT_MS);

  static const char* detail_url = "https://api.alerts.in.ua/v1/alerts/active.json";
  if (!http.begin(client, detail_url)) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: detail http.begin() failed - plain alert");
    return;
  }
  http.addHeader("Authorization", "Bearer " ALERTS_TOKEN);

  int code = http.GET();

  // Навмисно НЕ чіпає ні _poll_interval_ms, ні http_code у знімку: 429 на цьому
  // нечастому запиті не має ні гальмувати 15-секундне опитування, ні показуватись
  // як "API err" на сторінці AIRRAID, яка стежить лише за основним ендпоінтом.
  if (code != 200) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: detail HTTP %d - plain alert", code);
    http.end();
    return;
  }

  alert_scanner.begin(millis() + ALERT_DETAIL_DEADLINE_MS);
  http.writeToStream(&alert_scanner);   // відʼємний результат очікуваний при достроковому обриві
  http.end();

  const AlertRecord* rec = alert_scanner.result();
  if (rec == NULL) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: detail - no record for uid %s or %s in %u bytes - plain alert",
                        DETAIL_TARGET_UID, DETAIL_FALLBACK_UID, (unsigned)alert_scanner.bytesScanned());
    return;
  }
  if (rec->level == ALERT_LEVEL_UNKNOWN) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: detail - record has no usable alert_level - plain alert");
    return;
  }

  _pending_snap.level = rec->level;
  renderThreatList(*rec, _pending_snap.threat_list, sizeof(_pending_snap.threat_list));
  _pending_snap.has_details = true;

  MESH_DEBUG_PRINTLN("AirRaidGateway: detail ok (%s, uid %s, %u threats, %u bytes scanned) -> '%s'",
                      rec->level == ALERT_LEVEL_RED ? "red" : "yellow",
                      alert_scanner.foundPrimary() ? DETAIL_TARGET_UID : DETAIL_FALLBACK_UID,
                      (unsigned)rec->threat_count, (unsigned)alert_scanner.bytesScanned(),
                      _pending_snap.threat_list);
}

#endif
