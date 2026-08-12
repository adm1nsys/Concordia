#include "commands.h"

#include <WiFi.h>

#include <new>

#include "config.h"
#include "ext_api.h"
#include "log_buf.h"
#include "net_manager.h"
#include "nrf_link.h"
#include "sd_card.h"
#include "storage.h"
#include "users.h"

namespace {

/* Word `n` of `line`; `toEnd` takes everything that is left, which is what you
 * want for names and URLs. */
String arg(const String &line, uint8_t n, bool toEnd = false) {
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
  if (toEnd) {
    return line.substring(start);
  }
  const int space = line.indexOf(' ', start);
  return space < 0 ? line.substring(start) : line.substring(start, space);
}

/* Percent-decoding for arguments that may contain spaces.
 *
 * `wifi add` used to take the SSID as "everything up to the first space", which
 * is fine until the network is called "iPhone 16 Pro Max": the board saved
 * "iPhone" and treated the rest as the start of the password, then quietly
 * failed to associate with a network nobody could see was wrong.
 *
 * Only %XX is decoded, and '+' is left alone - this is not form encoding, and
 * an SSID containing a plus sign is a real thing. Text without any '%' passes
 * through untouched, so anything typed by hand at the console still works. */
String percentDecode(const String &in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); ++i) {
    if (in[i] == '%' && i + 2 < in.length() && isxdigit((unsigned char)in[i + 1]) &&
        isxdigit((unsigned char)in[i + 2])) {
      out += (char)strtol(in.substring(i + 1, i + 3).c_str(), nullptr, 16);
      i += 2;
    } else {
      out += in[i];
    }
  }
  return out;
}

String describeBinding(const Binding &b) {
  String out = "EP" + String(b.endpointId) + "  " + b.url;
  if (b.path.length()) {
    out += "  path=" + b.path;
  }
  if (b.asBoolean) {
    out += "  boolean";
  } else if (b.scale != 1.0f) {
    out += "  x" + String(b.scale, 3);
  }
  out += "  every " + String(b.pollSeconds) + "s";
  return out;
}

String cmdStatus() {
  uint16_t used = 0, total = 0;
  const bool up = NrfLink::status(used, total);

  String out;
  out += "firmware   " FW_VERSION "\n";
  out += "wi-fi      ";
  switch (Net::mode()) {
  case NET_ONLINE:
    out += "online, \"" + Net::ssid() + "\" " + Net::ipAddress() + " (" +
           String(Net::rssi()) + " dBm)\n";
    break;
  case NET_PORTAL:
    out += "setup portal \"" + Net::apSsid() + "\" -> " + Net::ipAddress() + "\n";
    break;
  default:
    out += "connecting\n";
    break;
  }
  out += "nRF link   ";
  out += up ? "responding" : "silent";
  out += " (TX=GPIO" + String(PIN_TX_TO_NRF) + " RX=GPIO" + String(PIN_RX_FROM_NRF) + ")\n";
  if (up) {
    out += "endpoints  " + String(used) + " of " + String(total) + " slots used\n";
  }
  out += "bindings   " + String(Storage::bindingCount()) + " of " + String(MAX_BINDINGS) + "\n";
  out += "free heap  " + String(ESP.getFreeHeap()) + " bytes\n";
  out += "uptime     " + String(millis() / 1000) + " s";
  return out;
}

String cmdList() {
  static BridgeDevice *devices = nullptr;
  if (devices == nullptr) {
    devices = new (std::nothrow) BridgeDevice[kMaxDevices];
    if (devices == nullptr) {
      return "out of memory";
    }
  }
  const uint8_t count = NrfLink::devices(devices, kMaxDevices);
  if (count == 0) {
    return "No devices. Add one with:  add <type> <name>";
  }

  String out;
  for (uint8_t i = 0; i < count; ++i) {
    const BridgeDevice &d = devices[i];
    out += "EP" + String(d.endpointId) + "  " + d.type + "  \"" + d.name + "\"";
    out += d.hasValue ? ("  = " + String(d.value)) : String("  = -");
    Binding b;
    if (Storage::bindingFor(d.endpointId, b)) {
      out += "  <- " + b.url;
    }
    if (i + 1 < count) {
      out += '\n';
    }
  }
  return out;
}

String cmdAdd(const String &line) {
  const String type = arg(line, 1);
  const String name = arg(line, 2, true);
  if (type.length() == 0 || name.length() == 0) {
    return "usage: add <type> <name>     (see `types`)";
  }
  const uint16_t ep = NrfLink::addDevice(type, name);
  if (ep == 0) {
    return "The nRF refused it. Is \"" + type + "\" in `types`, and is there a free slot?";
  }
  Log::addf("[dev] added EP%u %s \"%s\"", ep, type.c_str(), name.c_str());
  return "Added EP" + String(ep) + " (" + type + ", \"" + name + "\")";
}

/* The nRF shell has no rename, so both rename and retype are remove+add. The
 * endpoint id changes, and any binding has to follow it. */
String recreate(uint16_t ep, const String &type, const String &name) {
  if (!NrfLink::removeDevice(ep)) {
    return "No endpoint EP" + String(ep);
  }
  const uint16_t fresh = NrfLink::addDevice(type, name);
  if (fresh == 0) {
    Storage::bindingRemove(ep);
    return "Removed EP" + String(ep) + " but could not recreate it as \"" + type + "\"";
  }
  Storage::bindingMove(ep, fresh);
  Log::addf("[dev] EP%u -> EP%u (%s, \"%s\")", ep, fresh, type.c_str(), name.c_str());
  return "EP" + String(ep) + " is now EP" + String(fresh) + " (" + type + ", \"" + name + "\")";
}

bool lookup(uint16_t ep, BridgeDevice &out) {
  BridgeDevice devices[8];
  const uint8_t count = NrfLink::devices(devices, 8);
  for (uint8_t i = 0; i < count; ++i) {
    if (devices[i].endpointId == ep) {
      out = devices[i];
      return true;
    }
  }
  return false;
}

String cmdWifi(const String &line) {
  const String sub = arg(line, 1);

  if (sub.length() == 0 || sub == "list") {
    String out = "Saved networks:";
    for (uint8_t i = 0; i < Storage::wifiCount(); ++i) {
      out += "\n  " + String(i) + "  " + Storage::wifiAt(i).ssid;
    }
    if (Storage::wifiCount() == 0) {
      out += " none";
    }
    return out;
  }
  if (sub == "add") {
    const String ssid = percentDecode(arg(line, 2));
    const String pass = percentDecode(arg(line, 3, true));
    if (ssid.length() == 0) {
      return "usage: wifi add <ssid> <password>\n"
             "       percent-encode spaces: wifi add iPhone%2016%20Pro 1234";
    }
    if (!Storage::wifiAdd(ssid, pass)) {
      return "The list is full (" + String(MAX_WIFI_NETWORKS) + " networks).";
    }
    return "Saved \"" + ssid + "\". Use `wifi join` to switch to it now.";
  }
  if (sub == "del" || sub == "rm") {
    if (!Storage::wifiRemove((uint8_t)arg(line, 2).toInt())) {
      return "No such entry.";
    }
    return "Removed.";
  }
  if (sub == "join") {
    Net::reconnect();
    return cmdStatus();
  }
  if (sub == "scan") {
    const int n = WiFi.scanNetworks();
    if (n <= 0) {
      return "Nothing in range.";
    }
    String out = "Networks in range:";
    for (int i = 0; i < n && i < 15; ++i) {
      out += "\n  " + WiFi.SSID(i) + "  (" + String(WiFi.RSSI(i)) + " dBm)";
    }
    WiFi.scanDelete();
    return out;
  }
  return "usage: wifi [list|add|del|join|scan]";
}

String cmdBind(const String &line) {
  const uint16_t ep = (uint16_t)arg(line, 1).toInt();
  const String url = arg(line, 2);
  if (ep == 0 || url.length() == 0) {
    return "usage: bind <ep> <url> [json.path] [scale] [seconds]";
  }

  Binding b;
  b.endpointId = ep;
  b.url = url;
  b.path = arg(line, 3);
  const String scale = arg(line, 4);
  b.scale = scale.length() ? scale.toFloat() : 1.0f;
  if (b.scale == 0.0f) {
    b.scale = 1.0f;
  }
  const String poll = arg(line, 5);
  b.pollSeconds = poll.length() ? (uint16_t)poll.toInt() : 300;
  if (b.pollSeconds < MIN_POLL_SECONDS) {
    b.pollSeconds = MIN_POLL_SECONDS;
  }

  /* Check before saving: a binding that silently never works is worse than one
   * that refuses to be created. */
  const FetchResult probe = ExtApi::fetch(b.url, b.path, b.scale, b.asBoolean);
  if (!probe.ok) {
    String out = "Not saved - the test fetch failed: " + probe.error;
    if (probe.fields.length()) {
      out += "\nFields the response does offer:\n" + probe.fields;
    }
    return out;
  }
  if (!Storage::bindingSet(b)) {
    return "The binding table is full (" + String(MAX_BINDINGS) + ").";
  }
  NrfLink::setValue(ep, probe.scaled);
  return "EP" + String(ep) + " now follows " + probe.usedPath + " = " +
         String(probe.raw, 3) + " -> " + String(probe.scaled);
}

String cmdTest(const String &line) {
  const String url = arg(line, 1);
  if (url.length() == 0) {
    return "usage: test <url> [json.path]";
  }
  const FetchResult r = ExtApi::fetch(url, arg(line, 2), 1.0f, false);
  String out = "HTTP " + String(r.httpCode) + "\n";
  if (r.ok) {
    out += r.usedPath + " = " + String(r.raw, 3);
  } else {
    out += "failed: " + r.error;
  }
  if (r.fields.length()) {
    out += "\n\nAvailable fields:\n" + r.fields;
  }
  return out;
}

String cmdNrf(const String &line) {
  const String sub = arg(line, 1);
  if (sub == "reboot") {
    Log::add("[sys] rebooting the nRF");
    NrfLink::reboot();
    return "Reboot sent. The link comes back in a few seconds.";
  }
  if (sub == "reset") {
    Log::add("[sys] factory reset of the nRF");
    return NrfLink::factoryReset()
               ? "The nRF is wiping its Matter pairings and rebooting."
               : "The nRF did not confirm. Try `nrf bridge factoryreset confirm`.";
  }
  const String raw = arg(line, 1, true);
  if (raw.length() == 0) {
    return "usage: nrf reboot | nrf reset | nrf <any shell command>";
  }
  const String reply = NrfLink::send(raw);
  return reply.length() ? reply : "(no answer)";
}

String describePermissions(const Users::User &u) {
  if (u.role == "admin") {
    return "admin (everything)";
  }
  String ctrl = u.perm.control == Users::Control::kAll    ? "all"
               : u.perm.control == Users::Control::kNone ? "none"
                                                          : u.perm.controlList;
  return "control=" + ctrl + " add=" + (u.perm.addDevices ? "y" : "n") +
         " remove=" + (u.perm.removeDevices ? "y" : "n") +
         " settings=" + (u.perm.settings ? "y" : "n");
}

/* Console commands are physical access, trusted like the BOOT button already
 * is - no session or permission check here. This is the bootstrap path: the
 * first `user add` is the only way to create an admin before the wizard
 * (roadmap item 5) exists. */
String cmdUser(const String &line) {
  const String sub = arg(line, 1);

  if (sub.length() == 0 || sub == "list") {
    if (!Users::anyExist()) {
      return "No users yet - /api/cmd is open to the network until one exists.\n"
             "  user add <name> <password>          (the first one becomes admin)";
    }
    String out = "Users (sessions last " + String(Users::sessionLifetimeHours()) + "h):";
    for (uint8_t i = 0; i < Users::count(); ++i) {
      Users::User u;
      if (!Users::at(i, u)) {
        continue;
      }
      out += "\n  #" + String(u.id) + "  " + u.name + "  " + describePermissions(u);
    }
    return out;
  }
  if (sub == "add") {
    const String name = percentDecode(arg(line, 2));
    const String pass = percentDecode(arg(line, 3));
    const String roleArg = arg(line, 4);
    if (name.length() == 0 || pass.length() == 0) {
      return "usage: user add <name> <password> [admin]\n"
             "       password needs 8+ characters; percent-encode any spaces";
    }
    const bool first = !Users::anyExist();
    String error;
    if (!Users::create(name, pass, roleArg, error)) {
      return "Not created: " + error;
    }
    Log::addf("[auth] user \"%s\" created", name.c_str());
    return "Created \"" + name + "\"" +
           (first ? " as admin - /api/cmd now needs a session for everyone else."
                  : (roleArg == "admin" ? " as admin." : " as a plain user - grant it permissions with `user grant`."));
  }
  if (sub == "rm") {
    const String name = arg(line, 2, true);
    Users::User u;
    if (name.length() == 0 || !Users::findByName(name, u)) {
      return "No such user.";
    }
    String error;
    if (!Users::remove(u.id, error)) {
      return "Not removed: " + error;
    }
    Log::addf("[auth] user \"%s\" removed", name.c_str());
    return "Removed \"" + name + "\".";
  }
  if (sub == "passwd") {
    const String name = arg(line, 2);
    const String pass = percentDecode(arg(line, 3, true));
    Users::User u;
    if (name.length() == 0 || !Users::findByName(name, u)) {
      return "No such user.";
    }
    String error;
    if (!Users::setPassword(u.id, pass, error)) {
      return "Not changed: " + error;
    }
    Log::addf("[auth] password changed for \"%s\"", name.c_str());
    return "Password changed.";
  }
  if (sub == "role") {
    const String name = arg(line, 2);
    const String role = arg(line, 3);
    Users::User u;
    if (!Users::findByName(name, u)) {
      return "No such user.";
    }
    String error;
    if (!Users::setRole(u.id, role, error)) {
      return "Not changed: " + error;
    }
    return "\"" + name + "\" is now " + role + ".";
  }
  if (sub == "grant") {
    const String name = arg(line, 2);
    const String what = arg(line, 3);
    const String value = arg(line, 4, true);
    Users::User u;
    if (!Users::findByName(name, u)) {
      return "No such user.";
    }
    String error;
    bool ok;
    if (what == "control") {
      ok = Users::setControl(u.id, value, error);
    } else if (what == "add" || what == "remove" || what == "settings") {
      ok = Users::setFlag(u.id, what, value == "yes" || value == "y", error);
    } else {
      return "usage: user grant <name> control all|none|<ep,ep,...>\n"
             "       user grant <name> add|remove|settings yes|no";
    }
    if (!ok) {
      return "Not changed: " + error;
    }
    return "Updated \"" + name + "\".";
  }
  if (sub == "session") {
    const String hours = arg(line, 2);
    if (hours.length() == 0) {
      return "Sessions currently last " + String(Users::sessionLifetimeHours()) + "h.";
    }
    Users::setSessionLifetimeHours((uint32_t)hours.toInt());
    return "Sessions now last " + String(Users::sessionLifetimeHours()) + "h.";
  }
  return "usage: user list | add | rm | passwd | role | grant | session";
}

} // namespace

namespace Commands {

String help() {
  return "Commands\n"
         "  status                     what this box and the nRF are doing\n"
         "  list                       bridged devices\n"
         "  types                      device types the nRF can host\n"
         "  add <type> <name>          create a device\n"
         "  rm <ep>                    delete a device\n"
         "  rename <ep> <name>         rename (recreates the endpoint)\n"
         "  retype <ep> <type>         change type (recreates the endpoint)\n"
         "  set <ep> <v> [lvl] [mrd]   set a value by hand\n"
         "  bind <ep> <url> [path] [scale] [sec]   feed it from an API\n"
         "  unbind <ep>                back to manual\n"
         "  bindings                   list the API feeds\n"
         "  test <url> [path]          try an API without saving anything\n"
         "  wifi [list|add|del|join|scan]\n"
         "  hostname [<name>]          mDNS name, e.g. concordia -> concordia.local\n"
         "  lang [en|de|da]            web UI language\n"
         "  log [clear]                recent events\n"
         "  pairing                    Matter pairing code and QR payload\n"
         "  nrf reboot | reset | <cmd> talk to the nRF directly\n"
         "  user list | add | rm | passwd | role | grant | session\n"
         "                             accounts for the web panel (see `user`)\n"
         "  setup [done]               wizard-pending status, or force it clear\n"
         "  reboot                     restart this ESP\n"
         "  factoryreset confirm       wipe this ESP, the nRF and every user";
}

String execute(const String &raw) {
  String line = raw;
  line.trim();
  if (line.length() == 0) {
    return String();
  }

  const String cmd = arg(line, 0);

  if (cmd == "help" || cmd == "?") {
    return help();
  }
  if (cmd == "status") {
    return cmdStatus();
  }
  if (cmd == "list") {
    return cmdList();
  }
  if (cmd == "types") {
    const String reply = NrfLink::send("bridge types");
    return reply.length() ? reply : "(no answer from the nRF)";
  }
  if (cmd == "add") {
    return cmdAdd(line);
  }
  if (cmd == "rm" || cmd == "remove") {
    const uint16_t ep = (uint16_t)arg(line, 1).toInt();
    if (ep == 0) {
      return "usage: rm <ep>";
    }
    if (!NrfLink::removeDevice(ep)) {
      return "No endpoint EP" + String(ep);
    }
    Storage::bindingRemove(ep);
    Log::addf("[dev] removed EP%u", ep);
    return "Removed EP" + String(ep);
  }
  if (cmd == "rename" || cmd == "retype") {
    const uint16_t ep = (uint16_t)arg(line, 1).toInt();
    const String rest = arg(line, 2, true);
    if (ep == 0 || rest.length() == 0) {
      return String("usage: ") + cmd + " <ep> <" + (cmd == "rename" ? "name" : "type") + ">";
    }
    BridgeDevice current;
    if (!lookup(ep, current)) {
      return "No endpoint EP" + String(ep);
    }
    return cmd == "rename" ? recreate(ep, current.type, rest)
                           : recreate(ep, rest, current.name);
  }
  if (cmd == "set") {
    const uint16_t ep = (uint16_t)arg(line, 1).toInt();
    const String value = arg(line, 2);
    if (ep == 0 || value.length() == 0) {
      return "usage: set <ep> <value> [level] [mireds]";
    }
    const String level = arg(line, 3);
    const String mireds = arg(line, 4);
    const bool ok = NrfLink::setValue(ep, value.toInt(),
                                      level.length() ? level.toInt() : -1,
                                      mireds.length() ? mireds.toInt() : -1);
    return ok ? "EP" + String(ep) + " = " + value : "The nRF refused that value.";
  }
  if (cmd == "bind") {
    return cmdBind(line);
  }
  if (cmd == "unbind") {
    const uint16_t ep = (uint16_t)arg(line, 1).toInt();
    return Storage::bindingRemove(ep) ? "EP" + String(ep) + " is manual again."
                                      : "EP" + String(ep) + " had no binding.";
  }
  if (cmd == "bindings") {
    if (Storage::bindingCount() == 0) {
      return "No API feeds.";
    }
    String out;
    for (uint8_t i = 0; i < Storage::bindingCount(); ++i) {
      if (i) {
        out += '\n';
      }
      out += describeBinding(Storage::bindingAt(i));
    }
    return out;
  }
  if (cmd == "test") {
    return cmdTest(line);
  }
  if (cmd == "wifi") {
    return cmdWifi(line);
  }
  if (cmd == "hostname") {
    const String name = arg(line, 1);
    if (name.length() == 0) {
      return "Hostname: " + Net::hostname() + "   (http://" + Net::hostname() + ".local/)";
    }
    if (!Net::setHostname(name)) {
      return "Not saved - letters, digits and hyphens only, 1-32 characters.";
    }
    return "Hostname set to \"" + name + "\" - http://" + name + ".local/ should answer now.";
  }
  if (cmd == "lang") {
    const String lang = arg(line, 1);
    if (lang.length() == 0) {
      return "Language: " + Storage::language() + "   (en, de, da)";
    }
    if (lang != "en" && lang != "de" && lang != "da") {
      return "Available: en, de, da";
    }
    Storage::setLanguage(lang);
    return "Language set to " + lang;
  }
  if (cmd == "log") {
    if (arg(line, 1) == "clear") {
      Log::clear();
      return "Log cleared.";
    }
    const String dump = Log::dump();
    return dump.length() ? dump : "(empty)";
  }
  if (cmd == "pairing") {
    const String reply = NrfLink::send("bridge pairing");
    return reply.length() ? reply : "(no answer from the nRF)";
  }
  if (cmd == "nrf") {
    return cmdNrf(line);
  }
  if (cmd == "user") {
    return cmdUser(line);
  }
  if (cmd == "setup") {
    if (arg(line, 1) != "done") {
      return Card::setupPending()
                 ? "The wizard has an unfinished setup pending - the panel will keep\n"
                   "showing it on \"/\" until finishSetup() saves, or until this:\n"
                   "  setup done"
                 : "Nothing pending.";
    }
    /* The escape hatch: an abandoned wizard (closed the tab after step 1,
     * before Finish) would otherwise strand the real panel behind the wizard
     * forever, since nothing else clears the marker - see Card::setSetupPending().
     * No permission check, same trust level as everything else on this
     * console: whoever is on the cable already has more access than this. */
    Card::setSetupPending(false);
    return "Cleared. \"/\" will serve the real panel now.";
  }
  if (cmd == "reboot") {
    Log::add("[sys] restarting the ESP");
    delay(150);
    ESP.restart();
    return "Restarting.";
  }
  if (cmd == "factoryreset") {
    if (arg(line, 1) != "confirm") {
      return "This wipes Wi-Fi, bindings and the nRF's Matter pairings.\n"
             "Type:  factoryreset confirm";
    }
    Log::add("[sys] factory reset of both boards");
    NrfLink::factoryReset();
    Storage::factoryReset();
    Users::factoryReset();
    delay(300);
    ESP.restart();
    return "Wiping.";
  }

  return "Unknown command \"" + cmd + "\". Type `help`.";
}

} // namespace Commands
