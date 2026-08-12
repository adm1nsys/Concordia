/*
 * In-memory log ring, so the panel and the USB console can both show what
 * happened without anyone holding a serial cable at the right moment — plus the
 * two filters that decide what is worth keeping.
 *
 * ## Levels
 *
 * 0 off · 1 errors · 2 + warnings · 3 + info · 4 + debug · 5 everything,
 * which also mirrors the log to the card. Anything above the configured level
 * is dropped before it costs a string.
 *
 * ## Categories
 *
 * A level alone is all-or-nothing: turning the noise down loses the one line
 * you were watching for. So each message also belongs to a category that can be
 * switched off on its own — chase the link without the web server's chatter, or
 * the other way round.
 *
 * The category is taken from the `[tag]` every message already starts with,
 * rather than from a new argument at hundreds of call sites. Messages without a
 * recognised tag fall into `sys`, which is never a surprise: they are the
 * board's own lifecycle lines.
 */
#pragma once

#include <Arduino.h>

namespace Log {

enum Level : uint8_t {
  kOff = 0,
  kError = 1,
  kWarn = 2,
  kInfo = 3,
  kDebug = 4,
  kEverything = 5,
};

/* Bit per category. The order here is the order the panel shows them in. */
enum Cat : uint16_t {
  kSys = 1 << 0,
  kLink = 1 << 1,
  kNet = 1 << 2,
  kWeb = 1 << 3,
  kFs = 1 << 4,
  kSd = 1 << 5,
  kApi = 1 << 6,
  kAuth = 1 << 7,
  kOta = 1 << 8,
  kAlert = 1 << 9,
  kAllCats = 0x03ff,
};

/* Must run before anything logs: it also makes USB writes non-blocking. */
void begin();

/* Reads /Concordia/this/logging.json. Call once the card is up — before that
 * the defaults apply, so early boot lines are never lost to a setting that has
 * not been read yet. */
void loadConfig();
bool saveConfig();

void configure(uint8_t level, uint16_t categories);
uint8_t level();
uint16_t categories();

/* Names in bit order, for the settings screen. */
uint8_t catCount();
const char *catName(uint8_t index);

/* Info level. The category comes from the message's own `[tag]`. */
void add(const String &line);
void addf(const char *fmt, ...);

/* Same, with the level stated. Use kDebug for anything that would be noise at
 * ordinary settings. */
void at(uint8_t level, const String &line);
void atf(uint8_t level, const char *fmt, ...);

/* Newest first, '\n' separated. */
String dump();
void clear();

} // namespace Log
