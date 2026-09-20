#pragma once

#include "MyMesh.h"

#if defined(ESP32) && defined(WITH_AIR_RAID_GATEWAY)

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

class UITask;   // ui-new/UITask.h - forward decl only, kept out of the header

// Severity of an active alert, as reported by the detail endpoint's "alert_level"
// field. UNKNOWN means the detail request failed, was skipped, or the record had
// no usable level - in that case the alert message degrades to the plain form.
enum AlertLevel : uint8_t { ALERT_LEVEL_UNKNOWN = 0, ALERT_LEVEL_RED, ALERT_LEVEL_YELLOW };

// Byte budget for the rendered, translated, deduped threat list ("дрони, ракети").
// Hard ceiling, not a style choice: BaseChatMesh::sendGroupMessage() truncates the
// whole text at MAX_TEXT_LEN (160) *including* the "<node_name>: " prefix, and that
// truncation is a raw byte cut that would split a UTF-8 char in half. Worst case:
// 52 B base message + 75 B list + 33 B prefix (31-char name) = 160 B exactly.
// The list is always trimmed on whole tokens, never mid-token, so a short list can
// never end up as mojibake.
#define THREAT_LIST_MAX_BYTES  75

// Polls alerts.in.ua for a single region's air-raid status and injects a
// group-channel text message via MyMesh::injectChannelText() whenever the
// state changes (alert <-> all-clear).
//
// The actual WiFi/HTTP work runs on a dedicated FreeRTOS task pinned to core 0
// (the core the WiFi driver task already lives on), so the Arduino loop()
// (mesh/UI/button, pinned to core 1) is never blocked waiting on a TLS
// handshake or a slow HTTP round-trip. The task hands results back to
// loop() via a length-1 "mailbox" queue (xQueueOverwrite/xQueueReceive) -
// only the latest snapshot ever matters, so the task never blocks on a slow
// consumer. Mesh/UI objects (MyMesh, UITask, the Dispatcher/channel state
// they touch) are not thread-safe and are only ever touched from the main
// thread today, so injectChannelText()/UI calls happen exclusively from
// loop() after draining the queue - never from the background task.
class AirRaidGateway {
public:
  void begin(MyMesh* mesh, UITask* ui = NULL);
  void loop();

  bool hasBaseline() const { return _state != STATE_UNKNOWN; }
  bool isAlertActive() const { return _state == STATE_ALERT; }
  AlertLevel getAlertLevel() const { return _level; }   // UNKNOWN unless an alert is active with details
  bool isWifiConnected() const { return _wifi_connected_cached; }
  int getLastHttpCode() const { return _last_http_code; }
  long secondsSinceLastSuccess() const;   // -1 if never succeeded yet
  UBaseType_t getPollTaskStackBytesFree() const {
    return _poll_task ? uxTaskGetStackHighWaterMark(_poll_task) : 0;
  }
  uint8_t getPollTaskStackPercentFree() const;   // 0-100, relative to ALERT_POLL_TASK_STACK

private:
  enum AlertState { STATE_UNKNOWN, STATE_CLEAR, STATE_ALERT };

  // One "result" produced per background-task iteration and posted to
  // _result_queue. Consumed only by loop() on the main thread.
  struct PollSnapshot {
    bool has_state;        // true if this poll parsed a valid ALERT/CLEAR
    AlertState state;      // valid only if has_state
    bool has_http_result;  // true if an HTTP request was actually attempted this cycle
    bool success;          // valid only if has_http_result; true if it returned 200
    int http_code;         // valid only if has_http_result
    bool wifi_connected;
    // Threat detail, filled by fetchDetails() only on a CLEAR->ALERT transition.
    // has_details == false means "send the plain alert message" - every failure
    // path (timeout, 429, parse error, record missing, level missing) lands here.
    bool has_details;
    AlertLevel level;                             // valid only if has_details
    char threat_list[THREAT_LIST_MAX_BYTES + 1];  // valid only if has_details; may be ""
  };

  MyMesh* _mesh = NULL;
  UITask* _ui = NULL;
  AlertState _state = STATE_UNKNOWN;
  AlertLevel _level = ALERT_LEVEL_UNKNOWN;          // set on CLEAR->ALERT, cleared on all-clear
  char _threat_list[THREAT_LIST_MAX_BYTES + 1] = {0};
  unsigned long _last_success_at = 0;
  int _last_http_code = 0;
  bool _wifi_connected_cached = false;
  uint8_t _channel_idx = 0;   // resolved by registerChannel(); falls back to Public (0) on failure

  // Background-task-only state (never touched from the main thread).
  unsigned long _next_poll_at = 0;
  unsigned long _poll_interval_ms = 0;
  unsigned long _last_wifi_reconnect_attempt = 0;
  uint32_t _poll_count_for_stack_log = 0;
  // Shadow of _state, advanced by the task from the same parse results, so the task
  // can spot a CLEAR->ALERT transition itself and fetch detail *before* the snapshot
  // reaches loop(). The main thread's _state stays the single source of truth for
  // what was actually sent.
  AlertState _task_state = STATE_UNKNOWN;
  bool _detail_pending = false;   // hold _pending_snap back for one frame to fetch detail
  PollSnapshot _pending_snap;     // a member, not a local: keeps ~90 B off the task stack

  TaskHandle_t _poll_task = NULL;
  QueueHandle_t _result_queue = NULL;

  void registerChannel();
  void handleState(const PollSnapshot& snap);
  void sendChannelText(const char* text);

  static void pollTaskTrampoline(void* param);
  void pollTaskLoop();   // runs forever on the background task
  void pollOnce();       // one HTTP GET + parse, posts a PollSnapshot
  void fetchDetails();   // one extra HTTP GET, streamed+scanned; fills _pending_snap
};

extern AirRaidGateway air_raid_gateway;

#endif
