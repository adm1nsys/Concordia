/*
 * Feeding bridged endpoints from the outside world: fetch a URL, pull one
 * number (or boolean) out of the answer, push it to the nRF.
 *
 * The parser is deliberately generic - no schema, no library, no assumptions
 * about who is on the other end. Give it a path and it follows it; give it
 * nothing and it finds the first usable number and tells you where it was.
 */
#pragma once

#include <Arduino.h>

struct FetchResult {
  bool ok = false;
  double raw = 0;        /* the value exactly as the API reported it */
  long scaled = 0;       /* what actually gets sent to the endpoint */
  String usedPath;       /* the path that produced it (handy when auto-picked) */
  String fields;         /* "path = value" candidates, '\n' separated */
  String error;
  int httpCode = 0;
};

namespace ExtApi {

/* One-shot fetch, used both by the poller and by the UI's "Test" button. */
FetchResult fetch(const String &url, const String &path, float scale, bool asBoolean);

/* Polls every binding on its own schedule. Does at most one fetch per call so
 * the loop never stalls on a slow server. */
void loop(bool online);

} // namespace ExtApi
