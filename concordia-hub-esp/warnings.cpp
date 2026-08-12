#include "warnings.h"

#include <SD.h>
#include <WiFi.h>
#include <esp_random.h>

#include "log_buf.h"
#include "sd_card.h"
#include "web_ui.h"

namespace Warn {

namespace {

const char *const kFile = "/Concordia/logs/warnings.jsonl";
const char *const kFileOld = "/Concordia/logs/warnings.1.jsonl";

/* Rotate at 48 KB and keep one generation. Two files of that size is nothing on
 * a card, and it bounds the read the panel does on every refresh. */
const uint32_t kRotateAt = 48 * 1024;

/* Enough to be useful on a board with no card fitted, small enough to be free. */
const uint8_t kRamSlots = 12;

struct Entry {
  uint32_t up = 0;
  uint8_t sev = kWarn;
  String src;
  String msg;
  String detail;
};

Entry gRam[kRamSlots];
uint8_t gRamCount = 0;
uint8_t gRamNext = 0;
uint16_t gPending = 0;
uint32_t gBootId = 0;

const char *sevName(uint8_t s) {
  return s == kError ? "error" : s == kInfo ? "info" : "warn";
}

String toJson(const Entry &e) {
  String o = "{\"boot\":" + String(gBootId);
  o += ",\"up\":" + String(e.up);
  o += ",\"sev\":\"" + String(sevName(e.sev)) + "\"";
  o += ",\"src\":\"" + WebUi::escape(e.src) + "\"";
  o += ",\"msg\":\"" + WebUi::escape(e.msg) + "\"";
  if (e.detail.length()) {
    o += ",\"detail\":\"" + WebUi::escape(e.detail) + "\"";
  }
  o += "}";
  return o;
}

void append(const Entry &e) {
  if (!Card::ready()) {
    return;
  }
  File f = SD.open(kFile, FILE_APPEND);
  if (!f) {
    return;
  }
  if (f.size() > kRotateAt) {
    f.close();
    SD.remove(kFileOld);
    SD.rename(kFile, kFileOld);
    f = SD.open(kFile, FILE_APPEND);
    if (!f) {
      return;
    }
  }
  f.println(toJson(e));
  f.close();
}

/* ------------------------------------------------------------ connectivity
 * Two questions, deliberately asked separately.
 *
 * "Is the local network there" is answered by the Wi-Fi association and a
 * gateway address - it fails when the router reboots or the signal dies.
 *
 * "Is the internet there" is answered by reaching something outside the house.
 * It fails when the line is down, which looks identical from inside unless you
 * check. A TCP connection to a public resolver on port 53 is the cheapest
 * honest probe: no DNS needed to make it (so a broken resolver cannot fake a
 * failure), no payload, no dependency on anyone's uptime page. */
const uint32_t kProbeEveryMs = 60000;
const IPAddress kProbeHost(1, 1, 1, 1);
const uint16_t kProbePort = 53;

bool gLocalUp = true;
bool gExternalUp = true;
uint32_t gLocalDownAt = 0;
uint32_t gExternalDownAt = 0;

String forHowLong(uint32_t sinceMs) {
  const uint32_t s = (millis() - sinceMs) / 1000;
  if (s < 90) {
    return String(s) + "s";
  }
  if (s < 5400) {
    return String(s / 60) + "m";
  }
  return String(s / 3600) + "h" + String((s % 3600) / 60) + "m";
}

bool probeExternal() {
  WiFiClient c;
  c.setTimeout(3000);
  const bool ok = c.connect(kProbeHost, kProbePort, 3000);
  c.stop();
  return ok;
}

} // namespace

void begin() {
  gBootId = esp_random() & 0xffffff;
  Log::addf("[warn] run id %lu", (unsigned long)gBootId);
}

void raise(const char *src, Severity sev, const String &msg, const String &detail) {
  Entry e;
  e.up = millis() / 1000;
  e.sev = (uint8_t)sev;
  e.src = src;
  e.msg = msg;
  e.detail = detail;

  gRam[gRamNext] = e;
  gRamNext = (gRamNext + 1) % kRamSlots;
  if (gRamCount < kRamSlots) {
    gRamCount++;
  }
  if (gPending < 0xffff) {
    gPending++;
  }

  Log::addf("[%s] %s%s%s", src, msg.c_str(), detail.length() ? " - " : "",
            detail.c_str());
  append(e);
}

void loop(bool wifiUp) {
  static uint32_t nextAt = 15000; /* let the radio settle before the first ask */
  if ((long)(millis() - nextAt) < 0) {
    return;
  }
  nextAt = millis() + kProbeEveryMs;

  const bool local = wifiUp && WiFi.status() == WL_CONNECTED &&
                     WiFi.gatewayIP() != IPAddress((uint32_t)0);
  if (local != gLocalUp) {
    gLocalUp = local;
    if (local) {
      raise("net", kInfo, "local network is back", "after " + forHowLong(gLocalDownAt));
    } else {
      gLocalDownAt = millis();
      raise("net", kWarn, "local network lost", WiFi.SSID());
    }
  }

  /* No point asking about the internet while the house network is down: the
   * answer would be "no" for a reason that is already recorded. */
  if (!local) {
    return;
  }

  const bool ext = probeExternal();
  if (ext != gExternalUp) {
    gExternalUp = ext;
    if (ext) {
      raise("net", kInfo, "internet is back", "after " + forHowLong(gExternalDownAt));
    } else {
      gExternalDownAt = millis();
      raise("net", kWarn, "internet unreachable",
            "local network is fine, the line outside is not");
    }
  }
}

String recentJson(uint16_t limit) {
  String out = "[";
  uint16_t taken = 0;

  /* The card holds the whole history, but reading it backwards is awkward on
   * FAT; the file is small and bounded, so it is read forward into a rolling
   * window of the last `limit` lines. */
  if (Card::ready() && SD.exists(kFile)) {
    File f = SD.open(kFile, FILE_READ);
    if (f) {
      String window[24];
      const uint16_t cap = limit < 24 ? limit : 24;
      uint16_t n = 0, next = 0;
      while (f.available()) {
        const String line = f.readStringUntil('\n');
        if (line.length() < 8) {
          continue;
        }
        window[next] = line;
        next = (next + 1) % cap;
        if (n < cap) {
          n++;
        }
      }
      f.close();
      for (uint16_t i = 0; i < n; ++i) {
        const uint16_t at = (next + cap - 1 - i) % cap; /* newest first */
        if (taken++) {
          out += ',';
        }
        out += window[at];
      }
      out += "]";
      return out;
    }
  }

  for (uint8_t i = 0; i < gRamCount && taken < limit; ++i) {
    const uint8_t at = (gRamNext + kRamSlots - 1 - i) % kRamSlots;
    if (taken++) {
      out += ',';
    }
    out += toJson(gRam[at]);
  }
  out += "]";
  return out;
}

uint16_t sinceBoot() { return gPending; }

uint32_t runId() { return gBootId; }

void clear() {
  gPending = 0;
  gRamCount = 0;
  gRamNext = 0;
  if (Card::ready()) {
    SD.remove(kFile);
    SD.remove(kFileOld);
  }
  Log::add("[warn] cleared");
}

} // namespace Warn
