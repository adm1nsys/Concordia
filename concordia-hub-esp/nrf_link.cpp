#include "nrf_link.h"

#include "config.h"
#include "log_buf.h"
#include "warnings.h"

#define NRF_SERIAL Serial1

namespace {

/* One reply buffer for the whole firmware.
 *
 * The previous version appended to a String one character at a time, which for
 * a 10 KB `bridge types` answer means thousands of reallocations and a heap
 * chopped into confetti. A single static buffer costs the memory once, at a
 * fixed address, and the String is built exactly once at the end. */
char gReply[NRF_REPLY_MAX_BYTES + 1];

const char kPrompt[] = "uart:~$";

/* How many answers in a row came back empty. One is normal (`ot ping` prints
 * asynchronously); a run of them means the link is gone. */
uint8_t gSilentReplies = 0;
const uint8_t kSilentBeforeRecovery = 4;
/* Never hammer a link that is already unhappy. */
const unsigned long kRecoverIntervalMs = 60000;

/* Set from every successful `bridge status` reply's `paired=` field - see
 * app_task.cpp on the node side. Starts false, which is the correct default:
 * a board that has never answered has never told us it was paired either. */
bool gLastPaired = false;

void capturePaired(const String &reply) {
  const int at = reply.indexOf("paired=");
  if (at < 0) {
    return;
  }
  gLastPaired = reply.substring(at + 7).startsWith("yes");
}

/* See NrfLink::pairingCode() - filled in once by refreshPairingCache() and
 * never touched again after that, because the code itself never changes
 * while this firmware image does not. */
String gCachedManual;
String gCachedQr;

/* Sent exactly once per boot, the moment the code above is safely cached -
 * see refreshPairingCache(). */
bool gCommissionRequested = false;

void drainInput() {
  while (NRF_SERIAL.available() > 0) {
    (void)NRF_SERIAL.read();
  }
}

/* Pull `key=value` out of a shell line, stopping at the next space. */
String fieldAfter(const String &line, const String &key) {
  const int at = line.indexOf(key);
  if (at < 0) {
    return String();
  }
  const int start = at + key.length();
  int end = line.indexOf(' ', start);
  if (end < 0) {
    end = line.length();
  }
  return line.substring(start, end);
}

/* Same, but takes the rest of the line. Only `name=` uses this, and only
 * because a name may legitimately contain spaces. */
String tailAfter(const String &line, const String &key) {
  const int at = line.indexOf(key);
  return at < 0 ? String() : line.substring(at + key.length());
}

/* Percent-encode anything that is not plain ASCII text.
 *
 * The Zephyr shell line editor silently drops every byte that fails isprint(),
 * so a UTF-8 name never reached the nRF at all - "Кухня" arrived empty and the
 * endpoint quietly took its default name. Encoding turns those bytes into
 * ASCII that survives the trip; the nRF decodes them back. Spaces go the same
 * way, which also stops a multi-word name from being split into arguments. */
String encodeName(const String &name) {
  String out;
  out.reserve(name.length() + 12);
  for (size_t i = 0; i < name.length(); ++i) {
    const uint8_t c = (uint8_t)name[i];
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (safe) {
      out += (char)c;
    } else {
      char esc[4];
      snprintf(esc, sizeof(esc), "%%%02X", c);
      out += esc;
    }
  }
  return out;
}

/* Calls `fn` for every line of `text`, without copying the whole thing. */
template <typename Fn> void forEachLine(const String &text, Fn fn) {
  int start = 0;
  while (start < (int)text.length()) {
    int end = text.indexOf('\n', start);
    if (end < 0) {
      end = text.length();
    }
    String line = text.substring(start, end);
    line.trim();
    if (line.length()) {
      fn(line);
    }
    start = end + 1;
  }
}

} // namespace

namespace NrfLink {

void begin() {
  /* A `bridge types` answer is ~10 KB. The default 256-byte driver buffer only
   * survives because we read continuously, and under Wi-Fi load that is not a
   * promise worth making: a single overrun desynchronises every reply after
   * it. */
  NRF_SERIAL.begin(NRF_LINK_BAUD, SERIAL_8N1, PIN_RX_FROM_NRF, PIN_TX_TO_NRF);
  delay(50);

  /* Clear whatever half-typed line the shell may be holding, so the first real
   * command is not appended to leftovers. */
  NRF_SERIAL.print("\r\n");
  delay(20);
  drainInput();
  gSilentReplies = 0;
}

/* Nudges a quiet link back to life - without hurting it.
 *
 * The first version called Serial1.end() before re-opening. That releases the
 * TX pin, and a released pin reads as a long low on the other side: a BREAK.
 * The nRF's UARTE latches BREAK into ERRORSRC and, in interrupt-driven mode,
 * stops receiving until it reboots - so the "recovery" was the thing killing
 * the link, over and over, once per loop iteration. Measured on the chip:
 * ERRORSRC=0xc (FRAMING|BREAK) with no further RX events.
 *
 * Now it only clears local state and sends a newline, and not more than once a
 * minute, so a dead link costs one line rather than a storm of resets. */
void recover() {
  static unsigned long nextAttemptAt = 0;
  if (nextAttemptAt != 0 && (long)(millis() - nextAttemptAt) < 0) {
    return;
  }
  nextAttemptAt = millis() + kRecoverIntervalMs;

  /* Recorded on the way down, not only on the way back up: an outage that never
   * recovers is precisely the one worth having a record of. */
  static bool down = false;
  static unsigned long downAt = 0;
  if (!down) {
    down = true;
    downAt = millis();
    Warn::raise("link", Warn::kWarn, "link to the node went quiet");
  }

  Log::add("[link] quiet - resyncing");
  drainInput();
  NRF_SERIAL.print("\r\n"); /* clear any half-typed line in the shell */
  delay(30);
  drainInput();
  gSilentReplies = 0;

  const String reply = send("bridge status");
  if (reply.indexOf("Bridge base endpoints") >= 0) {
    capturePaired(reply);
    const unsigned long outage = (millis() - downAt) / 1000;
    down = false;
    Warn::raise("link", Warn::kInfo, "link to the node is back",
                "down for " + String(outage) + "s");
    Log::add("[link] back");
    refreshPairingCache(); /* a no-op if the first attempt, back at boot, already worked */
    return;
  }
  Log::add("[link] no answer - reset the node (hold USR) if this persists");
}

String send(const String &command, unsigned long timeoutMs) {
  drainInput();
  NRF_SERIAL.print(command);
  NRF_SERIAL.print("\r\n");

  size_t len = 0;
  const unsigned long deadline =
      millis() + (timeoutMs ? timeoutMs : NRF_REPLY_TIMEOUT_MS);
  const unsigned long idleLimit =
      timeoutMs ? NRF_REPLY_IDLE_SLOW_MS : NRF_REPLY_IDLE_MS;
  unsigned long lastByte = millis();

  while ((long)(millis() - deadline) < 0) {
    bool got = false;
    while (NRF_SERIAL.available() > 0 && len < NRF_REPLY_MAX_BYTES) {
      gReply[len++] = (char)NRF_SERIAL.read();
      got = true;
    }
    if (got) {
      lastByte = millis();
    }
    gReply[len] = '\0';

    /* The prompt marks the end of an answer. If it never arrives - `ot ping`
     * prints its result asynchronously, for one - settle for a quiet line. */
    if (len >= sizeof(kPrompt) - 1 &&
        strcmp(gReply + len - (sizeof(kPrompt) - 1), kPrompt) == 0) {
      break;
    }
    if (len >= NRF_REPLY_MAX_BYTES) {
      break;
    }
    if (len > 0 && millis() - lastByte > idleLimit) {
      break;
    }
    delay(2); /* also yields to Wi-Fi and the web server */
  }
  gReply[len] = '\0';

  String reply(gReply);

  /* Drop the echoed command line. */
  const int firstNewline = reply.indexOf('\n');
  if (firstNewline >= 0 && reply.substring(0, firstNewline).indexOf(command) >= 0) {
    reply = reply.substring(firstNewline + 1);
  }
  /* Drop the trailing prompt. */
  const int promptAt = reply.lastIndexOf(kPrompt);
  if (promptAt >= 0) {
    reply = reply.substring(0, promptAt);
  }
  reply.trim();

  if (reply.length() == 0) {
    ++gSilentReplies;
  } else {
    gSilentReplies = 0;
  }
  return reply;
}

bool online() {
  const String reply = send("bridge status");
  const bool ok = reply.indexOf("Bridge base endpoints") >= 0;
  if (ok) {
    capturePaired(reply);
    refreshPairingCache(); /* no-op once cached - see there for why this backfill exists */
  }
  return ok;
}

bool status(uint16_t &slotsUsed, uint16_t &slotsTotal) {
  const String reply = send("bridge status");
  if (reply.indexOf("Bridge base endpoints") < 0) {
    return false;
  }
  capturePaired(reply);
  refreshPairingCache(); /* no-op once cached - see there for why this backfill exists */
  /* "Virtual endpoint slots: 3/100 used" */
  const int at = reply.indexOf("slots:");
  if (at >= 0) {
    String tail = reply.substring(at + 6);
    tail.trim();
    const int slash = tail.indexOf('/');
    if (slash > 0) {
      slotsUsed = (uint16_t)tail.substring(0, slash).toInt();
      slotsTotal = (uint16_t)tail.substring(slash + 1).toInt();
    }
  }
  return true;
}

uint8_t devices(BridgeDevice *out, uint8_t maxItems) {
  const String reply = send("bridge list");
  uint8_t found = 0;

  forEachLine(reply, [&](const String &line) {
    if (found >= maxItems || !line.startsWith("EP")) {
      return;
    }
    /* "EP2 type=occupancy value=0 name=kitchen sensor"
     * name= is last so the rest of the line is the name, spaces and all. */
    const int space = line.indexOf(' ');
    if (space <= 2) {
      return;
    }
    BridgeDevice dev;
    dev.endpointId = (uint16_t)line.substring(2, space).toInt();
    dev.type = fieldAfter(line, "type=");
    dev.name = tailAfter(line, "name=");
    const String value = fieldAfter(line, "value=");
    dev.hasValue = value.length() > 0 && value != "<unset>";
    dev.value = dev.hasValue ? value.toInt() : 0;
    const String v2 = fieldAfter(line, "v2=");
    const String v3 = fieldAfter(line, "v3=");
    dev.value2 = v2.length() ? v2.toInt() : -1;
    dev.value3 = v3.length() ? v3.toInt() : -1;
    out[found++] = dev;
  });
  return found;
}

uint8_t types(BridgeType *out, uint8_t maxItems, bool *complete) {
  /* Read this one with the patient timeout. The list is thirty-odd lines and
   * the shell emits them in bursts; at the brisk idle limit the reader gave up
   * inside a gap and returned the handful of types it had managed to collect,
   * which is what made the panel offer six device types instead of all of them.
   *
   * The last line the board prints is the marker for a complete answer - see
   * the caller, which refuses to cache anything that lacks it. */
  const String reply = send("bridge types", NRF_REPLY_TIMEOUT_SLOW_MS);
  if (complete != nullptr) {
    *complete = reply.indexOf("Use: bridge add") >= 0;
  }
  uint8_t found = 0;

  forEachLine(reply, [&](const String &line) {
    /* "temp   Temperature Sensor (0x0302), kind=centidegree, value: centi-C" */
    if (found >= maxItems || line.indexOf('(') < 0) {
      return;
    }
    const int space = line.indexOf(' ');
    if (space <= 0) {
      return;
    }

    BridgeType type;
    type.slug = line.substring(0, space);
    String rest = line.substring(space);
    rest.trim();

    const int valueAt = rest.indexOf(", value:");
    if (valueAt > 0) {
      type.display = rest.substring(0, valueAt);
      type.hint = rest.substring(valueAt + 8);
      type.hint.trim();
    } else {
      type.display = rest;
    }
    const int paren = type.display.indexOf(" (0x");
    if (paren > 0) {
      const int close = type.display.indexOf(')', paren);
      if (close > paren) {
        type.id = type.display.substring(paren + 2, close);
      }
      type.display = type.display.substring(0, paren);
    }

    /* "kind=percent," - the comma is where it ends. */
    type.kind = fieldAfter(line, "kind=");
    if (type.kind.endsWith(",")) {
      type.kind = type.kind.substring(0, type.kind.length() - 1);
    }
    if (type.kind.length() == 0) {
      type.kind = "number"; /* older nRF firmware without the kind field */
    }
    out[found++] = type;
  });
  return found;
}

uint16_t addDevice(const String &type, const String &name) {
  const String reply =
      send("bridge add " + type + " " + encodeName(name), NRF_REPLY_TIMEOUT_SLOW_MS);
  const int at = reply.indexOf("Allocated EP");
  if (at < 0) {
    return 0;
  }
  return (uint16_t)reply.substring(at + 12).toInt();
}

bool removeDevice(uint16_t endpointId) {
  return send("bridge remove " + String(endpointId), NRF_REPLY_TIMEOUT_SLOW_MS)
             .indexOf("Removed virtual endpoint") >= 0;
}

bool setValue(uint16_t endpointId, long value, long level, long mireds) {
  String cmd = "bridge set " + String(endpointId) + " " + String(value);
  if (level >= 0) {
    cmd += " " + String(level);
    if (mireds >= 0) {
      cmd += " " + String(mireds);
    }
  }
  return send(cmd).indexOf("Updated EP") >= 0;
}

bool factoryReset() {
  return send("bridge factoryreset confirm", NRF_REPLY_TIMEOUT_SLOW_MS)
             .indexOf("Performing factory reset") >= 0;
}

void reboot() { send("bridge reboot"); }

bool linkSuspect() { return gSilentReplies >= kSilentBeforeRecovery; }

void refreshPairingCache() {
  if (!(gCachedManual.length() && gCachedQr.length())) {
    const String reply = send("bridge pairing");
    int at = reply.indexOf("manual=");
    if (at >= 0) {
      String manual = reply.substring(at + 7);
      manual = manual.substring(0, manual.indexOf('\n'));
      manual.trim();
      if (manual.length()) {
        gCachedManual = manual;
      }
    }
    at = reply.indexOf("qr=");
    if (at >= 0) {
      String qr = reply.substring(at + 3);
      qr = qr.substring(0, qr.indexOf('\n'));
      qr.trim();
      if (qr.length()) {
        gCachedQr = qr;
      }
    }
    if (gCachedManual.length() || gCachedQr.length()) {
      Log::add("[link] pairing code cached");
    }
  }

  /* The other half of the deferred-BLE scheme: the code was just read off a
   * clean line with no radio active, so it is now safe to let the node open
   * its commissioning window and start advertising - see nrf_link.h's
   * "deferred pairing" block and `bridge commission` on the node side. Sent
   * once per boot; a node that is already paired treats it as a no-op. */
  if (!gCommissionRequested && gCachedManual.length() && gCachedQr.length()) {
    gCommissionRequested = true;
    send("bridge commission");
    Log::add("[link] commissioning window requested");
  }
}

void pairingCode(String &manual, String &qr) {
  manual = gCachedManual;
  qr = gCachedQr;
}

LinkState linkState() {
  if (!linkSuspect()) {
    return kLinkUp;
  }
  return gLastPaired ? kLinkDown : kLinkPairing;
}

} // namespace NrfLink
