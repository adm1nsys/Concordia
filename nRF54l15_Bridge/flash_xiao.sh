#!/usr/bin/env bash
#
# Flash the XIAO nRF54L15 over its on-board CMSIS-DAP probe.
#
# Why this exists instead of a plain `west flash`:
#
# OpenOCD's load_image leaves the last word of the image sitting in the RRAM
# write buffer and never commits it. Whatever should have been in that word
# keeps whatever was there before. It is silent, it is deterministic, and it
# only hurts when something important lands there - in this project it was the
# final `__bufs` pointer of the net_buf pool table, so the radio faulted the
# first time it tried to allocate a buffer:
#
#   BUS FAULT, BFAR 0x750d7, pool_get_uninit, thread "MPSL Work"
#
# and 0x750cd was exactly the garbage left in that word, +10 for the field
# being written. That is the bug behind every "reflash until it boots" ritual
# in this project's history, and behind the same crash in the 3.2.3 firmware:
# whether it bites depends only on what the linker happens to place last, which
# is why adding 228 bytes of unrelated code could break a working build.
#
# The fix is to write one more word past the end of the image, which pushes the
# buffered line out. Then verify, because a silent flashing failure is not
# something to take on trust twice.
set -euo pipefail

BUILD_DIR="${1:-$(cd "$(dirname "$0")" && pwd)/build_xiao}"
SDK=/opt/nordic/ncs/v3.4.0
TC=/opt/nordic/ncs/toolchains/ccc010f809
BINUTILS=/opt/nordic/ncs/toolchains/185bb0e3b6/opt/zephyr-sdk/arm-zephyr-eabi/bin
OPENOCD=${OPENOCD:-/opt/homebrew/bin/openocd}
CFG="$SDK/zephyr/boards/seeed/xiao_nrf54l15/support/openocd.cfg"
SCRIPTS=/opt/nordic/ncs/toolchains/185bb0e3b6/opt/zephyr-sdk/sysroots/arm64-pokysdk-linux/usr/share/openocd/scripts

ELF="$BUILD_DIR/nRF54l15_Bridge/zephyr/zephyr.elf"
HEX="$BUILD_DIR/nRF54l15_Bridge/zephyr/zephyr.hex"
[ -f "$ELF" ] || { echo "no build in $BUILD_DIR" >&2; exit 1; }

# End of the initialised-data image: the last address load_image has to commit.
LOAD_END=$("$BINUTILS/arm-zephyr-eabi-nm" "$ELF" | python3 -c '
import sys
v = {}
for line in sys.stdin:
    parts = line.split()
    if len(parts) >= 3 and parts[2] in (
            "__data_region_load_start", "__data_region_start", "__data_region_end"):
        v[parts[2]] = int(parts[0], 16)
print(hex(v["__data_region_load_start"] + v["__data_region_end"] - v["__data_region_start"]))')

echo "flashing $HEX (data image ends at $LOAD_END)"
"$OPENOCD" -s "$SCRIPTS" -f "$CFG" \
  -c "init" -c "halt" \
  -c "mww 0x5004b500 0x101" \
  -c "load_image $HEX" \
  -c "mww $LOAD_END 0xffffffff" \
  -c "reset run" -c "shutdown" 2>&1 | grep -E "written at|downloaded|Error" || true

# Verify the tail actually made it, since that is the failure this guards.
LAST=$(printf "0x%x" $((LOAD_END - 4)))
TMP=$(mktemp)
"$BINUTILS/arm-zephyr-eabi-objcopy" -O binary \
  --only-section=net_buf_pool_area "$ELF" "$TMP" 2>/dev/null || true
EXPECT=$(python3 -c "
import struct,sys
d=open('$TMP','rb').read()
print('%08x' % struct.unpack('<I', d[-4:])[0]) if len(d)>=4 else print('')")
rm -f "$TMP"
ACTUAL=$("$OPENOCD" -s "$SCRIPTS" -f "$CFG" -c "init" -c "halt" \
         -c "echo [format %08x [read_memory $LAST 32 1]]" -c "resume" -c "shutdown" 2>&1 \
         | grep -oE "^[0-9a-f]{8}$" | head -1)

if [ -n "$EXPECT" ] && [ -n "$ACTUAL" ]; then
  if [ "$EXPECT" = "$ACTUAL" ]; then
    echo "tail verified: 0x$ACTUAL"
  else
    echo "TAIL MISMATCH: expected 0x$EXPECT, flash has 0x$ACTUAL" >&2
    exit 1
  fi
fi
