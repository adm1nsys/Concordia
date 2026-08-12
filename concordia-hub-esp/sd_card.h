/*
 * The SD card, and the directory tree Concordia keeps on it.
 *
 * The point is to stop spending the ESP's flash on things that are not code.
 * The panel, its assets, saved configuration, logs and backups all belong on a
 * card that has gigabytes to spare, which leaves the firmware free to be just
 * firmware - and lets the interface grow without ever asking what it costs.
 *
 * The layout, which is deliberately readable when the card is put in a laptop:
 *
 *   /Concordia/            anything that does not fit a category
 *   /Concordia/security/   the API token, and whatever secrets come later
 *   /Concordia/this/       configuration for the board running this firmware
 *   /Concordia/node/       configuration for the Matter node it bridges
 *   /Concordia/web/        the admin panel
 *   /Concordia/logs/       logs that should outlive a reboot
 *   /Concordia/backup/     the previous copy of anything replaced in place
 *   /Concordia/tmp/        uploads in progress, never served
 *
 * `this` and `node` avoid naming the hardware on purpose: the board under the
 * firmware can be replaced without every path on the card becoming a lie.
 *
 * Nothing in here ever formats a card. A card that will not mount is reported
 * and left untouched - the alternative is a tool that can erase a user's data
 * because a contact was dirty.
 */
#pragma once

#include <Arduino.h>

namespace Card {

/* Mounts the card and ensures the tree exists. Safe with no card fitted: it
 * reports the reason and the firmware carries on without one. */
bool begin();

bool ready();
const String &status(); /* one line, for the console and /api/sd */

uint64_t totalBytes();
uint64_t usedBytes();

/* The token guarding the upload API, read from the card or generated on first
 * run. Empty when there is no card - and with no token the API refuses every
 * request rather than falling open. */
const String &token();
bool authorised(const String &candidate);

extern const char *const kRoot;
extern const char *const kSecurity;
extern const char *const kThis;
extern const char *const kNode;
extern const char *const kWeb;
extern const char *const kLogs;
extern const char *const kBackup;
extern const char *const kTmp;
extern const char *const kAvatars;

/* The panel held on the card, "" when the card carries none. A gzipped copy
 * wins over a plain one: it is what the browser wants and what the packer
 * produces. */
String webPagePath();

/* Same idea, for /Concordia/web/setup.html - the first-run wizard. Checked
 * only while no account exists yet; once one does, the wizard has done its
 * job and the normal panel takes over. "" when the card carries none, which
 * is not an error - a board without the wizard uploaded still boots into the
 * open bootstrap panel exactly as it always has. */
String setupPagePath();

/* True from the moment the wizard's own /api/users call creates the bootstrap
 * admin until its finishSetup() saves the UI config - see the .cpp for why
 * this exists. False for a board bootstrapped from the console instead, which
 * never sets it. */
bool setupPending();
void setSetupPending(bool pending);

/* Replaces a file under /Concordia/web with what was staged in tmp, keeping the
 * old copy in backup. Returns false and changes nothing on failure. */
bool commitStaged(const String &stagedPath, const String &targetName);

} // namespace Card
