#include "users.h"

#include <SD.h>
#include <esp_random.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

#include "config.h"
#include "log_buf.h"
#include "sd_card.h"
#include "web_ui.h"

namespace Users {

namespace {

const char *const kFile = "/Concordia/security/users.json";

/* Salt and hash travel as hex in the JSON and as raw bytes everywhere else -
 * there is no reason to keep both forms of a 32-byte hash in RAM at once. */
struct Record {
  uint32_t id = 0;
  String name;
  String role;
  Permissions perm;
  uint8_t salt[16] = {0};
  uint8_t hash[32] = {0};
  uint32_t iter = PBKDF2_ITERATIONS;
};

struct Session {
  char token[33] = {0}; /* empty = free slot */
  uint32_t userId = 0;
  unsigned long expiresAt = 0;
};

Record gUsers[MAX_USERS];
uint8_t gCount = 0;
uint32_t gNextId = 1;
uint32_t gSessionHours = SESSION_LIFETIME_HOURS_DEFAULT;

Session gSessions[MAX_SESSIONS];

/* --------------------------------------------------------------- utilities */

String toHex(const uint8_t *buf, size_t n) {
  static const char *digits = "0123456789abcdef";
  String out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    out += digits[buf[i] >> 4];
    out += digits[buf[i] & 0x0f];
  }
  return out;
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool fromHex(const String &hex, uint8_t *out, size_t n) {
  if (hex.length() != n * 2) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    const int hi = hexVal(hex[i * 2]);
    const int lo = hexVal(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

bool derive(const String &password, const uint8_t salt[16], uint32_t iterations,
            uint8_t out[32]) {
  return mbedtls_pkcs5_pbkdf2_hmac_ext(
             MBEDTLS_MD_SHA256, (const unsigned char *)password.c_str(),
             password.length(), salt, 16, iterations, 32, out) == 0;
}

String randomToken() {
  uint8_t buf[16];
  esp_fill_random(buf, sizeof(buf));
  return toHex(buf, sizeof(buf));
}

/* First word (n=0) is the command itself. Deliberately not the arg() helper
 * in commands.cpp - classification only ever needs a raw token, never the
 * percent-decoding that names and passwords want. */
String word(const String &line, uint8_t n) {
  int start = 0;
  for (uint8_t i = 0; i < n; ++i) {
    const int space = line.indexOf(' ', start);
    if (space < 0) {
      return String();
    }
    start = space + 1;
    while (start < (int)line.length() && line[start] == ' ') {
      ++start;
    }
  }
  const int space = line.indexOf(' ', start);
  return space < 0 ? line.substring(start) : line.substring(start, space);
}

Record *find(uint32_t id) {
  for (uint8_t i = 0; i < gCount; ++i) {
    if (gUsers[i].id == id) {
      return &gUsers[i];
    }
  }
  return nullptr;
}

User toPublic(const Record &r) {
  User u;
  u.id = r.id;
  u.name = r.name;
  u.role = r.role;
  u.perm = r.perm;
  return u;
}

/* Admin is a constant: every path that can touch a role or a permission
 * routes through here rather than trusting the caller to have remembered. */
void forceAdminPermissions(Record &r) {
  if (r.role != "admin") {
    return;
  }
  r.perm.control = Control::kAll;
  r.perm.addDevices = true;
  r.perm.removeDevices = true;
  r.perm.settings = true;
}

/* ---------------------------------------------------------- tiny JSON read
 * Reads only what this module ever wrote itself, so it is a fixed-shape
 * reader rather than a general parser - see json_path.h for that job. */

String jsonStr(const String &obj, const String &key) {
  const int at = obj.indexOf("\"" + key + "\"");
  if (at < 0) {
    return "";
  }
  int i = obj.indexOf(':', at);
  if (i < 0) {
    return "";
  }
  ++i;
  while (i < (int)obj.length() && obj[i] == ' ') {
    ++i;
  }
  if (i >= (int)obj.length() || obj[i] != '"') {
    return "";
  }
  ++i;
  String out;
  while (i < (int)obj.length() && obj[i] != '"') {
    if (obj[i] == '\\' && i + 1 < (int)obj.length()) {
      ++i;
      switch (obj[i]) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case 'u':
        if (i + 4 < (int)obj.length()) {
          const long v = strtol(obj.substring(i + 1, i + 5).c_str(), nullptr, 16);
          if (v > 0 && v < 0x100) {
            out += (char)v;
          }
          i += 4;
        }
        break;
      default: out += obj[i]; break;
      }
      ++i;
      continue;
    }
    out += obj[i++];
  }
  return out;
}

long jsonNum(const String &obj, const String &key, long fallback) {
  const int at = obj.indexOf("\"" + key + "\"");
  if (at < 0) {
    return fallback;
  }
  int i = obj.indexOf(':', at);
  if (i < 0) {
    return fallback;
  }
  ++i;
  while (i < (int)obj.length() && obj[i] == ' ') {
    ++i;
  }
  const int start = i;
  if (i < (int)obj.length() && obj[i] == '-') {
    ++i;
  }
  while (i < (int)obj.length() && isdigit((unsigned char)obj[i])) {
    ++i;
  }
  if (i == start) {
    return fallback;
  }
  return obj.substring(start, i).toInt();
}

bool jsonBool(const String &obj, const String &key, bool fallback) {
  const int at = obj.indexOf("\"" + key + "\"");
  if (at < 0) {
    return fallback;
  }
  int i = obj.indexOf(':', at);
  if (i < 0) {
    return fallback;
  }
  ++i;
  while (i < (int)obj.length() && obj[i] == ' ') {
    ++i;
  }
  return obj.substring(i, i + 4) == "true";
}

void parseUser(const String &obj) {
  if (gCount >= MAX_USERS) {
    return;
  }
  Record r;
  r.id = (uint32_t)jsonNum(obj, "id", 0);
  r.name = jsonStr(obj, "name");
  r.role = jsonStr(obj, "role");
  const String salt = jsonStr(obj, "salt");
  const String hash = jsonStr(obj, "hash");
  if (r.id == 0 || r.name.length() == 0 || !fromHex(salt, r.salt, sizeof(r.salt)) ||
      !fromHex(hash, r.hash, sizeof(r.hash))) {
    Log::add("[auth] skipped an unreadable entry in users.json");
    return;
  }
  r.iter = (uint32_t)jsonNum(obj, "iter", PBKDF2_ITERATIONS);
  const String ctrl = jsonStr(obj, "control");
  if (ctrl == "all") {
    r.perm.control = Control::kAll;
  } else if (ctrl.length() == 0 || ctrl == "none") {
    r.perm.control = Control::kNone;
  } else {
    r.perm.control = Control::kList;
    r.perm.controlList = ctrl;
  }
  r.perm.addDevices = jsonBool(obj, "add", false);
  r.perm.removeDevices = jsonBool(obj, "remove", false);
  r.perm.settings = jsonBool(obj, "settings", false);
  forceAdminPermissions(r);

  gUsers[gCount++] = r;
  if (r.id >= gNextId) {
    gNextId = r.id + 1;
  }
}

/* ---------------------------------------------------------------- writing
 * Staged then renamed, same as every other file this firmware writes to the
 * card - a failure partway through never corrupts the working copy. */
bool save() {
  if (!Card::ready()) {
    return false;
  }
  String out = "{\n  \"nextId\": " + String(gNextId) +
               ",\n  \"sessionHours\": " + String(gSessionHours) + ",\n  \"users\": [\n";
  for (uint8_t i = 0; i < gCount; ++i) {
    const Record &r = gUsers[i];
    const String ctrl = r.perm.control == Control::kAll   ? "all"
                        : r.perm.control == Control::kNone ? "none"
                                                            : r.perm.controlList;
    out += "    {\"id\":" + String(r.id) + ",\"name\":\"" + WebUi::escape(r.name) +
           "\",\"role\":\"" + r.role + "\",\"salt\":\"" + toHex(r.salt, sizeof(r.salt)) +
           "\",\"hash\":\"" + toHex(r.hash, sizeof(r.hash)) + "\",\"iter\":" +
           String(r.iter) + ",\"control\":\"" + WebUi::escape(ctrl) + "\",\"add\":" +
           (r.perm.addDevices ? "true" : "false") + ",\"remove\":" +
           (r.perm.removeDevices ? "true" : "false") + ",\"settings\":" +
           (r.perm.settings ? "true" : "false") + "}";
    out += (i + 1 < gCount) ? ",\n" : "\n";
  }
  out += "  ]\n}\n";

  const String staged = String(Card::kTmp) + "/users_save";
  if (SD.exists(staged)) {
    SD.remove(staged);
  }
  File f = SD.open(staged, FILE_WRITE);
  if (!f) {
    return false;
  }
  const size_t wrote = f.print(out);
  f.close();
  if (wrote != out.length()) {
    SD.remove(staged);
    return false;
  }
  if (SD.exists(kFile)) {
    SD.remove(kFile);
  }
  return SD.rename(staged, kFile);
}

} // namespace

void begin() {
  gCount = 0;
  gNextId = 1;
  gSessionHours = SESSION_LIFETIME_HOURS_DEFAULT;

  if (!Card::ready() || !SD.exists(kFile)) {
    Log::add("[auth] no users.json yet - /api/cmd stays open until `user add` "
             "creates the first admin");
    return;
  }
  File f = SD.open(kFile, FILE_READ);
  if (!f) {
    Log::add("[auth] users.json exists but would not open");
    return;
  }
  String doc;
  doc.reserve(f.size() + 1);
  while (f.available()) {
    doc += (char)f.read();
  }
  f.close();

  gNextId = (uint32_t)jsonNum(doc, "nextId", 1);
  gSessionHours = (uint32_t)jsonNum(doc, "sessionHours", SESSION_LIFETIME_HOURS_DEFAULT);

  const int arrAt = doc.indexOf("\"users\"");
  const int bracket = arrAt < 0 ? -1 : doc.indexOf('[', arrAt);
  if (bracket < 0) {
    return;
  }

  int i = bracket + 1;
  while (gCount < MAX_USERS) {
    while (i < (int)doc.length() && doc[i] != '{' && doc[i] != ']') {
      ++i;
    }
    if (i >= (int)doc.length() || doc[i] == ']') {
      break;
    }
    const int objStart = i;
    int depth = 0;
    bool inStr = false;
    for (; i < (int)doc.length(); ++i) {
      const char c = doc[i];
      if (inStr) {
        if (c == '\\') {
          ++i;
        } else if (c == '"') {
          inStr = false;
        }
        continue;
      }
      if (c == '"') {
        inStr = true;
      } else if (c == '{') {
        ++depth;
      } else if (c == '}') {
        --depth;
        if (depth == 0) {
          ++i;
          break;
        }
      }
    }
    parseUser(doc.substring(objStart, i));
  }
  Log::addf("[auth] %u user(s) loaded, sessions last %luh", gCount,
            (unsigned long)gSessionHours);
}

void factoryReset() {
  gCount = 0;
  gNextId = 1;
  for (auto &s : gSessions) {
    s.token[0] = '\0';
  }
  if (Card::ready() && SD.exists(kFile)) {
    SD.remove(kFile);
  }
  Log::add("[auth] users wiped - /api/cmd is open again until a new admin is created");
}

bool anyExist() { return gCount > 0; }
uint8_t count() { return gCount; }

bool at(uint8_t index, User &out) {
  if (index >= gCount) {
    return false;
  }
  out = toPublic(gUsers[index]);
  return true;
}

bool findByName(const String &name, User &out) {
  for (uint8_t i = 0; i < gCount; ++i) {
    if (gUsers[i].name.equalsIgnoreCase(name)) {
      out = toPublic(gUsers[i]);
      return true;
    }
  }
  return false;
}

bool findById(uint32_t id, User &out) {
  Record *r = find(id);
  if (!r) {
    return false;
  }
  out = toPublic(*r);
  return true;
}

bool create(const String &name, const String &password, const String &role, String &error) {
  if (!Card::ready()) {
    error = "no card";
    return false;
  }
  if (name.length() < 2 || name.length() > 32) {
    error = "name must be 2-32 characters";
    return false;
  }
  if (password.length() < 8) {
    error = "password must be at least 8 characters";
    return false;
  }
  for (uint8_t i = 0; i < gCount; ++i) {
    if (gUsers[i].name.equalsIgnoreCase(name)) {
      error = "that name is taken";
      return false;
    }
  }
  if (gCount >= MAX_USERS) {
    error = "the user table is full (" + String(MAX_USERS) + ")";
    return false;
  }

  Record r;
  r.id = gNextId;
  r.name = name;
  /* The first account ever created has nobody to grant it admin, so it grants
   * itself - every account after that gets exactly the role asked for. */
  r.role = (gCount == 0 || role == "admin") ? "admin" : "user";
  esp_fill_random(r.salt, sizeof(r.salt));
  r.iter = PBKDF2_ITERATIONS;
  if (!derive(password, r.salt, r.iter, r.hash)) {
    error = "hashing failed";
    return false;
  }
  forceAdminPermissions(r);

  /* gCount has to move first - save() writes exactly the first gCount
   * records, so a save issued before the count advances would silently drop
   * the very user it was just asked to persist. */
  gUsers[gCount] = r;
  ++gCount;
  if (!save()) {
    --gCount;
    error = "could not write users.json";
    return false;
  }
  ++gNextId;
  return true;
}

bool remove(uint32_t id, String &error) {
  int idx = -1;
  for (uint8_t i = 0; i < gCount; ++i) {
    if (gUsers[i].id == id) {
      idx = i;
      break;
    }
  }
  if (idx < 0) {
    error = "no such user";
    return false;
  }
  /* Admin is a constant, not just "at least one must remain" - see users.h.
   * The only way to remove an admin account at all is the BOOT-button
   * factory reset, the same physical recovery path as a lost password. */
  if (gUsers[idx].role == "admin") {
    error = "admin accounts cannot be removed";
    return false;
  }
  const Record removed = gUsers[idx];
  for (uint8_t i = idx; i + 1 < gCount; ++i) {
    gUsers[i] = gUsers[i + 1];
  }
  --gCount;
  if (!save()) {
    /* Put it back rather than leave RAM and card disagreeing. */
    for (uint8_t i = gCount; i > (uint8_t)idx; --i) {
      gUsers[i] = gUsers[i - 1];
    }
    gUsers[idx] = removed;
    ++gCount;
    error = "could not write users.json";
    return false;
  }
  for (auto &s : gSessions) {
    if (s.userId == id) {
      s.token[0] = '\0';
    }
  }
  return true;
}

bool setPassword(uint32_t id, const String &password, String &error) {
  Record *r = find(id);
  if (!r) {
    error = "no such user";
    return false;
  }
  if (password.length() < 8) {
    error = "password must be at least 8 characters";
    return false;
  }
  uint8_t salt[16], hash[32];
  esp_fill_random(salt, sizeof(salt));
  if (!derive(password, salt, r->iter, hash)) {
    error = "hashing failed";
    return false;
  }
  memcpy(r->salt, salt, sizeof(salt));
  memcpy(r->hash, hash, sizeof(hash));
  if (!save()) {
    error = "could not write users.json";
    return false;
  }
  return true;
}

bool setRole(uint32_t id, const String &role, String &error) {
  Record *r = find(id);
  if (!r) {
    error = "no such user";
    return false;
  }
  if (role != "admin" && role != "user") {
    error = "role must be admin or user";
    return false;
  }
  /* Once admin, always admin - not "unless it is the last one". Promotion
   * (user -> admin) is still how a second admin ever gets made. */
  if (r->role == "admin" && role != "admin") {
    error = "admin is permanent and cannot be demoted";
    return false;
  }
  const String previous = r->role;
  r->role = role;
  forceAdminPermissions(*r);
  if (!save()) {
    r->role = previous;
    error = "could not write users.json";
    return false;
  }
  return true;
}

bool setControl(uint32_t id, const String &spec, String &error) {
  Record *r = find(id);
  if (!r) {
    error = "no such user";
    return false;
  }
  if (r->role == "admin") {
    error = "admin permissions are fixed";
    return false;
  }
  const Permissions previous = r->perm;
  if (spec == "all") {
    r->perm.control = Control::kAll;
  } else if (spec.length() == 0 || spec == "none") {
    r->perm.control = Control::kNone;
  } else {
    r->perm.control = Control::kList;
    r->perm.controlList = spec;
  }
  if (!save()) {
    r->perm = previous;
    error = "could not write users.json";
    return false;
  }
  return true;
}

bool setFlag(uint32_t id, const String &which, bool value, String &error) {
  Record *r = find(id);
  if (!r) {
    error = "no such user";
    return false;
  }
  if (r->role == "admin") {
    error = "admin permissions are fixed";
    return false;
  }
  const Permissions previous = r->perm;
  if (which == "add") {
    r->perm.addDevices = value;
  } else if (which == "remove") {
    r->perm.removeDevices = value;
  } else if (which == "settings") {
    r->perm.settings = value;
  } else {
    error = "unknown permission \"" + which + "\"";
    return false;
  }
  if (!save()) {
    r->perm = previous;
    error = "could not write users.json";
    return false;
  }
  return true;
}

uint32_t sessionLifetimeHours() { return gSessionHours; }

void setSessionLifetimeHours(uint32_t hours) {
  gSessionHours = hours < 1 ? 1 : (hours > 720 ? 720 : hours); /* 1h..30d */
  save();
}

bool verifyPassword(const String &name, const String &password, User &out) {
  for (uint8_t i = 0; i < gCount; ++i) {
    if (!gUsers[i].name.equalsIgnoreCase(name)) {
      continue;
    }
    uint8_t attempt[32];
    if (!derive(password, gUsers[i].salt, gUsers[i].iter, attempt)) {
      return false;
    }
    uint8_t diff = 0;
    for (uint8_t k = 0; k < sizeof(attempt); ++k) {
      diff |= attempt[k] ^ gUsers[i].hash[k];
    }
    if (diff != 0) {
      return false;
    }
    out = toPublic(gUsers[i]);
    return true;
  }
  /* No such name: still spend a hash, so guessing a valid name is not faster
   * than guessing a valid password. */
  uint8_t salt[16] = {0}, discard[32];
  derive(password, salt, PBKDF2_ITERATIONS, discard);
  return false;
}

String createSession(uint32_t userId) {
  int slot = -1;
  for (uint8_t i = 0; i < MAX_SESSIONS; ++i) {
    if (gSessions[i].token[0] == '\0' || (long)(millis() - gSessions[i].expiresAt) > 0) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    slot = 0; /* every slot busy and still fresh: evict slot 0 rather than
               * fail the login - MAX_SESSIONS browsers open at once on a
               * home hub is not a case worth an eviction policy for. */
  }
  const String token = randomToken();
  token.toCharArray(gSessions[slot].token, sizeof(gSessions[slot].token));
  gSessions[slot].userId = userId;
  gSessions[slot].expiresAt = millis() + gSessionHours * 3600000UL;
  return token;
}

bool destroySession(const String &token) {
  for (uint8_t i = 0; i < MAX_SESSIONS; ++i) {
    if (token.equals(gSessions[i].token)) {
      gSessions[i].token[0] = '\0';
      return true;
    }
  }
  return false;
}

bool sessionUser(const String &token, User &out) {
  if (token.length() == 0) {
    return false;
  }
  for (uint8_t i = 0; i < MAX_SESSIONS; ++i) {
    if (!token.equals(gSessions[i].token)) {
      continue;
    }
    if ((long)(millis() - gSessions[i].expiresAt) > 0) {
      gSessions[i].token[0] = '\0';
      return false;
    }
    Record *r = find(gSessions[i].userId);
    if (!r) {
      return false;
    }
    out = toPublic(*r);
    return true;
  }
  return false;
}

namespace {
bool controlAllows(const Permissions &p, uint16_t ep) {
  if (p.control == Control::kAll) {
    return true;
  }
  if (p.control == Control::kNone) {
    return false;
  }
  const String needle = "," + String(ep) + ",";
  return ("," + p.controlList + ",").indexOf(needle) >= 0;
}
} // namespace

bool allow(const User &u, const String &commandLine, String &why) {
  String line = commandLine;
  line.trim();
  const String cmd = word(line, 0);

  static const char *const kReadOnly[] = {"help", "?",     "status",  "list",
                                           "types", "bindings", "test", "log",
                                           "pairing", "lang"};
  for (const char *rc : kReadOnly) {
    if (cmd == rc) {
      return true;
    }
  }
  if (u.role == "admin") {
    return true;
  }

  if (cmd == "set") {
    const uint16_t ep = (uint16_t)word(line, 1).toInt();
    if (controlAllows(u.perm, ep)) {
      return true;
    }
    why = "no control permission for EP" + String(ep);
    return false;
  }
  if (cmd == "add") {
    if (u.perm.addDevices) {
      return true;
    }
    why = "no permission to add devices";
    return false;
  }
  if (cmd == "rm" || cmd == "remove") {
    if (u.perm.removeDevices) {
      return true;
    }
    why = "no permission to remove devices";
    return false;
  }
  if (cmd == "rename" || cmd == "retype") {
    if (u.perm.addDevices && u.perm.removeDevices) {
      return true;
    }
    why = "renaming needs both add and remove permission";
    return false;
  }
  if (cmd == "user") {
    why = "user management is admin-only";
    return false;
  }
  /* Everything else - wifi, bind, unbind, nrf, reboot, factoryreset, log
   * clear - is a hub setting. */
  if (u.perm.settings) {
    return true;
  }
  why = "no settings permission";
  return false;
}

String avatarPath(uint32_t id, const char *ext) {
  return String(Card::kAvatars) + "/" + String(id) + "." + ext;
}

const char *avatarExt(uint32_t id) {
  if (SD.exists(avatarPath(id, "png"))) {
    return "png";
  }
  if (SD.exists(avatarPath(id, "jpg"))) {
    return "jpg";
  }
  return "";
}

String toJson(const User &u) {
  const String ctrl = u.perm.control == Control::kAll   ? "all"
                      : u.perm.control == Control::kNone ? "none"
                                                          : u.perm.controlList;
  String out = "{\"id\":" + String(u.id) + ",\"name\":\"" + WebUi::escape(u.name) +
               "\",\"role\":\"" + u.role + "\",\"avatar\":" +
               (avatarExt(u.id)[0] ? "true" : "false") + ",\"perm\":{\"control\":\"" +
               WebUi::escape(ctrl) + "\",\"add\":" +
               (u.perm.addDevices ? "true" : "false") + ",\"remove\":" +
               (u.perm.removeDevices ? "true" : "false") + ",\"settings\":" +
               (u.perm.settings ? "true" : "false") + "}}";
  return out;
}

String listJson() {
  String out = "{\"sessionHours\":" + String(gSessionHours) + ",\"users\":[";
  for (uint8_t i = 0; i < gCount; ++i) {
    if (i) {
      out += ',';
    }
    out += toJson(toPublic(gUsers[i]));
  }
  out += "]}";
  return out;
}

} // namespace Users
