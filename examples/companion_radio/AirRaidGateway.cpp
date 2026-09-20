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

#define ALERT_POLL_INTERVAL_MS     15000    // alerts.in.ua hard limit: 12 req/min
#define ALERT_BACKOFF_MAX_MS      300000    // cap for 429 backoff
#define ALERT_HTTP_TIMEOUT_MS       5000
#define ALERT_WIFI_RETRY_MS        10000
#define ALERT_WIFI_DOWN_IDLE_MS      500    // task sleep between reconnect attempts while WiFi is down
#define ALERT_POLL_IDLE_MS           200    // task sleep between "not due yet" checks
#define ALERT_POLL_TASK_STACK      12288    // bytes; sized via uxTaskGetStackHighWaterMark() logging below
                                            // (was 10240; +2K headroom for the detail fetch's extra frames)

#define ALERT_POLL_TASK_CORE           0    // WiFi driver task already lives on core 0; keep loopTask (core 1) free
#define ALERT_STACK_LOG_EVERY_N_POLLS 20    // throttle MESH_DEBUG stack watermark logging

// ---- Threat-detail fetch (only fired on a CLEAR->ALERT transition) ----
#define ALERT_DETAIL_HTTP_TIMEOUT_MS  6000  // connect + read; keeps the alert's extra delay bounded
#define ALERT_DETAIL_DEADLINE_MS      8000  // hard wall-clock cap on the streamed scan
#define ALERT_DETAIL_MAX_BYTES    (64*1024) // hard cap on bytes consumed, in case the doc grows
#define MAX_THREATS                     8   // per record; extras are dropped
#define THREAT_TYPE_MAX                28   // "strategic_aircraft_activity" = 27 chars + NUL
#define JSON_TOKEN_MAX                 40   // longest string we care about; longer ones are truncated
#define JSON_KEY_MAX                   24

// Record we want ("Криворізький район"), and the oblast-level record we fall back
// to when it is absent. Matched against the "location_uid" string value.
#define DETAIL_TARGET_UID   "46"
#define DETAIL_FALLBACK_UID "9"

// Anything below this means NTP hasn't synced yet (fresh boot reads back an
// implausible epoch) - never print a bogus timestamp in an alert message.
#define NTP_READY_EPOCH_THRESHOLD  1700000000UL   // ~2023-11-14 UTC

// CMD_SEND_CHANNEL_TXT_MSG value (private #define in MyMesh.cpp:8, not exposed
// via MyMesh.h) - kept in sync manually since injectChannelText() must not change.
#define CMD_SEND_CHANNEL_TXT_MSG_VAL   3
#define TXT_TYPE_PLAIN_VAL             0

// Index 0 is always "Public" (added by MyMesh::begin() on every boot).
// We claim slot 1 for our own alert channel.
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
  _next_poll_at = millis();  // poll as soon as WiFi comes up
  registerChannel();

  // One-time, non-blocking kick-off from the main thread. From this point on,
  // WiFi.begin()/.disconnect()/.status() are only ever called from the
  // background poll task (see pollTaskLoop()) - exactly one thread owns the
  // WiFi connection lifecycle.
  WiFi.mode(WIFI_STA);
  WiFi.begin(GW_WIFI_SSID, GW_WIFI_PASS);
  MESH_DEBUG_PRINTLN("AirRaidGateway: connecting to WiFi '%s'...", GW_WIFI_SSID);

  // Kyiv local time (EET/EEST) for alert message timestamps, via NTP over our
  // own WiFi - independent of the mesh clock. getRTCClock() stays UTC, synced
  // from advert packets, and is still used only for the wire frame timestamp.
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

// ---- Main-thread side: drains the mailbox, does all Mesh/UI side effects ----

void AirRaidGateway::loop() {
  if (_result_queue == NULL) return;

  PollSnapshot snap;
  if (xQueueReceive(_result_queue, &snap, 0) != pdTRUE) return;   // nothing new - non-blocking

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
    _state = new_state;  // establish baseline silently, no message on boot
    MESH_DEBUG_PRINTLN("AirRaidGateway: baseline = %s", new_state == STATE_ALERT ? "ALERT" : "CLEAR");
    return;
  }
  if (new_state == _state) return;  // no change -> no message

  _state = new_state;

  if (new_state == STATE_ALERT && snap.has_details) {
    _level = snap.level;
    StrHelper::strncpy(_threat_list, snap.threat_list, sizeof(_threat_list));
  } else {
    // All-clear, or an alert whose detail fetch failed - either way, nothing to show.
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

  // " HH:MM", or "" while NTP hasn't synced yet (a fresh boot reads back an
  // implausible epoch - never print a bogus timestamp).
  char when[8];
  when[0] = 0;
  time_t t = time(nullptr);  // system time: NTP-synced, already Kyiv-local via configTzTime()
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
    // No usable detail - the plain message, exactly as before.
    snprintf(msg, sizeof(msg), "\xF0\x9F\x94\xB4 ПОВІТРЯНА ТРИВОГА — %s%s", REGION_NAME, when);
  } else {
    const char* emoji = (_level == ALERT_LEVEL_YELLOW) ? "\xF0\x9F\x9F\xA1"   // U+1F7E1
                                                       : "\xF0\x9F\x94\xB4";  // U+1F534
    if (_threat_list[0]) {
      snprintf(msg, sizeof(msg), "%s ТРИВОГА — %s%s (%s)", emoji, REGION_NAME, when, _threat_list);
    } else {
      // Level known but no threats listed - drop the empty parens.
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

// ---- Streaming JSON scanner for /v1/alerts/active.json ----
//
// That document is tens of KB - far too big for getString(). Instead we hand a
// Stream to HTTPClient::writeToStream(), which feeds us the body in ~1.4 KB chunks
// (from its own heap buffer) and, importantly, de-frames chunked transfer-encoding
// for us - reading getStreamPtr() raw would leave hex chunk headers embedded in the
// data. We abort the download by returning a short write once the record is found;
// HTTPClient then gives up with HTTPC_ERROR_STREAM_WRITE, which we treat as success.
//
// Nothing buffers a whole record. A byte-at-a-time tokenizer pulls the three fields
// we care about into a small per-record scratch as it passes them, and commits that
// scratch when the record's closing brace arrives - so a record with many threats
// can never overflow anything. Total state is ~600 B, in .bss, off the task stack.
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

  // NULL when neither the target nor the fallback record turned up.
  const AlertRecord* result() const {
    if (_have_primary) return &_primary;
    if (_have_fallback) return &_fallback;
    return NULL;
  }
  bool foundPrimary() const { return _have_primary; }
  uint32_t bytesScanned() const { return _total; }

  // Print/Stream plumbing. Only write() does real work; this is a sink, never a source.
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, size_t len) override {
    if (_abort) return 0;   // already done - short write tells HTTPClient to stop
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
    if (_tok_len < JSON_TOKEN_MAX - 1) _tok[_tok_len++] = c;   // longer strings are truncated, not overflowed
  }

  void addThreat(const char* type) {
    if (_scratch.threat_count >= MAX_THREATS) return;
    for (uint8_t i = 0; i < _scratch.threat_count; i++) {      // cheap raw-level dedupe
      if (strcmp(_scratch.threats[i], type) == 0) return;
    }
    StrHelper::strncpy(_scratch.threats[_scratch.threat_count], type, THREAT_TYPE_MAX);
    _scratch.threat_count++;
  }

  // A completed string that turned out to be a value (not a key).
  void onValue(const char* val) {
    if (!_in_record) return;
    if (strcmp(_key, "location_uid") == 0) {
      StrHelper::strncpy(_uid, val, sizeof(_uid));
    } else if (strcmp(_key, "alert_level") == 0) {
      if (strcmp(val, "red") == 0) _scratch.level = ALERT_LEVEL_RED;
      else if (strcmp(val, "yellow") == 0) _scratch.level = ALERT_LEVEL_YELLOW;
      // anything else (incl. a JSON null, which never reaches here) leaves it UNKNOWN
    } else if (strcmp(_key, "threat_type") == 0 || strcmp(_key, "threats") == 0) {
      // Tolerates both shapes: [{"threat_type":"drones"}] and the flat ["drones"].
      // In the flat case _key stays "threats" across the whole array.
      addThreat(val);
    }
  }

  void closeRecord() {
    _in_record = false;
    if (strcmp(_uid, DETAIL_TARGET_UID) == 0) {
      _primary = _scratch;
      _have_primary = true;
      _abort = true;   // got what we came for - stop the download here
    } else if (!_have_fallback && strcmp(_uid, DETAIL_FALLBACK_UID) == 0) {
      _fallback = _scratch;
      _have_fallback = true;   // keep scanning; the target record may still be ahead
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

    // A string is only classifiable once we see what follows it: ':' makes it a key,
    // anything else makes it a value. Resolve that here, then fall through so the
    // current char still gets its normal structural handling.
    if (_pending_string) {
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return;   // still undecided
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
        // The very first brace is the document root, not a record. Every other object
        // opening while we are not already inside one starts a record - which works for
        // both {"alerts":[...]} and a bare top-level [...] array.
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

static AlertJsonScanner alert_scanner;   // .bss, not the task stack

// alerts.in.ua threat_type -> short Ukrainian label. Unmapped types fall back to
// "невідомо" so an API addition can never produce an empty or raw-English entry.
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

// Renders "дрони, ракети" into out, deduped on the *translated* label (several raw
// types can collapse onto "невідомо"). A label that would not fit is skipped whole -
// never cut - so the result can never end mid-UTF-8-sequence and show up as mojibake
// in the app. Skipping rather than stopping lets a shorter later label still make it in.
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
    if (used + need + 1 > out_sz) continue;   // does not fit whole -> skip it entirely

    if (sep) { out[used++] = ','; out[used++] = ' '; }
    strcpy(&out[used], uk);
    used += strlen(uk);
    picked[n_picked++] = uk;
  }
}

// ---- Background-task side: WiFi + HTTP only, never touches _mesh/_ui ----

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
      // WiFi died between the poll and the detail fetch. The alert must still go out,
      // so release it now without detail rather than holding it until WiFi returns.
      if (_detail_pending) {
        MESH_DEBUG_PRINTLN("AirRaidGateway: WiFi lost before detail fetch - sending plain alert");
        _detail_pending = false;
        _pending_snap.wifi_connected = false;
        xQueueOverwrite(_result_queue, &_pending_snap);
      } else {
        // Only post the plain "WiFi is down" snapshot when there is no held alert -
        // the mailbox holds exactly one entry, so posting both would drop the alert.
        PollSnapshot snap;
        memset(&snap, 0, sizeof(snap));
        xQueueOverwrite(_result_queue, &snap);
      }
      vTaskDelay(pdMS_TO_TICKS(ALERT_WIFI_DOWN_IDLE_MS));
      continue;
    }

    // Detail runs in its own frame, after pollOnce() has returned and its
    // stack-local WiFiClientSecure/HTTPClient have been destructed - two TLS
    // sessions must never be alive at the same time on this heap.
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
  client.setInsecure();   // TODO(v2): pin/verify alerts.in.ua cert

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
    _poll_interval_ms = ALERT_POLL_INTERVAL_MS;  // clear any backoff
    snap.success = true;

    // Body is a JSON string literal, e.g. "   NNN...A...N" - strip the surrounding quotes
    // (if present) so index 0 of the content lines up with UID 0. Verified against live data:
    // UID 9 (Dnipropetrovsk oblast) and UID 279 (Kryvyi Rih) both matched known live state at
    // this 0-based offset.
    int start = 0;
    int content_len = (int)body.length();
    if (content_len >= 2 && body[0] == '"' && body[content_len - 1] == '"') {
      start = 1;
      content_len -= 2;
    }

    if (content_len <= ALERTS_UID) {
      // Truncated/short/unexpected response - do NOT report a state, so the main
      // thread never treats this as a false all-clear (or false alert).
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

  snap.wifi_connected = true;  // we only get here when WiFi.status() == WL_CONNECTED

  // Stack sizing aid: throttled so it doesn't spam serial every 15s. Once a
  // safe/comfortable ALERT_POLL_TASK_STACK is picked from real readings, this
  // logging (and the counter) can be dropped.
  if ((++_poll_count_for_stack_log % ALERT_STACK_LOG_EVERY_N_POLLS) == 1) {
    UBaseType_t words_free = uxTaskGetStackHighWaterMark(NULL);
    MESH_DEBUG_PRINTLN("AirRaidGateway: poll task stack high-water mark = %u bytes free (of %d)",
                        (unsigned)words_free, ALERT_POLL_TASK_STACK);
  }

  // A CLEAR->ALERT transition is the only thing that earns a detail fetch. Hold the
  // snapshot back for one frame so the alert can carry the threat list; everything
  // else goes straight out. _task_state is UNKNOWN on the first reading after boot,
  // so the silent baseline never triggers a fetch.
  if (snap.has_state) {
    bool transition_to_alert = (snap.state == STATE_ALERT && _task_state == STATE_CLEAR);
    _task_state = snap.state;
    if (transition_to_alert) {
      _pending_snap = snap;
      _detail_pending = true;
      return;   // posted by pollTaskLoop() once fetchDetails() has run
    }
  }

  xQueueOverwrite(_result_queue, &snap);
}

// One extra request, fired only on a CLEAR->ALERT transition, to put the threat list
// into the alert message. Every failure path here is non-fatal by design: has_details
// stays false and handleState() falls back to the plain message. The alert always goes
// out - detail is a bonus, never a precondition.
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

  // Deliberately does NOT touch _poll_interval_ms or the snapshot's http_code: a 429
  // on this occasional request must not throttle the 15s bulk poll, and must not show
  // up as "API err" on the AIRRAID page, which tracks the bulk endpoint only.
  if (code != 200) {
    MESH_DEBUG_PRINTLN("AirRaidGateway: detail HTTP %d - plain alert", code);
    http.end();
    return;
  }

  alert_scanner.begin(millis() + ALERT_DETAIL_DEADLINE_MS);
  http.writeToStream(&alert_scanner);   // negative return is expected when we abort early
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
