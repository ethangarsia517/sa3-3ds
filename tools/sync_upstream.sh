#!/bin/sh
# Keeps build/upstream (SAT-R/sa3) on the port's current patch.
#
# `git pull` brings a new patches/sa3.patch, but build/upstream still has the
# patch it was bootstrapped with. Run before each build (make prelink does):
# if the patch changed since it was last applied (or was applied by an older
# bootstrap that kept no record), sa3's own files are put back as they are
# at the pinned commit, the current patch is applied, and the game's build
# objects are removed (a patch can change compiler flags). Your ROM and
# anything else not part of sa3's repository stay.
set -e
cd "$(dirname "$0")/.."
UP=build/upstream
STAMP=$UP/.sa3-3ds-patch

if [ ! -d "$UP/.git" ]; then
    echo "Missing $UP: run tools/bootstrap.sh first."
    exit 1
fi

WANT=$(sha1sum patches/sa3.patch | cut -d' ' -f1)
HAVE=$(cat "$STAMP" 2>/dev/null || true)
if [ "$WANT" = "$HAVE" ]; then
    exit 0
fi

echo "patches/sa3.patch changed: applying it to $UP again (and building the game half from clean)"
COMMIT=$(sed -n 's/^commit = "\(.*\)"/\1/p' upstream.lock)
if ! git -C "$UP" cat-file -e "$COMMIT^{commit}" 2>/dev/null; then
    git -C "$UP" fetch --quiet origin "$COMMIT"
fi
git -C "$UP" checkout --quiet --force "$COMMIT"
git -C "$UP" apply ../../patches/sa3.patch
[ -e "$UP/src/platform/n3ds" ] || ln -s ../../../../port/core "$UP/src/platform/n3ds"
rm -rf "$UP/build"
echo "$WANT" > "$STAMP"
