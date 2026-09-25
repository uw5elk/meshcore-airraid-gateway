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

// ---- Детальний запит про загрози ----
// Перший запит нової тривоги (перехід CLEAR->ALERT): повідомлення чекає на нього, тож
// таймаути жорсткіші. Не вклався - іде звичайне повідомлення, а рівень доганяє перечитування.
// Дедлайн рахується від початку запиту; найгірший випадок (повільний connect + мовчазний
// сервер) - приблизно connect + read таймаути, тобто ~6 с, типово 1-3 с.
#define ALERT_FIRST_DETAIL_TIMEOUT_MS   3000
#define ALERT_FIRST_DETAIL_DEADLINE_MS  4000
// Планові перечитування під час тривоги - нічого не чекає, тож таймаути ширші.
#define ALERT_DETAIL_HTTP_TIMEOUT_MS  6000  // підключення + читання
#define ALERT_DETAIL_DEADLINE_MS      8000  // жорсткий ліміт часу на потокове сканування
// Документ вже ~40 КБ у звичайний день (~600 Б на запис, а запис району 46 стоїть ближче
// до кінця), тож стеля 64 КБ була б близько. Гальмує лише ліміт часу вище, це - запобіжник.
#define ALERT_DETAIL_MAX_BYTES    (128*1024)
#define ALERT_DETAIL_RECHECK_MS      30000  // швидке перечитування після переходу CLEAR->ALERT: доганяє занижений перший рівень
#define ALERT_DETAIL_REFRESH_MS      60000  // далі раз на хвилину, поки триває тривога
#define ALERT_DETAIL_RETRY_MS        15000  // єдина повторна спроба, якщо швидке перечитування не вдалось
// Швидке перечитування і кожне 3-тє планове йдуть БЕЗ If-Modified-Since: Last-Modified має
// секундну точність, тож 304 міг би сховати зміну, що сталась у ту саму секунду, що й
// попередній запит. Так це може приховати підвищення не довше ~3 хв (3 x 60 с).
#define ALERT_DETAIL_FULL_EVERY_N        3
#define MAX_THREATS                     8   // на один запис; надлишок відкидаємо
#define THREAT_TYPE_MAX                28   // "strategic_aircraft_activity" = 27 символів + NUL
#define JSON_TOKEN_MAX                 40   // найдовший рядок, який нас цікавить; довші обрізаються
#define JSON_KEY_MAX                   24

// Запис, який нам потрібен ("Криворізький район"), і обласний запис, на який
// відкочуємось, якщо першого немає. Звіряємо з рядковим значенням "location_uid".
// Можна перевизначити в AirRaidGatewayConfig.h (він підключений вище). Значення -
// рядки в лапках, бо location_uid порівнюється як рядок через strcmp.
#ifndef ALERT_DETAIL_LOCATION_UID
  #define ALERT_DETAIL_LOCATION_UID "46"   // Криворізький район
#endif
#ifndef ALERT_DETAIL_HROMADA_UID
  #define ALERT_DETAIL_HROMADA_UID  "279"  // Криворізька громада
#endif
#ifndef ALERT_DETAIL_CITY_UID
  #define ALERT_DETAIL_CITY_UID     "5279" // м. Кривий Ріг
#endif
#ifndef ALERT_DETAIL_FALLBACK_UID
  #define ALERT_DETAIL_FALLBACK_UID "9"    // Дніпропетровська обл.
#endif

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

// ---- Бюджет переліку загроз на кожен тип повідомлення ----
// sendGroupMessage() ріже текст на MAX_TEXT_LEN (160) разом із префіксом "<node_name>: ", по
// сирих байтах - тож на перелік лишається те, що не зайняв решта шаблону. Рахуємо для
// найгіршого випадку: імʼя вузла на всі 31 символ + ": " = 33 Б, час " HH:MM" присутній.
// "Решта шаблону" - це весь текст повідомлення, крім самого переліку, разом із дужками " ()".
// Шаблони нижче мусять збігатися з тими, що збирають повідомлення в handleState() і
// handleDetailRefresh(); REGION_NAME береться з конфігу, тож довше імʼя саме зменшить бюджет.
// Перевірено на "Кривий Ріг": ТРИВОГА 52 Б -> 75, ПІДВИЩЕНО 56 Б -> 71, ЗНИЖЕНО 82 Б -> 45.
#define NODE_PREFIX_WORST_BYTES  (sizeof(NodePrefs::node_name) - 1 + 2)
#define LIST_BUDGET(fixed_part) \
  ((int)MAX_TEXT_LEN - (int)NODE_PREFIX_WORST_BYTES - (int)(sizeof(fixed_part) - 1))
#define EMOJI_RED_LIT     "\xF0\x9F\x94\xB4"   // U+1F534, як і 🟡 U+1F7E1 - 4 байти
static const int LIST_BUDGET_FIRST = LIST_BUDGET(EMOJI_RED_LIT " ТРИВОГА — " REGION_NAME " 22:22 ()");
static const int LIST_BUDGET_RAISE = LIST_BUDGET(EMOJI_RED_LIT " ПІДВИЩЕНО — " REGION_NAME " 22:22 ()");
static const int LIST_BUDGET_LOWER = LIST_BUDGET(EMOJI_RED_LIT " ЗНИЖЕНО — " REGION_NAME " 22:22 (). Тривога триває.");
// Хоч один короткий токен ("дрони" = 10 Б) має влізти, інакше REGION_NAME задовгий для шаблону.
static_assert(LIST_BUDGET_FIRST >= 10 && LIST_BUDGET_RAISE >= 10 && LIST_BUDGET_LOWER >= 10,
              "REGION_NAME is too long: no room left for the threat list in a 160-byte message");

// Обрізає перелік "дрони, ракети, ..." до max_bytes по цілих токенах (роздільник ", "): токен,
// що не влазить, відкидається разом з усім, що після нього, тож UTF-8 не розрубується.
// Якщо не влазить навіть перший токен - порожній результат (повідомлення без дужок).
static void fitThreatList(const char* src, char* dst, size_t dst_sz, int max_bytes) {
  size_t len = strlen(src);
  size_t cut = len;
  if ((int)len > max_bytes) {
    cut = 0;
    for (size_t i = 0; i + 1 < len && (int)i <= max_bytes; i++) {
      if (src[i] == ',' && src[i + 1] == ' ') cut = i;
    }
  }
  if (cut >= dst_sz) cut = dst_sz - 1;   // недосяжно при буфері THREAT_LIST_MAX_BYTES + 1, але дешево
  memcpy(dst, src, cut);
  dst[cut] = 0;
}

// Лише для налагоджувальних рядків.
static const char* levelName(AlertLevel l) {
  switch (l) {
    case ALERT_LEVEL_RED:    return "red";
    case ALERT_LEVEL_YELLOW: return "yellow";
    default:                 return "unknown";
  }
}

// Для розбивки "raion=... hromada=... city=..." - "-" означає "запис не знайдено
// в цьому скануванні", а не "рівень невідомий" (друге на практиці не трапляється:
// запис без придатного alert_level просто не потрапляє в жоден із трьох _have_*).
static const char* dash(AlertLevel l) {
  return l == ALERT_LEVEL_UNKNOWN ? "-" : levelName(l);
}

void AirRaidGateway::begin(MyMesh* mesh, UITask* ui) {
  _mesh = mesh;
  _ui = ui;
  _state = STATE_UNKNOWN;
  _level = ALERT_LEVEL_UNKNOWN;
  _threat_list[0] = 0;
  _announced = AlertAnnounced();
  _task_state = STATE_UNKNOWN;
  _detail_pending = false;
  _detail_sched = false;
  _detail_recheck_pending = false;
  _detail_periodic_count = 0;
  _last_read_level = ALERT_LEVEL_UNKNOWN;
  _detail_last_modified[0] = 0;
  memset(&_pending_snap, 0, sizeof(_pending_snap));
  _poll_interval_ms = ALERT_POLL_INTERVAL_MS;
  _next_poll_at = millis();  // опитати одразу, щойно підніметься WiFi
  MESH_DEBUG_PRINTLN("AirRaidGateway: threat list budgets (bytes): first=%d raise=%d lower=%d, buffer=%d",
                      LIST_BUDGET_FIRST, LIST_BUDGET_RAISE, LIST_BUDGET_LOWER, THREAT_LIST_MAX_BYTES);
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
  if (snap.is_refresh) handleDetailRefresh(snap);
  else if (snap.has_state) handleState(snap);
}

// " HH:MM" або "", поки NTP не синхронізувався (щойно після старту система
// віддає неправдоподібну епоху - фальшивий час не друкуємо).
void AirRaidGateway::formatWhen(char* when, size_t sz) {
  when[0] = 0;
  time_t t = time(nullptr);  // системний час: із NTP, уже київський завдяки configTzTime()
  if (t < NTP_READY_EPOCH_THRESHOLD) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: NTP not synced yet, sending alert without a timestamp");
  } else {
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(when, sz, " %02d:%02d", lt.tm_hour, lt.tm_min);
  }
}

void AirRaidGateway::handleState(const PollSnapshot& snap) {
  AlertState new_state = snap.state;

  if (_state == STATE_UNKNOWN) {
    // Мовчки встановлюємо базовий стан, при старті не шлемо нічого. Якщо це ALERT
    // (ребут посеред тривоги), _announced лишається невалідним: перші деталі, що
    // прийдуть із перечитування, стануть базою мовчки, без "ПІДВИЩЕНО".
    _state = new_state;
    MESH_DEBUG_PRINTLN("AirRaidGateway: baseline = %s", new_state == STATE_ALERT ? "ALERT" : "CLEAR");
    return;
  }
  if (new_state == _state) return;  // стан не змінився -> нічого не шлемо

  _state = new_state;

  // Нова тривога чи відбій - оголошене починається з нуля.
  _announced = AlertAnnounced();
  if (new_state == STATE_ALERT && snap.has_details) {
    _level = snap.level;
    StrHelper::strncpy(_threat_list, snap.threat_list, sizeof(_threat_list));
    _announced.adopt(snap.level, snap.threat_mask, snap.from_primary);
  } else {
    // Відбій або тривога, для якої детальний запит не вдався - показувати нічого.
    // (Для тривоги "звичайне" повідомлення все одно оголошено: рівень UNKNOWN.)
    _level = ALERT_LEVEL_UNKNOWN;
    _threat_list[0] = 0;
    if (new_state == STATE_ALERT) _announced.adopt(ALERT_LEVEL_UNKNOWN, 0, false);
  }

  if (_ui != NULL) {
    _ui->wakeDisplay();
    if (new_state == STATE_ALERT) {
      _ui->showAlert("TRYVOGA", 5000);
    } else {
      _ui->showAlert("VIDBIY", 5000);
    }
  }

  char when[8];
  formatWhen(when, sizeof(when));

  char msg[160];
  if (new_state != STATE_ALERT) {
    snprintf(msg, sizeof(msg), "\xF0\x9F\x9F\xA2 Відбій — %s%s", REGION_NAME, when);
  } else if (_level == ALERT_LEVEL_UNKNOWN) {
    // Придатних деталей немає - звичайне повідомлення, точно як раніше.
    snprintf(msg, sizeof(msg), "\xF0\x9F\x94\xB4 ПОВІТРЯНА ТРИВОГА — %s%s", REGION_NAME, when);
  } else {
    const char* emoji = (_level == ALERT_LEVEL_YELLOW) ? "\xF0\x9F\x9F\xA1"   // U+1F7E1
                                                       : "\xF0\x9F\x94\xB4";  // U+1F534
    char list[THREAT_LIST_MAX_BYTES + 1];
    fitThreatList(_threat_list, list, sizeof(list), LIST_BUDGET_FIRST);
    if (list[0]) {
      snprintf(msg, sizeof(msg), "%s ТРИВОГА — %s%s (%s)", emoji, REGION_NAME, when, list);
    } else {
      // Рівень відомий, але загроз не перелічено - порожні дужки прибираємо.
      snprintf(msg, sizeof(msg), "%s ТРИВОГА — %s%s", emoji, REGION_NAME, when);
    }
  }
  sendChannelText(msg);
}

// Свіжі деталі активної тривоги (планове перечитування). Рішення приймає
// evaluateEscalation(); тут лише його наслідки: OLED і, за потреби, повідомлення.
void AirRaidGateway::handleDetailRefresh(const PollSnapshot& snap) {
  // Запізніле перечитування після відбою чи до першого зчитування - відкидаємо.
  if (_state != STATE_ALERT || !snap.has_details) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: refresh dropped (state is not ALERT) -> none");
    return;
  }

  const bool had_base = _announced.valid;
  // Запис області після запису району - не дані про наш район: ні канал, ні OLED
  // за ним не рухаємо (evaluateEscalation() теж поверне ESC_NONE).
  const bool foreign = had_base && _announced.from_primary && !snap.from_primary;
  EscalationAction act = evaluateEscalation(_announced, snap.level, snap.threat_mask, snap.from_primary);

  // Два окремі рівні. _announced - що востаннє оголошено в каналі (змінюється лише
  // оголошенням, після пониження - нижчий). _level - для екрана: ПОТОЧНИЙ рівень
  // останнього успішного перечитування, тож TRYVOGA R -> TRYVOGA Y одразу, ще до
  // того, як пониження підтвердиться для каналу.
  if (!foreign) {
    _level = snap.level;
    StrHelper::strncpy(_threat_list, snap.threat_list, sizeof(_threat_list));
  }

  // Один рядок на кожне перечитування, щоб при тривозі звіряти з офіційним застосунком.
  //   announce raise / announce raise (new threat) / announce lower - пішло в канал;
  //   pending lower (n/N) - нижчий рівень за записом району побачено, але ще не підтверджено;
  //   none (oblast record cannot lower) - нижчий рівень лише за записом області: у канал нічого,
  //   екран показує його; baseline - перші деталі після ребуту взято мовчки;
  //   none - у канал нічого (рівень і набір загроз без змін, або зникла загроза, або запис
  //   області не може змінити оголошене за районом - тоді екран теж не чіпаємо).
  char decision[40];
  switch (act) {
    case ESC_SEND_RAISE:   strcpy(decision, "announce raise"); break;
    case ESC_SEND_THREAT:  strcpy(decision, "announce raise (new threat)"); break;
    case ESC_SEND_LOWER:   strcpy(decision, "announce lower"); break;
    case ESC_PENDING_LOWER:
      snprintf(decision, sizeof(decision), "pending lower (%u/%u)",
               (unsigned)_announced.lower_count, (unsigned)ESC_LOWER_CONFIRMATIONS);
      break;
    case ESC_BLOCKED_LOWER: strcpy(decision, "none (oblast record cannot lower)"); break;
    case ESC_SILENT:       strcpy(decision, had_base ? "silent" : "baseline"); break;
    default:               strcpy(decision, "none"); break;
  }
  MESH_DEBUG_PRINTLN("AirRaidGateway: refresh raion=%s hromada=%s city=%s -> %s%s mask=0x%04X threats='%s' 200(%s) -> %s%s [announced %s 0x%04X]",
                      dash(snap.raion_level), dash(snap.hromada_level), dash(snap.city_level), levelName(snap.level),
                      snap.from_primary ? "" : " (oblast fallback uid=" ALERT_DETAIL_FALLBACK_UID ")",
                      (unsigned)snap.threat_mask, snap.threat_list, snap.sent_ims ? "cond" : "uncond", decision,
                      foreign ? " (oblast record ignored)" : "",
                      levelName(_announced.level), (unsigned)_announced.threat_mask);

  if (!escalationSends(act)) return;

  const bool lower = (act == ESC_SEND_LOWER);
  if (_ui != NULL) {
    _ui->wakeDisplay();
    _ui->showAlert(lower ? "ZNYZHENO" : "PIDVYSHENO", 5000);
  }

  char when[8];
  formatWhen(when, sizeof(when));

  // Емодзі - за оголошеним рівнем, який щойно записано: 🔴 при підвищенні, 🟡 при пониженні.
  const char* emoji = (_announced.level == ALERT_LEVEL_YELLOW) ? "\xF0\x9F\x9F\xA1"   // U+1F7E1
                                                               : "\xF0\x9F\x94\xB4";  // U+1F534
  const char* verb = lower ? "ЗНИЖЕНО" : "ПІДВИЩЕНО";
  const char* tail = lower ? ". Тривога триває." : "";   // "Тривога триває" обовʼязкова: 🟡 - це не відбій
  // Перелік обрізається під бюджет саме цього типу повідомлення ("ЗНИЖЕНО" довше за "ПІДВИЩЕНО").
  char list[THREAT_LIST_MAX_BYTES + 1];
  fitThreatList(snap.threat_list, list, sizeof(list), lower ? LIST_BUDGET_LOWER : LIST_BUDGET_RAISE);
  char msg[160];
  if (list[0]) {
    snprintf(msg, sizeof(msg), "%s %s — %s%s (%s)%s", emoji, verb, REGION_NAME, when, list, tail);
  } else {
    snprintf(msg, sizeof(msg), "%s %s — %s%s%s", emoji, verb, REGION_NAME, when, tail);
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
  if (text_len + NODE_PREFIX_WORST_BYTES > MAX_TEXT_LEN) {   // сторожа бюджетів LIST_BUDGET_*: не має спрацьовувати
    MESH_DEBUG_PRINTLN("AirRaidGateway: WARNING message %u B + worst node prefix exceeds %d B - would be cut mid-text",
                        (unsigned)text_len, (int)MAX_TEXT_LEN);
  }
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
  char uid[12];     // лише для налагоджувального логу: з якого запису взято рівень
  char atype[20];   // (завжди "air_raid" - інші відкидаються, але логуємо те, що справді зчитано)
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
    _have_raion = _have_hromada = _have_city = _have_fallback = false;
    _raion_level = _hromada_level = _city_level = ALERT_LEVEL_UNKNOWN;
    _local.level = ALERT_LEVEL_UNKNOWN;
    _local.threat_count = 0;
    _local.uid[0] = 0;
    _local.atype[0] = 0;
    resetScratch();
  }

  // Об'єднання знайдених серед району/громади/міста; NULL, якщо жоден не знайшовся.
  const AlertRecord* localResult() const { return foundLocal() ? &_local : NULL; }
  // Запис області; чинний лише як запасний варіант, коли localResult() == NULL.
  const AlertRecord* fallbackResult() const { return _have_fallback ? &_fallback : NULL; }
  bool foundLocal() const { return _have_raion || _have_hromada || _have_city; }
  // Лише для налагоджувального рядка "raion=... hromada=... city=...".
  AlertLevel raionLevel() const { return _raion_level; }
  AlertLevel hromadaLevel() const { return _hromada_level; }
  AlertLevel cityLevel() const { return _city_level; }
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
    _atype[0] = 0;
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

  // Вливає щойно закритий локальний запис (район/громада/місто) в об'єднаний _local:
  // рівень - максимум за alertLevelRank() (RED=1 < YELLOW=2 чисельно, тож порівнювати
  // "сирі" значення enum-а не можна - та сама пастка, що вже врахована в
  // AirRaidEscalation.h), загрози - об'єднання без дублів за сирою назвою, той самий
  // патерн, що в addThreat(). uid/atype лишаємо від першого влитого запису - лише
  // для довідки, на рішення не впливає.
  void mergeIntoLocal(const AlertRecord& rec) {
    if (alertLevelRank(rec.level) > alertLevelRank(_local.level)) _local.level = rec.level;
    if (_local.uid[0] == 0) {
      StrHelper::strncpy(_local.uid, rec.uid, sizeof(_local.uid));
      StrHelper::strncpy(_local.atype, rec.atype, sizeof(_local.atype));
    }
    for (uint8_t i = 0; i < rec.threat_count; i++) {
      if (_local.threat_count >= MAX_THREATS) break;
      bool dup = false;
      for (uint8_t j = 0; j < _local.threat_count; j++) {
        if (strcmp(_local.threats[j], rec.threats[i]) == 0) { dup = true; break; }
      }
      if (!dup) {
        StrHelper::strncpy(_local.threats[_local.threat_count], rec.threats[i], THREAT_TYPE_MAX);
        _local.threat_count++;
      }
    }
  }

  // Завершений рядок, який виявився значенням (а не ключем).
  void onValue(const char* val) {
    if (!_in_record) return;
    if (strcmp(_key, "location_uid") == 0) {
      StrHelper::strncpy(_uid, val, sizeof(_uid));
    } else if (strcmp(_key, "alert_type") == 0) {
      StrHelper::strncpy(_atype, val, sizeof(_atype));
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
    // Один location_uid може мати кілька записів із різним alert_type (наприклад,
    // окремо "air_raid" і "artillery_shelling"). Цікавить лише повітряна тривога;
    // решту пропускаємо і скануємо далі.
    if (strcmp(_atype, "air_raid") != 0) {
      resetScratch();
      return;
    }
    StrHelper::strncpy(_scratch.uid, _uid, sizeof(_scratch.uid));
    StrHelper::strncpy(_scratch.atype, _atype, sizeof(_scratch.atype));
    // Район/громада/місто зливаються в об'єднаний _local - жоден не "виграє" й не
    // обриває пошук інших. Область - лише запасний варіант, якщо жодного з трьох
    // не знайдеться до кінця документа.
    if (strcmp(_uid, ALERT_DETAIL_LOCATION_UID) == 0) {
      mergeIntoLocal(_scratch);
      _raion_level = _scratch.level;
      _have_raion = true;
    } else if (strcmp(_uid, ALERT_DETAIL_HROMADA_UID) == 0) {
      mergeIntoLocal(_scratch);
      _hromada_level = _scratch.level;
      _have_hromada = true;
    } else if (strcmp(_uid, ALERT_DETAIL_CITY_UID) == 0) {
      mergeIntoLocal(_scratch);
      _city_level = _scratch.level;
      _have_city = true;
    } else if (!_have_fallback && strcmp(_uid, ALERT_DETAIL_FALLBACK_UID) == 0) {
      _fallback = _scratch;
      _have_fallback = true;   // скануємо далі; локальні записи ще можуть трапитись
    }
    if (_have_raion && _have_hromada && _have_city) {
      _abort = true;   // усі три знайдено - область більше не потрібна, обриваємо завантаження
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
  char _atype[20];   // "artillery_shelling" = 18 символів; довші обрізаються і все одно не дорівнюють "air_raid"

  AlertRecord _scratch, _local, _fallback;
  bool _have_raion = false, _have_hromada = false, _have_city = false, _have_fallback = false;
  AlertLevel _raion_level = ALERT_LEVEL_UNKNOWN, _hromada_level = ALERT_LEVEL_UNKNOWN,
             _city_level = ALERT_LEVEL_UNKNOWN;
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

// Біт на кожну загрозу з THREAT_NAMES (індекс = номер біта); незнайомі типи зливаються
// в останній біт, "unknown" - так само, як у translateThreat(). Ескалацію за новою
// загрозою визначаємо саме за цією маскою, а не за відрендереним рядком: рядок
// обмежений THREAT_LIST_MAX_BYTES і не показує загрозу, що не влізла.
static_assert(sizeof(THREAT_NAMES) / sizeof(THREAT_NAMES[0]) <= 16, "threat_mask is uint16_t");
static uint16_t threatMask(const AlertRecord& rec) {
  const size_t n = sizeof(THREAT_NAMES) / sizeof(THREAT_NAMES[0]);
  uint16_t mask = 0;
  for (uint8_t i = 0; i < rec.threat_count; i++) {
    size_t bit = n - 1;   // "unknown" - останній запис таблиці
    for (size_t k = 0; k < n; k++) {
      if (strcmp(rec.threats[i], THREAT_NAMES[k].raw) == 0) { bit = k; break; }
    }
    mask |= (uint16_t)(1u << bit);
  }
  return mask;
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
      fetchDetailRecord(true);   // відмова лишає has_details == false -> звичайне повідомлення
      _detail_pending = false;
      xQueueOverwrite(_result_queue, &_pending_snap);
      continue;
    }

    // Планове перечитування деталей, поки триває тривога: перше через ~30 с після
    // переходу, далі раз на хвилину. Так само окремим кадром - TLS-сесії послідовні.
    if (_detail_sched && _task_state == STATE_ALERT && (long)(millis() - _detail_next_at) >= 0) {
      refreshDetails();
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

  // Перехід CLEAR->ALERT: перший детальний запит іде негайно, і знімок притримується
  // на один кадр, щоб тривога понесла з собою список загроз; усе інше йде одразу.
  // Після старту _task_state дорівнює UNKNOWN, тож тихий baseline першого запиту
  // не спричиняє (перше повідомлення чекати не мусить - його нема), але якщо
  // baseline - ALERT (ребут посеред тривоги), одразу плануємо перечитування, щоб
  // OLED дізнався рівень. Відбій зупиняє перечитування і скидає Last-Modified.
  if (snap.has_state) {
    bool transition_to_alert = (snap.state == STATE_ALERT && _task_state == STATE_CLEAR);
    bool baseline_alert = (snap.state == STATE_ALERT && _task_state == STATE_UNKNOWN);
    _task_state = snap.state;
    if (snap.state == STATE_CLEAR) {
      _detail_sched = false;
      _detail_last_modified[0] = 0;
      _last_read_level = ALERT_LEVEL_UNKNOWN;
    } else if (transition_to_alert || baseline_alert) {
      _detail_last_modified[0] = 0;   // перший запит нової тривоги завжди безумовний
      _last_read_level = ALERT_LEVEL_UNKNOWN;
      _detail_sched = true;
      _detail_recheck_pending = transition_to_alert;
      _detail_periodic_count = 0;
      _detail_next_at = millis() + (transition_to_alert ? ALERT_DETAIL_RECHECK_MS : 0);
    }
    if (transition_to_alert) {
      _pending_snap = snap;
      _detail_pending = true;
      return;   // покладе pollTaskLoop() після того, як відпрацює fetchDetailRecord()
    }
  }

  xQueueOverwrite(_result_queue, &snap);
}

// Один детальний запит: на переході CLEAR->ALERT (first == true, щоб внести список
// загроз у перше повідомлення) і для планових перечитувань під час тривоги.
// Усі шляхи відмови нефатальні за задумом: has_details лишається false, тож перше
// повідомлення відкочується на звичайне, а перечитування просто мовчить. Тривога
// виходить завжди - деталі це бонус, а не передумова.
AirRaidGateway::FetchResult AirRaidGateway::fetchDetailRecord(bool first) {
  _pending_snap.has_details = false;
  _pending_snap.level = ALERT_LEVEL_UNKNOWN;
  _pending_snap.threat_mask = 0;
  _pending_snap.from_primary = false;
  _pending_snap.threat_list[0] = 0;

  const unsigned long started_at = millis();
  const uint16_t timeout_ms = first ? ALERT_FIRST_DETAIL_TIMEOUT_MS : ALERT_DETAIL_HTTP_TIMEOUT_MS;
  const unsigned long deadline_ms = first ? ALERT_FIRST_DETAIL_DEADLINE_MS : ALERT_DETAIL_DEADLINE_MS;

  WiFiClientSecure client;
  client.setInsecure();   // TODO(v2): закріпити/перевіряти сертифікат alerts.in.ua

  HTTPClient http;
  http.setConnectTimeout(timeout_ms);
  http.setTimeout(timeout_ms);

  // Last-Modified потрібен, щоб наступний запит пішов з If-Modified-Since: сервер
  // (перевірено на живому ендпоінті) віддає 304 без тіла, поки документ не змінився.
  static const char* header_keys[] = { "Last-Modified" };
  http.collectHeaders(header_keys, 1);

  static const char* detail_url = "https://api.alerts.in.ua/v1/alerts/active.json";
  if (!http.begin(client, detail_url)) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: detail http.begin() failed");
    return FETCH_FAIL;
  }
  http.addHeader("Authorization", "Bearer " ALERTS_TOKEN);
  const bool sent_ims = _detail_last_modified[0] != 0;
  if (sent_ims) http.addHeader("If-Modified-Since", _detail_last_modified);

  int code = http.GET();

  // Навмисно НЕ чіпає ні _poll_interval_ms, ні http_code у знімку: 429 на цьому
  // запиті не має ні гальмувати 15-секундне опитування, ні показуватись як
  // "API err" на сторінці AIRRAID, яка стежить лише за основним ендпоінтом.
  if (code == 304) {
    http.end();
    return FETCH_NOT_MODIFIED;
  }
  if (code != 200) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: detail HTTP %d", code);
    http.end();
    return FETCH_FAIL;
  }

  alert_scanner.begin(started_at + deadline_ms);
  http.writeToStream(&alert_scanner);   // відʼємний результат очікуваний при достроковому обриві

  char last_modified[sizeof(_detail_last_modified)];
  last_modified[0] = 0;
  if (http.hasHeader("Last-Modified")) {
    StrHelper::strncpy(last_modified, http.header("Last-Modified").c_str(), sizeof(last_modified));
  }
  http.end();

  // Розбивка по трьох uid дійсна для цього сканування незалежно від того, чи знайшлося
  // хоч щось - виставляємо її одразу, щоб обидва шляхи нижче (успіх і "нічого не знайдено") могли її залогувати.
  _pending_snap.raion_level = alert_scanner.raionLevel();
  _pending_snap.hromada_level = alert_scanner.hromadaLevel();
  _pending_snap.city_level = alert_scanner.cityLevel();

  const AlertRecord* rec = alert_scanner.localResult();
  if (rec == NULL) rec = alert_scanner.fallbackResult();
  if (rec == NULL) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: detail - no air_raid record for uid %s/%s/%s or fallback %s in %u bytes"
                        " (raion=%s hromada=%s city=%s)",
                        ALERT_DETAIL_LOCATION_UID, ALERT_DETAIL_HROMADA_UID, ALERT_DETAIL_CITY_UID,
                        ALERT_DETAIL_FALLBACK_UID, (unsigned)alert_scanner.bytesScanned(),
                        dash(_pending_snap.raion_level), dash(_pending_snap.hromada_level), dash(_pending_snap.city_level));
    return FETCH_FAIL;
  }
  if (rec->level == ALERT_LEVEL_UNKNOWN) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: detail - record has no usable alert_level");
    return FETCH_FAIL;
  }

  _pending_snap.level = rec->level;
  _pending_snap.threat_mask = threatMask(*rec);
  _pending_snap.from_primary = alert_scanner.foundLocal();
  _pending_snap.sent_ims = sent_ims;
  renderThreatList(*rec, _pending_snap.threat_list, sizeof(_pending_snap.threat_list));
  _pending_snap.has_details = true;

  // Запамʼятовуємо Last-Modified лише після вдалого розбору з відомим рівнем: 304 на
  // відповідь, з якої ми нічого не вичитали, назавжди сховав би зміни. Немає заголовка
  // - наступний запит просто безумовний.
  // Виняток: рівень нижчий, ніж при попередньому читанні. Пониження йде в канал лише після
  // двох перечитувань ПОСПІЛЬ, а 304 знімка не дає, тож друге підтвердження могло б
  // зникнути. Порожній Last-Modified робить наступний запит безумовним, тобто реальним читанням.
  // Якщо попереднього читання не було (перший запит нової тривоги не вдався, і в канал пішло
  // звичайне 🔴), то воно для каналу рівнозначне червоному: жовте перше читання - теж пониження.
  const uint8_t prev_rank = (_last_read_level == ALERT_LEVEL_UNKNOWN) ? alertLevelRank(ALERT_LEVEL_RED)
                                                                     : alertLevelRank(_last_read_level);
  const bool lower_than_prev = alertLevelRank(rec->level) < prev_rank;
  _last_read_level = rec->level;
  StrHelper::strncpy(_detail_last_modified, lower_than_prev ? "" : last_modified,
                     sizeof(_detail_last_modified));

  // Перечитування логує один рядок у handleDetailRefresh() (разом із рішенням); тут лише перший запит.
  if (first) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: first detail ok (raion=%s hromada=%s city=%s -> %s%s, %u threats, %u bytes scanned, %s) -> '%s'",
                        dash(_pending_snap.raion_level), dash(_pending_snap.hromada_level), dash(_pending_snap.city_level),
                        levelName(rec->level), _pending_snap.from_primary ? "" : " (oblast fallback uid=" ALERT_DETAIL_FALLBACK_UID ")",
                        (unsigned)rec->threat_count, (unsigned)alert_scanner.bytesScanned(),
                        last_modified[0] ? "LM stored" : "no LM", _pending_snap.threat_list);
  }
  return FETCH_OK;
}

// Планове перечитування під час тривоги. Знімок у чергу лише за FETCH_OK: 304, помилка,
// таймаут чи 429 - тиша, стан ескалації не скидається. Швидке перечитування (~30 с після
// переходу) при невдачі повторюється один раз через ALERT_DETAIL_RETRY_MS.
void AirRaidGateway::refreshDetails() {
  memset(&_pending_snap, 0, sizeof(_pending_snap));
  _pending_snap.is_refresh = true;
  _pending_snap.wifi_connected = true;

  // Безумовний запит: швидке перечитування і кожне ALERT_DETAIL_FULL_EVERY_N-те планове.
  // Достатньо скинути Last-Modified - без нього fetchDetailRecord() не шле If-Modified-Since.
  // (Якщо швидке перечитування впаде, Last-Modified лишиться порожнім, і повторна спроба
  // теж безумовна.)
  bool unconditional = _detail_recheck_pending;
  if (!_detail_recheck_pending && (++_detail_periodic_count % ALERT_DETAIL_FULL_EVERY_N) == 0) {
    unconditional = true;
  }
  if (unconditional) _detail_last_modified[0] = 0;

  FetchResult r = fetchDetailRecord(false);

  unsigned long delay_ms = ALERT_DETAIL_REFRESH_MS;
  if (r == FETCH_OK) {
    xQueueOverwrite(_result_queue, &_pending_snap);   // рядок логу з рішенням - у handleDetailRefresh()
  } else if (r == FETCH_NOT_MODIFIED) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: refresh 304 not modified (cond) -> none");
  } else {
    MESH_DEBUG_PRINTLN("AirRaidGateway: refresh failed (%s) -> none, state kept",
                        unconditional ? "uncond" : "cond");
    if (_detail_recheck_pending) delay_ms = ALERT_DETAIL_RETRY_MS;
  }
  _detail_recheck_pending = false;
  _detail_next_at = millis() + delay_ms;
}

#endif
