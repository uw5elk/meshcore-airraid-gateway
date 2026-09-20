#pragma once

// ============================================================================
// esp32_loraprs_e22 — варіант MeshCore для саморобної плати ESP32-DEV + E22
// (SX1268), зібраної за схемою sh123/esp32_loraprs (variants/esp32dev_e22).
//
// Цей файл суто довідковий: збірка його не підключає — ESP32-ціль MeshCore
// бере піни з прапорців -D у platformio.ini, а не з цього заголовка. Він
// існує, щоб реальна розводка була записана поруч із варіантом, так само як
// це зроблено у власному variants/generic-e22 самого MeshCore.
//
// Джерело цих номерів пінів: sh123/esp32_loraprs,
// variants/esp32dev_e22/variant.h (типова конфігурація збірки з E22/SX1268).
// ============================================================================

// Радіо: модуль EBYTE E22-400M30S/33S, чіп SX1268
#define USE_SX1268

// Піни SPI та керування (з типових значень CFG_LORA_PIN_* в esp32_loraprs)
#define LORA_NSS   5   // CFG_LORA_PIN_NSS
#define LORA_RESET 27  // CFG_LORA_PIN_RST (на ранніх платах esp32_loraprs був 26 — звірте зі СВОЄЮ схемою)
#define LORA_DIO1  12  // CFG_LORA_PIN_DIO1
#define LORA_BUSY  14  // CFG_LORA_PIN_BUSY
#define LORA_RXEN  32  // CFG_LORA_PIN_RXEN
#define LORA_TXEN  33  // CFG_LORA_PIN_TXEN

// ПРИПУЩЕННЯ, не підтверджене вихідним кодом esp32_loraprs: він не
// перевизначає SPI SCK/MISO/MOSI, тож, найпевніше, покладається на типові піни
// VSPI з ядра Arduino для ESP32. Звірте зі схемою в extras/schematics
// репозиторію esp32_loraprs (або з власними нотатками щодо монтажу) ПЕРЕД
// прошивкою — якщо ці піни неправильні, радіо не проініціалізується.
#define LORA_SCK  18
#define LORA_MISO 19
#define LORA_MOSI 23

// Різне, з типових значень esp32_loraprs
#define LED_PIN        2   // BUILTIN_LED (світлодіод "серцебиття")
#define BATTERY_PIN    36  // CFG_TLM_BAT_MON_PIN (в esp32_loraprs був коефіцієнт корекції 0.37 —
                            // MeshCore в ESP32Board застосовує фіксований дільник x2, тож показану
                            // напругу батареї вважайте приблизною, доки не відкалібруєте)

// Дисплея, GPS і кнопки користувача на цій платі не передбачено. Якщо на вашій
// збірці щось із цього фізично є, додайте сюди відповідні визначення (назви
// макросів дивіться в інших варіантах MeshCore, напр. variants/heltec_v3/variant.h)
// і пропишіть відповідні рядки build_src_filter / build_flags у platformio.ini.
