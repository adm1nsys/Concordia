/*
 * Pulling one value out of an arbitrary JSON document.
 *
 * Depends on nothing but String, on purpose: the same source file compiles on
 * a computer against a stub, which is how it gets tested against real API
 * responses instead of hopeful guesses.
 */
#pragma once

#include <Arduino.h>

namespace Json {

/* Follows `path` (empty = auto-pick the first number) and returns the value.
 * `usedPath` reports where the value actually came from, which matters when
 * the path was picked automatically. */
bool extract(const String &doc, const String &path, bool wantBoolean, double &out,
             String &usedPath, String &error);

/* Lists up to `maxFields` numeric/boolean leaves as "path = value" lines, so a
 * failed lookup can show what the response does contain. */
String discover(const String &doc, uint8_t maxFields);

} // namespace Json
