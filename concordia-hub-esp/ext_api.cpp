#include "ext_api.h"

#include "json_path.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "config.h"
#include "log_buf.h"
#include "nrf_link.h"
#include "storage.h"

/* ==================================================================== fetch */
namespace {

/* Per-binding schedule. Indexes match Storage's binding table; a table change
 * just makes one poll happen early, which is harmless. */
unsigned long nextPollAt[MAX_BINDINGS] = {0};

} // namespace

namespace ExtApi {

FetchResult fetch(const String &url, const String &path, float scale, bool asBoolean) {
  FetchResult result;

  if (!url.startsWith("http://") && !url.startsWith("https://")) {
    result.error = "URL must start with http:// or https://";
    return result;
  }

  HTTPClient http;
  http.setTimeout(HTTP_FETCH_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_FETCH_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  bool started = false;
  WiFiClientSecure secure;
  if (url.startsWith("https://")) {
    /* No certificate store on this device, and pinning one would break the
     * moment the API rotates it. The payload is a temperature, not a secret. */
    secure.setInsecure();
    started = http.begin(secure, url);
  } else {
    started = http.begin(url);
  }

  if (!started) {
    result.error = "malformed URL";
    return result;
  }

  result.httpCode = http.GET();
  if (result.httpCode != HTTP_CODE_OK) {
    result.error = "HTTP " + String(result.httpCode);
    http.end();
    return result;
  }

  const String payload = http.getString();
  http.end();

  String error;
  double raw = 0;
  String usedPath;
  if (!Json::extract(payload, path, asBoolean, raw, usedPath, error)) {
    result.error = error;
    result.fields = Json::discover(payload, 12);
    return result;
  }

  result.ok = true;
  result.raw = raw;
  result.usedPath = usedPath;
  result.scaled = asBoolean ? (raw != 0 ? 1 : 0) : lround(raw * scale);
  result.fields = Json::discover(payload, 12);
  return result;
}

void loop(bool online) {
  if (!online) {
    return;
  }

  const uint8_t count = Storage::bindingCount();
  for (uint8_t i = 0; i < count && i < MAX_BINDINGS; ++i) {
    if (nextPollAt[i] != 0 && (long)(millis() - nextPollAt[i]) < 0) {
      continue;
    }

    const Binding b = Storage::bindingAt(i);
    nextPollAt[i] = millis() + (unsigned long)b.pollSeconds * 1000UL;

    const FetchResult r = fetch(b.url, b.path, b.scale, b.asBoolean);
    if (!r.ok) {
      Log::addf("[api] EP%u failed: %s", b.endpointId, r.error.c_str());
    } else if (NrfLink::setValue(b.endpointId, r.scaled)) {
      Log::addf("[api] EP%u = %ld (%s = %.3f)", b.endpointId, r.scaled,
                r.usedPath.c_str(), r.raw);
    } else {
      Log::addf("[api] EP%u: the nRF refused the value", b.endpointId);
    }
    return; /* one fetch per loop: never stall the web server */
  }
}

} // namespace ExtApi
