#pragma once

// ============================================================================
// esp32_loraprs_e22 — MeshCore variant for a DIY ESP32-DEV + E22 (SX1268)
// board built according to sh123/esp32_loraprs (variants/esp32dev_e22).
//
// This file is documentation only (nothing here is #included by the build —
// MeshCore's ESP32 target picks up pins from the -D build_flags in
// platformio.ini, not from this header). It exists so the real wiring is
// recorded next to the variant, same as MeshCore's own variants/generic-e22.
//
// Source of these pin numbers: sh123/esp32_loraprs,
// variants/esp32dev_e22/variant.h (default config for the E22/SX1268 build).
// ============================================================================

// Radio: EBYTE E22-400M30S/33S module, SX1268 chip
#define USE_SX1268

// SPI + control pins (from esp32_loraprs CFG_LORA_PIN_* defaults)
#define LORA_NSS   5   // CFG_LORA_PIN_NSS
#define LORA_RESET 27  // CFG_LORA_PIN_RST (early esp32_loraprs boards used 26 — verify against YOUR schematic)
#define LORA_DIO1  12  // CFG_LORA_PIN_DIO1
#define LORA_BUSY  14  // CFG_LORA_PIN_BUSY
#define LORA_RXEN  32  // CFG_LORA_PIN_RXEN
#define LORA_TXEN  33  // CFG_LORA_PIN_TXEN

// ASSUMPTION, not confirmed from esp32_loraprs source: esp32_loraprs does not
// override SPI SCK/MISO/MOSI, so it likely relies on the ESP32 Arduino core's
// default VSPI pins. Verify against extras/schematics in the esp32_loraprs
// repo (or your own build notes) BEFORE flashing — if these are wrong, the
// radio will fail to initialise.
#define LORA_SCK  18
#define LORA_MISO 19
#define LORA_MOSI 23

// Misc, from esp32_loraprs defaults
#define LED_PIN        2   // BUILTIN_LED (heartbeat)
#define BATTERY_PIN    36  // CFG_TLM_BAT_MON_PIN (correction factor in esp32_loraprs was 0.37 —
                            // MeshCore's ESP32Board applies a fixed x2 divider instead, so treat
                            // the reported battery voltage as approximate until you recalibrate)

// No OLED / GPS / user button assumed on this board. If your physical build
// does have one of these, add the matching defines here (see other MeshCore
// variants, e.g. variants/heltec_v3/variant.h, for the macro names) and wire
// the corresponding build_src_filter / build_flags lines into platformio.ini.
