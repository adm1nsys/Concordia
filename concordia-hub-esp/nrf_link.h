/*
 * The UART link to the nRF54L15 shell.
 *
 * Everything the bridge can do goes through here as a `bridge ...` (or `ot ...`)
 * command line, exactly as if a person had typed it.
 */
#pragma once

#include <Arduino.h>

struct BridgeDevice {
  uint16_t endpointId = 0;
  String type;
  String name;
  long value = 0;
  bool hasValue = false;
  /* Secondary values for multi-control devices: brightness, colour temperature.
   * -1 means the type does not have one. */
  long value2 = -1;
  long value3 = -1;
};

struct BridgeType {
  String slug;
  String display;
  /* Matter device type id as printed by the nRF, e.g. "0x0100". Shown in the
   * panel when the user asks for it. */
  String id;
  String hint;
  /* What the primary value means, straight from the nRF: bool, percent, level,
   * centipercent, centidegree, enum or number. The UI picks a switch, a slider
   * or a plain field from this instead of making everyone type raw numbers. */
  String kind;
};

namespace NrfLink {

void begin();

/* Sends one line and returns everything the shell answered, with the echoed
 * command and the trailing prompt stripped.
 *
 * `timeoutMs` is how long to wait for the first byte. Reads and value changes
 * answer immediately; creating or deleting an endpoint rebuilds Matter's
 * attribute storage and writes settings to flash, which on a board with 100
 * slots takes several seconds. Waiting only the default there made a
 * successful `bridge add` look like a failure while the device was in fact
 * created - see NRF_REPLY_TIMEOUT_SLOW_MS. */
String send(const String &command, unsigned long timeoutMs = 0);

bool online();
bool status(uint16_t &slotsUsed, uint16_t &slotsTotal);

uint8_t devices(BridgeDevice *out, uint8_t maxItems);
/* `complete`, when given, reports whether the board finished printing the list.
 * A truncated answer still yields usable entries, but it must not be kept. */
uint8_t types(BridgeType *out, uint8_t maxItems, bool *complete = nullptr);

/* Returns the id of the freshly allocated endpoint, or 0 on failure. */
uint16_t addDevice(const String &type, const String &name);
bool removeDevice(uint16_t endpointId);
bool setValue(uint16_t endpointId, long value, long level = -1, long mireds = -1);

/* True when several answers in a row came back empty. */
bool linkSuspect();
/* Reopens the port and reports what it found. */
void recover();

bool factoryReset();
void reboot();

/* -------------------------------------------------------- deferred pairing
 * The node holds BLE advertising off until told to start (`bridge commission`
 * on its console) - see nRF54l15_Bridge/PORTING_NOTES.md, "Shell thread
 * priority" and what came after it, for why: advertising, once running,
 * makes the UART line unreliable enough that querying the code *after* it
 * starts is exactly the wrong order. So the code is fetched once, right after
 * boot while the line is still clean, and kept - the HTTP handler never has
 * to touch the link at all. */

/* Grabs `bridge pairing` now and caches it. Called at boot, and again on
 * every successful online()/status()/recover(), in case the boot-time
 * attempt landed before the node had settled - a real possibility, since the
 * two boards power up independently. A no-op the instant both fields are
 * already cached, so this costs nothing once it has worked once.
 *
 * The moment the code is cached, this also sends `bridge commission` - once
 * per boot - so the node opens its commissioning window right after, not
 * before, the line has given up the code it needed a clean line for. */
void refreshPairingCache();

/* Whatever is cached, immediately, never touching the link. Both empty until
 * refreshPairingCache() has succeeded at least once. */
void pairingCode(String &manual, String &qr);

enum LinkState { kLinkUp, kLinkPairing, kLinkDown };

/* kLinkUp: the last exchange succeeded, commissioned or not.
 * kLinkPairing: the link is currently silent, and the last time it answered
 * the node was not yet paired - almost certainly commissioning under way
 * (BLE advertising makes the UART unreliable while it runs; see above), not a
 * fault.
 * kLinkDown: silent, and the last time it answered it *was* paired already -
 * a real outage, not the pairing window. */
LinkState linkState();

} // namespace NrfLink
