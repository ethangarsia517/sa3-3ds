#!/bin/sh
# Fetches SAT-R/sa3 at the pinned commit into build/upstream, applies the
# port's patch, and links the port's platform core into it
# (build/upstream/src/platform/n3ds -> port/core). Then put your own ROM at
# build/upstream/baserom_sa3.gba and run `make prelink`.
set -e
cd "$(dirname "$0")/.."
COMMIT=$(sed -n 's/^commit = "\(.*\)"/\1/p' upstream.lock)
REPO=$(sed -n 's/^repository = "\(.*\)"/\1/p' upstream.lock)

if [ -d build/upstream/.git ]; then
    echo "build/upstream already exists; delete it to bootstrap again."
    exit 1
fi
mkdir -p build
git clone "$REPO" build/upstream
git -C build/upstream checkout --quiet "$COMMIT"
git -C build/upstream apply ../../patches/sa3.patch
ln -s ../../../../port/core build/upstream/src/platform/n3ds
sha1sum patches/sa3.patch | cut -d' ' -f1 > build/upstream/.sa3-3ds-patch
echo "Done. Copy your ROM to build/upstream/baserom_sa3.gba, then run: make prelink"
