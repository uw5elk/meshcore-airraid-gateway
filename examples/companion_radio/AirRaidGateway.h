#pragma once

#include "MyMesh.h"

#if defined(ESP32) && defined(WITH_AIR_RAID_GATEWAY)

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

class UITask;   // ui-new/UITask.h - forward decl only, kept out of the header

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
  };

  MyMesh* _mesh = NULL;
  UITask* _ui = NULL;
  AlertState _state = STATE_UNKNOWN;
  unsigned long _last_success_at = 0;
  int _last_http_code = 0;
  bool _wifi_connected_cached = false;
  uint8_t _channel_idx = 0;   // resolved by registerChannel(); falls back to Public (0) on failure

  // Background-task-only state (never touched from the main thread).
  unsigned long _next_poll_at = 0;
  unsigned long _poll_interval_ms = 0;
  unsigned long _last_wifi_reconnect_attempt = 0;
  uint32_t _poll_count_for_stack_log = 0;

  TaskHandle_t _poll_task = NULL;
  QueueHandle_t _result_queue = NULL;

  void registerChannel();
  void handleState(AlertState new_state);
  void sendChannelText(const char* text);

  static void pollTaskTrampoline(void* param);
  void pollTaskLoop();   // runs forever on the background task
  void pollOnce();       // one HTTP GET + parse, posts a PollSnapshot
};

extern AirRaidGateway air_raid_gateway;

#endif
