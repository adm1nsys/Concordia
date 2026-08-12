#include "log_buf.h"

#include <SD.h>
#include <stdarg.h>

namespace {

const uint8_t kLines = 80;
String buffer[kLines];
uint8_t head = 0; /* next slot to write */
uint8_t stored = 0;

uint8_t gLevel = Log::kInfo;
uint16_t gCats = Log::kAllCats;

const char *const kConfigFile = "/Concordia/this/logging.json";
const char *const kMirrorFile = "/Concordia/logs/hub.log";
const char *const kMirrorOld = "/Concordia/logs/hub.1.log";
const uint32_t kMirrorRotateAt = 128 * 1024;

/* Bit order must match the enum, because the panel indexes by position. */
const char *const kCatNames[] = {"sys", "link", "net",  "web", "fs",
                                 "sd",  "api",  "auth", "ota", "alert"};
const uint8_t kCatTotal = sizeof(kCatNames) / sizeof(kCatNames[0]);

String stamp() {
  const unsigned long ms = millis();
  char out[16];
  snprintf(out, sizeof(out), "%lu.%03lu", ms / 1000, ms % 1000);
  return String(out);
}

/* Reads the leading `[tag]`. Anything unrecognised is the board talking about
 * itself, which is what `sys` means. */
uint16_t catOf(const String &line) {
  if (!line.startsWith("[")) {
    return Log::kSys;
  }
  const int close = line.indexOf(']');
  if (close < 2 || close > 12) {
    return Log::kSys;
  }
  const String tag = line.substring(1, close);
  for (uint8_t i = 0; i < kCatTotal; ++i) {
    if (tag == kCatNames[i]) {
      return (uint16_t)(1 << i);
    }
  }
  /* Aliases for tags that read better in a log than as a category name. */
  if (tag == "warn") {
    return Log::kAlert;
  }
  return Log::kSys;
}

/* Only at the highest setting, and capped: a chatty log on a card that fills up
 * is an outage of its own making. */
void mirror(const String &entry) {
  if (gLevel < Log::kEverything) {
    return;
  }
  File f = SD.open(kMirrorFile, FILE_APPEND);
  if (!f) {
    return;
  }
  if (f.size() > kMirrorRotateAt) {
    f.close();
    SD.remove(kMirrorOld);
    SD.rename(kMirrorFile, kMirrorOld);
    f = SD.open(kMirrorFile, FILE_APPEND);
    if (!f) {
      return;
    }
  }
  f.println(entry);
  f.close();
}

void store(uint8_t lvl, const String &line) {
  if (gLevel == Log::kOff || lvl > gLevel) {
    return;
  }
  if ((catOf(line) & gCats) == 0) {
    return;
  }

  const String entry = stamp() + "  " + line;
  buffer[head] = entry;
  head = (head + 1) % kLines;
  if (stored < kLines) {
    ++stored;
  }
  Serial.println(entry);
  mirror(entry);
}

} // namespace

namespace Log {

void begin() {
  /* The USB-Serial-JTAG peripheral keeps the port enumerated whether or not a
   * host is draining it. With the default timeout every Serial.print() then
   * blocks for 100 ms once the FIFO fills, and a panic message - which the
   * handler prints before rebooting - blocks forever. That is exactly how the
   * previous firmware died: no output, no reboot, off the air until someone
   * pulled the cable.
   *
   * Zero never waits, but it also throws away anything that does not fit, and
   * a 25-line `list` came out with its tail missing. A small non-zero limit
   * keeps long output intact while still capping the worst case at 25 ms per
   * write, which no amount of unread USB can turn into a hang.
   */
  Serial.setTxTimeoutMs(25);
  Serial.setDebugOutput(false);
  for (uint8_t i = 0; i < kLines; ++i) {
    buffer[i] = String();
  }
  head = 0;
  stored = 0;
}

/* Deliberately forgiving: a missing or unreadable settings file leaves the
 * defaults in place rather than turning logging off, because the one moment you
 * need the log is when something is already wrong. */
void loadConfig() {
  File f = SD.open(kConfigFile, FILE_READ);
  if (!f) {
    return;
  }
  const String body = f.readString();
  f.close();

  const int lvlAt = body.indexOf("\"level\"");
  const int catAt = body.indexOf("\"cats\"");
  if (lvlAt < 0 || catAt < 0) {
    return;
  }
  const long lvl = body.substring(body.indexOf(':', lvlAt) + 1).toInt();
  const long cats = body.substring(body.indexOf(':', catAt) + 1).toInt();
  if (lvl < 0 || lvl > kEverything) {
    return;
  }
  gLevel = (uint8_t)lvl;
  gCats = (uint16_t)(cats & kAllCats);
  addf("[sys] logging level %u, categories 0x%03x", gLevel, gCats);
}

bool saveConfig() {
  File f = SD.open(kConfigFile, FILE_WRITE);
  if (!f) {
    return false;
  }
  f.printf("{\"level\":%u,\"cats\":%u}\n", gLevel, gCats);
  f.close();
  return true;
}

void configure(uint8_t lvl, uint16_t cats) {
  gLevel = lvl > kEverything ? kEverything : lvl;
  gCats = cats & kAllCats;
}

uint8_t level() { return gLevel; }
uint16_t categories() { return gCats; }

uint8_t catCount() { return kCatTotal; }
const char *catName(uint8_t index) {
  return index < kCatTotal ? kCatNames[index] : "";
}

void add(const String &line) { store(kInfo, line); }

void addf(const char *fmt, ...) {
  char scratch[224];
  va_list args;
  va_start(args, fmt);
  vsnprintf(scratch, sizeof(scratch), fmt, args);
  va_end(args);
  store(kInfo, String(scratch));
}

void at(uint8_t lvl, const String &line) { store(lvl, line); }

void atf(uint8_t lvl, const char *fmt, ...) {
  /* Checked before formatting, not after: at ordinary settings a debug line
   * should not cost a vsnprintf and a String allocation just to be discarded. */
  if (gLevel == kOff || lvl > gLevel) {
    return;
  }
  char scratch[224];
  va_list args;
  va_start(args, fmt);
  vsnprintf(scratch, sizeof(scratch), fmt, args);
  va_end(args);
  store(lvl, String(scratch));
}

String dump() {
  String out;
  out.reserve((size_t)stored * 56);
  for (uint8_t i = 0; i < stored; ++i) {
    const uint8_t index = (head + kLines - 1 - i) % kLines;
    if (out.length()) {
      out += '\n';
    }
    out += buffer[index];
  }
  return out;
}

void clear() {
  for (uint8_t i = 0; i < kLines; ++i) {
    buffer[i] = String();
  }
  head = 0;
  stored = 0;
}

} // namespace Log
