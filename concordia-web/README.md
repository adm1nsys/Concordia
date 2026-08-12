# Concordia web

The admin panel, in a form you can work on with nothing but a browser. No board,
no hub, no network.

## Working on it

Open `index.html`. That is the whole workflow — double-click it, or drag it into
a browser window.

`mock.js` intercepts every call the panel would make to the hub, prints it to
the browser console, and answers with data held in memory. So the page is fully
clickable: devices can be added, renamed, switched on, bound to an API, and the
list updates as it would on real hardware. Open the console (⌥⌘J in Chrome,
⌥⌘C in Safari) to watch what the panel is asking for:

```
[hub] POST /api/cmd cmd=set 4 1 200
[hub]   -> EP4 = 1
```

Everything the stub knows is taken from the real bridge — all 32 device types,
their value kinds and their hints — so what you design against is what the
hardware sends.

## Shipping it to the board

The panel lives on the SD card, not in the firmware. Nothing about it is
compiled in any more, so it can grow to whatever size it likes — 4 GB of card
against about 45 KB of spare flash.

```bash
./pack.py --upload <ip> <token>
```

That gzips the page and posts it to `/api/web`. No card removal, no reflashing.
The token is in `/Concordia/security/api_token` on the card, and the board also
hands it out over `/api/sd` while no panel is installed yet — a bootstrap window
that closes by itself as soon as the first upload lands.

Run `./pack.py` with no arguments to just produce `index.html.gz` and copy it to
`/Concordia/web/` yourself.

### The card

```
/Concordia/            anything uncategorised
/Concordia/security/   the API token, and secrets to come
/Concordia/this/       configuration for the board running the firmware
/Concordia/node/       configuration for the Matter node it bridges
/Concordia/web/        the panel
/Concordia/logs/       logs meant to outlive a reboot
/Concordia/backup/     the previous copy of anything replaced in place
/Concordia/tmp/        uploads in progress, never served
```

`this` and `node` deliberately avoid naming the hardware, so replacing a board
does not make every path on the card a lie. The firmware creates the tree on
first boot and **never formats anything** — a card that will not mount is
reported and left alone.

### The rescue page

`fallback.html` is a separate, deliberately tiny page baked into the firmware
(about 1.3 KB packed). It appears only when the card has no panel, and it does
one thing: put one there, either through a file picker or by showing you the
curl command. Keep it small — unlike the panel, every byte of it is firmware.

```bash
./pack.py --fallback --install     # then rebuild and flash the sketch
```

## What the panel talks to

Endpoints on the hub, and one of them does most of the work:

| Call | Answer |
| --- | --- |
| `GET /api/status` | firmware, Wi-Fi, whether the node answers, slots used |
| `GET /api/devices` | the bridged devices, with values and any API binding |
| `GET /api/types` | device types the bridge can host |
| `GET /api/log` | recent log lines |
| `POST /api/cmd` | runs one console command and returns its output |
| `GET /api/sd` | card status, and the token while none is installed |
| `POST /api/web` | installs a file under `/Concordia/web` (needs `X-Token`) |
| `DELETE /api/web?file=` | moves one to `/Concordia/backup` (needs `X-Token`) |

`/api/cmd` is the interesting one. Every button in the panel composes a command
string and posts it — `add temp Kitchen`, `set 4 1 200`, `rm 7`. The same
commands work over the hub's USB console, so there is exactly one implementation
of "add a device" and no second one to drift out of sync.

## Reading a device type

`/api/types` gives each type a `kind`, and that is what decides which control
the panel should draw:

| kind | meaning | sensible control |
| --- | --- | --- |
| `bool` | 0 or 1 | switch |
| `level` | 1..254 | slider |
| `percent` | 0..100 | slider, shown as % |
| `centipercent` | 0..10000 | slider, shown as % with one decimal |
| `centidegree` | centi-C, 2137 = 21.37 °C | number field in degrees |
| `enum` | a small set of numbered states | picker |
| `number` | plain integer | number field |

A `+` joins the parts of a multi-value device, in the order `set` takes them:

- `bool+level` — a dimmable light: on/off, then brightness
- `bool+level+mireds` — a colour light: on/off, brightness, colour temperature
- `centidegree+setpoint+mode` — the climate family: temperature, target, mode

`set <ep> <value> [second] [third]` carries them, so a thermostat is
`set 5 2000 2250 1` — 20.00 °C, target 22.50 °C, mode auto.

## Known gaps, worth designing around

- **Device information is the bridge's, not per-device.** Manufacturer, model
  and serial are answered from the board's own identity for every bridged
  device, so a per-device form for them has nothing to write to yet. That needs
  storage on the node first.
- **Thermostat, room AC and fan do not follow commands from a controller.** They
  can be set from this panel, but the bridge has no handler for the attribute
  writes HomeKit sends, so changes made there do not reach the device.
Fixed since, kept here because the symptom was so misleading: the hub used to
cache the type list even when the answer arrived truncated, so one slow moment
on the link left the panel offering six device types out of thirty-two for as
long as the hub stayed up. It now reads the list with the patient timeout and
keeps it only when the board's closing line is present.
