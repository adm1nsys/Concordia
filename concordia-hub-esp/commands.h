/*
 * One command set, three ways in: the USB console, the web console, and the
 * quick-action buttons in the UI all end up here. Anything you can do from a
 * serial terminal you can do from a browser, and the other way round.
 */
#pragma once

#include <Arduino.h>

namespace Commands {

/* Runs one line and returns what a human should see. Never throws, never
 * blocks longer than one nRF exchange plus one HTTP fetch. */
String execute(const String &line);

/* The help text, also used as the console banner. */
String help();

} // namespace Commands
