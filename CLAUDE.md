# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

MeshCore is a portable C++ library for multi-hop LoRa packet routing on embedded devices (ESP32, nRF52, RP2040, STM32). `src/` is the core protocol library; `examples/` contains the firmware applications built on top of it; `variants/` contains per-board PlatformIO configs that combine a board + an example into a flashable firmware.

## Build system

This is a PlatformIO project (`platformio.ini` at the root, extended by every `variants/*/platformio.ini`). There is no CMake/Makefile workflow — always go through `pio`.

Build a specific firmware target (env names follow `<Board>_<example>[_<variant>]`, e.g. `Heltec_v3_repeater`, `RAK_4631_companion_radio_ble`):
```
pio run -e Heltec_v3_repeater
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

- `pr-build-check.yml` compiles a representative matrix of environments (ESP32-S3, nRF52, RP2040, STM32, ESP32-C6, SX1276) on every PR touching `src/`, `examples/`, `variants/`, or `platformio.ini`. If you change core code, sanity-check against at least one board of each MCU family when possible.
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

## Current task: air-raid alert gateway

Air-raid alert gateway on top of `companion_radio`, targeting a LilyGo T3 LoRa32 v1.6.1 board (SX1276 radio, no TCXO), running as a Companion-role node (not repeater/room-server).

**Alert source:** polls alerts.in.ua, endpoint `https://api.alerts.in.ua/v1/iot/active_air_raid_alerts/279.json` (uid 279 = Kryvyi Rih city + Kryvyi Rih hromada). Response is a single-char JSON string: `"A"`/`"P"` = alert, `"N"` = all-clear. Poll interval 15s (service hard limit is 12 req/min).

**Implemented and committed:**
- `MyMesh::injectChannelText(const uint8_t* frame, size_t len)` (`examples/companion_radio/MyMesh.h`/`.cpp`, commit `4451966b`) — clamps `len` to `MAX_FRAME_SIZE`, copies into `cmd_frame`, invokes `handleCmdFrame(len)`. Injection point for pushing a group-channel text alert without going through the normal serial/BLE frame path. Do not modify this method.
- `examples/companion_radio/AirRaidGateway.h`/`.cpp` (commit `8ccb4822`) — polls the endpoint above over `WiFiClientSecure`/`HTTPClient`, dedupes on state change only, stays silent on the first reading after boot, backs off (doubling, capped at 5 min) on HTTP 429, logs on HTTP 401.
- **Dedicated channel, not Public** (commit `a3c9aaab`): the gateway registers its own group channel, `"Тривога KR"`, at slot **1** (slot 0 stays `"Public"`, always re-added by `MyMesh::begin()` on every boot) — see `AirRaidGateway::registerChannel()`. The 16-byte PSK is baked into `AirRaidGatewayConfig.h` as hex (`CHANNEL_PSK_HEX`) and converted via `mesh::Utils::fromHex()`, then written directly via the public `BaseChatMesh::setChannel()` API (same mechanism the wire protocol's `CMD_SET_CHANNEL` uses — raw secret bytes, not base64). Registration happens in memory only, every boot (idempotent check against the existing slot first) — **not persisted to flash**, since `MyMesh::saveChannels()` is private and this was a deliberate choice to avoid touching `MyMesh.h`. `sendChannelText()` targets the resolved `_channel_idx`, not a hardcoded index.
- `examples/companion_radio/AirRaidGatewayConfig.h` — config header: `GW_WIFI_SSID`/`GW_WIFI_PASS` (deliberately not `WIFI_SSID`/`WIFI_PASS`, since `main.cpp` already uses `WIFI_SSID`/`WIFI_PWD` to switch the companion serial protocol itself onto WiFi/TCP transport), `ALERTS_TOKEN`, `ALERTS_UID=279`, `REGION_NAME="Кривий Ріг"`, `CHANNEL_NAME="Тривога KR"`, `CHANNEL_PSK_HEX`. **This file is in `.gitignore` and must never be committed** — it holds the real alerts.in.ua Bearer token, WiFi credentials, and the channel PSK. Verified: never appears in git history on any branch.
- **OLED status page** (commit `fa6806e2`): a new `HomePage::AIRRAID` page in `examples/companion_radio/ui-new/UITask.cpp`'s `HomeScreen` (feature-flagged like the existing GPS/SENSORS pages) shows title `"Tryvoga KR"`, big state text `"TRYVOGA"` (red) / `"VIDBIY"` (green), plus `API: Xs ago` (or the last HTTP error code), `WiFi: OK`/`---`, and battery voltage. Latin, not Cyrillic, on purpose — the OLED's default Adafruit_GFX font has no Cyrillic glyphs (confirmed via `DisplayDriver::translateUTF8ToBlocks()`, which replaces any non-ASCII byte with a solid block char). On every alert/all-clear transition, `AirRaidGateway` also fires a ~5s `UITask::showAlert("TRYVOGA"/"VIDBIY", 5000)` toast, for the same font reason. The group-channel message text (via `injectChannelText`) is unaffected — it still goes out in Cyrillic with emoji, since that's rendered on the phone/desktop app, not this OLED. New getters on `AirRaidGateway` (`hasBaseline()`, `isAlertActive()`, `isWifiConnected()`, `getLastHttpCode()`, `secondsSinceLastSuccess()`) back the page; `begin()` now optionally takes a `UITask*` (passed from `main.cpp` as `&ui_task` under `#ifdef DISPLAY_CLASS`).
- `main.cpp` changes wiring `AirRaidGateway::begin()`/`loop()` into `setup()`/`loop()`, gated behind `#if defined(ESP32) && defined(WITH_AIR_RAID_GATEWAY)` — doesn't affect any of the other 100+ companion_radio build environments.
- Build env `LilyGo_TLora_V2_1_1_6_airraid` (`variants/lilygo_tlora_v2_1/platformio.ini`) — extends the existing `LilyGo_TLora_V2_1_1_6` base section (same SX1276 chip/pins), USB companion variant (no `BLE_PIN_CODE`, no `WIFI_SSID`). Radio set via build flags: `LORA_FREQ=433.650`, `LORA_BW=62.5`, `LORA_SF=8`, `LORA_CR=8` (power stays at the base section's inherited `LORA_TX_POWER=20`), plus `-D WITH_AIR_RAID_GATEWAY` to enable the feature.
- Pushed to a personal fork remote named `mine` (`github.com/uw5elk/meshcore-airraid-gateway`, branch `main`). `origin` remains the upstream `meshcore-dev/MeshCore` repo and was never pushed to.

**Build status: SUCCESS.** The user built `LilyGo_TLora_V2_1_1_6_airraid` manually in a separate VS Code terminal (where PlatformIO is installed — `pio` is not available in this Claude Code terminal, so the assistant cannot re-verify this directly) and confirmed it compiles clean with the final config (real token/WiFi credentials, channel PSK, and radio params 433.650/62.5/8/8) and the OLED status page changes. Treat the firmware as build-verified and ready to flash.

**Next step:** physically flash the T3 v1.6.1 board over USB with the built `LilyGo_TLora_V2_1_1_6_airraid` firmware, confirm the OLED status page and channel show up correctly, and validate against a real alert/all-clear cycle from alerts.in.ua.

## Contribution conventions (from CONTRIBUTING.md)

- Target the `dev` branch for PRs, not `main`.
- No dynamic memory allocation outside of setup/`begin()` functions — this is embedded, keep it concise, avoid unnecessary abstraction layers.
- One feature/fix per PR.
- If you change public API, update `README.md`.
