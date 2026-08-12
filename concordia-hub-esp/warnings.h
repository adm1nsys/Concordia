/*
 * Warnings — the things that went wrong and were survived.
 *
 * Not crashes and not chatter. A warning is an event worth seeing after the
 * fact: the link dropped and came back, an API would not answer, the internet
 * went away, somebody tried a password they did not have. Each one is a
 * question you would otherwise have to ask the logs, and the logs are a ring
 * buffer that a reboot erases.
 *
 * So they go on the card, one JSON object per line, and survive everything.
 *
 * ## About the timestamps
 *
 * There is no clock yet — no NTP, no timezone, no battery. Rather than write a
 * wall time that would be a lie, each entry carries seconds since boot plus a
 * `boot` id randomised at every start. Entries from different runs can then be
 * told apart, and the panel can say "1h 20m into the previous run" honestly.
 * When the clock arrives, real epochs get added beside these, not instead of
 * them: the old lines stay readable.
 */
#pragma once

#include <Arduino.h>

namespace Warn {

enum Severity { kInfo = 0, kWarn = 1, kError = 2 };

void begin();

/* `src` is a short category - "link", "net", "api", "auth", "fs", "ota" - and
 * is what the logging settings will switch on and off later. Cheap to call and
 * safe before the card is up: without one, warnings still collect in RAM. */
void raise(const char *src, Severity sev, const String &msg,
           const String &detail = "");

/* Watches the two kinds of connectivity. They fail for different reasons and
 * telling them apart is most of the diagnosis, so they are separate checks. */
void loop(bool wifiUp);

/* Recent entries, newest first, as a JSON array. Reads the card when there is
 * one, and falls back to what is held in RAM when there is not. */
String recentJson(uint16_t limit);

/* How many have been raised since this boot. Not the same as "how many are
 * stored": the file survives a restart and this counter does not, so the widget
 * decides on emptiness by the list, not by this. It is a badge, not a state. */
uint16_t sinceBoot();

/* Identifies this run, so the panel can tell entries from before the last
 * restart apart from ones happening now. */
uint32_t runId();

void clear();

} // namespace Warn
