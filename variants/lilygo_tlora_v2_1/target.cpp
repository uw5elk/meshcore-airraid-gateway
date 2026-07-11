#include <Arduino.h>
#include "target.h"

LilyGoTLoraBoard board;

static SPIClass spi;
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_0, P_LORA_RESET, P_LORA_DIO_1, spi);

WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);
EnvironmentSensorManager sensors;

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
#ifndef PIN_USER_BTN_PULLUP
  #define PIN_USER_BTN_PULLUP false
#endif
#ifdef WITH_AIR_RAID_GATEWAY
  // This board's ad-hoc GPIO4 button wiring shows noticeably more mechanical
  // contact bounce than a native PCB button footprint - without this, single
  // clicks were being counted as DOUBLE/TRIPLE_CLICK. ~25ms is comfortably
  // above typical bounce (1-20ms) and comfortably below human double-click
  // gaps (150-250ms). Scoped to this env only: every other board (and this
  // variant's own base env) keeps MomentaryButton's default debounce_ms=0.
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true, PIN_USER_BTN_PULLUP, true, 25);
#else
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true, PIN_USER_BTN_PULLUP);
#endif
#endif

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);

#if defined(P_LORA_SCLK)
  return radio.std_init(&spi);
#else
  return radio.std_init();
#endif
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}