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
  // Кнопка, підпаяна навісом до GPIO4 на цій платі, дає помітно більший
  // дребезг контактів, ніж штатне посадкове місце кнопки на друкованій платі -
  // без цього одиночні натискання рахувались як DOUBLE/TRIPLE_CLICK. ~25 мс із
  // запасом більші за типовий дребезг (1-20 мс) і з запасом менші за людську
  // паузу між подвійним кліком (150-250 мс). Діє лише в цьому середовищі: усі
  // інші плати (і базове середовище цього ж варіанта) лишаються зі стандартним
  // для MomentaryButton debounce_ms=0.
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