#include "storage.h"

#include <Preferences.h>

#include "log_buf.h"

namespace {

Preferences prefs;
const char *kNamespace = "bridge1";
const char kSep = '\x1f'; /* unit separator: cannot occur in an SSID or a URL */

/* Records are kept as one string per slot rather than a blob, so a single
 * corrupt entry can never desynchronise the rest of the table. */
String slotKey(const char *prefix, uint8_t index) {
  return String(prefix) + String(index);
}

String field(const String &record, uint8_t wanted) {
  uint8_t seen = 0;
  int start = 0;
  while (start <= (int)record.length()) {
    int end = record.indexOf(kSep, start);
    if (end < 0) {
      end = record.length();
    }
    if (seen == wanted) {
      return record.substring(start, end);
    }
    ++seen;
    start = end + 1;
  }
  return String();
}

uint8_t wifiStored = 0;
uint8_t bindingsStored = 0;

void seedDefaultWifi() {
  prefs.putString(slotKey("w", 0).c_str(),
                  String(WIFI_DEFAULT_SSID) + kSep + String(WIFI_DEFAULT_PASS));
  prefs.putUChar("wcount", 1);
  wifiStored = 1;
  Log::addf("[cfg] seeded default network \"%s\"", WIFI_DEFAULT_SSID);
}

} // namespace

namespace Storage {

void begin() {
  prefs.begin(kNamespace, false);
  wifiStored = prefs.getUChar("wcount", 0);
  bindingsStored = prefs.getUChar("bcount", 0);

  if (wifiStored == 0) {
    seedDefaultWifi();
  }
}

void factoryReset() {
  prefs.clear();
  wifiStored = 0;
  bindingsStored = 0;
  seedDefaultWifi();
}

/* ------------------------------------------------------------------ Wi-Fi */

uint8_t wifiCount() { return wifiStored; }

WifiNetwork wifiAt(uint8_t index) {
  WifiNetwork net;
  if (index >= wifiStored) {
    return net;
  }
  const String record = prefs.getString(slotKey("w", index).c_str(), "");
  net.ssid = field(record, 0);
  net.password = field(record, 1);
  return net;
}

bool wifiAdd(const String &ssid, const String &password) {
  if (ssid.length() == 0) {
    return false;
  }

  /* Same SSID twice is a mistake, not a feature: overwrite in place. */
  for (uint8_t i = 0; i < wifiStored; ++i) {
    if (wifiAt(i).ssid == ssid) {
      prefs.putString(slotKey("w", i).c_str(), ssid + kSep + password);
      return true;
    }
  }

  if (wifiStored >= MAX_WIFI_NETWORKS) {
    return false;
  }
  prefs.putString(slotKey("w", wifiStored).c_str(), ssid + kSep + password);
  prefs.putUChar("wcount", ++wifiStored);
  return true;
}

bool wifiRemove(uint8_t index) {
  if (index >= wifiStored) {
    return false;
  }
  for (uint8_t i = index; i + 1 < wifiStored; ++i) {
    prefs.putString(slotKey("w", i).c_str(),
                    prefs.getString(slotKey("w", i + 1).c_str(), ""));
  }
  prefs.remove(slotKey("w", wifiStored - 1).c_str());
  prefs.putUChar("wcount", --wifiStored);
  return true;
}

/* ---------------------------------------------------------------- language */

String language() { return prefs.getString("lang", "en"); }

void setLanguage(const String &lang) { prefs.putString("lang", lang); }

String uiConfig() { return prefs.getString("ui", "{}"); }

bool setUiConfig(const String &json) {
  if (json.length() > 3072) { /* NVS entries are not free; this is plenty */
    return false;
  }
  return prefs.putString("ui", json) > 0;
}

/* ---------------------------------------------------------------- bindings */

uint8_t bindingCount() { return bindingsStored; }

Binding bindingAt(uint8_t index) {
  Binding b;
  if (index >= bindingsStored) {
    return b;
  }
  const String record = prefs.getString(slotKey("b", index).c_str(), "");
  b.endpointId = (uint16_t)field(record, 0).toInt();
  b.url = field(record, 1);
  b.path = field(record, 2);
  b.scale = field(record, 3).toFloat();
  b.asBoolean = field(record, 4) == "1";
  b.pollSeconds = (uint16_t)field(record, 5).toInt();
  if (b.scale == 0.0f) {
    b.scale = 1.0f;
  }
  if (b.pollSeconds < MIN_POLL_SECONDS) {
    b.pollSeconds = MIN_POLL_SECONDS;
  }
  return b;
}

namespace {
String encode(const Binding &b) {
  return String(b.endpointId) + kSep + b.url + kSep + b.path + kSep +
         String(b.scale, 4) + kSep + (b.asBoolean ? "1" : "0") + kSep +
         String(b.pollSeconds);
}
} // namespace

bool bindingSet(const Binding &binding) {
  for (uint8_t i = 0; i < bindingsStored; ++i) {
    if (bindingAt(i).endpointId == binding.endpointId) {
      prefs.putString(slotKey("b", i).c_str(), encode(binding));
      return true;
    }
  }
  if (bindingsStored >= MAX_BINDINGS) {
    return false;
  }
  prefs.putString(slotKey("b", bindingsStored).c_str(), encode(binding));
  prefs.putUChar("bcount", ++bindingsStored);
  return true;
}

bool bindingRemove(uint16_t endpointId) {
  for (uint8_t i = 0; i < bindingsStored; ++i) {
    if (bindingAt(i).endpointId != endpointId) {
      continue;
    }
    for (uint8_t j = i; j + 1 < bindingsStored; ++j) {
      prefs.putString(slotKey("b", j).c_str(),
                      prefs.getString(slotKey("b", j + 1).c_str(), ""));
    }
    prefs.remove(slotKey("b", bindingsStored - 1).c_str());
    prefs.putUChar("bcount", --bindingsStored);
    return true;
  }
  return false;
}

bool bindingFor(uint16_t endpointId, Binding &out) {
  for (uint8_t i = 0; i < bindingsStored; ++i) {
    const Binding b = bindingAt(i);
    if (b.endpointId == endpointId) {
      out = b;
      return true;
    }
  }
  return false;
}

void bindingMove(uint16_t fromEndpoint, uint16_t toEndpoint) {
  Binding b;
  if (!bindingFor(fromEndpoint, b)) {
    return;
  }
  bindingRemove(fromEndpoint);
  b.endpointId = toEndpoint;
  bindingSet(b);
}

} // namespace Storage
