/*
 * The web admin panel.
 *
 * Deliberately thin: every action the page offers is composed into the same
 * command string the USB console accepts and handed to Commands::execute().
 * There is no second implementation of "add a device" to drift out of sync.
 */
#pragma once

#include <Arduino.h>

namespace WebUi {

void begin();
void loop();

/* Escapes a string for JSON and, just as importantly, guarantees the result is
 * valid UTF-8. Shared with the file API, which puts names chosen by whoever
 * formatted the card straight into JSON replies. */
String escape(const String &in);

} // namespace WebUi
