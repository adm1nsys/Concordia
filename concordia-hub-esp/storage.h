/*
 * Persistent settings in NVS: Wi-Fi networks, UI language, and the bindings
 * that feed bridged endpoints from an external API.
 *
 * Kept in its own namespace ("bridge1") rather than reusing the one from the
 * previous firmware, so a half-written record from that version cannot come
 * back to haunt this one.
 */
#pragma once

#include <Arduino.h>

#include "config.h"

struct WifiNetwork {
  String ssid;
  String password;
};

struct Binding {
  uint16_t endpointId = 0;
  String url;
  String path;              /* JSON path; empty = pick the first number found */
  float scale = 1.0f;       /* raw value is multiplied by this before sending */
  bool asBoolean = false;   /* send 0/1 instead of a scaled number */
  uint16_t pollSeconds = 300;
};

namespace Storage {

void begin();
void factoryReset();

/* ------------------------------------------------------------------ Wi-Fi */
uint8_t wifiCount();
WifiNetwork wifiAt(uint8_t index);
bool wifiAdd(const String &ssid, const String &password);
bool wifiRemove(uint8_t index);

/* ---------------------------------------------------------------- language */
String language();
void setLanguage(const String &lang);

/* ------------------------------------------------------------- panel layout
 * One opaque JSON document owned entirely by the browser: theme, whether to
 * show cluster ids, which devices are pinned to the home screen, their order,
 * the per-device manufacturer/model/serial notes and the home-screen widgets.
 *
 * Deliberately not parsed here. Every one of those is a presentation choice
 * with no meaning to the firmware, and giving each its own key would mean
 * touching three files every time the panel grows a checkbox. */
String uiConfig();
bool setUiConfig(const String &json);

/* ---------------------------------------------------------------- bindings */
uint8_t bindingCount();
Binding bindingAt(uint8_t index);
/* Returns false only when the table is full. Replaces any binding that already
 * targets the same endpoint. */
bool bindingSet(const Binding &binding);
bool bindingRemove(uint16_t endpointId);
bool bindingFor(uint16_t endpointId, Binding &out);
/* Follows a binding when an endpoint is recreated under a new id. */
void bindingMove(uint16_t fromEndpoint, uint16_t toEndpoint);

} // namespace Storage
