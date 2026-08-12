#include "fs_api.h"

#include <SD.h>

#include "log_buf.h"
#include "sd_card.h"
#include "warnings.h"
#include "web_ui.h"
#include "zip_unpack.h"

namespace FsApi {

namespace {

WebServer *gServer = nullptr;

/* Text small enough to sit in RAM twice - once as the request body, once on its
 * way to the card. Editing a config file is the use here, not moving video. */
const size_t kMaxEditBytes = 64 * 1024;

void reply(int code, const String &body) {
  gServer->sendHeader("Cache-Control", "no-store");
  gServer->send(code, "application/json", body);
}

void fail(int code, const String &why) {
  reply(code, "{\"ok\":false,\"error\":\"" + WebUi::escape(why) + "\"}");
}

/* A refused token is recorded, with where it came from. One is a typo; a stream
 * of them from one address is somebody working through a list, and that is the
 * difference the record exists to show. */
bool authorised() {
  if (Card::authorised(gServer->header("X-Token"))) {
    return true;
  }
  static uint16_t refusals = 0;
  Warn::raise("auth", Warn::kWarn, "file API refused a token",
              gServer->client().remoteIP().toString() + ", attempt " +
                  String(++refusals) + " since boot");
  return false;
}

/* Guards every path this API touches.
 *
 * Two rules, and both matter. It has to live under /Concordia, because the rest
 * of the card belongs to whoever owns it and not to this firmware. And it must
 * not contain "..", because "/Concordia/../../etc" resolves straight out of the
 * sandbox - the oldest hole in the book, and the easiest to leave open. */
bool pathOk(const String &path) {
  if (path.length() < 10 || path.length() > 160) {
    return false;
  }
  if (!path.startsWith(Card::kRoot)) {
    return false;
  }
  if (path.indexOf("..") >= 0) {
    return false;
  }
  /* Exactly /Concordia, or something properly inside it - not /ConcordiaEvil. */
  return path.length() == strlen(Card::kRoot) || path.charAt(strlen(Card::kRoot)) == '/';
}

/* Rejects the request and says why, so the caller is not left guessing between
 * "denied", "no card" and "that path is not allowed". */
bool ready(const String &path) {
  if (!Card::ready()) {
    fail(409, "no card");
    return false;
  }
  if (!authorised()) {
    fail(403, "bad or missing X-Token");
    return false;
  }
  if (path.length() && !pathOk(path)) {
    fail(400, "path must sit inside " + String(Card::kRoot));
    return false;
  }
  return true;
}

const char *mimeFor(const String &name) {
  if (name.endsWith(".html") || name.endsWith(".html.gz")) return "text/html";
  if (name.endsWith(".css")) return "text/css";
  if (name.endsWith(".js")) return "application/javascript";
  if (name.endsWith(".json")) return "application/json";
  if (name.endsWith(".txt") || name.endsWith(".log")) return "text/plain";
  if (name.endsWith(".png")) return "image/png";
  if (name.endsWith(".jpg") || name.endsWith(".jpeg")) return "image/jpeg";
  if (name.endsWith(".svg")) return "image/svg+xml";
  if (name.endsWith(".woff2")) return "font/woff2";
  return "application/octet-stream";
}

/* ------------------------------------------------------------------ routes */

void handleList() {
  const String path = gServer->arg("path");
  if (!ready(path)) {
    return;
  }
  const String dir = path.length() ? path : String(Card::kRoot);

  File d = SD.open(dir);
  if (!d) {
    fail(404, "no such directory");
    return;
  }
  if (!d.isDirectory()) {
    d.close();
    fail(400, "that is a file, not a directory");
    return;
  }

  String out = "{\"path\":\"" + WebUi::escape(dir) + "\",\"entries\":[";
  bool first = true;
  for (File e = d.openNextFile(); e; e = d.openNextFile()) {
    if (!first) {
      out += ',';
    }
    first = false;
    out += "{\"name\":\"" + WebUi::escape(String(e.name())) + "\"";
    out += ",\"dir\":" + String(e.isDirectory() ? "true" : "false");
    out += ",\"size\":" + String((unsigned long)e.size()) + "}";
    e.close();
  }
  d.close();
  out += "]}";

  gServer->sendHeader("Cache-Control", "no-store");
  gServer->send(200, "application/json", out);
}

void handleRead() {
  const String path = gServer->arg("path");
  if (!ready(path)) {
    return;
  }
  File f = SD.open(path, FILE_READ);
  if (!f) {
    fail(404, "no such file");
    return;
  }
  if (f.isDirectory()) {
    f.close();
    fail(400, "that is a directory");
    return;
  }

  /* A .gz here is a file the caller asked for by name, not a page being served,
   * so it goes out as the bytes it is. streamFile would otherwise announce
   * Content-Encoding: gzip and the browser would silently unpack a download. */
  const bool asDownload = gServer->arg("dl") == "1" || path.endsWith(".gz");
  if (asDownload) {
    const String name = path.substring(path.lastIndexOf('/') + 1);
    gServer->sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
    /* octet-stream is doing two jobs: it marks the reply as bytes to save, and
     * it is the one content type streamFile treats as "do not add
     * Content-Encoding: gzip" - so a .gz downloads as the .gz it is instead of
     * being silently unpacked on the way out.
     *
     * streamFile rather than a hand-rolled send(): it sets Content-Length from
     * the file itself. Sending an empty body and setting the header afterwards
     * produced a perfectly formed reply containing nothing at all. */
    gServer->streamFile(f, "application/octet-stream");
  } else {
    gServer->streamFile(f, mimeFor(path));
  }
  f.close();
}

void handleSave() {
  const String path = gServer->arg("path");
  if (!ready(path)) {
    return;
  }
  const String content = gServer->arg("content");
  if (content.length() > kMaxEditBytes) {
    fail(413, "too large to edit here - upload it as a file instead");
    return;
  }

  /* Write beside it and rename, so a failure halfway leaves the old file whole
   * rather than truncated. */
  const String staged = String(Card::kTmp) + "/edit";
  if (SD.exists(staged)) {
    SD.remove(staged);
  }
  File out = SD.open(staged, FILE_WRITE);
  if (!out) {
    fail(500, "cannot open a staging file");
    return;
  }
  const size_t wrote = out.print(content);
  out.close();
  if (wrote != content.length()) {
    SD.remove(staged);
    fail(507, "the card would not take it all - is it full?");
    return;
  }
  if (SD.exists(path)) {
    SD.remove(path);
  }
  if (!SD.rename(staged, path)) {
    fail(500, "could not put the file in place");
    return;
  }
  Log::addf("[fs] saved %s (%u bytes)", path.c_str(), (unsigned)wrote);
  reply(200, "{\"ok\":true,\"size\":" + String((unsigned long)wrote) + "}");
}

void handleMkdir() {
  const String path = gServer->arg("path");
  if (!ready(path)) {
    return;
  }
  if (SD.exists(path)) {
    fail(409, "something is already there");
    return;
  }
  if (!SD.mkdir(path)) {
    fail(500, "could not create it");
    return;
  }
  Log::addf("[fs] created %s", path.c_str());
  reply(200, "{\"ok\":true}");
}

void handleRename() {
  const String from = gServer->arg("path");
  const String to = gServer->arg("to");
  if (!ready(from)) {
    return;
  }
  if (!pathOk(to)) {
    fail(400, "the new path must sit inside " + String(Card::kRoot));
    return;
  }
  if (!SD.exists(from)) {
    fail(404, "no such file");
    return;
  }
  if (SD.exists(to)) {
    fail(409, "something is already at the new name");
    return;
  }
  if (!SD.rename(from, to)) {
    fail(500, "could not rename it");
    return;
  }
  Log::addf("[fs] %s -> %s", from.c_str(), to.c_str());
  reply(200, "{\"ok\":true}");
}

/* Deletes for real. Files under /Concordia/web are moved to backup instead,
 * because that is the directory people will be experimenting in and a mistaken
 * click there should be survivable. Directories must be empty first - a
 * recursive delete behind one HTTP call is too much power for one typo. */
void handleDelete() {
  const String path = gServer->arg("path");
  if (!ready(path)) {
    return;
  }
  if (path == Card::kRoot) {
    fail(400, "not the root of the tree");
    return;
  }
  File f = SD.open(path);
  if (!f) {
    fail(404, "no such file");
    return;
  }
  const bool isDir = f.isDirectory();
  f.close();

  if (isDir) {
    if (!SD.rmdir(path)) {
      fail(409, "the folder is not empty");
      return;
    }
    Log::addf("[fs] removed folder %s", path.c_str());
    reply(200, "{\"ok\":true}");
    return;
  }

  if (path.startsWith(String(Card::kWeb) + "/")) {
    const String name = path.substring(path.lastIndexOf('/') + 1);
    const String backup = String(Card::kBackup) + "/" + name;
    if (SD.exists(backup)) {
      SD.remove(backup);
    }
    if (SD.rename(path, backup)) {
      Log::addf("[fs] %s moved to backup", name.c_str());
      reply(200, "{\"ok\":true,\"backup\":\"" + WebUi::escape(backup) + "\"}");
      return;
    }
  }
  if (!SD.remove(path)) {
    fail(500, "could not delete it");
    return;
  }
  Log::addf("[fs] deleted %s", path.c_str());
  reply(200, "{\"ok\":true}");
}

/* ------------------------------------------------------------- uploading
 * The target directory travels in the query string rather than as a form field
 * on purpose: multipart fields arrive interleaved with the file, and by the time
 * one could be read the first bytes have already needed somewhere to go. Query
 * arguments are parsed before any of that. */
File gUp;
String gUpTarget;
String gUpError;
bool gUpIsZip = false;

void handleUploadData() {
  HTTPUpload &up = gServer->upload();

  if (up.status == UPLOAD_FILE_START) {
    gUpError = "";
    gUpTarget = "";
    const String dir = gServer->arg("path");
    if (!Card::ready()) {
      gUpError = "no card";
      return;
    }
    if (!authorised()) {
      gUpError = "bad or missing X-Token";
      return;
    }
    if (!pathOk(dir)) {
      gUpError = "path must sit inside " + String(Card::kRoot);
      return;
    }
    String name = up.filename;
    const int cut = max((int)name.lastIndexOf('/'), (int)name.lastIndexOf('\\'));
    if (cut >= 0) {
      name = name.substring(cut + 1);
    }
    if (name.length() == 0 || name.indexOf("..") >= 0) {
      gUpError = "unusable filename";
      return;
    }
    gUpIsZip = name.endsWith(".zip");
    gUpTarget = gUpIsZip ? dir : (dir + "/" + name);

    const String staged = String(Card::kTmp) + "/incoming";
    if (SD.exists(staged)) {
      SD.remove(staged);
    }
    gUp = SD.open(staged, FILE_WRITE);
    if (!gUp) {
      gUpError = "cannot open a staging file";
    }
    return;
  }

  if (up.status == UPLOAD_FILE_WRITE) {
    if (gUp && gUp.write(up.buf, up.currentSize) != up.currentSize) {
      gUpError = "the card would not take it - is it full?";
      gUp.close();
    }
    return;
  }

  if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (gUp) {
      gUp.close();
    }
    if (up.status == UPLOAD_FILE_ABORTED && gUpError.length() == 0) {
      gUpError = "the upload was cut short";
    }
  }
}

void handleUploadDone() {
  const String staged = String(Card::kTmp) + "/incoming";

  if (gUpError.length()) {
    if (SD.exists(staged)) {
      SD.remove(staged);
    }
    fail(gUpError.startsWith("bad or missing") ? 403 : 400, gUpError);
    return;
  }

  if (gUpIsZip) {
    Zip::Result r = Zip::extract(staged, gUpTarget);
    SD.remove(staged);
    if (!r.ok) {
      fail(400, r.message);
      return;
    }
    Log::addf("[fs] unpacked %u file(s) into %s", r.files, gUpTarget.c_str());
    reply(200, "{\"ok\":true,\"unpacked\":" + String(r.files) +
                   ",\"bytes\":" + String((unsigned long)r.bytes) +
                   ",\"into\":\"" + WebUi::escape(gUpTarget) + "\"}");
    return;
  }

  if (SD.exists(gUpTarget)) {
    SD.remove(gUpTarget);
  }
  if (!SD.rename(staged, gUpTarget)) {
    SD.remove(staged);
    fail(500, "could not put the file in place");
    return;
  }
  Log::addf("[fs] uploaded %s", gUpTarget.c_str());
  reply(200, "{\"ok\":true,\"file\":\"" + WebUi::escape(gUpTarget) + "\"}");
}

} // namespace

void attach(WebServer &server) {
  gServer = &server;
  server.on("/api/fs/list", HTTP_GET, handleList);
  server.on("/api/fs/read", HTTP_GET, handleRead);
  server.on("/api/fs/save", HTTP_POST, handleSave);
  server.on("/api/fs/mkdir", HTTP_POST, handleMkdir);
  server.on("/api/fs/rename", HTTP_POST, handleRename);
  server.on("/api/fs/delete", HTTP_POST, handleDelete);
  server.on("/api/fs/upload", HTTP_POST, handleUploadDone, handleUploadData);
}

} // namespace FsApi
