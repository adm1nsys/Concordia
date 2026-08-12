/*
 * Accounts, permissions and browser sessions.
 *
 * Modelled on Homebridge, with one hard rule: admin is a constant. The role
 * cannot be restricted, cannot lose a permission, cannot be deleted, and at
 * least one must always exist - enforced here, not only by the panel that
 * calls in.
 *
 * Records live at /Concordia/security/users.json, written as real JSON so a
 * laptop with the card in it can read the shape even though only this file
 * parses it back. Passwords are never stored: a random salt plus a
 * PBKDF2-HMAC-SHA256 hash are, and mbedtls already ships with the core.
 *
 * ## The bootstrap window
 *
 * There is no first-run wizard yet (that is a later roadmap item), so there
 * is deliberately no chicken-and-egg lock: while no user exists, anyExist()
 * is false and the web layer leaves /api/cmd exactly as open as it is today.
 * The moment `user add` creates the first account from the trusted USB
 * console, that account is forced to admin and every command from the network
 * needs a session and a permission from then on. Same shape as the SD upload
 * token, which hands itself out only while no panel is installed yet.
 *
 * ## Permissions
 *
 * - **control**: `all` a device, `none`, or a specific list of endpoint ids -
 *   governs `set`. Readings (`list`, `status`, `/api/devices`) are visible to
 *   any signed-in user regardless; that was the point of separating it out.
 * - **add / remove devices**: two independent yes/no switches, governing
 *   `add` and `rm`. `rename`/`retype` need both, because both recreate the
 *   endpoint.
 * - **settings**: everything else a command can do - Wi-Fi, bindings, the
 *   nRF passthrough, reboot, factory reset.
 *
 * User management itself (the `user` command, `/api/users`) is admin-only,
 * full stop - it is how permissions get handed out in the first place.
 *
 * Sessions are a token in a cookie, a small table in RAM, and nothing more:
 * a reboot signs everyone out, which is the same trade every other piece of
 * runtime state in this firmware already makes.
 */
#pragma once

#include <Arduino.h>

namespace Users {

enum class Control { kNone, kAll, kList };

struct Permissions {
  Control control = Control::kNone;
  String controlList;      /* csv of endpoint ids; only when control == kList */
  bool addDevices = false;
  bool removeDevices = false;
  bool settings = false;
};

struct User {
  uint32_t id = 0;
  String name;
  String role; /* "admin" | "user" */
  Permissions perm;
};

/* Loads users.json from the card, if there is one. Safe to call with no card
 * or no file - that is the bootstrap state, not an error. */
void begin();

/* Deletes users.json and every session. The recovery path for a lost
 * password: hold the same BOOT button that already wipes Wi-Fi and the nRF's
 * pairings. */
void factoryReset();

bool anyExist();
uint8_t count();
bool at(uint8_t index, User &out);
bool findByName(const String &name, User &out);
bool findById(uint32_t id, User &out);

/* Management. Callers are responsible for their own trust check - the console
 * is physical access and always allowed; the HTTP handlers require the caller
 * to already be an admin. The first user ever created becomes admin no matter
 * what role is asked for, since nobody exists yet to grant one. */
bool create(const String &name, const String &password, const String &role, String &error);
bool remove(uint32_t id, String &error);
bool setPassword(uint32_t id, const String &password, String &error);
bool setRole(uint32_t id, const String &role, String &error);
bool setControl(uint32_t id, const String &spec, String &error); /* "all"|"none"|csv */
bool setFlag(uint32_t id, const String &which, bool value, String &error); /* add|remove|settings */

uint32_t sessionLifetimeHours();
void setSessionLifetimeHours(uint32_t hours);

/* /Concordia/security/avatars/<id>.png or .jpg - existence alone is the
 * signal, nothing about it lives in users.json, the same way the panel on
 * the card is found by looking rather than by a flag - one fact, one place
 * it can go stale. Two extensions because a phone photo is JPEG far more
 * often than PNG, and there is no image library on this board to convert
 * one to the other. The HTTP handlers that read and write it are in
 * web_ui.cpp; `ext` there is whichever the upload sniffed. */
String avatarPath(uint32_t id, const char *ext);
/* Either extension that currently exists on the card, "" if neither does. */
const char *avatarExt(uint32_t id);

/* ------------------------------------------------------------- HTTP-facing */

/* Constant-time either way: a wrong password and an unknown name cost the
 * same PBKDF2 run, so neither the timing nor the error tells an attacker
 * which one was wrong. */
bool verifyPassword(const String &name, const String &password, User &out);

String createSession(uint32_t userId); /* returns the cookie value */
bool destroySession(const String &token);
bool sessionUser(const String &token, User &out);

/* The gate in front of Commands::execute(). Admins pass everything; anyone
 * else is checked against the permission the command actually needs. `why`
 * is set only on a refusal. */
bool allow(const User &user, const String &commandLine, String &why);

/* JSON for the panel. Never includes salt or hash. */
String toJson(const User &u);
String listJson(); /* {"sessionHours":N,"users":[...]} - admin-only, caller checks */

} // namespace Users
