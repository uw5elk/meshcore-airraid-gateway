#pragma once

#include <stdint.h>

// Чиста логіка рішення "що слати в канал про зміну рівня/загроз під час тривоги".
// Жодних залежностей від Arduino/FreeRTOS/mesh - лише stdint, тож її можна компілювати
// і перевіряти на хості (native) окремо від решти шлюзу.

// Рівень небезпеки активної тривоги, як його віддає поле "alert_level" детального
// ендпоінта. UNKNOWN означає, що детальний запит не вдався, не робився, або в
// записі не було придатного рівня - тоді повідомлення про тривогу відкочується
// до звичайної форми.
enum AlertLevel : uint8_t { ALERT_LEVEL_UNKNOWN = 0, ALERT_LEVEL_RED, ALERT_LEVEL_YELLOW };

// Числові значення enum-а не впорядковані за серйозністю (RED=1, YELLOW=2), тому
// порівнювати рівні можна тільки через цю функцію.
inline uint8_t alertLevelRank(AlertLevel l) {
  switch (l) {
    case ALERT_LEVEL_RED:    return 2;
    case ALERT_LEVEL_YELLOW: return 1;
    default:                 return 0;
  }
}

// Скільки перечитувань ПОСПІЛЬ мають показати нижчий рівень, щоб пониження пішло в канал.
// Підвищення йде одразу (недооцінка небезпеки гірша за запізніле "ЗНИЖЕНО"), а от
// пониження перед оголошенням підтверджуємо: перечитування може впіймати дані посеред
// оновлення, а хибне "тривога слабшає" людей заспокоює.
static const uint8_t ESC_LOWER_CONFIRMATIONS = 2;

// Що востаннє ОГОЛОШЕНО в каналі про поточну тривогу. Це не high-water mark: після
// оголошеного пониження воно дорівнює нижчому рівню, тож наступне підвищення оголошується
// знову. Маска загроз - це набір із останнього оголошеного повідомлення: зникнення загрози
// без зміни рівня нічого не оголошує і маску не міняє, а "нова загроза" - це біт, якого в
// цій масці не було.
struct AlertAnnounced {
  bool valid = false;         // false: після ребуту посеред тривоги базу ще не взято
  AlertLevel level = ALERT_LEVEL_UNKNOWN;   // UNKNOWN: пішло звичайне повідомлення без рівня (для каналу ≈ 🔴)
  uint16_t threat_mask = 0;   // біт на кожну загрозу з останнього оголошення (індекс у THREAT_NAMES)
  bool from_primary = false;  // true, якщо оголошене взято з об'єднаного район+громада+місто джерела, а не запасного (області)
  uint8_t lower_count = 0;    // скільки РАЙОННИХ перечитувань поспіль показали нижчий за оголошений рівень

  // Записує щойно оголошене (перше повідомлення нової тривоги, підвищення, пониження,
  // нова загроза). level UNKNOWN - якщо деталей не було.
  void adopt(AlertLevel lvl, uint16_t mask, bool primary) {
    valid = true;
    level = lvl;
    threat_mask = mask;
    from_primary = primary;
    lower_count = 0;
  }
};

enum EscalationAction : uint8_t {
  ESC_NONE = 0,       // у канал нічого
  ESC_SILENT,         // у канал нічого, але стан змінено: перші деталі після ребуту взято як базу
  ESC_SEND_RAISE,     // слати "ПІДВИЩЕНО": рівень зріс
  ESC_SEND_THREAT,    // слати "ПІДВИЩЕНО": рівень той самий, зʼявилась нова загроза
  ESC_SEND_LOWER,     // слати "ЗНИЖЕНО ... Тривога триває.": нижчий рівень підтверджено
  ESC_PENDING_LOWER,  // нижчий рівень побачено, але ще не підтверджено (a.lower_count з ESC_LOWER_CONFIRMATIONS)
  ESC_BLOCKED_LOWER   // нижчий рівень, але лише за записом області: знижувати можна тільки за записом району - у канал нічого
};

inline bool escalationSends(EscalationAction act) {
  return act == ESC_SEND_RAISE || act == ESC_SEND_THREAT || act == ESC_SEND_LOWER;
}

// Розглядає свіжі деталі активної тривоги і оновлює `a`.
//
//  - Базу після ребуту (a.valid == false) беремо мовчки.
//  - Запис області (запасний) не може змінити те, що вже оголошено за об'єднаним
//    район+громада+місто джерелом - ні підвищити, ні знизити.
//  - Знижувати можна ТІЛЬКИ за цим об'єднаним джерелом (from_primary): запис області може лише
//    підвищувати. Це стосується й оголошеного без рівня - обласний жовтий у канал не йде
//    (ESC_BLOCKED_LOWER; екран може показати жовтий). Таке читання обриває серію підтверджень.
//  - Якщо оголошене повідомлення було звичайним (без рівня): воно несло 🔴, тож перечитування
//    з червоним - це підвищення (слати одразу), а з жовтим за район/громада/місто - пониження.
//  - Рівень зріс: слати одразу.
//  - Рівень нижчий (за район/громада/місто): слати лише після ESC_LOWER_CONFIRMATIONS таких
//    перечитувань поспіль; будь-яке перечитування без пониження (у т.ч. обласне) обнуляє лічильник. Нової загрози в цей час не
//    шукаємо: якщо пониження підтвердиться, оголошення й так покаже весь поточний набір.
//  - Рівень той самий: слати, лише якщо зʼявилась загроза, якої не було в оголошеному наборі.
inline EscalationAction evaluateEscalation(AlertAnnounced& a, AlertLevel level, uint16_t mask,
                                           bool from_primary) {
  if (level == ALERT_LEVEL_UNKNOWN) return ESC_NONE;

  if (!a.valid) {
    a.adopt(level, mask, from_primary);
    return ESC_SILENT;
  }

  // "Два поспіль" - це два читання ЗА ОБ'ЄДНАНИМ РАЙОН+ГРОМАДА+МІСТО джерелом підряд без
  // проміжків: читання без жодного з трьох (лише обласне) обриває серію, що б із ним далі
  // не сталося. Невдалі запити і 304 знімка не дають, тож читаннями не є і серію не рвуть.
  if (!from_primary) a.lower_count = 0;

  if (a.from_primary && !from_primary) return ESC_NONE;

  // Оголошено звичайне повідомлення без рівня ("🔴 ПОВІТРЯНА ТРИВОГА"): для каналу воно
  // рівнозначне червоному. Червоне перечитування - це "невідомий -> червоний": оголошуємо
  // одразу. Жовте - це пониження щодо того 🔴, тож проходить те саме підтвердження.
  if (a.level == ALERT_LEVEL_UNKNOWN) {
    if (level == ALERT_LEVEL_RED) {
      a.adopt(level, mask, from_primary);
      return ESC_SEND_RAISE;
    }
    if (!from_primary) return ESC_BLOCKED_LOWER;   // область знижувати не може
    if (++a.lower_count < ESC_LOWER_CONFIRMATIONS) return ESC_PENDING_LOWER;
    a.adopt(level, mask, from_primary);
    return ESC_SEND_LOWER;
  }

  uint8_t now = alertLevelRank(level);
  uint8_t was = alertLevelRank(a.level);

  if (now > was) {
    a.adopt(level, mask, from_primary);
    return ESC_SEND_RAISE;
  }
  if (now < was) {
    if (!from_primary) return ESC_BLOCKED_LOWER;   // область знижувати не може
    if (++a.lower_count < ESC_LOWER_CONFIRMATIONS) return ESC_PENDING_LOWER;
    a.adopt(level, mask, from_primary);
    return ESC_SEND_LOWER;
  }

  a.lower_count = 0;
  if ((mask & (uint16_t)~a.threat_mask) != 0) {
    a.adopt(level, mask, from_primary);
    return ESC_SEND_THREAT;
  }
  return ESC_NONE;
}
