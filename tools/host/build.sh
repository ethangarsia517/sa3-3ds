#!/bin/sh
# Builds the headless PC runner (build/sa3host) for testing on a Linux x86-64
# machine: the game half is compiled for the PC with the 3DS port's settings,
# then linked with tools/host/host_main.c. Needs `make prelink` to have set up
# build/upstream with the ROM first. Uses its own copy (build/host-upstream).
#
#   tools/host/build.sh
#   printf '250 press START\n480 shot menu\n482 quit\n' > s.txt
#   build/sa3host s.txt          # writes host-out/menu.ppm
#
# Script lines: <frame> press <KEYS+> [n] | <frame> shot <name> |
#               <frame> dump <name> | <frame> quit
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=$ROOT/build/upstream
DST=$ROOT/build/host-upstream
[ -f "$SRC/Makefile" ] || { echo "run 'make prelink' first"; exit 1; }
WANT=$(sed -n 's/^sha1 = "\(.*\)"/\1/p' "$ROOT/upstream.lock")
HAVE=$(sha1sum "$SRC/baserom_sa3.gba" | cut -d' ' -f1)
[ "$WANT" = "$HAVE" ] || { echo "build/upstream/baserom_sa3.gba has sha1 $HAVE, expected $WANT"; exit 1; }
mkdir -p "$DST"
# Copy sources (not build outputs) and keep the symlink to port/core.
(cd "$SRC" && tar --exclude=./build --exclude=./.git -cf - .) | (cd "$DST" && tar -xf -)
rm -rf "$DST/src/platform/n3ds"
ln -s "$ROOT/port/core" "$DST/src/platform/n3ds"
cd "$DST"
make PLATFORM=3ds PREFIX= PORT_TOOLS="$ROOT/tools" ASFLAGS= \
  CC1FLAGS="-Wimplicit -Werror -Wstrict-overflow=1 -ffunction-sections -fdata-sections -fno-strict-aliasing -fwrapv -fno-common -fsigned-char -fno-pie -Wno-parentheses -Wno-unused-value -Wno-stringop-overflow -Wno-format-security -x c -S -O2 -g" \
  -j"$(nproc)" "$@"
gcc -O2 -g -fno-pie -no-pie -o "$ROOT/build/sa3host" "$ROOT/tools/host/host_main.c" "$DST/sa3-game.o" -lm
echo "built $ROOT/build/sa3host"
