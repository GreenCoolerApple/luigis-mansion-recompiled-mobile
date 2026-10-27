#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${LM_ANDROID_BUILD_DIR:-$ROOT/build/android-arm64}"
NDK="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-}}"
VARIANT="${LM_ANDROID_VARIANT:-release}"

if [ -z "$NDK" ] || [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
    echo "Set ANDROID_NDK_ROOT to an Android NDK (r27 or newer)." >&2
    exit 1
fi

VERSION="$(sed -n 's/^project(lmport LANGUAGES C CXX VERSION \([0-9.]*\)).*/\1/p' "$ROOT/CMakeLists.txt")"
VERSION_CODE="$(echo "$VERSION" | awk -F. '{ printf "%d", $1 * 10000 + $2 * 100 + $3 }')"

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-29 \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DLM_HOST_CXX="${LM_HOST_CXX:-c++}"
cmake --build "$BUILD" --target LuigisMansionRecomp

cache_value() {
    sed -n "s|^$1:[A-Z]*=||p" "$BUILD/CMakeCache.txt" | head -n 1
}
SDL_JAVA="$(cache_value AURORA_SDL3_JAVA_SOURCE_DIR)"
AURORA_JAVA="$(cache_value AURORA_ANDROID_JAVA_SOURCE_DIR)"
if [ ! -d "$SDL_JAVA" ] || [ ! -d "$AURORA_JAVA" ]; then
    echo "SDL or Aurora Java sources not found in $BUILD/CMakeCache.txt" >&2
    exit 1
fi

APP="$ROOT/packaging/android/app"
rm -rf "$APP/jniLibs" "$APP/sdl-java" "$APP/aurora-java" "$APP/assets"
mkdir -p "$APP/jniLibs/arm64-v8a" "$APP/assets"
cp "$BUILD/libmain.so" "$APP/jniLibs/arm64-v8a/libmain.so"
"$NDK/toolchains/llvm/prebuilt/"*/bin/llvm-strip --strip-debug "$APP/jniLibs/arm64-v8a/libmain.so" || true
cp -R "$SDL_JAVA" "$APP/sdl-java"
cp -R "$AURORA_JAVA" "$APP/aurora-java"
find "$APP/aurora-java" -type f ! -name '*.java' -delete
cp "$ROOT/data/pipelines.bin" "$APP/assets/pipelines.bin"

GRADLE="${LM_GRADLE:-gradle}"
TASK="assemble$(echo "${VARIANT:0:1}" | tr '[:lower:]' '[:upper:]')${VARIANT:1}"
(cd "$ROOT/packaging/android" && "$GRADLE" --no-daemon "$TASK" -PlmVersion="$VERSION" -PlmVersionCode="$VERSION_CODE")

OUT="$ROOT/build/LuigisMansionRecompiled-$VERSION-android-arm64.apk"
cp "$ROOT/packaging/android/app/build/outputs/apk/$VARIANT/app-$VARIANT.apk" "$OUT"
echo "Built $OUT"
