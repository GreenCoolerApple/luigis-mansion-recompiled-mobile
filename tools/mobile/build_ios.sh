#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${LM_IOS_BUILD_DIR:-$ROOT/build/ios-arm64}"

VERSION="$(sed -n 's/^project(lmport LANGUAGES C CXX VERSION \([0-9.]*\)).*/\1/p' "$ROOT/CMakeLists.txt")"

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DCMAKE_OSX_SYSROOT=iphoneos \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" \
    -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON \
    -DLM_HOST_CXX="${LM_HOST_CXX:-/usr/bin/clang++}" \
    ${LM_CMAKE_ARGS:-}
cmake --build "$BUILD" --target LuigisMansionRecomp

APP="$(find "$BUILD" -maxdepth 2 -name 'LuigisMansionRecomp.app' -type d | head -n 1)"
if [ -z "$APP" ]; then
    echo "LuigisMansionRecomp.app not found in $BUILD" >&2
    exit 1
fi
mkdir -p "$APP/Frameworks"
cp "$ROOT/data/pipelines.bin" "$APP/pipelines.bin"
strip -S "$APP/LuigisMansionRecomp" || true

STAGE="$BUILD/ipa"
rm -rf "$STAGE"
mkdir -p "$STAGE/Payload"
cp -R "$APP" "$STAGE/Payload/"
OUT="$ROOT/build/LuigisMansionRecompiled-$VERSION-ios-arm64-unsigned.ipa"
rm -f "$OUT"
(cd "$STAGE" && zip -qry "$OUT" Payload)
echo "Built $OUT"
