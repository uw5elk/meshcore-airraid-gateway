#pragma once

#include "MyMesh.h"

#if defined(ESP32) && defined(WITH_AIR_RAID_GATEWAY)

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "AirRaidEscalation.h"   // AlertLevel + чиста логіка рішення про підвищення

class UITask;   // ui-new/UITask.h - лише попереднє оголошення, заголовок не тягнемо

// Розмір буфера (у байтах) для зібраного, перекладеного списку загроз без дублів
// ("дрони, ракети"): це НАЙБІЛЬШИЙ бюджет серед усіх типів повідомлень - для першого
// "ТРИВОГА" (75 Б). Реальний бюджет свій для кожного типу і рахується в AirRaidGateway.cpp
// (LIST_BUDGET_*) під час компіляції з MAX_TEXT_LEN, найдовшого імені вузла і REGION_NAME;
// перед відправкою список обрізається до нього (fitThreatList).
// Це жорстка стеля, а не питання стилю: BaseChatMesh::sendGroupMessage() обрізає весь
// текст по MAX_TEXT_LEN (160) *разом* із префіксом "<node_name>: ", і те обрізання -
// сирий різ по байтах, який розрубав би UTF-8 символ навпіл. Список завжди обрізається
// по цілих токенах, ніколи всередині токена.
#define THREAT_LIST_MAX_BYTES  75

// Опитує alerts.in.ua про стан повітряної тривоги для однієї локації і, коли
// стан змінюється (тривога <-> відбій), шле текст у груповий канал через
// MyMesh::injectChannelText().
//
// Сама робота з WiFi/HTTP виконується в окремій задачі FreeRTOS, прив'язаній
// до ядра 0 (там уже живе задача драйвера WiFi), щоб Arduino-шний loop()
// (mesh/UI/кнопка, прив'язаний до ядра 1) ніколи не блокувався в очікуванні
// TLS-хендшейку чи повільного HTTP-запиту. Задача віддає результати назад у
// loop() через чергу-"поштову скриньку" довжиною 1
// (xQueueOverwrite/xQueueReceive) - значення має лише найсвіжіший знімок, тож
// задача ніколи не чекає на повільного споживача. Об'єкти mesh/UI (MyMesh,
// UITask, стан Dispatcher-а і каналів, який вони чіпають) не є
// потокобезпечними, і сьогодні їх торкається лише головний потік, тому
// виклики injectChannelText() та UI відбуваються виключно з loop() після
// вичитування черги - ніколи з фонової задачі.
class AirRaidGateway {
public:
  void begin(MyMesh* mesh, UITask* ui = NULL);
  void loop();

  bool hasBaseline() const { return _state != STATE_UNKNOWN; }
  bool isAlertActive() const { return _state == STATE_ALERT; }
  AlertLevel getAlertLevel() const { return _level; }   // UNKNOWN, якщо немає активної тривоги з деталями
  bool isWifiConnected() const { return _wifi_connected_cached; }
  int getLastHttpCode() const { return _last_http_code; }
  long secondsSinceLastSuccess() const;   // -1, якщо успіху ще не було жодного разу
  UBaseType_t getPollTaskStackBytesFree() const {
    return _poll_task ? uxTaskGetStackHighWaterMark(_poll_task) : 0;
  }
  uint8_t getPollTaskStackPercentFree() const;   // 0-100, відносно ALERT_POLL_TASK_STACK

private:
  enum AlertState { STATE_UNKNOWN, STATE_CLEAR, STATE_ALERT };

  // Один "результат" на кожну ітерацію фонової задачі, кладеться в
  // _result_queue. Читає його лише loop() у головному потоці.
  struct PollSnapshot {
    bool has_state;        // true, якщо це опитування розібрало коректний ALERT/CLEAR
    AlertState state;      // дійсне лише за has_state
    bool has_http_result;  // true, якщо цього циклу справді робився HTTP-запит
    bool success;          // дійсне лише за has_http_result; true, якщо повернулось 200
    int http_code;         // дійсне лише за has_http_result
    bool wifi_connected;
    // Планове перечитування деталей під час тривоги: is_refresh=true, has_state=false,
    // has_http_result=false, has_details=true. Це окремий вид знімка, а не "стан ALERT" -
    // так запізнілий знімок перечитування не можна сприйняти за перехід CLEAR->ALERT.
    // Невдале перечитування знімка не створює зовсім: помилка = мовчання.
    bool is_refresh;
    // Деталі про загрози; заповнює fetchDetailRecord() на переході CLEAR->ALERT і
    // для знімків перечитування. На переході has_details == false означає "слати
    // звичайне повідомлення" - сюди веде кожен шлях відмови (таймаут, 429, помилка
    // розбору, немає запису, немає рівня).
    bool has_details;
    AlertLevel level;                             // дійсне лише за has_details; максимум серед знайдених район/громада/місто
    uint16_t threat_mask;                         // дійсне лише за has_details; біт на загрозу (THREAT_NAMES)
    bool from_primary;                            // дійсне лише за has_details; знайдено серед району/громади/міста, а не лише запасний запис області
    // Лише для налагоджувального рядка про кожне перечитування (звірка з офіційним
    // застосунком); на логіку не впливають. Дійсні лише за has_details.
    bool sent_ims;                                // запит ішов з If-Modified-Since
    AlertLevel raion_level, hromada_level, city_level;  // рівень кожного з трьох окремо; UNKNOWN = не знайдено
    char threat_list[THREAT_LIST_MAX_BYTES + 1];  // дійсне лише за has_details; може бути ""
  };

  // Результат одного детального запиту.
  enum FetchResult : uint8_t {
    FETCH_OK,             // запис знайдено, рівень відомий, _pending_snap заповнено
    FETCH_NOT_MODIFIED,   // 304: документ не змінився з минулого вдалого запиту
    FETCH_FAIL            // будь-яка відмова - тиша
  };

  MyMesh* _mesh = NULL;
  UITask* _ui = NULL;
  AlertState _state = STATE_UNKNOWN;
  AlertLevel _level = ALERT_LEVEL_UNKNOWN;          // ПОТОЧНИЙ рівень для екрана (з останнього успішного перечитування, може знижуватись); скидається на відбої
  char _threat_list[THREAT_LIST_MAX_BYTES + 1] = {0};
  AlertAnnounced _announced;                        // що вже відомо каналу; скидається на відбої
  unsigned long _last_success_at = 0;
  int _last_http_code = 0;
  bool _wifi_connected_cached = false;
  uint8_t _channel_idx = 0;   // визначає registerChannel(); при невдачі відкочується на Public (0)

  // Стан, з яким працює лише фонова задача (головний потік його не чіпає).
  unsigned long _next_poll_at = 0;
  unsigned long _poll_interval_ms = 0;
  unsigned long _last_wifi_reconnect_attempt = 0;
  uint32_t _poll_count_for_stack_log = 0;
  // Тінь _state, яку задача рухає за тими самими результатами розбору, щоб вона
  // могла сама помітити перехід CLEAR->ALERT і забрати деталі *до* того, як знімок
  // дійде до loop(). Єдиним джерелом істини про те, що справді відправлено,
  // лишається _state головного потоку.
  AlertState _task_state = STATE_UNKNOWN;
  bool _detail_pending = false;   // притримати _pending_snap на кадр, щоб забрати деталі
  PollSnapshot _pending_snap;     // поле, а не локальна змінна: тримає ~90 Б поза стеком задачі

  // Розклад перечитувань деталей, поки _task_state == ALERT (лише задача).
  bool _detail_sched = false;           // чи тривають планові перечитування
  unsigned long _detail_next_at = 0;
  bool _detail_recheck_pending = false; // наступне перечитування - швидка перевірка через ~30 с після переходу
  uint8_t _detail_periodic_count = 0;   // лічильник планових (60 с) перечитувань; кожне ALERT_DETAIL_FULL_EVERY_N-те безумовне
  AlertLevel _last_read_level = ALERT_LEVEL_UNKNOWN;   // рівень останнього вдалого читання цієї тривоги (лише задача)
  char _detail_last_modified[40] = {0}; // "Last-Modified" останньої вдалої відповіді; "" -> запит безумовний

  TaskHandle_t _poll_task = NULL;
  QueueHandle_t _result_queue = NULL;

  void registerChannel();
  void handleState(const PollSnapshot& snap);
  void handleDetailRefresh(const PollSnapshot& snap);
  void formatWhen(char* when, size_t sz);   // " HH:MM" або ""
  void sendChannelText(const char* text);

  static void pollTaskTrampoline(void* param);
  void pollTaskLoop();   // крутиться вічно у фоновій задачі
  void pollOnce();       // один HTTP GET + розбір, кладе PollSnapshot у чергу
  // Один детальний HTTP GET, потоково сканується; заповнює деталі в _pending_snap.
  // first == true: перший запит нової тривоги - жорсткіші таймаути, і повідомлення
  // про тривогу чекає на нього.
  FetchResult fetchDetailRecord(bool first);
  void refreshDetails();   // планове перечитування; кладе знімок у чергу лише за FETCH_OK
};

extern AirRaidGateway air_raid_gateway;

#endif
