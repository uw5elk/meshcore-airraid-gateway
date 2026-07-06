#pragma once

#include "MyMesh.h"

#if defined(ESP32) && defined(WITH_AIR_RAID_GATEWAY)

// Polls alerts.in.ua for a single region's air-raid status and injects a
// group-channel text message via MyMesh::injectChannelText() whenever the
// state changes (alert <-> all-clear). Non-blocking between polls; the GET
// itself is a bounded (5s timeout) synchronous call.
class AirRaidGateway {
public:
  void begin(MyMesh* mesh);
  void loop();

private:
  enum AlertState { STATE_UNKNOWN, STATE_CLEAR, STATE_ALERT };

  MyMesh* _mesh = NULL;
  AlertState _state = STATE_UNKNOWN;
  unsigned long _next_poll_at = 0;
  unsigned long _poll_interval_ms = 0;
  unsigned long _last_wifi_reconnect_attempt = 0;

  void poll();
  void handleState(AlertState new_state);
  void sendChannelText(const char* text);
};

#endif
