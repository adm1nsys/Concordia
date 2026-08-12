# Concordia — roadmap

Version 1. What's built, what's known to be missing or rough, and what's
next. Legend: `[x]` done · `[~]` partly done · `[ ]` not started.

---

## Done

- **Panel from the SD card** — firmware keeps only a rescue page.
  `sd_card.cpp`, `web_ui.cpp`, `concordia-web/fallback.html`.
- **File manager** — list, read, save, mkdir, rename, delete, upload, ZIP
  unpack. `fs_api.cpp`, Settings tab in the panel. Still a flat table, not an
  explorer (two panes, breadcrumbs, previews) — open, see below.
- **Warnings** — ring in RAM, JSONL on the card, rotation at 48 KB. Link
  down/up, local-vs-internet reachability, refused upload tokens, failed
  sign-ins. `warnings.h/.cpp`, `GET /api/warnings`, home widget. External API
  failures not yet hooked up (`ext_api.cpp`).
- **Logging levels** — 0-5, ten independently switchable categories, mirrors
  to `/Concordia/logs/hub.log` at level 5. `logging.json`, Settings screen.
- **Users** — `/Concordia/security/users.json`, PBKDF2-SHA256 passwords,
  per-user control/add/remove/settings permissions, admin permissions
  enforced in firmware, sessions via cookie. Login screen and admin card in
  the panel. Per-user home layout and an avatar shown above the widgets are
  not built yet.
- **Wi-Fi management** — saved-network list, edit (forget + rejoin), remove,
  inline scan-and-pick — in the main panel, the captive portal, and the
  setup wizard alike.
- **First-run setup wizard** — `/Concordia/web/setup.html`, shown until the
  first admin exists. Users, timezone, location/weather, extra Wi-Fi
  networks, default widget layout. Reload-safe (`setup_pending` marker).
- **Hostname / mDNS** — configurable name, live rename with no reboot,
  `chaseHost()` auto-follows the panel to wherever the board's address moved
  (network change, portal join, hostname change) instead of leaving a tab
  pointed at a dead IP.
- **Node pairing, independent of link state** — see below, this is the
  session's main piece of new architecture.

---

## Node pairing — deferred BLE + cached code

Problem: the node's pairing code only existed once Matter had started
advertising, and advertising made the UART line to it unreliable (BLE radio
work starves the shell thread badly enough to look like a dead link). That
made pairing circular — the code needed to unblock the link was itself
blocked by the thing generating it.

Fixed by changing the order, not by fighting the radio:

- **The node holds BLE advertising off at boot** (`advertiseCommissionableIfNoFabrics
  = false`, `app_task.cpp`) instead of starting it in the first couple hundred
  milliseconds. `bridge pairing` and `bridge status` work over a clean line
  from first boot.
- **The ESP reads `bridge pairing` once, right after boot, and caches it in
  RAM** (`NrfLink::refreshPairingCache()`). `/api/pairing` serves that cache
  directly — the HTTP handler never touches the link.
- **The moment the code is cached, the ESP sends `bridge commission`**,
  which is what actually opens the node's commissioning window and starts
  advertising — so the noisy part of the boot only begins after the one
  thing that needed a clean line already has it.
- **`/api/status` reports `link: up | pairing | down`.** `pairing` means the
  link is quiet *and* the node was last seen unpaired — expected, not a
  fault, while advertising runs. `down` means it was quiet after having been
  paired — a real outage. The panel shows "Pairing…" instead of alarming
  anyone.

No hardcoded fallback code anywhere — the cache is the only source, always
populated from a real exchange with the node.

**Known gap, unresolved:** the UART line is still intermittently noisy for
reasons that don't fully track BLE advertising, SWD debugger activity, IMU
sampling, or Thread-join state — ruled out one at a time, not explained.
Correlates strongly with "not yet paired" but not perfectly. Most likely a
physical/electrical issue (wiring, connector, ground) rather than firmware.
Worth a proper hardware-level look next session rather than more code
guesses.

**If advertising never starts on its own** (the ESP's `bridge commission`
never landed, or the link was down at the moment it tried): press the node's
**USR button once, briefly**. It maps to the same
`OpenBasicCommissioningWindow()` call the ESP triggers automatically — a
direct, link-independent way to start pairing by hand. Don't hold it: a long
press is the factory-reset trigger on the same button.

**This build's current pairing identity** (fixed by
`CONFIG_CHIP_DEVICE_DISCRIMINATOR` / `CONFIG_CHIP_DEVICE_SPAKE2_PASSCODE` in
`boards/xiao_nrf54l15_nrf54l15_cpuapp.conf`, not generated per boot — see
"Board identity" in that file):

```
manual: 3497-011-2332
qr:     MT:06PS042C00KA0648G00
```

Changes only if that file's discriminator or passcode does.

---

## TODO — priority order, set 2026-08-12

### 1. Pairing code and QR, always available — for real
The architecture is built (deferred BLE + ESP-side cache, see "Node pairing"
above), but the known gap there — intermittent UART noise with no confirmed
cause — means it isn't reliable enough yet to call this actually done. Needs
the hardware-level look mentioned there before this closes.

### 2. User permissions, properly worked out
The four-way split (control/add/remove/settings) exists and is enforced in
firmware (`users.cpp`, `forceAdminPermissions()`), but it was designed and
built in one pass, not exercised against real multi-user, multi-household use.
Revisit the model itself — per-endpoint control lists, what a "guest" should
actually be able to see or touch, whether a role needs anything the current
four toggles don't cover.

### 3. "Forgot password" form
No recovery path exists today beyond an admin resetting another account, or
physical console access (`user passwd`, fully trusted, same tier as the
BOOT-button factory reset). This board has no email/SMS to send a reset link
through, so the design question comes first: a console-only reset made
discoverable from the panel's login error, a one-time recovery code shown
once at account creation, or something else. Pick one before writing any code.

### 4. Fix: Wi-Fi network deletion
Reported broken. `wifi del <idx>` and its three callers (main panel, portal,
wizard — all under the Wi-Fi management work) need to be re-checked against
real hardware, not just the console-text format that was verified last time.
Likely an index drifting between `list` and `del` (deleting by position after
the list has since changed), but confirm before fixing.

### 5. Fix: changing a device already hosted by the bridge
Reported broken. Rename and retype exist (`commands.cpp`'s `recreate()`), but
the nRF shell has no true rename — both are implemented as remove-then-add,
which gives the device a *new* endpoint id under the hood (`ui.pins`/
`ui.order`/bindings get remapped to follow it, see `remapUi()` in the panel).
That is exactly the kind of change a Matter controller does not read as "the
same accessory, renamed" — the old endpoint disappearing and a new one
appearing is a plausible explanation for the report, not a confirmed one.
Worth checking against a real controller before deciding whether the fix is
a bug in the remap, or a rewrite to a true in-place attribute update instead
of remove+add.

### 6. External indicators — status LEDs, control buttons
Onboard LEDs reserved for critical faults and operating modes only (node
down, no Wi-Fi, card failed, factory-reset countdown) — everything ordinary
gets external LEDs instead, driven by actual GPIOs, not shared with the
board's own status light. External buttons for control, separate from BOOT/
USR which stay as the always-available physical-access fallback (factory
reset, manual commissioning — see "Node pairing" above). Wiring already
scoped: node button on **D0 (P1.04)**, external LED on **D1 (P1.05)**; hub
button on **D6** needs a 1 kΩ series resistor in series, since that pin is
U0TXD and the ROM drives it at every reset.

### 7. File manager, finished
Still a flat table with a button per row. What was actually asked for: two
panes or a tree, clickable breadcrumbs, double-click to open, selection
instead of per-row buttons, sort by name/size/date, drag-to-upload, inline
preview. Backend already supports all of it — `fs_api.cpp` needs nothing
more; this is panel-only work.

### 8. OTA — firmware for both boards, and the web bundle
- **Hub.** Layout settled: a small launcher in its own partition plus one
  large application slot (3.44 MB vs. today's 1.94 MB two-slot split), with
  the ESP-IDF bootloader falling back to the launcher automatically on a bad
  image — no marker or cooperation needed for that half. The launcher reads a
  card-side marker (path, size, sha256) before writing anything.
  `partitions.csv` for the two-slot version is already in place and proven on
  hardware; the launcher itself has not been built.
- **Node.** Deferred until the hub half is proven. Prefer bit-banging SWD
  from two spare hub GPIOs into the node's SWDIO/SWCLK over
  MCUboot+SMP-over-UART — it can't brick the node (the debug port works
  whether or not the application boots), needs no change to the node's
  firmware, and the RRAM tail-write quirk `flash_xiao.sh` already handles
  transfers directly. MCUboot is the well-trodden alternative but moves the
  partition map and its failure mode is a board that doesn't boot — only
  worth it on a branch, debugger attached.
- **Web bundle.** Already the cheapest of the three to update —
  `pack.py --upload` posts a new panel to a running board with no reflash at
  all. What's missing is doing that from the panel itself (pick a version,
  download, install) instead of a terminal on a laptop.

### 9. Auto-update: flash only a minimal starter image
Ship one small launcher by hand; the real firmware and the web bundle come
down and install themselves over the air afterward. This is the same
launcher from point 8 — the difference is making it the actual first-boot
path (a blank board reaches for its own update on first Wi-Fi join) rather
than only the recovery path for a board that already had something installed.

### 10. Version control and rollback, over the air
More than "whatever is currently flashed": a version history the panel can
show, the ability to roll back to a previous known-good image and not just
forward to a new one, and the mark-valid-after-Wi-Fi-and-link-come-up check
(already scoped under point 8) so a bad update rolls itself back without
needing a cable.

### 11. Support for other boards
Right now both sides are pinned to one exact board each: the ESP half through
`config.h`'s `D0`..`D10` aliases (Seeed XIAO ESP32-C6 specifically — the
generic `esp32c6` target doesn't even define them), the node half through a
hand-written `boards/xiao_nrf54l15_nrf54l15_cpuapp.conf` that took a real
porting effort (`PORTING_NOTES.md`) to get right on hardware Nordic doesn't
official support out of the box. Neither the UART `bridge ...` protocol nor
the web panel cares what silicon is on either end — only the two board-config
files do. Generalising means: a config choice, not a source edit, for another
board in the same family on the commander side (other Seeed XIAO ESP32
variants share the same pin-alias convention), and a new board `.conf` plus
whatever porting it needs on the bridge side for another Thread-capable
Nordic part.

### 12. Web UI bug fixes
General sweep, not scoped to one feature — whatever real use turns up, beyond
the specific Wi-Fi-deletion and device-edit bugs already called out above.

---

## Also open, lower priority

### Naming cleanup
Console command names (`nrf reboot` → `node reboot`, old spelling kept as a
hidden alias) and the `NrfLink` namespace / `nrf_link.*` filenames are still
board-branded. Parked, not forgotten.

### Panel over HTTPS
Geolocation is refused outright on plain HTTP by every mainstream browser,
unconditionally — confirmed, not assumed, and nothing client-side routes
around it. Only real fix is TLS, which for a board with no public hostname
means self-signing and walking a first-time visitor through trusting it —
a real project, not scoped yet.

---

## Reference

- **Hub firmware:** `concordia-hub-esp/` — `arduino-cli --fqbn
  esp32:esp32:XIAO_ESP32C6[:PartitionScheme=huge_app]`. Board is a Seeed
  XIAO ESP32-C6; the generic `esp32c6` FQBN lacks the `D0`..`D10` aliases
  `config.h` uses and won't build.
- **Panel:** `concordia-web/` — `index.html` is the real interface,
  `setup.html` the first-run wizard, `portal.html` the captive-AP recovery
  page, `fallback.html` the tiny rescue page baked into firmware. `pack.py`
  packs and optionally uploads any of them.
- **Node firmware:** `nRF54l15_Bridge/` — built on Nordic's own **Matter
  Template** sample from the nRF Connect SDK (`nRF54l15_Bridge/README.rst`
  is that sample's stock documentation, unedited), not written from scratch.
  The bridge logic, the `bridge ...` shell, endpoint table and all of it are
  this project's; commissioning, the Matter/Thread stack underneath, and the
  board's event/button/LED plumbing are the template's. NCS v3.4.0,
  `west build -b xiao_nrf54l15/nrf54l15/cpuapp --sysbuild`. Flash with
  `./flash_xiao.sh <builddir>`, not a plain `west flash` — works around an
  OpenOCD `load_image` bug that drops the last RRAM word (script writes one
  word past the image and verifies the tail). Its own notes:
  `nRF54l15_Bridge/PORTING_NOTES.md`.
- Physical/serial console on either board is fully trusted, no login —
  115200 baud, line-based, prompt `bridge> ` on the hub, `uart:~$` on the
  node. Useful whenever Wi-Fi or the link itself is in question.
