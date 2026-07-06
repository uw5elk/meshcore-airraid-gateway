#include "AirRaidGateway.h"

#if defined(ESP32) && defined(WITH_AIR_RAID_GATEWAY)

#include "AirRaidGatewayConfig.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <RTClib.h>
#include <Utils.h>
#include <helpers/TxtDataHelpers.h>

#define ALERT_POLL_INTERVAL_MS     15000    // alerts.in.ua hard limit: 12 req/min
#define ALERT_BACKOFF_MAX_MS      300000    // cap for 429 backoff
#define ALERT_HTTP_TIMEOUT_MS       5000
#define ALERT_WIFI_RETRY_MS        10000

// CMD_SEND_CHANNEL_TXT_MSG value (private #define in MyMesh.cpp:8, not exposed
// via MyMesh.h) - kept in sync manually since injectChannelText() must not change.
#define CMD_SEND_CHANNEL_TXT_MSG_VAL   3
#define TXT_TYPE_PLAIN_VAL             0

// Index 0 is always "Public" (added by MyMesh::begin() on every boot).
// We claim slot 1 for our own alert channel.
#define AIR_RAID_CHANNEL_SLOT          1

void AirRaidGateway::begin(MyMesh* mesh) {
  _mesh = mesh;
  _state = STATE_UNKNOWN;
  _poll_interval_ms = ALERT_POLL_INTERVAL_MS;
  _next_poll_at = millis();  // poll as soon as WiFi comes up
  registerChannel();

  WiFi.mode(WIFI_STA);
  WiFi.begin(GW_WIFI_SSID, GW_WIFI_PASS);
  MESH_DEBUG_PRINTLN("AirRaidGateway: connecting to WiFi '%s'...", GW_WIFI_SSID);
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
  memcpy(desired.channel.secret, psk, sizeof(psk));  // remaining bytes stay 0 -> 128-bit key

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

void AirRaidGateway::loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - _last_wifi_reconnect_attempt > ALERT_WIFI_RETRY_MS) {
      MESH_DEBUG_PRINTLN("AirRaidGateway: WiFi down, reconnecting...");
      WiFi.disconnect();
      WiFi.begin(GW_WIFI_SSID, GW_WIFI_PASS);
      _last_wifi_reconnect_attempt = millis();
    }
    return;
  }

  if ((long)(millis() - _next_poll_at) < 0) return;  // not due yet
  _next_poll_at = millis() + _poll_interval_ms;

  poll();
}

void AirRaidGateway::poll() {
  WiFiClientSecure client;
  client.setInsecure();   // TODO(v2): pin/verify alerts.in.ua cert

  HTTPClient http;
  http.setConnectTimeout(ALERT_HTTP_TIMEOUT_MS);
  http.setTimeout(ALERT_HTTP_TIMEOUT_MS);

  char url[96];
  snprintf(url, sizeof(url), "https://api.alerts.in.ua/v1/iot/active_air_raid_alerts/%d.json", ALERTS_UID);

  if (!http.begin(client, url)) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: http.begin() failed");
    return;
  }
  http.addHeader("Authorization", "Bearer " ALERTS_TOKEN);

  int code = http.GET();

  if (code == 200) {
    String body = http.getString();
    _poll_interval_ms = ALERT_POLL_INTERVAL_MS;  // clear any backoff

    AlertState new_state = STATE_UNKNOWN;
    for (size_t i = 0; i < body.length(); i++) {
      char c = body[i];
      if (c == 'A' || c == 'P') { new_state = STATE_ALERT; break; }
      if (c == 'N') { new_state = STATE_CLEAR; break; }
    }

    if (new_state == STATE_UNKNOWN) {
      MESH_DEBUG_PRINTLN("AirRaidGateway: unrecognised response: %s", body.c_str());
    } else {
      handleState(new_state);
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
}

void AirRaidGateway::handleState(AlertState new_state) {
  if (_state == STATE_UNKNOWN) {
    _state = new_state;  // establish baseline silently, no message on boot
    MESH_DEBUG_PRINTLN("AirRaidGateway: baseline = %s", new_state == STATE_ALERT ? "ALERT" : "CLEAR");
    return;
  }
  if (new_state == _state) return;  // no change -> no message

  _state = new_state;

  DateTime dt(_mesh->getRTCClock()->getCurrentTime());
  char msg[96];
  if (new_state == STATE_ALERT) {
    snprintf(msg, sizeof(msg), "\xF0\x9F\x94\xB4 ПОВІТРЯНА ТРИВОГА — %s %02d:%02d", REGION_NAME, dt.hour(), dt.minute());
  } else {
    snprintf(msg, sizeof(msg), "\xF0\x9F\x9F\xA2 Відбій — %s %02d:%02d", REGION_NAME, dt.hour(), dt.minute());
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

#endif
