# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

MeshCore is a portable C++ library for multi-hop LoRa packet routing on embedded devices (ESP32, nRF52, RP2040, STM32). `src/` is the core protocol library; `examples/` contains the firmware applications built on top of it; `variants/` contains per-board PlatformIO configs that combine a board + an example into a flashable firmware.

**This repo is a fork.** On top of upstream MeshCore it adds an air-raid alert gateway (see "The air-raid gateway" below). Upstream's own README is preserved as `README-meshcore.md`; the root `README.md` documents the fork.

## Build system

This is a PlatformIO project (`platformio.ini` at the root, extended by every `variants/*/platformio.ini`). There is no CMake/Makefile workflow — always go through `pio`.

Build a specific firmware target (env names follow `<Board>_<example>[_<variant>]`, e.g. `Heltec_v3_repeater`, `RAK_4631_companion_radio_ble`):
```
pio run -e Heltec_v3_repeater
```

The two envs that matter in this fork:
```
pio run -e LilyGo_TLora_V2_1_1_6_airraid              # the air-raid gateway
pio run -e Esp32_loraprs_E22_companion_radio_ble      # plain BT companion on DIY E22 hardware
```

List all available environments (there are 100+, one per board/example combo):
```
pio project config | grep 'env:'
```

`build.sh` is a convenience wrapper used mainly by CI/releases:
```
sh build.sh list                                  # list all firmware targets
sh build.sh build-firmware RAK_4631_repeater       # build one target
sh build.sh build-matching-firmwares RAK_4631      # build all targets matching a substring
sh build.sh build-companion-firmwares              # build all companion firmwares
sh build.sh build-repeater-firmwares               # build all repeater firmwares
sh build.sh build-room-server-firmwares            # build all room-server firmwares
```
`DISABLE_DEBUG=1` strips debug logging flags (`MESH_DEBUG`, `MESH_PACKET_LOGGING`) from the build.

### Tests

Unit tests run on the `native` platform (host machine, not embedded), via GoogleTest. Test sources live in `test/`; only `src/Utils.cpp` plus test files are compiled in (see `build_src_filter` for `env:native` in `platformio.ini`) — the native env does not build the full mesh/radio stack.
```
pio test -e native -vv
```
To run a single test file, add `-f <pattern>`, e.g. `pio test -e native -f test_tohex`.

There is no lint/format check step in CI — `.clang-format` exists (2-space indent, 110 col limit) but is **not** auto-applied. Do not reformat existing code you didn't write; it creates noisy diffs (explicitly called out in CONTRIBUTING.md).

### CI

- `pr-build-check.yml` compiles a representative matrix of environments (ESP32-S3, nRF52, RP2040, STM32, ESP32-C6, SX1276) on every PR touching `src/`, `examples/`, `variants/`, or `platformio.ini`. Neither of this fork's two envs is in that matrix — build them locally before pushing.
- `run-unit-tests.yml` runs `pio test -e native -vv`.

## Architecture

### Layered core (`src/`)

```
Dispatcher (Dispatcher.h/.cpp)   generic send/receive queue + retry/backoff engine, radio-agnostic
   -> Mesh (Mesh.h/.cpp)          understands Packet payload types, routing (flood vs direct vs transport), ACKs
        -> BaseChatMesh (helpers/BaseChatMesh.h/.cpp)   contact/identity/channel abstractions, text messaging, login/ANON_REQ flows
             -> MyMesh (per-example, e.g. examples/companion_radio/MyMesh.h)   concrete app: wires up serial/BLE frame protocol, CLI, or repeater/room-server behavior
```
Everything is virtual-method extension point based: `Radio`, `RTCClock`, `MainBoard`, `MeshTables`, `BaseSerialInterface` etc. are abstract interfaces (in `MeshCore.h`, `Mesh.h`, `helpers/*.h`) implemented per-board/platform. Adding support for new hardware means implementing these interfaces, not touching the core routing logic.

`Packet` (`src/Packet.h`) is the wire unit: a `header` byte (route type + payload type + version bits), path bytes, and an encrypted payload. `PAYLOAD_TYPE_*` constants define the payload kinds (advert, text msg, group text/data, ACK, ANON_REQ, trace, control, etc.) — see `docs/packet_format.md` and `docs/payloads.md` for the on-wire layout.

### Examples (`examples/`)

Each example is a self-contained firmware `main.cpp` + a `MyMesh` subclass that specializes `BaseChatMesh`/`Mesh` for a role:
- `companion_radio` — pairs with the mobile/desktop apps over BLE/USB/Wi-Fi using a binary command-frame protocol (`docs/companion_protocol.md`). `MyMesh::handleCmdFrame()` in `examples/companion_radio/MyMesh.cpp` is the central command dispatcher — one big `if/else if` chain keyed on `cmd_frame[0]` (`CMD_*` codes). Has three swappable UI implementations (`ui-new`, `ui-orig`, `ui-tiny`) selected via `build_src_filter` per variant.
- `simple_repeater` — forwards flood/direct packets to extend range; supports optional bridges (RS232/ESPNow, see `helpers/bridges/`) to link separate mesh networks.
- `simple_room_server` — BBS-style server that stores posts for offline pickup (`docs/terminal_chat_cli.md` covers the CLI).
- `simple_sensor` — telemetry-emitting node using `helpers/SensorManager.h` and `TimeSeriesData`.
- `simple_secure_chat` — terminal chat client, driven over the Serial Monitor.
- `kiss_modem` — bridges the mesh to KISS-protocol host applications (`docs/kiss_modem_protocol.md`).

### Board wiring (`variants/`, `boards/`, `arch/`)

Each `variants/<name>/platformio.ini` defines a base board section (pins, radio chip `-D RADIO_CLASS=...`, SPI mapping) and then one `[env:...]` per example it supports, layering `build_src_filter` to pull in the right `examples/<x>` sources plus board-specific helpers (display driver, sensors, bridges). `boards/*.json` are PlatformIO board defs for hardware not upstreamed to PlatformIO itself; `arch/{esp32,stm32}/` hold vendored/patched libraries needed only on those MCUs (e.g. LittleFS port, AsyncElegantOTA).

### Key size/config constants

Frame and packet size limits (`MAX_FRAME_SIZE`, `MAX_PACKET_PAYLOAD`, `MAX_PATH_SIZE`, contact/channel counts like `MAX_CONTACTS`/`MAX_GROUP_CHANNELS`) are set via `-D` build flags per variant/env, not hardcoded — check the relevant `platformio.ini` env before assuming a value.

Radio parameters (`LORA_FREQ`, `LORA_BW`, `LORA_SF`, `LORA_CR`, `LORA_TX_POWER`) work the same way. On `companion_radio` these are only the first-boot defaults — the paired phone/desktop app writes them into the stored prefs, so a companion build does not need them pinned per variant.

## The air-raid gateway

An air-raid alert gateway layered on top of `companion_radio`, running as a Companion-role node (not repeater/room-server). User-facing docs are in the root `README.md`; this section covers what a code change needs to know.

**Target hardware:** LilyGo T3 LoRa32 v1.6.1 (SX1276, no TCXO), 433 MHz.

**Alert source:** the bulk endpoint `https://api.alerts.in.ua/v1/iot/active_air_raid_alerts.json` — note there is **no uid in the path** (the older per-region `.../active_air_raid_alerts/<uid>.json` form is what returns 404 now). The response body is a single JSON string with one character per location (`N` = clear, `A`/`P` = alert). `ALERTS_UID` is therefore a **0-based index into that string**, not a URL component; `pollOnce()` strips the surrounding quotes before indexing. Known-good values: `279` = Kryvyi Rih city/hromada, `9` = Dnipropetrovsk oblast. Auth is `Authorization: Bearer <token>`; the service documents only 401 (bad/expired token) and 429 (rate limit). Poll interval 15 s (service cap is 12 req/min), doubling backoff capped at 5 min on 429.

**Threading model.** `AirRaidGateway` runs the WiFi/HTTP work on its own FreeRTOS task pinned to **core 0** (where the WiFi driver already lives), so a TLS handshake never blocks `loopTask` on core 1. The two threads communicate **only** through a length-1 mailbox queue (`xQueueOverwrite`/`xQueueReceive`) carrying a `PollSnapshot`. Mesh and UI objects are not thread-safe: `injectChannelText()`, `UITask` calls and all `_state` handling happen exclusively on the main thread, from `loop()`, after draining the queue. **Keep this split** — do not touch `_mesh`/`_ui` from `pollTaskLoop()`/`pollOnce()`.

**Key pieces:**
- `MyMesh::injectChannelText(const uint8_t*, size_t)` (`examples/companion_radio/MyMesh.h`/`.cpp`) — clamps `len` to `MAX_FRAME_SIZE`, copies into `cmd_frame`, calls `handleCmdFrame(len)`. Injection point for pushing a group-channel text alert without going through the serial/BLE frame path.
- `examples/companion_radio/AirRaidGateway.h`/`.cpp` — polling, state dedupe (message only on change), silent baseline on the first reading after boot.
- **Dedicated channel, not Public.** `registerChannel()` writes the channel at slot **1** (slot 0 is always `"Public"`, re-added by `MyMesh::begin()` every boot) via the public `BaseChatMesh::setChannel()`. The 16-byte PSK comes from `CHANNEL_PSK_HEX` through `mesh::Utils::fromHex()`. Registration is **in-memory only, re-done every boot** (idempotent check first) — deliberately not persisted, to avoid touching the private `MyMesh::saveChannels()`.
- `examples/companion_radio/AirRaidGatewayConfig.h` — `GW_WIFI_SSID`/`GW_WIFI_PASS` (deliberately *not* `WIFI_SSID`/`WIFI_PWD`, which `main.cpp` already uses to move the companion protocol itself onto WiFi/TCP), `ALERTS_TOKEN`, `ALERTS_UID`, `REGION_NAME`, `CHANNEL_NAME`, `CHANNEL_PSK_HEX`. **In `.gitignore`, must never be committed** — holds the real token, WiFi credentials and channel PSK. Verified absent from git history on all branches.
- **OLED pages** (`ui-new/UITask.cpp`): `HomePage::AIRRAID` (state, seconds since last successful poll or last HTTP error, WiFi, battery) and `HomePage::DIAG` (uptime, free heap/stack, unread count), both feature-flagged like the existing GPS/SENSORS pages. Text is Latin on purpose — `DisplayDriver::translateUTF8ToBlocks()` replaces any non-ASCII byte with a block glyph, so Cyrillic is unrenderable in the default Adafruit_GFX font. The channel message itself stays Cyrillic (rendered by the phone/desktop app, not the OLED).
- `main.cpp` wires `begin()`/`loop()` in, gated behind `#if defined(ESP32) && defined(WITH_AIR_RAID_GATEWAY)`, so the other 100+ companion_radio envs are unaffected.
- `MomentaryButton` gained an optional trailing `debounce_ms` constructor arg (default `0` = previous behaviour for every other board); the gateway uses ~25 ms.

**Build env:** `LilyGo_TLora_V2_1_1_6_airraid` in `variants/lilygo_tlora_v2_1/platformio.ini`, extending the existing `LilyGo_TLora_V2_1_1_6` base. Sets `LORA_FREQ=433.650`, `LORA_BW=62.5`, `LORA_SF=8`, `LORA_CR=8` (TX power 20 inherited), `PIN_USER_BTN=4` + `PIN_USER_BTN_PULLUP=true`, and `-D WITH_AIR_RAID_GATEWAY`. `MESH_DEBUG` and `MESH_PACKET_LOGGING` are intentionally **off** — there are `; NOTE: DO NOT ENABLE` markers on them; re-enable only for a bring-up session and turn them back off before pushing.

**Status:** working on hardware — boots clean, registers the channel, connects to WiFi, polls the API, and delivers alerts to the channel.

### Strapping-pin trap (resolved, do not regress)

The board booted straight into CLI Rescue (`SPIFFS: mount failed, -10025` then `========= CLI Rescue =========`) whenever `PIN_USER_BTN` was GPIO0 or GPIO12. Both are ESP32 **strapping pins** (GPIO0 = BOOT, GPIO12 = MTDI/VDD_SDIO flash voltage select): an external pull during reset corrupts flash-voltage strapping *and* reads as a held button inside `UITask`'s 8-second rescue window. `enterCLIRescue()` is purely button-driven — there is no boot-loop or watchdog auto-rescue anywhere in this codebase. GPIO36 also failed, because `MomentaryButton` enables no internal pull unless asked, so an unwired pin floats. Resolved by moving the button to **GPIO4** with `PIN_USER_BTN_PULLUP=true`. `PIN_USER_BTN=-1` is the codebase convention for "no button" if one is ever needed.

## The `esp32_loraprs_e22` variant

A second, unrelated variant: `variants/esp32_loraprs_e22/` targets a DIY ESP32-DEV + EBYTE E22 (SX1268) board wired per [sh123/esp32_loraprs](https://github.com/sh123/esp32_loraprs) (`variants/esp32dev_e22`). Its `target.h`/`target.cpp` are copies of upstream's `variants/generic-e22` (the pins come from build flags, so the glue is generic).

**It builds a stock BT Companion — `WITH_AIR_RAID_GATEWAY` is deliberately not set**, so no alert polling and no AIRRAID/DIAG pages (that board has no display).

Pins were traced against the esp32_loraprs KiCad schematic (`extras/schematics/esp32dev/lora_tracker.sch` + netlist): RESET/DIO1/BUSY/RXEN/TXEN/MOSI are hard-wired, while **NSS/SCK/MISO route through solder jumpers JP1/JP2/JP3** (the project's 36-pin vs 38-pin board option). The configured 5/18/19 is the standard-ESP32-VSPI side of those jumpers. If the radio fails to initialise on a given board, check which side is soldered before suspecting anything else. Note also that sh123 ships its own MeshCore variant for this board in `extras/meshcore/loraprs_esp32dev_e22/` — useful as a cross-reference.

## Known issues / open work

Found in review, not yet fixed. Ranked roughly by consequence:

1. **A state change can be lost permanently.** `AirRaidGateway::handleState()` commits `_state` *before* `sendChannelText()`, and the send result is discarded (`injectChannelText()` returns `void`). If `sendGroupMessage()` fails — e.g. the packet pool is exhausted during congestion — the alert is never transmitted and never retried. Fix: track an `_announced_state` separately and only advance it on a confirmed send. `getChannel()` and `sendGroupMessage()` are both public on `BaseChatMesh`, so `sendChannelText()` can call them directly and get the `bool`.
2. **Quote-strip fallback can shift the index by one.** In `pollOnce()`, a body that starts with `"` but does not *end* with `"` (trailing newline, or a truncated read) leaves `start = 0`, so the character for the neighbouring location is read — a silent, permanent wrong-region state, possibly a false all-clear. Should fail closed and ignore the response instead.
3. **TLS certificate validation is disabled** (`client.setInsecure()`, already marked `TODO(v2)`). On a safety feed this lets an on-path attacker forge an all-clear. Wants `setCACert()` with a pinned root.
4. **Injection emits an unsolicited `RESP_CODE_OK`** into the companion protocol stream, because it goes through `handleCmdFrame()`, whose `CMD_SEND_CHANNEL_TXT_MSG` branch ends in `writeOKFrame()`. An alert firing mid-`CMD_GET_CONTACTS` desyncs request/response for the app. The fix for (1) removes this too.
5. **An alert spanning a reboot is never announced** — the first reading after boot is always silent, so a device that restarts mid-alert only ever sends the eventual all-clear.
6. **No staleness alarm.** If polling dies permanently (expired token, AP gone), `_state` freezes and the channel goes quiet — which subscribers read as "no alert". Consider a periodic "gateway offline" message.
7. Smaller: `xQueueCreate()`'s return is not checked before the task is started; a malformed `CHANNEL_PSK_HEX` silently falls back to the **Public** channel (alerts would go out unencrypted); `http.getString()` allocates a `String` every 15 s, against CONTRIBUTING's "no dynamic allocation outside setup"; `msg[96]` has only a few bytes of headroom and would truncate mid-UTF-8 with a longer `REGION_NAME`; `ui-new/UITask.cpp` guards on `WITH_AIR_RAID_GATEWAY` alone while the class is declared under `defined(ESP32) && ...`.

Verified **not** problems: the two-thread split is clean (every member is either main-thread-only, task-only, or seeded before the task starts); `millis()` rollover is handled correctly everywhere in `AirRaidGateway`; `sendChannelText()`'s buffer arithmetic cannot overflow; `cmd_frame` has no re-entrancy risk (`handleCmdFrame` only ever runs on `loopTask`); the button debounce cannot change behaviour for any other board.

## Contribution conventions (from CONTRIBUTING.md)

- Target the `dev` branch for PRs, not `main`.
- No dynamic memory allocation outside of setup/`begin()` functions — this is embedded, keep it concise, avoid unnecessary abstraction layers.
- One feature/fix per PR.
- If you change public API, update `README.md`.
