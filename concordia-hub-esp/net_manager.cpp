#include "net_manager.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <SD.h>
#include <WiFi.h>

#include "config.h"
#include "log_buf.h"
#include "storage.h"

namespace {

DNSServer dnsServer;
NetMode currentMode = NET_CONNECTING;
String apSsidCache;
unsigned long nextRetryAt = 0;
bool portalRunning = false;

const IPAddress kApIp(192, 168, 4, 1);
const IPAddress kApMask(255, 255, 255, 0);

/* Putting the address in the SSID means nobody has to guess where the portal
 * lives, even on a phone that refuses to pop the sign-in sheet. */
String buildApSsid() { return String(BRAND_NAME) + " " + kApIp.toString(); }

/* ------------------------------------------------------------- hostname */
const char *const kHostnameFile = "/Concordia/this/network.json";
const char *const kDefaultHostname = "concordia";
String gHostname = kDefaultHostname;

/* mDNS names: letters, digits, hyphen. Anything else either breaks resolution
 * on some resolvers or is someone pasting a whole URL by mistake. */
bool validHostname(const String &name) {
  if (name.length() == 0 || name.length() > 32) {
    return false;
  }
  for (size_t i = 0; i < name.length(); ++i) {
    const char c = name[i];
    if (!isalnum((unsigned char)c) && c != '-') {
      return false;
    }
  }
  return true;
}

/* Forgiving on purpose, same rule as logging.json: a missing or unreadable
 * file leaves the default in place rather than the board going nameless. */
void loadHostnameConfig() {
  File f = SD.open(kHostnameFile, FILE_READ);
  if (!f) {
    return;
  }
  const String body = f.readString();
  f.close();

  const int at = body.indexOf("\"hostname\"");
  if (at < 0) {
    return;
  }
  const int q1 = body.indexOf('"', body.indexOf(':', at) + 1);
  const int q2 = q1 >= 0 ? body.indexOf('"', q1 + 1) : -1;
  if (q1 < 0 || q2 < 0) {
    return;
  }
  const String name = body.substring(q1 + 1, q2);
  if (validHostname(name)) {
    gHostname = name;
  }
}

bool saveHostnameConfig() {
  File f = SD.open(kHostnameFile, FILE_WRITE);
  if (!f) {
    return false;
  }
  f.printf("{\"hostname\":\"%s\"}\n", gHostname.c_str());
  f.close();
  return true;
}

/* Safe to call whenever the board comes online, including a second time on a
 * reconnect - MDNS.end() is a no-op if nothing was running. */
void startMdns() {
  MDNS.end();
  if (MDNS.begin(gHostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
    Log::addf("[net] mDNS up: http://%s.local/", gHostname.c_str());
  } else {
    Log::add("[net] mDNS failed to start");
  }
}

void stopPortal() {
  if (!portalRunning) {
    return;
  }
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  portalRunning = false;
}

bool joinSaved() {
  const uint8_t count = Storage::wifiCount();
  if (count == 0) {
    Log::add("[net] no saved networks");
    return false;
  }

  for (uint8_t i = 0; i < count; ++i) {
    const WifiNetwork net = Storage::wifiAt(i);
    if (net.ssid.length() == 0) {
      continue;
    }

    Log::addf("[net] trying \"%s\"", net.ssid.c_str());
    WiFi.begin(net.ssid.c_str(), net.password.c_str());

    const unsigned long deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
    while ((long)(millis() - deadline) < 0) {
      if (WiFi.status() == WL_CONNECTED) {
        Log::addf("[net] online as %s (%s, %d dBm)", WiFi.localIP().toString().c_str(),
                  net.ssid.c_str(), WiFi.RSSI());
        startMdns();
        return true;
      }
      delay(100);
    }
    Log::addf("[net] \"%s\" did not answer", net.ssid.c_str());
    WiFi.disconnect(true, true);
    delay(100);
  }
  return false;
}

void startPortal() {
  if (portalRunning) {
    return;
  }
  apSsidCache = buildApSsid();

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(kApIp, kApIp, kApMask);
  WiFi.softAP(apSsidCache.c_str()); /* open: nothing secret is served */

  /* Answering every DNS query with our own address is what makes phones offer
   * "sign in to network" by themselves. */
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", kApIp);

  portalRunning = true;
  currentMode = NET_PORTAL;
  Log::addf("[net] portal up: \"%s\" -> http://%s/", apSsidCache.c_str(),
            kApIp.toString().c_str());
}

void joinOrPortal() {
  WiFi.mode(WIFI_STA);
  if (joinSaved()) {
    stopPortal();
    currentMode = NET_ONLINE;
  } else {
    startPortal();
  }
}

} // namespace

namespace Net {

void begin() {
  loadHostnameConfig(); /* the card is already up by the time this runs */
  WiFi.persistent(false); /* the credential list is ours, not the SDK's */
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);   /* power save makes the board miss ARP and drop off */
  joinOrPortal();
}

void loop() {
  if (currentMode == NET_PORTAL) {
    dnsServer.processNextRequest();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    currentMode = NET_ONLINE;
    return;
  }

  /* Dropped off: retry the saved list on a timer, then fall back to the portal
   * so there is always a way back in. */
  if (currentMode != NET_CONNECTING) {
    Log::add("[net] connection lost");
    currentMode = NET_CONNECTING;
    nextRetryAt = millis() + WIFI_RETRY_INTERVAL_MS;
    return;
  }
  if ((long)(millis() - nextRetryAt) < 0) {
    return;
  }
  nextRetryAt = millis() + WIFI_RETRY_INTERVAL_MS;
  joinOrPortal();
}

NetMode mode() { return currentMode; }

String ipAddress() {
  return currentMode == NET_PORTAL ? kApIp.toString() : WiFi.localIP().toString();
}

String ssid() { return currentMode == NET_PORTAL ? apSsidCache : WiFi.SSID(); }

String apSsid() { return apSsidCache.length() ? apSsidCache : buildApSsid(); }

int rssi() { return currentMode == NET_ONLINE ? WiFi.RSSI() : 0; }

void reconnect() {
  stopPortal();
  WiFi.disconnect(true, true);
  delay(200);
  joinOrPortal();
}

String hostname() { return gHostname; }

bool setHostname(const String &name) {
  if (name == gHostname) {
    return true; /* unchanged is not an error, just nothing to do */
  }
  if (!validHostname(name)) {
    return false;
  }
  gHostname = name;
  if (!saveHostnameConfig()) {
    return false;
  }
  if (currentMode == NET_ONLINE) {
    startMdns(); /* live, no reboot - see the header note */
  }
  return true;
}

} // namespace Net
