#pragma once

#include "MyMesh.h"

#if defined(ESP32) && defined(WITH_AIR_RAID_GATEWAY)

class UITask;   // ui-new/UITask.h - forward decl only, kept out of the header

// Polls alerts.in.ua for a single region's air-raid status and injects a
// group-channel text message via MyMesh::injectChannelText() whenever the
// state changes (alert <-> all-clear). Non-blocking between polls; the GET
// itself is a bounded (5s timeout) synchronous call.
class AirRaidGateway {
public:
  void begin(MyMesh* mesh, UITask* ui = NULL);
  void loop();

  bool hasBaseline() const { return _state != STATE_UNKNOWN; }
  bool isAlertActive() const { return _state == STATE_ALERT; }
  bool isWifiConnected() const;
  int getLastHttpCode() const { return _last_http_code; }
  long secondsSinceLastSuccess() const;   // -1 if never succeeded yet

private:
  enum AlertState { STATE_UNKNOWN, STATE_CLEAR, STATE_ALERT };

  MyMesh* _mesh = NULL;
  UITask* _ui = NULL;
  AlertState _state = STATE_UNKNOWN;
  unsigned long _next_poll_at = 0;
  unsigned long _poll_interval_ms = 0;
  unsigned long _last_wifi_reconnect_attempt = 0;
  unsigned long _last_success_at = 0;
  int _last_http_code = 0;
  uint8_t _channel_idx = 0;   // resolved by registerChannel(); falls back to Public (0) on failure

  void registerChannel();
  void poll();
  void handleState(AlertState new_state);
  void sendChannelText(const char* text);
};

extern AirRaidGateway air_raid_gateway;

#endif
