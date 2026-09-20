#pragma once

#include "MyMesh.h"

#if defined(ESP32) && defined(WITH_AIR_RAID_GATEWAY)

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

class UITask;   // ui-new/UITask.h - лише попереднє оголошення, заголовок не тягнемо

// Рівень небезпеки активної тривоги, як його віддає поле "alert_level" детального
// ендпоінта. UNKNOWN означає, що детальний запит не вдався, не робився, або в
// записі не було придатного рівня - тоді повідомлення про тривогу відкочується
// до звичайної форми.
enum AlertLevel : uint8_t { ALERT_LEVEL_UNKNOWN = 0, ALERT_LEVEL_RED, ALERT_LEVEL_YELLOW };

// Бюджет у байтах на зібраний, перекладений список загроз без дублів ("дрони, ракети").
// Це жорстка стеля, а не питання стилю: BaseChatMesh::sendGroupMessage() обрізає весь
// текст по MAX_TEXT_LEN (160) *разом* із префіксом "<node_name>: ", і те обрізання -
// сирий різ по байтах, який розрубав би UTF-8 символ навпіл. Найгірший випадок:
// 52 Б базового повідомлення + 75 Б списку + 33 Б префікса (ім'я на 31 символ) = рівно 160 Б.
// Список завжди обрізається по цілих токенах, ніколи всередині токена, тож короткий
// список не може перетворитись на кракозябри.
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
    // Деталі про загрози; заповнює fetchDetails() лише на переході CLEAR->ALERT.
    // has_details == false означає "слати звичайне повідомлення" - сюди веде
    // кожен шлях відмови (таймаут, 429, помилка розбору, немає запису, немає рівня).
    bool has_details;
    AlertLevel level;                             // дійсне лише за has_details
    char threat_list[THREAT_LIST_MAX_BYTES + 1];  // дійсне лише за has_details; може бути ""
  };

  MyMesh* _mesh = NULL;
  UITask* _ui = NULL;
  AlertState _state = STATE_UNKNOWN;
  AlertLevel _level = ALERT_LEVEL_UNKNOWN;          // ставиться на CLEAR->ALERT, скидається на відбої
  char _threat_list[THREAT_LIST_MAX_BYTES + 1] = {0};
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

  TaskHandle_t _poll_task = NULL;
  QueueHandle_t _result_queue = NULL;

  void registerChannel();
  void handleState(const PollSnapshot& snap);
  void sendChannelText(const char* text);

  static void pollTaskTrampoline(void* param);
  void pollTaskLoop();   // крутиться вічно у фоновій задачі
  void pollOnce();       // один HTTP GET + розбір, кладе PollSnapshot у чергу
  void fetchDetails();   // ще один HTTP GET, потоково сканується; заповнює _pending_snap
};

extern AirRaidGateway air_raid_gateway;

#endif
