#include "web_ui.h"

#include <WebServer.h>

#include <new>

#include "commands.h"
#include "config.h"
#include "fs_api.h"
#include "log_buf.h"
#include "net_manager.h"
#include "nrf_link.h"
#include "sd_card.h"
#include "storage.h"
#include "users.h"
#include "warnings.h"
#include "web_page.h"

#include <SD.h>

namespace {

WebServer server(80);

/* Escapes for JSON and, just as importantly, guarantees the result is valid
 * UTF-8.
 *
 * A single malformed byte makes JSON.parse() throw, and the browser then drops
 * the whole answer - one endpoint with a mangled name and the device list looks
 * empty for no visible reason. Multi-byte sequences are copied through only
 * when they are complete and well-formed; anything else becomes '?', which is
 * ugly but visible, and visible beats silently blank. */
String jsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 16);

  const size_t n = in.length();
  for (size_t i = 0; i < n; ++i) {
    const uint8_t c = (uint8_t)in[i];

    switch (c) {
    case '"': out += "\\\""; continue;
    case '\\': out += "\\\\"; continue;
    case '\n': out += "\\n"; continue;
    case '\r': continue;
    case '\t': out += "\\t"; continue;
    default: break;
    }

    if (c < 0x20) {
      char esc[7];
      snprintf(esc, sizeof(esc), "\\u%04x", c);
      out += esc;
      continue;
    }
    if (c < 0x80) {
      out += (char)c;
      continue;
    }

    /* Multi-byte: work out the length from the lead byte, then check that the
     * continuation bytes are really there. */
    size_t extra = 0;
    if ((c & 0xE0) == 0xC0) {
      extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
    } else {
      out += '?'; /* stray continuation byte or invalid lead */
      continue;
    }

    if (i + extra >= n) {
      out += '?'; /* truncated at the end of the string */
      break;
    }
    bool valid = true;
    for (size_t k = 1; k <= extra; ++k) {
      if (((uint8_t)in[i + k] & 0xC0) != 0x80) {
        valid = false;
        break;
      }
    }
    if (!valid) {
      out += '?';
      continue;
    }
    for (size_t k = 0; k <= extra; ++k) {
      out += in[i + k];
    }
    i += extra;
  }
  return out;
}

void sendJson(const String &body) {
  /* "Everything" means everything: at the top level each answered request is
   * recorded here, in the one place they all pass through, rather than by
   * sprinkling a log line into every handler and missing half of them. */
  Log::atf(Log::kEverything, "[api] %s -> %u bytes", server.uri().c_str(),
           (unsigned)body.length());
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

/* The panel comes from the card when there is one, and from flash when there is
 * not. That order is the whole point of the card: the interface can grow to
 * whatever size it likes without the firmware paying for it, while a missing or
 * dead card still leaves a working panel behind.
 *
 * Either way it travels gzipped, with the encoding declared - browsers have
 * handled that since the nineties, and it saves both flash and airtime. */
void handlePage() {
  /* No saved Wi-Fi answered, so this is the open portal AP rather than the
   * house network - the full panel would be reachable here too, but asking
   * for a Wi-Fi password has no business dragging in device tiles, widgets
   * and three languages of settings. This page needs nothing from the card,
   * on purpose: it has to work on a board that has never seen one. */
  if (Net::mode() == NET_PORTAL) {
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", reinterpret_cast<PGM_P>(kPortalPageGz),
                  kPortalPageGzLen);
    return;
  }

  /* No account yet: the wizard's job, if it has been uploaded. Users::anyExist()
   * alone used to be the whole condition, on the theory that once an account
   * exists the wizard has done its job - but the wizard's own step 1 creates
   * that account before the wizard is finished, so a plain reload mid-setup
   * satisfied anyExist() and dropped the visitor into the real panel with no
   * layout saved yet. Card::setupPending() covers exactly that window: set the
   * instant the wizard's own bootstrap admin lands, cleared the instant its
   * finishSetup() saves the UI config. A board bootstrapped from the console
   * instead never sets it, so it goes straight to the real panel as always.
   * Missing entirely is not an error: a board without the wizard uploaded
   * falls straight through to the bootstrap-open panel below, same as before
   * either of these existed. */
  String path = (!Users::anyExist() || Card::setupPending()) ? Card::setupPagePath() : "";
  if (path.length() == 0) {
    path = Card::webPagePath();
  }
  if (path.length()) {
    File f = SD.open(path, FILE_READ);
    if (f) {
      /* No Content-Encoding here: streamFile() adds it by itself for a name
       * ending in .gz. Setting it as well sent the header twice, and a browser
       * given two of them tries to un-gzip an already un-gzipped page and shows
       * nothing at all. */
      server.streamFile(f, "text/html");
      f.close();
      return;
    }
    Log::addf("[web] %s would not open - falling back to the built-in page",
              path.c_str());
  }

  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html", reinterpret_cast<PGM_P>(kIndexPageGz),
                kIndexPageGzLen);
}

void handleSdStatus() {
  String out = "{";
  out += "\"ready\":" + String(Card::ready() ? "true" : "false") + ",";
  out += "\"status\":\"" + jsonEscape(Card::status()) + "\",";
  out += "\"total\":" + String((unsigned long)(Card::totalBytes() / 1024ULL)) + ",";
  out += "\"used\":" + String((unsigned long)(Card::usedBytes() / 1024ULL)) + ",";
  out += "\"page\":\"" + jsonEscape(Card::webPagePath()) + "\"";

  /* The token is handed out only while the card carries no panel yet - a
   * bootstrap window that closes by itself the moment the first upload lands,
   * because from then on webPagePath() is no longer empty. It exists so a fresh
   * card can be filled over the network without prying it out of the board.
   *
   * After that the way to read the token is the card itself, in a laptop:
   * /Concordia/security/api_token. */
  if (Card::ready() && Card::webPagePath().length() == 0) {
    out += ",\"token\":\"" + jsonEscape(Card::token()) + "\"";
    out += ",\"bootstrap\":true";
  }
  out += "}";
  sendJson(out);
}

/* ---------------------------------------------------------------- uploading
 * One file at a time, straight to the card as it arrives, so neither the size
 * of the upload nor the free heap matters. It lands in tmp and is only moved
 * into place once the last byte is in - a connection that dies halfway leaves
 * the working panel exactly where it was.
 *
 * The token is checked here rather than in the completion handler because by
 * then the bytes have already been written. Without a card there is no token,
 * and without a token this refuses everything: it must not fall open. */
File gUpload;
String gUploadName;
String gUploadError;

bool uploadAuthorised() {
  return Card::authorised(server.header("X-Token"));
}

/* Anything that could climb out of /Concordia/web is rejected outright. */
String safeName(const String &raw) {
  int cut = raw.lastIndexOf('/');
  if (cut < 0) {
    cut = raw.lastIndexOf('\\');
  }
  String name = cut >= 0 ? raw.substring(cut + 1) : raw;
  name.trim();
  if (name.length() == 0 || name.length() > 48 || name.indexOf("..") >= 0) {
    return "";
  }
  for (size_t i = 0; i < name.length(); ++i) {
    const char c = name[i];
    const bool ok = isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_';
    if (!ok) {
      return "";
    }
  }
  return name;
}

void handleWebUploadData() {
  HTTPUpload &up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    gUploadError = "";
    gUploadName = "";
    if (!Card::ready()) {
      gUploadError = "no card";
      return;
    }
    if (!uploadAuthorised()) {
      gUploadError = "bad or missing X-Token";
      Log::add("[web] upload refused - token did not match");
      return;
    }
    const String name = safeName(up.filename);
    if (name.length() == 0) {
      gUploadError = "unusable filename";
      return;
    }
    gUploadName = name;
    const String staged = String(Card::kTmp) + "/incoming";
    if (SD.exists(staged)) {
      SD.remove(staged);
    }
    gUpload = SD.open(staged, FILE_WRITE);
    if (!gUpload) {
      gUploadError = "cannot open the staging file";
    }
    return;
  }

  if (up.status == UPLOAD_FILE_WRITE) {
    if (gUpload && gUpload.write(up.buf, up.currentSize) != up.currentSize) {
      gUploadError = "card is full, or the write failed";
      gUpload.close();
    }
    return;
  }

  if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (gUpload) {
      gUpload.close();
    }
    if (up.status == UPLOAD_FILE_ABORTED && gUploadError.length() == 0) {
      gUploadError = "the upload was cut short";
    }
  }
}

/* Removing a file is the other half of being able to put one there: without it
 * a bad asset can only be cleared by taking the card out, which is the very
 * thing the upload API exists to avoid. The removed copy goes to backup rather
 * than away, so a misfired delete is recoverable. */
void handleWebRemove() {
  if (!Card::ready()) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"no card\"}");
    return;
  }
  if (!uploadAuthorised()) {
    server.send(403, "application/json",
                "{\"ok\":false,\"error\":\"bad or missing X-Token\"}");
    return;
  }
  const String name = safeName(server.arg("file"));
  if (name.length() == 0) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"unusable filename\"}");
    return;
  }
  const String target = String(Card::kWeb) + "/" + name;
  if (!SD.exists(target)) {
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"no such file\"}");
    return;
  }
  const String backup = String(Card::kBackup) + "/" + name;
  if (SD.exists(backup)) {
    SD.remove(backup);
  }
  if (!SD.rename(target, backup)) {
    SD.remove(target);
  }
  Log::addf("[web] %s removed from the card", name.c_str());
  sendJson("{\"ok\":true,\"served\":\"" + jsonEscape(Card::webPagePath()) + "\"}");
}

void handleWebUploadDone() {
  const String staged = String(Card::kTmp) + "/incoming";

  if (gUploadError.length()) {
    if (SD.exists(staged)) {
      SD.remove(staged);
    }
    const int code = gUploadError.startsWith("bad or missing") ? 403 : 400;
    server.send(code, "application/json",
                "{\"ok\":false,\"error\":\"" + jsonEscape(gUploadError) + "\"}");
    return;
  }

  if (!Card::commitStaged(staged, gUploadName)) {
    if (SD.exists(staged)) {
      SD.remove(staged);
    }
    server.send(500, "application/json",
                "{\"ok\":false,\"error\":\"could not install the file\"}");
    return;
  }

  Log::addf("[web] %s replaced from upload", gUploadName.c_str());
  sendJson("{\"ok\":true,\"file\":\"" + jsonEscape(gUploadName) +
           "\",\"served\":\"" + jsonEscape(Card::webPagePath()) + "\"}");
}

void handleStatus() {
  uint16_t used = 0, total = 0;
  const bool up = NrfLink::status(used, total);

  String out = "{";
  out += "\"fw\":\"" FW_VERSION "\",";
  out += "\"brand\":\"" BRAND_NAME "\",";
  out += "\"portal\":" + String(Net::mode() == NET_PORTAL ? "true" : "false") + ",";
  out += "\"ip\":\"" + Net::ipAddress() + "\",";
  out += "\"ssid\":\"" + jsonEscape(Net::ssid()) + "\",";
  out += "\"apSsid\":\"" + jsonEscape(Net::apSsid()) + "\",";
  out += "\"rssi\":" + String(Net::rssi()) + ",";
  out += "\"hostname\":\"" + jsonEscape(Net::hostname()) + "\",";
  out += "\"lang\":\"" + Storage::language() + "\",";
  out += "\"bridge\":" + String(up ? "true" : "false") + ",";
  /* Distinguishes "commissioning is probably under way" (BLE advertising
   * makes the UART unreliable while it runs - deliberately, see
   * NrfLink::linkState()) from an actual outage, so the panel can say
   * "Pairing" instead of alarming anyone with "no link" for something that
   * is not a fault. */
  {
    const char *linkStr = "down";
    switch (NrfLink::linkState()) {
      case NrfLink::kLinkUp:
        linkStr = "up";
        break;
      case NrfLink::kLinkPairing:
        linkStr = "pairing";
        break;
      case NrfLink::kLinkDown:
        linkStr = "down";
        break;
    }
    out += "\"link\":\"" + String(linkStr) + "\",";
  }
  out += "\"used\":" + String(used) + ",";
  out += "\"total\":" + String(total) + ",";
  out += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
  out += "\"uptime\":" + String(millis() / 1000) + ",";
  /* Whether the panel should offer to sign in at all - not who is signed in
   * now, that is /api/session. No account existing yet is the deliberate
   * bootstrap state (see users.h), and the panel has nothing to show for it. */
  out += "\"users\":" + String(Users::anyExist() ? "true" : "false");
  out += "}";
  sendJson(out);
}

void handleDevices() {
  /* Sized to the nRF's slot table, not to a number that felt big enough: the
   * old array held 24, so the 25th endpoint and everything after it simply
   * never reached the browser. Heap, not stack - 100 of these is far too much
   * to put on a web-server task's stack. */
  static BridgeDevice *devices = nullptr;
  if (devices == nullptr) {
    devices = new (std::nothrow) BridgeDevice[kMaxDevices];
    if (devices == nullptr) {
      sendJson("{\"devices\":[]}");
      return;
    }
  }
  const uint8_t count = NrfLink::devices(devices, kMaxDevices);

  String out = "{\"devices\":[";
  for (uint8_t i = 0; i < count; ++i) {
    const BridgeDevice &d = devices[i];
    if (i) {
      out += ',';
    }
    out += "{\"ep\":" + String(d.endpointId);
    out += ",\"type\":\"" + jsonEscape(d.type) + "\"";
    out += ",\"name\":\"" + jsonEscape(d.name) + "\"";
    out += ",\"has\":" + String(d.hasValue ? "true" : "false");
    out += ",\"value\":" + String(d.value);
    if (d.value2 >= 0) {
      out += ",\"v2\":" + String(d.value2);
    }
    if (d.value3 >= 0) {
      out += ",\"v3\":" + String(d.value3);
    }

    Binding b;
    if (Storage::bindingFor(d.endpointId, b)) {
      out += ",\"bind\":{\"url\":\"" + jsonEscape(b.url) + "\"";
      out += ",\"path\":\"" + jsonEscape(b.path) + "\"";
      out += ",\"scale\":" + String(b.scale, 3);
      out += ",\"bool\":" + String(b.asBoolean ? "true" : "false");
      out += ",\"poll\":" + String(b.pollSeconds) + "}";
    }
    out += '}';
  }
  out += "]}";
  sendJson(out);
}

void handleTypes() {
  /* The list is fixed at build time on the nRF, so fetch it once and keep it -
   * but only a *whole* list may be kept. Caching whatever arrived meant one
   * slow moment on the link froze a partial list in place for as long as the
   * ESP stayed up: the panel then offered six device types out of thirty-two
   * and looked, convincingly, like a firmware that could not do the rest. */
  static String cached;
  if (cached.length() == 0) {
    BridgeType types[70];
    bool complete = false;
    const uint8_t count = NrfLink::types(types, 70, &complete);
    String out = "{\"types\":[";
    for (uint8_t i = 0; i < count; ++i) {
      if (i) {
        out += ',';
      }
      out += "{\"slug\":\"" + jsonEscape(types[i].slug) + "\"";
      out += ",\"name\":\"" + jsonEscape(types[i].display) + "\"";
      out += ",\"kind\":\"" + jsonEscape(types[i].kind) + "\"";
      out += ",\"id\":\"" + jsonEscape(types[i].id) + "\"";
      out += ",\"hint\":\"" + jsonEscape(types[i].hint) + "\"}";
    }
    out += "]}";
    if (count > 0 && complete) {
      cached = out;
    } else {
      /* Nothing at all from a sleeping nRF, or an answer cut short mid-list:
       * serve what there is and ask again next time. */
      sendJson(out);
      return;
    }
  }
  sendJson(cached);
}

/* ---------------------------------------------------------------- sessions
 * A cookie, a token, and Users:: owning everything past that. See users.h for
 * why /api/cmd is left open while no account exists yet. */

const char *const kCookieName = "concordia_session";

String cookieValue(const String &name) {
  const String all = server.header("Cookie");
  const int at = all.indexOf(name + "=");
  if (at < 0) {
    return "";
  }
  int start = at + name.length() + 1;
  int end = all.indexOf(';', start);
  if (end < 0) {
    end = all.length();
  }
  String v = all.substring(start, end);
  v.trim();
  return v;
}

bool currentUser(Users::User &out) {
  return Users::sessionUser(cookieValue(kCookieName), out);
}

void handleLogin() {
  const String name = server.arg("name");
  const String password = server.arg("password");
  Users::User u;
  if (name.length() == 0 || password.length() == 0 || !Users::verifyPassword(name, password, u)) {
    static uint16_t refusals = 0;
    Warn::raise("auth", Warn::kWarn, "failed sign-in",
                name + " from " + server.client().remoteIP().toString() + ", attempt " +
                    String(++refusals) + " since boot");
    server.send(401, "application/json", "{\"ok\":false,\"error\":\"wrong name or password\"}");
    return;
  }
  const String token = Users::createSession(u.id);
  server.sendHeader("Set-Cookie", String(kCookieName) + "=" + token +
                                       "; Path=/; Max-Age=" +
                                       String(Users::sessionLifetimeHours() * 3600UL) +
                                       "; HttpOnly; SameSite=Lax");
  Log::addf("[auth] \"%s\" signed in", u.name.c_str());
  sendJson(Users::toJson(u));
}

void handleLogout() {
  const String token = cookieValue(kCookieName);
  if (token.length()) {
    Users::destroySession(token);
  }
  server.sendHeader("Set-Cookie", String(kCookieName) + "=deleted; Path=/; Max-Age=0");
  sendJson("{\"ok\":true}");
}

/* 200, not 401: this is a status check ("am I signed in"), polled routinely
 * by every one of the three pages on every load, not a denied action - an
 * action that gets refused is what 401 means elsewhere on this API
 * (handleCmd(), for instance). No caller anywhere in this codebase looks at
 * the HTTP status here, only the {"ok":false} body, so this changes nothing
 * about what any of them do - it only stops a routine, already-handled
 * check from painting a red "Failed to load resource: 401" in devtools on
 * every single page load before anyone has signed in. */
void handleWhoAmI() {
  Users::User u;
  if (!currentUser(u)) {
    server.send(200, "application/json", "{\"ok\":false}");
    return;
  }
  sendJson(Users::toJson(u));
}

/* Admin-only management of the account table itself - separate from every
 * other permission, because it is how the others get handed out. */
bool requireAdmin(Users::User &out) {
  if (currentUser(out) && out.role == "admin") {
    return true;
  }
  server.send(403, "application/json", "{\"ok\":false,\"error\":\"admin only\"}");
  return false;
}

void handleUsersList() {
  Users::User admin;
  if (!requireAdmin(admin)) {
    return;
  }
  sendJson(Users::listJson());
}

void handleUsersPost() {
  /* The one bootstrap exception: creating the very first account has nobody
   * to be an admin yet, same shape as `user add` already having one at the
   * console and /api/cmd staying open until an account exists - see users.h.
   * Nothing here is reachable once anyone exists: `id > 0` can never resolve
   * (findById fails with an empty table) and Users::create() forces the
   * first-ever account to admin regardless of what role was asked for, so
   * there is no path from "no admin yet" to "arbitrary account created". */
  const bool bootstrap = !Users::anyExist();
  Users::User admin;
  if (!bootstrap && !requireAdmin(admin)) {
    return;
  }
  const long id = server.arg("id").toInt();
  String error;

  if (id > 0) {
    Users::User u;
    if (!Users::findById((uint32_t)id, u)) {
      server.send(404, "application/json", "{\"ok\":false,\"error\":\"no such user\"}");
      return;
    }
    bool ok = true;
    if (ok && server.hasArg("role")) {
      ok = Users::setRole((uint32_t)id, server.arg("role"), error);
    }
    if (ok && server.arg("password").length()) {
      ok = Users::setPassword((uint32_t)id, server.arg("password"), error);
    }
    if (ok && server.hasArg("control")) {
      ok = Users::setControl((uint32_t)id, server.arg("control"), error);
    }
    if (ok && server.hasArg("add")) {
      ok = Users::setFlag((uint32_t)id, "add", server.arg("add") == "1", error);
    }
    if (ok && server.hasArg("remove")) {
      ok = Users::setFlag((uint32_t)id, "remove", server.arg("remove") == "1", error);
    }
    if (ok && server.hasArg("settings")) {
      ok = Users::setFlag((uint32_t)id, "settings", server.arg("settings") == "1", error);
    }
    if (!ok) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
      return;
    }
    Users::findById((uint32_t)id, u);
    Log::addf("[auth] \"%s\" updated user #%ld", admin.name.c_str(), id);
    sendJson(Users::toJson(u));
    return;
  }

  const String name = server.arg("name");
  const String password = server.arg("password");
  const String role = server.arg("role");
  if (!Users::create(name, password, role, error)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
    return;
  }
  if (bootstrap) {
    /* This call is what just made Users::anyExist() true. If it came from the
     * wizard, mark setup as still in progress so a reload does not drop the
     * visitor into an unfinished panel - see Card::setupPending(). If it came
     * from the console instead, `user add` never runs this handler at all,
     * so this line simply never executes for that path. */
    Card::setSetupPending(true);
  }
  Users::User u;
  Users::findByName(name, u);
  const String by = bootstrap ? "(setup)" : "\"" + admin.name + "\"";
  Log::addf("[auth] %s created \"%s\"", by.c_str(), name.c_str());
  sendJson(Users::toJson(u));
}

void handleUsersDelete() {
  Users::User admin;
  if (!requireAdmin(admin)) {
    return;
  }
  const uint32_t id = (uint32_t)server.arg("id").toInt();
  String error;
  if (!Users::remove(id, error)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
    return;
  }
  Log::addf("[auth] \"%s\" removed user #%u", admin.name.c_str(), (unsigned)id);
  sendJson("{\"ok\":true}");
}

/* ----------------------------------------------------------------- avatars
 * A picture is not a secret - served openly, the same trust level as a name.
 * Written only by that person or an admin. PNG or JPEG, sniffed from the
 * signature rather than trusted from a filename or a Content-Type header -
 * both are just words the browser sent, and a phone photo is JPEG far more
 * often than PNG. */
const size_t kMaxAvatarBytes = 100 * 1024;
File gAvatarUp;
uint32_t gAvatarUpId = 0;
String gAvatarUpError;

void handleAvatarGet() {
  const uint32_t id = (uint32_t)server.arg("id").toInt();
  const char *ext = (id && Card::ready()) ? Users::avatarExt(id) : "";
  File f = ext[0] ? SD.open(Users::avatarPath(id, ext), FILE_READ) : File();
  if (!f) {
    server.send(404, "text/plain", "not found");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.streamFile(f, strcmp(ext, "png") == 0 ? "image/png" : "image/jpeg");
  f.close();
}

void handleAvatarUploadData() {
  HTTPUpload &up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    gAvatarUpError = "";
    gAvatarUpId = (uint32_t)server.arg("id").toInt();
    Users::User me;
    if (!Card::ready()) {
      gAvatarUpError = "no card";
    } else if (gAvatarUpId == 0 || !currentUser(me) ||
               (me.role != "admin" && me.id != gAvatarUpId)) {
      gAvatarUpError = "not allowed";
    }
    if (gAvatarUpError.length()) {
      return;
    }
    const String staged = String(Card::kTmp) + "/avatar_incoming";
    if (SD.exists(staged)) {
      SD.remove(staged);
    }
    gAvatarUp = SD.open(staged, FILE_WRITE);
    if (!gAvatarUp) {
      gAvatarUpError = "cannot open a staging file";
    }
    return;
  }

  if (up.status == UPLOAD_FILE_WRITE) {
    if (!gAvatarUp) {
      return;
    }
    if (gAvatarUp.size() + up.currentSize > kMaxAvatarBytes) {
      gAvatarUpError = "too large (100 KB max)";
      gAvatarUp.close();
      return;
    }
    if (gAvatarUp.write(up.buf, up.currentSize) != up.currentSize) {
      gAvatarUpError = "the card would not take it";
      gAvatarUp.close();
    }
    return;
  }

  if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (gAvatarUp) {
      gAvatarUp.close();
    }
    if (up.status == UPLOAD_FILE_ABORTED && gAvatarUpError.length() == 0) {
      gAvatarUpError = "the upload was cut short";
    }
  }
}

void handleAvatarUploadDone() {
  const String staged = String(Card::kTmp) + "/avatar_incoming";

  if (gAvatarUpError.length()) {
    if (SD.exists(staged)) {
      SD.remove(staged);
    }
    server.send(gAvatarUpError == "not allowed" ? 403 : 400, "application/json",
                "{\"ok\":false,\"error\":\"" + jsonEscape(gAvatarUpError) + "\"}");
    return;
  }

  File check = SD.open(staged, FILE_READ);
  uint8_t sig[8] = {0};
  const size_t got = check ? check.read(sig, sizeof(sig)) : 0;
  if (check) {
    check.close();
  }
  const bool isPng = got == sizeof(sig) && memcmp(sig, "\x89PNG\r\n\x1a\n", sizeof(sig)) == 0;
  const bool isJpeg = got >= 3 && sig[0] == 0xFF && sig[1] == 0xD8 && sig[2] == 0xFF;
  if (!isPng && !isJpeg) {
    SD.remove(staged);
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"not a PNG or JPEG\"}");
    return;
  }

  /* Only one avatar per user: drop whichever extension is not the one just
   * uploaded, so avatarExt() never has to choose between two files that
   * disagree about which is current. */
  const char *ext = isPng ? "png" : "jpg";
  const char *stale = isPng ? "jpg" : "png";
  const String staleTarget = Users::avatarPath(gAvatarUpId, stale);
  if (SD.exists(staleTarget)) {
    SD.remove(staleTarget);
  }

  const String target = Users::avatarPath(gAvatarUpId, ext);
  if (SD.exists(target)) {
    SD.remove(target);
  }
  if (!SD.rename(staged, target)) {
    SD.remove(staged);
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"could not save it\"}");
    return;
  }
  Log::addf("[auth] avatar updated for user #%u", (unsigned)gAvatarUpId);
  sendJson("{\"ok\":true}");
}

/* This is the endpoint the roadmap calls the open hole: it used to answer
 * anyone on the network, including a factory reset. While no account exists
 * it still does - see users.h for why that bootstrap window is deliberate -
 * but the instant the first admin is created, every command needs a session
 * and the permission it actually calls for. */
void handleCmd() {
  const String cmd = server.arg("cmd");
  if (cmd.length() == 0) {
    sendJson("{\"out\":\"\"}");
    return;
  }
  if (Users::anyExist()) {
    Users::User u;
    if (!currentUser(u)) {
      server.send(401, "application/json", "{\"out\":\"\",\"error\":\"sign in first\"}");
      return;
    }
    String why;
    if (!Users::allow(u, cmd, why)) {
      server.send(403, "application/json", "{\"out\":\"\",\"error\":\"" + jsonEscape(why) + "\"}");
      return;
    }
  }
  Log::addf("[web] %s", cmd.c_str());
  const String out = Commands::execute(cmd);
  sendJson("{\"out\":\"" + jsonEscape(out) + "\"}");
}

/* The onboarding codes - from the cache NrfLink filled in at boot, while the
 * line was still clean, never from a live query. Live would mean touching the
 * link at exactly the moment it is least likely to answer: once BLE
 * advertising is running, which is the whole reason this is cached instead -
 * see NrfLink::refreshPairingCache(). */
void handlePairing() {
  String manual, qr;
  NrfLink::pairingCode(manual, qr);
  sendJson("{\"manual\":\"" + jsonEscape(manual) + "\",\"qr\":\"" + jsonEscape(qr) + "\"}");
}

/* The panel's own settings travel as one opaque document - see Storage. */
void handleUiGet() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", Storage::uiConfig());
}

void handleUiPost() {
  const String body = server.hasArg("plain") ? server.arg("plain") : server.arg("ui");
  const bool ok = Storage::setUiConfig(body);
  /* The wizard's finishSetup() ends with exactly this call - while setup is
   * still pending, nothing else could have reached it, since handlePage()
   * refuses to serve the real panel until this fires. See Card::setupPending(). */
  if (ok && Card::setupPending()) {
    Card::setSetupPending(false);
  }
  sendJson(String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

void handleLog() { sendJson("{\"log\":\"" + jsonEscape(Log::dump()) + "\"}"); }

/* Readable without a token, unlike the file API: a warning list is meant to be
 * the first thing anyone sees, and it carries no secrets - only that something
 * happened and when. Clearing it does need the token, because that is a change. */
void handleLogging() {
  String out = "{\"level\":" + String(Log::level());
  out += ",\"cats\":" + String(Log::categories());
  out += ",\"names\":[";
  for (uint8_t i = 0; i < Log::catCount(); ++i) {
    if (i) {
      out += ',';
    }
    out += "\"" + String(Log::catName(i)) + "\"";
  }
  out += "]}";
  sendJson(out);
}

/* Changing what gets recorded is a change like any other, so it needs the
 * token. Reading the current setting does not. */
void handleLoggingSet() {
  if (!Card::authorised(server.header("X-Token"))) {
    server.send(403, "application/json",
                "{\"ok\":false,\"error\":\"bad or missing X-Token\"}");
    return;
  }
  const long lvl = server.arg("level").toInt();
  const long cats = server.arg("cats").toInt();
  if (lvl < 0 || lvl > Log::kEverything) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"level must be 0..5\"}");
    return;
  }
  Log::configure((uint8_t)lvl, (uint16_t)cats);
  const bool saved = Log::saveConfig();
  Log::addf("[sys] logging level %ld, categories 0x%03lx", lvl, cats);
  sendJson(String("{\"ok\":true,\"saved\":") + (saved ? "true" : "false") + "}");
}

void handleWarnings() {
  sendJson("{\"sinceBoot\":" + String(Warn::sinceBoot()) +
           ",\"run\":" + String(Warn::runId()) +
           ",\"items\":" + Warn::recentJson(24) + "}");
}

void handleWarningsClear() {
  if (!Card::authorised(server.header("X-Token"))) {
    server.send(403, "application/json",
                "{\"ok\":false,\"error\":\"bad or missing X-Token\"}");
    return;
  }
  Warn::clear();
  sendJson("{\"ok\":true}");
}

void handleLang() {
  const String lang = server.arg("lang");
  if (lang == "en" || lang == "de" || lang == "da") {
    Storage::setLanguage(lang);
  }
  sendJson("{\"lang\":\"" + Storage::language() + "\"}");
}

/* Any unknown path inside the portal returns the page itself, which is what
 * turns a phone's connectivity check into an automatic pop-up. */
void handleNotFound() {
  if (Net::mode() == NET_PORTAL) {
    server.sendHeader("Location", "http://" + Net::ipAddress() + "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  server.send(404, "text/plain", "not found");
}

} // namespace

namespace WebUi {

String escape(const String &in) { return jsonEscape(in); }

void begin() {
  server.on("/", HTTP_GET, handlePage);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/devices", HTTP_GET, handleDevices);
  server.on("/api/types", HTTP_GET, handleTypes);
  server.on("/api/log", HTTP_GET, handleLog);
  server.on("/api/pairing", HTTP_GET, handlePairing);
  server.on("/api/ui", HTTP_GET, handleUiGet);
  server.on("/api/ui", HTTP_POST, handleUiPost);
  server.on("/api/cmd", HTTP_POST, handleCmd);
  server.on("/api/lang", HTTP_POST, handleLang);
  server.on("/api/sd", HTTP_GET, handleSdStatus);
  server.on("/api/logging", HTTP_GET, handleLogging);
  server.on("/api/logging", HTTP_POST, handleLoggingSet);
  server.on("/api/warnings", HTTP_GET, handleWarnings);
  server.on("/api/warnings/clear", HTTP_POST, handleWarningsClear);
  server.on("/api/web", HTTP_POST, handleWebUploadDone, handleWebUploadData);
  server.on("/api/web", HTTP_DELETE, handleWebRemove);
  server.on("/api/session/login", HTTP_POST, handleLogin);
  server.on("/api/session/logout", HTTP_POST, handleLogout);
  server.on("/api/session", HTTP_GET, handleWhoAmI);
  server.on("/api/users", HTTP_GET, handleUsersList);
  server.on("/api/users", HTTP_POST, handleUsersPost);
  server.on("/api/users", HTTP_DELETE, handleUsersDelete);
  server.on("/api/users/avatar", HTTP_GET, handleAvatarGet);
  server.on("/api/users/avatar", HTTP_POST, handleAvatarUploadDone, handleAvatarUploadData);

  /* Custom headers are dropped unless they are asked for by name, and the
   * upload token travels in one, the session cookie in the other. Without
   * this the API would refuse every request for reasons that look like
   * nothing at all. */
  static const char *kWanted[] = {"X-Token", "Cookie"};
  server.collectHeaders(kWanted, 2);

  FsApi::attach(server);

  /* Captive-portal probes used by the major platforms. */
  server.on("/generate_204", HTTP_GET, handleNotFound);
  server.on("/gen_204", HTTP_GET, handleNotFound);
  server.on("/hotspot-detect.html", HTTP_GET, handleNotFound);
  server.on("/library/test/success.html", HTTP_GET, handleNotFound);
  server.on("/connecttest.txt", HTTP_GET, handleNotFound);
  server.on("/ncsi.txt", HTTP_GET, handleNotFound);
  server.onNotFound(handleNotFound);

  server.begin();
  Log::addf("[web] http://%s/", Net::ipAddress().c_str());
}

void loop() { server.handleClient(); }

} // namespace WebUi
