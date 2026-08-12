/*
 * Concordia web - development stub.
 *
 * Included only while working on the page locally. It intercepts every call the
 * panel makes to the hub, logs it to the browser console, and answers with
 * plausible data held in memory, so the interface can be built and clicked
 * through with no board, no hub and no network.
 *
 * Everything it knows is real: the device types, their value kinds and hints
 * are exactly what the bridge reports, so what you design against is what the
 * hardware will send.
 *
 * pack.py strips the <script src="mock.js"> tag when building web_page.h, so
 * nothing in this file ever reaches the firmware.
 */
(() => {
  const log = (...a) => console.log("%c[hub]", "color:#5b8bff;font-weight:bold", ...a);

  /* ---------------------------------------------------------------- types
   * Straight from `bridge types` on the board. `kind` is what the UI uses to
   * decide which control to draw:
   *   bool          a switch
   *   level         1..254
   *   percent       0..100
   *   centipercent  0..10000, shown as %
   *   centidegree   centi-C, shown as degrees
   *   enum          a small set of numbered states
   *   number        plain integer
   * A `+` joins the parts of a multi-value device, in the order `set` takes
   * them: bool+level is on/off then brightness.
   */
  const TYPES = [
    ["temp", "Temperature Sensor", "centidegree", "centi-C, example 2300 = 23.00 C"],
    ["humidity", "Humidity Sensor", "centipercent", "centi-percent, example 4500 = 45.00%"],
    ["light", "On/Off Light", "bool", "0 = off, 1 = on"],
    ["contact", "Contact Sensor", "bool", "0 = closed/false, 1 = open/true"],
    ["occupancy", "Occupancy Sensor", "bool", "0 = clear, 1 = occupied"],
    ["pressure", "Pressure Sensor", "number", "hPa as int16, example 1013 = 1013 hPa"],
    ["flow", "Flow Sensor", "number", "deci-m3/h as uint16, example 250 = 25.0 m3/h"],
    ["illuminance", "Light Sensor", "number", "lux mapped value as uint16 (0..65534)"],
    ["leak", "Water Leak Detector", "bool", "0 = dry, 1 = leak detected"],
    ["plug", "On/Off Plug", "bool", "0 = off, 1 = on"],
    ["dimmer", "Dimmable Light", "bool+level", "<on/off> [level 0-254], example: 1 128"],
    ["colorlight", "Color Temperature Light", "bool+level+mireds", "<on/off> [level 0-254] [mireds 153-500]"],
    ["blind", "Window Covering", "percent", "position 0-100 (0 = open, 100 = closed)"],
    ["lock", "Door Lock", "bool", "0 = unlocked, 1 = locked"],
    ["rgblight", "Extended Color Light", "bool+level+mireds", "<on/off> [level 0-254] [mireds 153-500]"],
    ["dimplug", "Dimmable Plug", "bool+level", "<on/off> [level 0-254], example: 1 200"],
    ["thermostat", "Thermostat", "centidegree+setpoint+mode", "<temp centi-C> [heat setpoint] [mode 0=off,1=auto,3=cool,4=heat]"],
    ["fan", "Fan", "percent", "speed 0-100 percent"],
    ["purifier", "Air Purifier", "percent", "speed 0-100 percent"],
    ["airquality", "Air Quality Sensor", "enum", "0=unknown,1=good,2=fair,3=moderate,4=poor,5=very poor,6=extreme"],
    ["smoke", "Smoke / CO Alarm", "bool", "0 = normal, 1 = warning, 2 = critical"],
    ["valve", "Water Valve", "bool", "0 = closed, 1 = open"],
    ["freeze", "Water Freeze Detector", "bool", "0 = ok, 1 = freeze detected"],
    ["rain", "Rain Sensor", "bool", "0 = dry, 1 = rain detected"],
    ["onoffsensor", "On/Off Sensor", "bool", "0 = off, 1 = on"],
    ["mountedonoff", "Mounted On/Off Control", "bool", "0 = off, 1 = on"],
    ["mounteddimmer", "Mounted Dimmable Load Control", "bool+level", "<on/off> [level 0-254]"],
    ["aircon", "Room Air Conditioner", "centidegree+setpoint+mode", "<temp centi-C> [setpoint] [mode]"],
    ["hvacunit", "Heating/Cooling Unit", "centidegree+setpoint+mode", "<temp centi-C> [setpoint] [mode]"],
    ["heatpump", "Heat Pump", "centidegree+setpoint+mode", "<temp centi-C> [setpoint] [mode]"],
    ["waterheater", "Water Heater", "centidegree+setpoint+mode", "<temp centi-C> [setpoint] [mode]"],
    ["soil", "Soil Sensor (moisture)", "centipercent", "centi-percent moisture, 4500 = 45.00%"],
  ].map(([slug, name, kind, hint]) => ({ slug, name, kind, hint }));

  /* ------------------------------------------------------------- state */
  let nextEp = 2;
  const devices = [];
  const addDevice = (type, name, value, extra = {}) =>
    devices.push({ ep: nextEp++, type, name, has: true, value, ...extra });

  addDevice("occupancy", "vibration", 0);
  addDevice("light", "Onboard LED", 1);
  addDevice("temp", "Kitchen", 2137, {
    bind: {
      url: "https://api.open-meteo.com/v1/forecast?latitude=52.86&longitude=8.58&current=temperature_2m",
      path: "current.temperature_2m", scale: 100, bool: false, poll: 300,
    },
  });
  addDevice("dimmer", "Desk Lamp", 1, { v2: 180 });
  addDevice("aircon", "Living Room AC", 2150, { v2: 2300, v3: 4 });
  addDevice("blind", "Bedroom Blind", 40);
  addDevice("fan", "Study Fan", 65);
  addDevice("humidity", "Bathroom", 5400);

  /* Flip to true to exercise the login screen locally. Off by default so
   * everyone else's usual click-through session is unaffected - most work on
   * this page has nothing to do with accounts. */
  const MOCK_USERS = false;
  const MOCK_ACCOUNT = { id: 1, name: "admin", role: "admin",
    perm: { control: "all", add: true, remove: true, settings: true } };
  const MOCK_PASSWORD = "admin1234";
  let session = null;

  const status = {
    fw: "1.0.0", brand: "Concordia", portal: false, ip: "192.168.178.98",
    ssid: "Meisel_Beckeln27243", apSsid: "Concordia 192.168.4.1", rssi: -71,
    lang: "en", bridge: true, used: () => devices.length, total: 50,
    heap: 297032, uptime: () => Math.floor((Date.now() - started) / 1000),
    users: MOCK_USERS,
  };
  const started = Date.now();
  const logLines = [
    "0.008  === Concordia companion v1.0.0 ===",
    "0.284  [link] node responding",
    "3.766  [net] online as 192.168.178.98",
    "3.768  [web] http://192.168.178.98/",
  ];

  /* --------------------------------------------------------- console cmds */
  const find = (ep) => devices.find((d) => d.ep === +ep);

  function runCommand(line) {
    const [cmd, ...args] = line.trim().split(/\s+/);

    switch (cmd) {
      case "status":
        return [
          `firmware   ${status.fw}`,
          `wi-fi      online, "${status.ssid}" ${status.ip} (${status.rssi} dBm)`,
          `Link       responding (TX=GPIO2 RX=GPIO21)`,
          `endpoints  ${devices.length} of ${status.total} slots used`,
          `free heap  ${status.heap} bytes`,
        ].join("\n");

      case "list":
        return devices.map((d) =>
          `EP${d.ep}  ${d.type}  "${d.name}"  = ${d.has ? d.value : "-"}` +
          (d.bind ? `  <- ${d.bind.url}` : "")).join("\n") || "No devices.";

      case "types":
        return TYPES.map((t) =>
          `  ${t.slug.padEnd(14)} ${t.name}, kind=${t.kind}, value: ${t.hint}`).join("\n");

      case "add": {
        const [type, ...rest] = args;
        if (!TYPES.some((t) => t.slug === type)) return `Unknown type "${type}".`;
        addDevice(type, rest.join(" ") || type, 0);
        return `Added EP${nextEp - 1} (${type}, "${rest.join(" ")}")`;
      }
      case "rm": case "remove": {
        const i = devices.findIndex((d) => d.ep === +args[0]);
        if (i < 0) return `No endpoint EP${args[0]}`;
        devices.splice(i, 1);
        return `Removed EP${args[0]}`;
      }
      case "rename": {
        const d = find(args[0]);
        if (!d) return `No endpoint EP${args[0]}`;
        d.name = args.slice(1).join(" ");
        return `EP${d.ep} is now EP${d.ep} (${d.type}, "${d.name}")`;
      }
      case "retype": {
        const d = find(args[0]);
        if (!d) return `No endpoint EP${args[0]}`;
        d.type = args[1];
        return `EP${d.ep} is now EP${d.ep} (${d.type}, "${d.name}")`;
      }
      case "set": {
        const d = find(args[0]);
        if (!d) return "The node refused that value.";
        d.value = +args[1]; d.has = true;
        if (args[2] !== undefined) d.v2 = +args[2];
        if (args[3] !== undefined) d.v3 = +args[3];
        return `EP${d.ep} = ${d.value}`;
      }
      case "bind": {
        const d = find(args[0]);
        if (!d) return `No endpoint EP${args[0]}`;
        d.bind = { url: args[1], path: args[2] === '""' ? "" : args[2] || "",
                   scale: +(args[3] || 1), bool: false, poll: +(args[4] || 300) };
        return `EP${d.ep} now follows ${d.bind.path || "(auto)"} = 21.4 -> 2140`;
      }
      case "unbind": {
        const d = find(args[0]);
        if (d) delete d.bind;
        return `EP${args[0]} is manual again.`;
      }
      case "test":
        return "HTTP 200\ncurrent.temperature_2m = 21.400\n\nAvailable fields:\n" +
               "latitude = 52.860\ncurrent.temperature_2m = 21.400\ncurrent.is_day = 1.000";
      case "wifi":
        return "Saved networks:\n  0  Meisel_Beckeln27243";
      case "log":
        return logLines.join("\n");
      case "help":
        return "status | list | types | add | rm | rename | retype | set | bind | unbind | test | wifi | log";
      default:
        return `Unknown command "${cmd}". Type \`help\`.`;
    }
  }

  /* ------------------------------------------------- pairing and UI state
   * The pairing code is the real one this firmware reports: discriminator
   * 0xF00 with passcode 20202021 always produces 3497-011-2332.
   *
   * UI preferences are kept in localStorage rather than memory, so a layout you
   * are in the middle of designing survives a page reload. The hub keeps them
   * in NVS; same idea, different drawer.
   */
  const PAIRING = { manual: "34970112332", qr: "MT:06PS042C00KA0648G00" };

  const UI_KEY = "concordia.ui";
  const loadUi = () => {
    try { return JSON.parse(localStorage.getItem(UI_KEY)) || {}; }
    catch { return {}; }
  };
  const saveUi = (v) => localStorage.setItem(UI_KEY, JSON.stringify(v));

  /* ------------------------------------------------------- fetch shim */
  const json = (body) =>
    Promise.resolve(new Response(JSON.stringify(body),
      { status: 200, headers: { "Content-Type": "application/json" } }));

  const realFetch = window.fetch.bind(window);

  window.fetch = (input, init = {}) => {
    const url = typeof input === "string" ? input : input.url;
    if (!url.includes("/api/")) return realFetch(input, init);

    const body = init.body ? decodeURIComponent(String(init.body)) : "";
    log(init.method || "GET", url, body || "");

    if (url.includes("/api/status"))
      return json({ ...status, used: devices.length, uptime: status.uptime() });

    if (url.includes("/api/devices"))
      return json({ devices: devices.map((d) => ({ ...d })) });

    if (url.includes("/api/types")) return json({ types: TYPES });
    if (url.includes("/api/pairing")) return json(PAIRING);

    if (url.includes("/api/ui")) {
      if ((init.method || "GET").toUpperCase() === "POST") {
        try { saveUi(JSON.parse(init.body)); } catch { /* leave it alone */ }
        return json({});
      }
      return json(loadUi());
    }
    if (url.includes("/api/log")) return json({ log: logLines.join("\n") });
    if (url.includes("/api/lang")) return json({ lang: body.split("=")[1] || "en" });

    /* Same shape as the hub: /api/cmd stays open until an account exists, then
     * needs a signed-in session - see users.h there for why. */
    if (url.includes("/api/session/login")) {
      const p = new URLSearchParams(init.body || "");
      const n = p.get("name") || "", pw = p.get("password") || "";
      if (n === MOCK_ACCOUNT.name && pw === MOCK_PASSWORD) {
        session = MOCK_ACCOUNT;
        log("  -> signed in as", n);
        return json(MOCK_ACCOUNT);
      }
      log("  -> wrong name or password");
      return json({ ok: false, error: "wrong name or password" });
    }
    if (url.includes("/api/session/logout")) { session = null; return json({ ok: true }); }
    if (url.includes("/api/session")) return session ? json(session) : json({ ok: false });

    if (url.includes("/api/cmd")) {
      const cmd = body.replace(/^cmd=/, "");
      if (MOCK_USERS && !session) return json({ out: "", error: "sign in first" });
      const out = runCommand(cmd);
      log("  ->", out.split("\n")[0] + (out.includes("\n") ? " …" : ""));
      return json({ out });
    }
    return json({});
  };

  log("development stub active - no hub is being contacted");
  log(`${TYPES.length} device types, ${devices.length} devices in memory`);
})();
