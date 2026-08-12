# Porting notes — nRF54L15 Matter bridge

Working notes for the rebuild. Read this first after any break in the session.

## Why this project exists

The previous project (`/Users/romanenko/template`, NCS **3.2.3**) worked for weeks
and then stopped booting: `BUS FAULT`, `BFAR 0xa`, in `net_buf pool_get_uninit`
on the **MPSL Work** thread, immediately after the BLE address is printed. The
fault always landed in the radio's neighbourhood rather than in bridge code, and
it moved every time the image grew — the signature of a bad memory boundary, not
a logic bug. It was never committed in a working state, so there was nothing to
roll back to (`git reflog` there holds only the stock template and one checkpoint
that already crashed).

So: rebuild from a clean NCS **3.4.0** template, one piece at a time, and commit
every green step. Wherever it breaks is the answer.

## Ground rules

- **One change at a time**, build + flash + verify it boots, then commit.
- Distinguish *the thing that broke it* from *the thing another thing needs*.
  Most steps so far have been the second kind.
- Never rebuild with a partial `EXTRA_CONF_FILE` list. In the old project a
  rebuild that silently dropped `bridge_xiao.conf` took `BRIDGE_MAX_ENDPOINTS`
  from 100 down to the Kconfig default of 8 while 25 endpoints sat in storage.
  That cost hours.

## Paths

| What | Where |
| --- | --- |
| This project | `/Users/romanenko/nRF54l15_Bridge` |
| Broken 3.2.3 project (source of everything being ported) | `/Users/romanenko/template` |
| Bare working overlays, 3.2.3 | `/Users/romanenko/template_1` |
| ESP companion firmware | `/Users/romanenko/Documents/Arduino/bridge_esp` |
| SDK | `/opt/nordic/ncs/v3.4.0`, toolchain `ccc010f809` |

## Build and flash

```bash
TC=/opt/nordic/ncs/toolchains/ccc010f809
export PATH="$TC/bin:$TC/usr/bin:/opt/homebrew/bin:$PATH"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=$TC/opt/zephyr-sdk
export NRFUTIL_HOME=$TC/nrfutil/home
cd /opt/nordic/ncs/v3.4.0
west build -p always -b xiao_nrf54l15/nrf54l15/cpuapp --sysbuild \
  -d /Users/romanenko/nRF54l15_Bridge/build_xiao /Users/romanenko/nRF54l15_Bridge
west flash -d /Users/romanenko/nRF54l15_Bridge/build_xiao --runner openocd
```

Flashing goes through the on-board CMSIS-DAP probe. Not J-Link — the nRF Connect
Programmer cannot do this board.

## Telling whether the board is alive

Both UARTs on this hardware have been unreliable, so trust the debugger instead.

```bash
CFG=/opt/nordic/ncs/v3.4.0/zephyr/boards/seeed/xiao_nrf54l15/support/openocd.cfg
SC=/opt/nordic/ncs/toolchains/185bb0e3b6/opt/zephyr-sdk/sysroots/arm64-pokysdk-linux/usr/share/openocd/scripts
openocd -s "$SC" -f "$CFG" -c init -c halt -c "reg pc" -c resume -c shutdown
```

Read the result like this:

- `current mode: Thread` + PC resolving to `arch_cpu_idle` / `__enable_irq` →
  **healthy and idle**. Sample two or three times; a healthy board can also be
  caught in `Handler SVCall`, which is just a syscall in flight.
- `current mode: Handler BusFault` → crashed. `arch_system_halt` is where Zephyr
  parks *after* the fault; the real address is in the exception frame.
- Same PC three times in a row in Thread mode with a non-idle symbol → stuck.

Resolve addresses with the 3.2.3 toolchain's binutils (3.4.0's are laid out
differently):
`/opt/nordic/ncs/toolchains/185bb0e3b6/opt/zephyr-sdk/arm-zephyr-eabi/bin/arm-zephyr-eabi-addr2line -f -C -e <elf> <addr>`

For an actual crash dump, build with `bridge_ramconsole.conf` (copy it from the
old project): it puts the console in a RAM buffer, and `dump_image <file>
<addr-of-ram_console_buf> 4096` over SWD reads it out. That is how the MPSL fault
above was finally captured after both UARTs came up empty.

## Done so far

Everything below is on top of the stock 3.4.0 Matter template.

**Step 1 — build for XIAO at all.** Four changes, all dependencies, none of them
features:

1. `Kconfig.sysbuild`: `MATTER_FACTORY_DATA_GENERATE` off for XIAO. The board has
   no `factory_data_partition`, so sysbuild aborts during configuration.
2. `prj.conf`: `CONFIG_CHIP_FACTORY_DATA_NONE=y`. **`CONFIG_CHIP_FACTORY_DATA=n`
   does not work in 3.4.0** — the provider is behind `choice
   CHIP_FACTORY_DATA_BACKEND` whose default member *selects* it, and a `select`
   ignores `=n`. In 3.2.3 there was no choice and plain `=n` was enough.
3. `Kconfig.sysbuild`: `BOOTLOADER_NONE` for XIAO, plus `MATTER_OTA` off. MCUboot
   left enabled does not just waste flash — it halts in `arch_system_halt` before
   ever reaching the application. And the OTA requestor then fails to link on
   `dfu_target_mcuboot_set_buf`, so the two must move together.
4. `boards/xiao_nrf54l15_nrf54l15_cpuapp.conf`: `CONFIG_LTO`,
   `CONFIG_ISR_TABLES_LOCAL_DECLARATION`, `CONFIG_CHIP_BOOTLOADER_NONE`. LTO was
   needed while MCUboot was still in the picture (the image overflowed the 664 KB
   slot by ~5 KB); kept because it costs nothing.

Result: **FLASH 629668 B of 1524 KB (40%), RAM 166044 B of 256 KB (63%)**, boots
and idles cleanly.

**Step 2 - ZAP / data model.** Copied `template.zap`, `template.matter` and all
of `zap-generated/` from the 3.2.3 project. It compiled against 3.4.0's Matter
unchanged - no regeneration needed. Cost: +1328 B flash, +192 B RAM, which is
small because nothing references those clusters yet (the bridge lives in
`app_task.cpp`, still to come) and LTO drops them. The real test of this step is
therefore still ahead.

Verified on hardware: boots, prints its QR and manual pairing code
(`34970112332`), starts BLE advertising, and stays in the idle thread for 70+
seconds.

**Step 3 - the bridge itself.** `src/app_task.cpp` (5621 lines) and
`src/app_task.h` copied over unchanged, the `menu "Matter bridge options"` block
spliced into `Kconfig`, and `CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT` added to
`src/chip_project_config.h`. **It compiled against 3.4.0's Matter with no source
changes at all** - the API breakage I expected did not materialise.
`BRIDGE_MAX_ENDPOINTS` left at its default of 8; `BRIDGE_XIAO_IMU` forced to `n`
because the IMU is a later step and would otherwise test two things at once.

Cost: +30.8 KB flash, +6.7 KB RAM. Verified on hardware from the RAM console -
not merely booting, but the bridge actually up:

```
I: Bridge CLI is available over Zephyr shell. Try: bridge types
I: Bridge started with no virtual endpoints.
```

**Step 4 - shell on the header pins.** `zephyr,shell-uart = &uart21` plus
`CONFIG_SHELL_VT100_COLORS=n` / `CONFIG_SHELL_CMD_BUFF_SIZE=256`. Put in
`boards/xiao_nrf54l15_nrf54l15_cpuapp.{overlay,conf}`, which Zephyr picks up by
filename, rather than in a fragment that has to be named on the command line -
that is precisely how the old project lost its endpoint limit. Builds, flashes,
board runs (+256 B RAM).

**Not verified end to end: the ESP still sees nothing on the wire.** The ESP was
rebooted (so its UART driver is fresh) and still reports `bridge silent`. Two
SDKs, two independently built firmwares, identical silence - and the nRF is
demonstrably alive with the shell bound to uart21 in the built devicetree. That
points at the three wires (nRF P2.08 -> ESP D3, nRF P2.07 <- ESP D2, common
GND), which is the one thing in this loop nobody has actually checked. Do not
spend more firmware time on it before someone looks at the cabling.

**Step 5 - endpoint limit, 8 -> 24 -> 50 -> 100.** The step the old project died
on. Each level built, flashed and watched; all four stable.

| limit | RAM | of 256 KB |
| --- | --- | --- |
| 8 | 173212 | 66% |
| 24 | 176476 | 67% |
| 50 | 181788 | 69% |
| 100 | **191964** | **73%** |

Linear at ~204 bytes per endpoint slot, exactly as predicted from the first two
points - no cliff, no threshold effect. The old project sat at 215860-220852 B
(82-84%) for the same limit of 100 and crashed; this build has ~24 KB more room.

Some of that margin is genuine (3.4.0, no hand-drawn memory boundaries) and some
is simply things not ported yet: no IMU, no OpenThread shell, and OpenThread is
still **MTD** here where the old project ran **FTD/Router**. FTD costs RAM. Do
not treat 73% as the final number until FTD is back.

**Step 6 - OpenThread FTD.** The template defaults to MTD; the bridge is
mains-powered, hosts subscriptions and should route for the mesh, so both
`OPENTHREAD_DEVICE_TYPE` and `OPENTHREAD_NORDIC_LIBRARY_CONFIGURATION` now
default to FTD.

Cost: **+46 KB flash, +14.4 KB RAM** - RAM 206364, 78.7% of 256 KB. Stable for
100+ seconds. This was the honest comparison the endpoint ramp still owed: the
old 3.2.3 project ran FTD too and sat at 215860-220852 B (82-84%) while
crashing. Same feature, same limit of 100, ~10-14 KB more room here, and steady.

Still not ported and still to be paid for: the IMU, the OpenThread shell, and
the board identity fragment.

**Step 7 - board identity.** Product/vendor name, product id 32770, serial,
software and hardware version, manufacturing date, discriminator 0xF00, passcode
20202021, BLE name "XIAO Bridge". Ported into the board conf; all of it verified
present in the generated `.config`, including `CONFIG_BT_DEVICE_NAME`, which
this time did win against `prj.conf`.

Pairing code is unchanged from the old project - discriminator and passcode are
the same - so it is still **3497-011-2332**.

Left out on purpose: `CONFIG_REGULATOR*` and the `LSM6DSL_*` settings from the
same old fragment belong to the IMU step, and
`CONFIG_PM_PARTITION_SIZE_SETTINGS_STORAGE` does nothing here at all.

**Settings storage is 36 KB on 3.4.0, and it comes from devicetree.** With
`SB_CONFIG_PARTITION_MANAGER=n` the `storage_partition` node decides
(`@0x174000`, 36 KB); the `CONFIG_PM_PARTITION_SIZE_SETTINGS_STORAGE=0x2000`
line still visible in `.config` is inert. Worth knowing because on 3.2.3 the
8 KB default held exactly one Matter fabric, and HomeKit needs two (phone and
hub) - that was the old "will not add" bug, cured there with 64 KB. 36 KB has
not been proven either way yet; the only real test is an actual pairing.

**Validated by an actual HomeKit pairing.** The board was added to HomeKit
successfully with the ported identity and pairing code 3497-011-2332, and the
debugger showed it in Thread mode continuously through 200 seconds of
commissioning - no fault at `AddNOC`, which is where the 3.2.3 project used to
fall over. Matter services are advertised on the network and the border router
still routes `fd28:f24f:6f9a::/64`.

That settles two open questions:

- **36 KB of settings storage is enough.** HomeKit creates two fabrics (phone
  and hub); on 3.2.3 the 8 KB default held one and the second was reverted. No
  need to enlarge the partition.
- The whole stack works end to end - BLE commissioning, Thread join, fabric
  storage, operational discovery - not merely "the board boots".

It hosts no devices yet, as expected: zero virtual endpoints, and none can be
added while the ESP link is down.

**Step 8 - Matter address fallback.** `CHIP_CONFIG_MDNS_RESOLVE_LOOKUP_RESULTS 4`
in `src/chip_project_config.h`. Matter otherwise keeps exactly one resolved
address per peer, and its sorter prefers a global unicast over a ULA; when this
ISP rotates its prefix the hub's advertised global address dies while its ULA
still works, and the CASE retry has nothing to fall back to. That is the bridge
"going offline" with a perfectly healthy radio. +896 B RAM (207260, 79.1%),
stable.

Worth noting: the same define appeared to cost nothing in the 3.2.3 project,
which should have been a warning that it was not taking effect there.

**Step 9 - OpenThread shell.** `CONFIG_OPENTHREAD_SHELL=y`. +44 KB flash,
+1.9 KB RAM (209180, 79.8%). Verified through the ESP:

```
nrf ot state   -> router
nrf ot ipaddr  -> fd28:f24f:6f9a:0:6a7c:79bf:56cc:261b  (OMR), mesh-local, link-local
status         -> 1 of 100 slots used
```

`state = router` also confirms the FTD step did what it was for - the board
routes rather than sleeping. And the endpoint survived a reflash, so settings
persistence works.

**The ESP link works - it just needs rebooting after the nRF is reflashed.**
The "silent link" that ate hours was not wiring. Reboot the ESP *after* the nRF
is up and past its first boot, and it finds the bridge. At step 4 the ESP was
rebooted while the nRF was still in its crashed first-boot state, which is why
that test came back silent and sent the search off towards cabling.

Confirmed working end to end:

```
nRF link   responding
endpoints  1 of 100 slots used
EP2  light  "On Off Light test"  = 1
```

Web panel -> ESP -> UART -> nRF shell -> Matter dynamic endpoint -> HomeKit,
with the nRF idling normally through it. The rebuild has functional parity with
the old project at this point, and is stable where the old one was not.

**A first boot after re-flashing can crash on stale settings.** Right after
flashing this step the board sat in `arch_system_halt` on three consecutive
samples. The identical code, reflashed, has been stable ever since. It then
happened again, identically, on step 3 - so this is a property of the board, not
a coincidence.

The RAM console named it: the first boot ends with `E: Failed to initialize
chip` and halts; the next boot is clean. The buffer survives a reset (that is
the point of a RAM console), so that line sits at offset 0 *ahead of the kernel
banner* - it is the tail of the previous boot, not the current one. Read it that
way or it looks like an impossible failure after a successful start.

Rule: **if a fresh flash halts, reflash and re-check before believing the change
caused it.** Twice now the second flash has been clean.

**Soak test passed.** Left running overnight on commit `094dcda` (bridge, 100
slots, FTD, shell on pins, identity, address fallback, OT shell - no IMU) and it
was still up in the morning, with the ESP link still alive. The "UART dies after
a few hours" behaviour that dominated the old project has not reproduced here.
Whatever caused it belongs to the 3.2.3 firmware, not to the boards or the
wiring.

**The MPSL crash has a RAM threshold, somewhere around 81-82%.** Builds at
80.7% run indefinitely; the same firmware plus debug logging, at 82.3%, faults
in `pool_get_uninit` on MPSL Work every time. The 3.2.3 project sat at 82-84%
and crashed. So LTO was one way to trigger it, not the whole story - there is a
real ceiling here, and it is lower than the 256 KB the linker reports. Treat
anything above ~80% as unsafe until that is understood.

**The IMU: `CONFIG_REGULATOR_NRF54L` must be off.** Enabling the sensor pulls in
`CONFIG_REGULATOR`, which on this SoC also brings up a chip-level regulator
driver managing its own power modes (see its `HIBERNATE_MIN_UPTIME_MS`
companion). With it in, the board crashed in MPSL right after BLE init; with
`CONFIG_REGULATOR_NRF54L=n` and only `REGULATOR_FIXED` for the sensor rail, it
boots first time and stays up. Seeed's zephyr-imu example gets away with the
default because it has no radio to destabilise.

**The sensor works - defer its init and do it from the application.** (Kept the
history below because the dead ends are worth not repeating.)

Two changes: `zephyr,deferred-init` on `&lsm6ds3tr_c`, and `device_init()` from
`StartImuVibrationSensor()`. Result:

```
IMU lsm6ds3tr-c@6a
  acceleration : 1022 mg   (1000 mg = 1 g at rest)
  endpoint     : EP24
```

Why the automatic init failed is still not fully explained - the rail is
demonstrably on (P0.01 reads back high) and the part answers an `i2c scan` at
0x6a perfectly well once the board is up - but the boot-time attempt fails its
software reset, and `device_is_ready()` then reports that one failure forever;
it never re-probes. Doing it from the application, seconds later, simply works.
A longer `startup-delay-us` on the rail (5 ms -> 50 ms) was tried first and did
not help on its own, so this is not purely a settling-time issue.

**Earlier, before that fix: the sensor did not answer.** With the crash gone, the driver's own
message is now visible in the RAM console:

```
D: failed to reboot device
```

That is the LSM6DSL driver failing its software reset of the chip - i.e. no I2C
response at 0x6a. Not a Matter problem and not LTO. Ruled out so far: device
power management (PM_DEVICE_RUNTIME is not enabled, the bus does not sleep) and
init ordering (regulator 45 -> i2c 50 -> sensor 90). Next places to look are the
rail actually being driven on P0.01 and whether anything else claims that pin.

**Onboard LED as a bridged device.** `led0` (P2.00) now gets an On/Off endpoint
called "Onboard LED", created on first boot the same way the IMU's vibration
endpoint is. The GPIO is driven from `NotifyEndpointValueChanged`, which every
value change already passes through, so it follows the endpoint whatever moved
it - shell, web panel or controller. Restored state is applied at startup so the
light matches what the bridge thinks after a reboot.

Useful as a permanent end-to-end check: it is the one endpoint whose state is
visible with no other equipment.

Watch out for stale ESP-side bindings: the new endpoint landed on EP25, which
still carried an old weather-API binding from a previous device with that id,
and the poller kept writing a temperature into the LED. `unbind <ep>` clears it.

**IntegerSqrt never terminated - and that is what silences the ESP link.**
Found by sampling the program counter while the link was dead: eight samples out
of eight landed in 64-bit division. The board was not stuck, it was busy.

```cpp
while (guess != previous) {           // wrong
  previous = guess;
  guess = (guess + value / guess) / 2;
}
```

Newton's method in integers stops converging near the root and then oscillates
between two adjacent values, so the two are never equal and the loop spins
forever. It runs in the IMU sampling thread, which therefore never sleeps again.

What that looks like from outside is the important part, because it is exactly
the old project's unexplained failure:

- the shell sits below the IMU thread in priority and is starved, so the ESP
  reports `bridge silent` and rebooting the ESP changes nothing;
- Matter and Thread sit above it and keep running, so HomeKit stays happy and
  the board answers pings;
- the debugger shows `Thread` mode, so the board looks alive by every check
  short of asking where the PC actually is.

Fix: stop as soon as the sequence fails to decrease (`do { ... } while (guess <
previous)`), which is both correct and guaranteed to finish. Verified against
`sqrt()` on the host across a dense range before flashing. After it, all eight
PC samples land in the idle thread and the link answers immediately.

This code came over from the 3.2.3 project unchanged, so it is a strong
candidate for that project's "UART dies after a few hours while HomeKit keeps
working" - the same signature, and it would trigger whenever the accelerometer
happened to produce a magnitude that makes the iteration oscillate.

**SOLVED: the MPSL fault was the flashing tool dropping the last word.**
Read this before chasing any crash on this board.

OpenOCD's `load_image` leaves the final word of the image sitting in the RRAM
write buffer and never commits it. Deterministically - not a race. That word
keeps whatever was in flash before.

Here it held the last `__bufs` pointer of the net_buf pool table, so the radio
faulted the first time it allocated a buffer:

```
BUS FAULT, BFAR 0x750d7, pool_get_uninit (buf.c:82), thread "MPSL Work"
```

`0x750cd` was exactly the stale word, +10 for the field being written. Proven by
reading the flash back against the image: expected `0x200309ec`, flash held
`0x750cd`, after every flash. Writing that single word by hand made the board
boot immediately.

That one bug accounts for nearly all the mystery in this project:

- the reflash-until-it-boots ritual - each flash left different residue there;
- builds broken by 228 bytes of unrelated code - it only decides *what* the
  linker places last;
- "more RAM makes it worse" - bigger images move the pool table around;
- LTO and the endpoint limit appearing to fix it - both changed the layout;
- the same crash in the 3.2.3 firmware, flashed the same way.

Use `./flash_xiao.sh`. It writes one word past the end of the data image to
flush the buffer, then reads the tail back and fails loudly on a mismatch.
Never flash this board with a bare `west flash`.

**Superseded: what this looked like before it was understood.** Branch
`ep-logic-wip` holds a change that is inert at boot - it only alters how
`bridge set` parses its 3rd and 4th arguments, plus what `ValueKindFor`
reports - and adds 228 bytes of flash and no RAM. That build does not boot:
six flashes and a settings wipe all faulted, while `e85dfb0` boots first time
every time.

So the fault is not triggered by what the code *does* but by where the code
*sits*. That is the same fragility the 3.2.3 project had, and the reason this
one has needed a reflash-until-it-boots ritual all along. Removing LTO,
dropping the endpoint limit to 50 and fixing IntegerSqrt each made it rarer -
none of them removed it.

Until that is understood, treat any new build as guilty until it boots, and
keep master at a commit that is known to.

**What has NOT been carried over from 3.2.3** (checked symbol by symbol, not
from memory):

- `CONFIG_CHIP_LIB_SHELL=n`, `CONFIG_NCS_SAMPLE_MATTER_TEST_SHELL=n`,
  `CONFIG_BRIDGE_DEBUG_LOG=n`, `CONFIG_MATTER_LOG_LEVEL_INF` - all four are
  currently *on*, so the heavy Matter test shell and verbose logging are being
  carried for nothing. Worth reclaiming, especially given the RAM ceiling.
- The hardware watchdog: `watchdog0 = &wdt31` in the overlay plus
  `CONFIG_NCS_SAMPLE_MATTER_WATCHDOG=y`. After the IntegerSqrt spin, this is
  the most valuable of the lot - it would have reset the board instead of
  letting it grind.
- `src/aux_shell.c` (second shell on USB - was default-off and never worked),
  `flash_xiao.sh`, `bridge_diag.conf`, `bridge_dk.conf`.
- `CONFIG_PM_PARTITION_SIZE_SETTINGS_STORAGE` - deliberately not ported, it is
  inert with Partition Manager off.

Everything else is across.

**An uncommissioned board looks silent to the ESP.** Reported from the field and
it holds up: while the nRF is not paired to an ecosystem it advertises over BLE
continuously, the radio keeps taking time slots, and the shell - which sits
below all of that in priority - stops answering inside the ESP's timeout. Pair
it and the link comes back.

This is worth knowing before diagnosing anything: a fresh flash leaves the board
uncommissioned, so `bridge silent` right after flashing usually means "not
paired yet", not "broken". Several dead ends in this project's history were
exactly that.

**Verified after the flashing fix**, on a board that had been wiped and
re-paired:

```
EP4 type=aircon     value=2150 v2=2300 v3=4   setpoint and mode both applied
EP5 type=thermostat value=2000 v2=2250 v3=1
EP6 type=dimmer     value=1    v2=200
```

The aircon setpoint used to stay at its default of 2100 no matter what was
asked for, because the argument was being checked against the dimmer's 1..254
range.

**The web's short device-type list is a stale cache, not a bridge problem.**
`/api/types` in the ESP firmware caches the list on the first successful fetch
and never refreshes it:

```cpp
static String cached;
if (cached.length() == 0) { ...fetch...; if (count > 0) cached = out; }
```

Measured on hardware: the nRF sends all 32 types (3332 bytes) and the ESP parses
all 32 - the `types` command on its console proves it. Only the web sees six.
Those six are the first chunk of a reply that got cut by the reader's idle
timeout, and `count > 0` was enough to cache it for good.

It gets cut when the board answers slowly, which is exactly what an
uncommissioned board does - see the BLE advertising note above. So the cache
most likely filled right after a flash, before pairing.

Same root cause as "the thermostat does not work": it was never in those six, so
it could not be added from the web at all.

Fix, when the ESP firmware is next touched: fetch the type list with the slow
timeout (it is a long reply), and only cache a reply that contains the trailing
"Use: bridge add" line the command prints after the list - a reliable marker
that nothing was truncated.

## Findings worth remembering

**CONFIG_LTO corrupts net_buf pool lookup - do not enable it here.** This is the
one that cost the most time in the rebuild, and it was self-inflicted: LTO was
copied in at step 1 to fit MCUboot's 664 KB slot, and left in place after the
bootloader was dropped and the whole 1524 KB became available.

Symptom, identical to the one the 3.2.3 project died of:

```
E: ***** BUS FAULT *****  Precise data bus error
E:   BFAR Address: 0xa
E: Faulting instruction address: pool_get_uninit (net_buf/buf.c:82)
E:   called from net_buf_alloc_len
E: Current thread: MPSL Work
```

`net_buf` pools live in a linker section and are found by index; LTO mangles
that, `net_buf_alloc_len` gets a null pool, and reading `pool->buf_count` at
offset 0xa faults. It presents as a memory bug - intermittent, more frequent as
the image grows - because that is exactly how LTO's decisions shift.

Removing it fixed it outright: first flash, clean boot, repeatedly. Cost is
52 KB of flash out of 1524 KB.

Note the old 3.2.3 project did **not** have LTO set, so its identical-looking
crash had a different cause. Same signature, two roots - which is worth knowing
before assuming this closes that case too.

**The FLPR memory reclaim is not needed on 3.4.0.** The old project carried this
in its board overlay:

```dts
&cpuapp_rram { reg = <0x0 DT_SIZE_K(1524)>; };
&cpuapp_sram { reg = <0x20000000 DT_SIZE_K(256)>; ranges = <0x0 0x20000000 0x40000>; };
```

It was the prime suspect for the MPSL crash — hand-drawn memory boundaries next
to a radio that owns part of that memory. On 3.4.0 the build already reports the
full 1524 KB / 256 KB **with no overlay from us**. Do not port those lines. If
they ever seem necessary again, that is a signal something else is wrong.

**Seeed rewrote the XIAO board files for 3.4.0.** Several of our hand-patches are
now upstream, so do not port them either:

- `&temp { status = "okay" }` — required by MPSL, we used to add it ourselves
- the IMU is declared correctly as `lsm6ds3tr_c` / `compatible = "st,lsm6dsl"`
  (the 3.2.3 files said `st,lsm6dso`, whose driver rejects the fitted part's
  WHO_AM_I of 0x6A)
- the IMU/mic regulators changed to `GPIO_ACTIVE_LOW` + `regulator-boot-on` —
  in 3.2.3 the rail was being driven the wrong way round
- `&hfxo` now configures its load capacitors. Nothing in 3.2.3 did. An untuned
  reference crystal is a plausible explanation for the garbled-at-every-baud
  UART output we chased for hours, and for instability in radio context.

**Board `.conf` fragments lose to `prj.conf`.** A board file saying `=n` against
an explicit `=y` in `prj.conf` does not win. Put the override in `prj.conf`.

**Sysbuild overrides both.** Anything arriving through `.config.sysbuild` (e.g.
`CONFIG_CHIP_OTA_REQUESTOR`) can only be changed in `Kconfig.sysbuild`. Check
`build_xiao/nRF54l15_Bridge/zephyr/.config-trace.json` when a setting refuses to
take — it records, per symbol, exactly which file and mechanism decided it.

## Next steps

1. ZAP / data model from the old project — the user's known-good part.
2. Shell on the header pins + the ESP link (`xiao_shell_on_gpio.overlay`).
3. Raise the endpoint limit in stages: 24 → 50 → 100, verifying each.
4. Identity (`boards/xiao_nrf54l15_extra.conf`), settings partition size, IMU.
5. `CHIP_CONFIG_MDNS_RESOLVE_LOOKUP_RESULTS 4` in `src/chip_project_config.h` —
   keeps HomeKit from going offline when the ISP rotates its prefix and the hub's
   cached global address dies. Without it Matter keeps exactly one resolved
   address and has nothing to fall back to.

Expect `app_task.cpp` to need work: it was written against the Matter in 3.2.3.

## `bridge pairing`, and the stack that could not hold it

The ESP had always asked the board for `bridge pairing` and always got nothing
back, because the command did not exist. It does now: `BridgePairingCommand` in
`app_task.cpp` prints

```
manual=34970112332
qr=MT:06PS042C00KA0648G00
```

from `GetManualPairingCode()` / `GetQRCode()` in
`<setup_payload/OnboardingCodesUtil.h>`, both with
`RendezvousInformationFlag::kBLE` — the same flag `matter_init.cpp` uses for the
banner it prints at boot. A different flag here would produce a payload that
quietly disagrees with the one on the console. The `manual=` and `qr=` prefixes
are what the ESP parses; they are contract, not decoration.

**The first version of it killed the board.** MemManage on the first call, same
PC on three consecutive samples. The proof was in two registers rather than in
reasoning:

```
MMFAR = 0x2002aa7c     <- the address that faulted
PSP   = 0x2002aab0     <- the thread's stack pointer
```

The faulting address sits 52 bytes *below* the stack pointer: the thread walked
off the bottom of its own stack and `CONFIG_HW_STACK_PROTECTION` caught it. Not
a wild pointer, not the Matter call — plain stack exhaustion. Two fixes, both
kept:

- the 156 bytes of code buffers are `static`, not automatic (every shell command
  runs on the one shell thread, so there is no second caller to race);
- `CONFIG_SHELL_STACK_SIZE=4096`, up from the default 3168, in the board conf.

Worth remembering: **`arch_system_halt` is never the answer.** It is where Zephyr
parks after the fatal handler has run. Both times this project has crashed, the
real story was in CFSR/MMFAR and the exception frame, and CFSR reads back as 0
because the handler clears it — MMFAR still holds the address.

### While that fault was live, the ESP looked broken too

It answered ping and refused every HTTP request, including the static page, and
its USB console printed nothing. That is worth recognising on sight: the Wi-Fi
task keeps replying to ping on its own, so **ping proves nothing about the
firmware**. The Arduino loop was wedged against a dead nRF and the 30-second loop
watchdog did not rescue it. Both boards came back only after the nRF was fixed
and the ESP was power-cycled; a `--after hard-reset` from esptool alone was not
enough while the nRF was still down.

## RAM figures, so a good build is not mistaken for a broken one

Earlier notes used 215860 B as the "known-good" RAM number. That was from the
100-endpoint era. With `CONFIG_BRIDGE_MAX_ENDPOINTS=50` the same healthy build
reports **200156 B**, and 201180 B once the shell stack grew to 4096. A lower
number here is the endpoint limit, not a dropped config fragment — check the
fragments themselves before panicking:

```bash
grep -E '^CONFIG_(BRIDGE_MAX_ENDPOINTS|CHIP_BOOTLOADER_NONE|CHIP_FACTORY_DATA_NONE|BRIDGE_XIAO_IMU|SHELL|LTO)=' \
  build_xiao/nRF54l15_Bridge/zephyr/.config
```

## Shell thread priority, and why the real fix is deferred BLE, not this

"An uncommissioned board looks silent to the ESP" (above) has a number behind
it: the shell runs at `K_LOWEST_APPLICATION_THREAD_PRIO` (14, Zephyr's own
default) while BT/OpenThread threads sit at priorities 6-8. On a single core
with no cross-priority time-slicing, an uncommissioned board's continuous BLE
advertising keeps those threads busy enough that the shell rarely gets to run
at all - not stuck, just starved.

`CONFIG_SHELL_THREAD_PRIORITY_OVERRIDE=y` / `CONFIG_SHELL_THREAD_PRIORITY=1`
in `boards/xiao_nrf54l15_nrf54l15_cpuapp.conf` puts the shell ahead of all of
them. Verified over SWD (RAM-console build) that this alone does not clear
the symptom - the node was confirmed alive and idle, not crashed, yet the
link stayed silent while advertising ran. Left in place anyway: harmless, and
priority alone was never going to be a complete fix for a thread that can
still lose a scheduling race against something above the preemptible tier.

**The actual fix is architectural, not a priority tweak:** the node now holds
BLE advertising off entirely until told to start (`bridge commission`), so
the shell has no radio contention to lose to in the first place during the
one window that matters - reading `bridge pairing` at boot. See
`src/app_task.cpp` (`DeferCommissioningWindow`, `BridgeCommissionCommand`)
and, on the ESP side, `concordia-hub-esp/nrf_link.h`'s "deferred pairing"
block and `concordia-hub-esp/ROADMAP.md`. The priority override stays as a
belt-and-braces improvement for whatever traffic still shares the link once
advertising is running (post-`bridge commission`, pre-pairing).

**Still open:** the UART line is intermittently noisy for reasons that don't
fully track BLE advertising, SWD activity, IMU sampling, or Thread-join state
- each ruled out individually, not explained. Correlates strongly with "not
yet paired" but not perfectly enough to call it solved. Most likely a
physical/electrical issue (wiring, connector, ground) rather than anything
left to fix in software. Next real step is a hardware-level look, not another
software guess.
