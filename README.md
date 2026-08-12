# Concordia

**A Matter over Thread bridge built entirely on microcontrollers — no Linux,
no Raspberry Pi, no always-on hub computer.**

Concordia turns whatever you can reach over an HTTP API — a weather service,
a cheap Wi-Fi sensor, a piece of gear with nothing but a REST endpoint — into
real Matter accessories on your Thread network, commissionable straight into
Apple Home, Google Home, Amazon Alexa, or anything else that speaks Matter.
No Home Assistant, no Homebridge, no server running 24/7 in a closet. Two
small boards, a few dollars of hardware, running off USB power.

## How it actually works

Most Matter bridges are software running on a computer, with a USB Thread
radio plugged into it. Concordia inverts that: the Thread radio **is** the
bridge, and a second microcontroller sits in front of it doing everything
that computer would otherwise be doing.

```
                     Wi-Fi                    UART                  Thread / 802.15.4
 HTTP APIs  <------------------->  ESP32-C6  <-------->  nRF54L15  <-------------------->  Apple Home
 (any URL,                        "commander"            "hub"                             Google Home
  JSON path                                                                                 Alexa, etc.
  + scale)                        Wi-Fi + web            Matter accessory
                                  admin panel             server, commissioned
                                  (served from            directly by the
                                  an SD card)              ecosystem over Thread
```

- **The nRF54L15 is the actual Matter hub.** It runs the full Matter/Thread
  stack (nRF Connect SDK, built on Zephyr) and is the thing your phone
  commissions — it advertises, pairs, and answers Matter traffic over Thread
  on its own, exactly like a commercial Matter accessory would. It hosts a
  configurable table of virtual endpoints (temperature, humidity, switches,
  contact sensors, lights, locks, thermostats, air quality, and more — over
  30 device types out of the box) that don't correspond to any of its own
  physical hardware.
- **The ESP32 is the commander.** It owns Wi-Fi, serves a full admin web
  panel with no computer or app needed to configure anything, manages users
  and permissions, and — the actual bridging work — polls arbitrary HTTP
  APIs and writes the results into the nRF's virtual endpoints over a simple
  UART link. Add a device in the panel, point it at a URL and a JSON path,
  and it shows up in Apple Home.
- **The link between them is a plain line-based shell protocol** (`bridge
  add temp "Outside"`, `bridge set 4 2137`, and so on) — the same commands
  work whether they arrive over UART from the ESP32 or typed by hand into
  the node's USB console. One implementation of every operation, nothing to
  drift out of sync between "what the panel does" and "what a human on a
  cable can do."

The result is a genuine two-chip Matter bridge: no board in the chain runs
an operating system heavier than an RTOS, nothing needs to stay running on a
laptop, and the whole thing draws the power of two microcontrollers, not a
single-board computer.

## Why this matters

Every existing "bridge anything into Matter" story assumes a computer:
Home Assistant, Homebridge, a Docker container, something with a package
manager and a filesystem to keep patched. That's a fine answer if you
already have one running. Concordia is the answer for when you don't want
one running just for this — a shelf, a wall socket, a battery bank. It boots
in seconds, has no update surface beyond its own firmware, and if it's
unplugged for a week nothing needs recovering.

## Compatibility

Built and verified on one specific pair of boards; designed so most of that
is a config choice, not a source-code rewrite, when a candidate board offers
the same underlying pieces (see the roadmap's "support for other boards" for
what that would actually take).

| Role | Board | Status | Why |
| --- | --- | --- | --- |
| Commander (Wi-Fi, web panel, orchestration) | **Seeed XIAO ESP32-C6** | ✅ Shipped, this repo | What `concordia-hub-esp/config.h` is written for — Wi-Fi 6, enough flash/RAM for the panel and SD stack, the `D0`..`D10` pin aliases the firmware relies on |
| Commander, likely portable | Other Seeed XIAO ESP32 boards (S3, C3) | 🧪 Untested | Same castellated pinout and `D`-alias convention as the C6; would need the flash/RAM budget and pin map in `config.h` re-checked, no architecture change |
| Bridge / Matter hub (Thread radio, accessory server) | **Seeed XIAO nRF54L15 (Sense)** | ✅ Shipped, this repo | A hand-ported board target (`nRF54l15_Bridge/boards/`, see `PORTING_NOTES.md`) — not one of Nordic's official dev kits, chosen for its price and its `XIAO` footprint |
| Bridge, likely portable | nRF52840 DK, nRF5340 DK, nRF54L15 DK/Tag, nRF54LM20 DK | 🧪 Untested | Nordic's own Matter template already supports these boards natively — porting means writing an equivalent board `.conf`, not changing the UART protocol or the ESP32 side at all |
| Bridge, not compatible | Anything without an 802.15.4 Thread radio | ❌ | Matter *over Thread* specifically — this project does not use Matter-over-Wi-Fi |

If you build this on hardware not in this table, the roadmap's "support for
other boards" item is the place that work would start.

## What's in the box

- **Full admin web panel**, served straight off an SD card — no compiling
  the interface into firmware, no size limit but the card's. Multi-user,
  with per-user permissions (control specific devices, add/remove devices,
  change settings), sessions, and a first-run setup wizard.
- **Wi-Fi management** from the panel itself: saved networks, scan-and-join,
  a captive-portal recovery mode if it ever loses the network entirely.
- **mDNS hostname**, so the panel is reachable at a name instead of chasing
  an IP around after a network change.
- **Warnings and structured logging**, written to the card, surviving
  reboots — link health, network reachability, auth failures.
- **Generic HTTP → Matter bridging**: any URL that answers JSON, a path
  into it, and a scale factor — no per-service integration to write. The
  same mechanism that pulls live weather into a virtual sensor during setup
  works for anything else with a URL.
- **Deferred-BLE pairing**: the commissioning code and QR are fetched once,
  early, over a clean line and kept in memory — the panel can show them
  before the node has ever been paired, not just after.

Version 1. See `concordia-hub-esp/ROADMAP.md` for exactly what's done,
what's rough, and what's next.

## Pairing

The admin panel shows the manual code and QR as soon as it's ever had a
working line to the node — see "Deferred-BLE pairing" above. This build's
identity is fixed at compile time, not generated per boot, so as shipped
it's always:

```
manual: 3497-011-2332
qr:     MT:06PS042C00KA0648G00
```

**If the node isn't advertising** (the panel has the code, but Apple
Home/Google Home/Alexa never finds anything to pair with): press the node's
**USR button once, briefly**. That directly opens the commissioning window
on the node itself, independent of the ESP32 link entirely — the most
reliable fallback there is. Don't hold it down: a long press is this same
button's factory-reset trigger.

## Repository layout

```
concordia-hub-esp/   ESP32-C6 "commander" firmware — Arduino/arduino-cli
concordia-web/       the admin panel — plain HTML/JS/CSS, no build step
nRF54l15_Bridge/      nRF54L15 "hub" firmware — nRF Connect SDK / Zephyr, Matter
```

The node firmware is built on Nordic's own **Matter Template** sample from
the nRF Connect SDK, not written from scratch — the bridge itself (virtual
endpoints, the `bridge ...` shell, the whole point of this project) is this
repo's; commissioning, the Matter/Thread stack underneath, and the board's
button/LED handling come from the template.

Each part has its own notes: `concordia-web/README.md` for working on the
panel against a mock hub with nothing but a browser,
`nRF54l15_Bridge/PORTING_NOTES.md` for the Matter/Thread side and how the
Matter Template got ported onto hardware Nordic doesn't officially support,
`concordia-hub-esp/ROADMAP.md`'s "Reference" section for the exact build and
flash commands for both boards.
