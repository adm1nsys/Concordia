/*
 * Wi-Fi: join one of the saved networks, or open a captive portal so the user
 * can enter new credentials with nothing but a phone. Also mDNS: once online,
 * the board answers to <hostname>.local as well as its bare IP.
 */
#pragma once

#include <Arduino.h>

enum NetMode { NET_CONNECTING, NET_ONLINE, NET_PORTAL };

namespace Net {

void begin();
void loop();

NetMode mode();
String ipAddress();
String ssid();
String apSsid();
int rssi();

/* Re-reads the saved list and joins again; falls back to the portal. */
void reconnect();

/* ------------------------------------------------------------- hostname
 * What the board answers to over mDNS - "concordia" by default, so
 * http://concordia.local/ works with nothing typed but that. Stored in
 * /Concordia/this/network.json; a missing or unreadable file leaves the
 * default in place rather than refusing to resolve at all.
 */
String hostname();
/* Persists the new name and restarts mDNS under it immediately - no reboot
 * needed, unlike the wording in the roadmap this replaces. Empty or unchanged
 * names are refused/no-ops respectively. */
bool setHostname(const String &name);

} // namespace Net
