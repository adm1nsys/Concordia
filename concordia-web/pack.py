#!/usr/bin/env python3
"""Build the admin panel for the board.

The panel lives on the SD card now, not in the ESP's flash. That is what lets it
grow: 4 GB of card against about 60 KB of spare firmware space, and the firmware
no longer pays for a single line of CSS.

    ./pack.py                         # -> index.html.gz, ready for the card
    ./pack.py --upload <ip> <token>   # ...and send it to the board over the API
    ./pack.py --fallback --install    # rebuild both of the firmware's baked-in pages

Three different pages, do not confuse them:

* `index.html` is the real panel. It is gzipped to `index.html.gz` and either
  copied onto the card or posted to `/api/web`. Nothing about it is compiled in.
* `fallback.html` is the rescue page baked into the firmware, shown only when the
  card has no panel on it. It exists to put one there and does nothing else, so
  it must stay small - every byte of it is firmware space.
* `portal.html` is the other baked-in page, shown only while the hub is running
  its own open Wi-Fi portal because it could not join a saved network. It has to
  work with no card and no saved credentials, which is exactly why it cannot
  live on the card the way the real panel does.

All three are stored gzipped and served with `Content-Encoding: gzip`, which
every browser has understood for twenty years and which cuts the panel to a
third of its size on the wire.

The development stub is stripped on the way out: any line carrying `dev-only`
goes, so `mock.js` never reaches the board.
"""
import argparse
import gzip
import pathlib
import shutil
import sys
import urllib.request
import uuid

HERE = pathlib.Path(__file__).parent
# Sibling of concordia-web under the repo root, not a hardcoded absolute path -
# the sketch moved to ~/Cancordia/concordia-hub-esp partway through this
# project (see ROADMAP.md) and this stayed pointed at the old, now-nonexistent
# location until this line. --fallback --install would have silently written
# nowhere useful, or errored, rather than reaching the sketch actually in use.
SKETCH = HERE.parent / "concordia-hub-esp" / "web_page.h"

HEADER = '''/*
 * The firmware's two baked-in pages, gzip-compressed.
 *
 * GENERATED - do not edit by hand.
 * Sources: concordia-web/fallback.html and concordia-web/portal.html,
 * packed with concordia-web/pack.py --fallback --install.
 *
 * Neither is the admin panel. The panel lives on the SD card, under
 * /Concordia/web, and is served from there. These two exist for the two ways
 * that can fail to be reachable: no panel on the card (kIndexPageGz, the
 * rescue page - a file picker and the upload API, nothing more), and no
 * Wi-Fi joined (kPortalPageGz - join a network, nothing more). Both stay
 * small on purpose, because unlike the panel they cost firmware space.
 *
 * rescue page  - uncompressed {plain} bytes, compressed {packed} bytes
 * portal page  - uncompressed {plain2} bytes, compressed {packed2} bytes
 */
#pragma once

#include <Arduino.h>

static const uint8_t kIndexPageGz[] PROGMEM = {{
{body}}};
static const size_t kIndexPageGzLen = sizeof(kIndexPageGz);

static const uint8_t kPortalPageGz[] PROGMEM = {{
{body2}}};
static const size_t kPortalPageGzLen = sizeof(kPortalPageGz);
'''


def squeeze(path: pathlib.Path) -> tuple[bytes, int]:
    raw = path.read_text()
    kept = [line for line in raw.splitlines(keepends=True) if "dev-only" not in line]
    dropped = len(raw.splitlines()) - len(kept)
    return "".join(kept).encode(), dropped


def gz(data: bytes) -> bytes:
    # mtime=0 so an unchanged page always packs to identical bytes; a rebuild
    # that changes nothing should not show up as a diff.
    return gzip.compress(data, compresslevel=9, mtime=0)


def post(url: str, token: str, blob: bytes, filename: str) -> None:
    boundary = uuid.uuid4().hex
    body = (
        f'--{boundary}\r\nContent-Disposition: form-data; name="file"; '
        f'filename="{filename}"\r\nContent-Type: application/gzip\r\n\r\n'
    ).encode() + blob + f"\r\n--{boundary}--\r\n".encode()

    req = urllib.request.Request(
        url,
        data=body,
        headers={
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "X-Token": token,
        },
    )
    with urllib.request.urlopen(req, timeout=90) as resp:
        print(f"board said: {resp.read().decode().strip()}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fallback", action="store_true",
                    help="pack fallback.html into the firmware's web_page.h")
    ap.add_argument("--install", action="store_true",
                    help="copy the generated web_page.h into the ESP sketch")
    ap.add_argument("--setup", action="store_true",
                    help="pack setup.html (the first-run wizard) instead of index.html")
    ap.add_argument("--upload", nargs=2, metavar=("IP", "TOKEN"),
                    help="post the panel to a running board")
    args = ap.parse_args()

    if args.fallback:
        def rows_for(name):
            html, dropped = squeeze(HERE / name)
            packed = gz(html)
            rows = ["  " + " ".join(f"0x{b:02x}," for b in packed[i:i + 16])
                    for i in range(0, len(packed), 16)]
            return html, packed, "\n".join(rows) + "\n"

        html, packed, body = rows_for("fallback.html")
        html2, packed2, body2 = rows_for("portal.html")
        out = HERE / "web_page.h"
        out.write_text(HEADER.format(plain=len(html), packed=len(packed), body=body,
                                     plain2=len(html2), packed2=len(packed2), body2=body2))
        print(f"rescue page {len(html)} -> {len(packed)} bytes")
        print(f"portal page {len(html2)} -> {len(packed2)} bytes")
        print(f"wrote {out}")
        if args.install:
            if not SKETCH.parent.is_dir():
                print(f"error: no sketch at {SKETCH.parent}", file=sys.stderr)
                return 1
            shutil.copy(out, SKETCH)
            print(f"installed -> {SKETCH}")
            print("now rebuild and flash the ESP sketch")
        return 0

    source = "setup.html" if args.setup else "index.html"
    label = "wizard" if args.setup else "panel"
    html, dropped = squeeze(HERE / source)
    packed = gz(html)
    out = HERE / (source + ".gz")
    out.write_bytes(packed)
    saved = len(html) - len(packed)
    print(f"{label} {len(html)} -> {len(packed)} bytes "
          f"({len(packed) * 100 // len(html)}%, {saved} saved)"
          f"{f', {dropped} dev-only line(s) dropped' if dropped else ''}")
    print(f"wrote {out}")

    if args.upload:
        ip, token = args.upload
        print(f"uploading to {ip} ...")
        post(f"http://{ip}/api/web", token, packed, source + ".gz")
    else:
        print(f"copy it to /Concordia/web/{source}.gz on the card, "
              "or re-run with --upload <ip> <token>")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
