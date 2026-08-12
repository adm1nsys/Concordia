#include "sd_card.h"

#include <SD.h>
#include <SPI.h>
#include <esp_random.h>

#include "config.h"
#include "log_buf.h"

namespace Card {

const char *const kRoot = "/Concordia";
const char *const kSecurity = "/Concordia/security";
const char *const kThis = "/Concordia/this";
const char *const kNode = "/Concordia/node";
const char *const kWeb = "/Concordia/web";
const char *const kLogs = "/Concordia/logs";
const char *const kBackup = "/Concordia/backup";
const char *const kTmp = "/Concordia/tmp";
const char *const kAvatars = "/Concordia/security/avatars";

namespace {

bool gReady = false;
String gStatus = "not started";
String gToken;

const char *const kTokenFile = "/Concordia/security/api_token";

/* Long filenames are available here - the core builds FatFs with
 * CONFIG_FATFS_LFN_STACK - so "Concordia" stays "Concordia" rather than
 * collapsing into CONCOR~1. Checked rather than assumed, because a card that
 * silently shortens its directories is a puzzle nobody enjoys. */
bool ensureDir(const char *path) {
  if (SD.exists(path)) {
    return true;
  }
  if (SD.mkdir(path)) {
    return true;
  }
  Log::addf("[sd] cannot create %s", path);
  return false;
}

String makeToken() {
  char buf[33];
  for (int i = 0; i < 32; i += 8) {
    snprintf(buf + i, 9, "%08x", (unsigned)esp_random());
  }
  return String(buf);
}

/* The token lives on the card rather than in NVS on purpose: pulling the card
 * and reading one file is the recovery path when everything else is
 * unreachable. */
void loadOrCreateToken() {
  File f = SD.open(kTokenFile, FILE_READ);
  if (f) {
    gToken = f.readStringUntil('\n');
    f.close();
    gToken.trim();
    if (gToken.length() >= 16) {
      Log::add("[sd] api token loaded");
      return;
    }
    Log::add("[sd] api token file was unusable, writing a new one");
  }

  gToken = makeToken();
  File out = SD.open(kTokenFile, FILE_WRITE);
  if (!out) {
    Log::add("[sd] cannot write the api token - upload API stays closed");
    gToken = "";
    return;
  }
  out.println(gToken);
  out.close();
  Log::add("[sd] api token created at " + String(kTokenFile));
}

const char *typeName(sdcard_type_t t) {
  switch (t) {
  case CARD_MMC: return "MMC";
  case CARD_SD: return "SD";
  case CARD_SDHC: return "SDHC";
  case CARD_NONE: return "none";
  default: return "unknown";
  }
}

} // namespace

bool begin() {
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

  /* format_if_empty stays false. An unreadable card is a card to hand back to
   * its owner, not one to erase. */
  if (!SD.begin(PIN_SD_CS, SPI, SD_SPI_HZ, "/sd", 5, false)) {
    gReady = false;
    gStatus = "no card, or it would not mount";
    Log::add("[sd] no card mounted - the built-in page will be served");
    return false;
  }

  const sdcard_type_t type = SD.cardType();
  if (type == CARD_NONE) {
    gReady = false;
    gStatus = "no card";
    Log::add("[sd] no card detected");
    return false;
  }

  const bool tree = ensureDir(kRoot) && ensureDir(kSecurity) && ensureDir(kThis) &&
                    ensureDir(kNode) && ensureDir(kWeb) && ensureDir(kLogs) &&
                    ensureDir(kBackup) && ensureDir(kTmp) && ensureDir(kAvatars);
  if (!tree) {
    gReady = false;
    gStatus = "card mounted but the directory tree could not be created";
    return false;
  }

  gReady = true;
  const uint64_t mb = SD.totalBytes() / (1024ULL * 1024ULL);
  gStatus = String(typeName(type)) + ", " + String((unsigned long)mb) + " MB";
  Log::addf("[sd] %s card, %lu MB, tree ready under %s", typeName(type),
            (unsigned long)mb, kRoot);

  loadOrCreateToken();
  return true;
}

bool ready() { return gReady; }
const String &status() { return gStatus; }

uint64_t totalBytes() { return gReady ? SD.totalBytes() : 0; }
uint64_t usedBytes() { return gReady ? SD.usedBytes() : 0; }

const String &token() { return gToken; }

/* No token, no access. The comparison is length-then-content rather than a
 * plain ==; it costs nothing and does not leak the answer through timing. */
bool authorised(const String &candidate) {
  if (!gReady || gToken.length() == 0) {
    return false;
  }
  if (candidate.length() != gToken.length()) {
    return false;
  }
  uint8_t diff = 0;
  for (size_t i = 0; i < gToken.length(); ++i) {
    diff |= (uint8_t)gToken[i] ^ (uint8_t)candidate[i];
  }
  return diff == 0;
}

String webPagePath() {
  if (!gReady) {
    return "";
  }
  const String gz = String(kWeb) + "/index.html.gz";
  if (SD.exists(gz)) {
    return gz;
  }
  const String plain = String(kWeb) + "/index.html";
  if (SD.exists(plain)) {
    return plain;
  }
  return "";
}

String setupPagePath() {
  if (!gReady) {
    return "";
  }
  const String gz = String(kWeb) + "/setup.html.gz";
  if (SD.exists(gz)) {
    return gz;
  }
  const String plain = String(kWeb) + "/setup.html";
  if (SD.exists(plain)) {
    return plain;
  }
  return "";
}

/* Marks that the wizard, specifically, created the bootstrap admin - not the
 * console's `user add`, which never touches this. Lets handlePage() keep
 * serving the wizard across a stray reload mid-setup, when Users::anyExist()
 * alone would already have switched it to the real panel with nothing set up
 * yet. Cleared the moment any /api/ui save lands, which only the wizard's own
 * finishSetup() could ever call while this file exists - the real panel is
 * not reachable to do it any other way. An empty file: its presence is the
 * whole signal, nothing is read out of it. */
const char *const kSetupPendingFile = "/Concordia/this/setup_pending";

bool setupPending() { return gReady && SD.exists(kSetupPendingFile); }

void setSetupPending(bool pending) {
  if (!gReady) {
    return;
  }
  if (pending) {
    File f = SD.open(kSetupPendingFile, FILE_WRITE);
    if (f) {
      f.close();
    }
  } else if (SD.exists(kSetupPendingFile)) {
    SD.remove(kSetupPendingFile);
  }
}

/* Replace in three steps, so a failure at any one of them leaves something
 * working: the previous copy goes to backup, the new one moves into place, and
 * only then is the old one out of reach. A half-written upload never becomes
 * the page, because it is written to tmp and only renamed once complete. */
bool commitStaged(const String &stagedPath, const String &targetName) {
  if (!gReady) {
    return false;
  }
  const String target = String(kWeb) + "/" + targetName;
  const String backup = String(kBackup) + "/" + targetName;

  if (SD.exists(target)) {
    if (SD.exists(backup)) {
      SD.remove(backup);
    }
    if (!SD.rename(target, backup)) {
      Log::addf("[sd] could not move %s aside - keeping what is there",
                target.c_str());
      return false;
    }
  }

  if (!SD.rename(stagedPath, target)) {
    Log::addf("[sd] could not install %s", target.c_str());
    if (SD.exists(backup)) {
      SD.rename(backup, target); /* put the old one back */
    }
    return false;
  }

  Log::addf("[sd] installed %s", target.c_str());
  return true;
}

} // namespace Card
