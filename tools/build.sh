#!/usr/bin/env bash
# ClipShield 一键构建：javac -> d8 -> dex 嵌入 -> NDK CMake -> 打包 zip
# 依赖：JDK 11+（javac/java）、Python 3、Android SDK（build-tools + platforms + cmake + ndk）
# 环境变量：
#   ANDROID_SDK_ROOT  SDK 路径（默认 D:/AndroidSDK）
#   ANDROID_NDK_HOME  NDK 路径（默认 $ANDROID_SDK_ROOT/ndk/27.0.12077973）
#   BUILD_TYPE        Release（默认）或 Debug
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-D:/AndroidSDK}}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
ABI="arm64-v8a"

# Git Bash/MSYS 下把 Windows 风格路径转为 POSIX（否则 PATH 中的 "D:" 会被冒号拆坏）
command -v cygpath >/dev/null 2>&1 && {
  SDK="$(cygpath -u "$SDK")"
}

# NDK：优先 ANDROID_NDK_HOME，否则在 SDK 下自动探测（优先 27.x，回退最新）
if [ -z "${ANDROID_NDK_HOME:-}" ] || [ ! -d "${ANDROID_NDK_HOME:-}" ]; then
  NDK="$(ls -d "$SDK"/ndk/27* 2>/dev/null | sort -V | tail -1)"
  [ -n "$NDK" ] || NDK="$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1)"
  [ -z "$NDK" ] && NDK="$SDK/ndk/27.0.12077973"
else
  NDK="$ANDROID_NDK_HOME"
fi

[ -d "$SDK" ] || { echo "ERROR: SDK not found: $SDK (set ANDROID_SDK_ROOT)"; exit 1; }
[ -d "$NDK" ] || { echo "ERROR: NDK not found: $NDK (set ANDROID_NDK_HOME)"; exit 1; }

# 自动挑选最新 build-tools / platform（本地与 CI 通用）
BT="$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)"
PLATFORM_JAR="$(ls "$SDK"/platforms/android-*/android.jar | sort -V | tail -1)"
echo "== SDK:     $SDK"
echo "== NDK:     $NDK"
echo "== build-tools: $BT"
echo "== platform:    $PLATFORM_JAR"

# cmake/ninja 若在 SDK 内则加入 PATH
for c in "$SDK"/cmake/*/bin; do
  [ -d "$c" ] && PATH="$c:$PATH"
done
command -v cmake >/dev/null || { echo "ERROR: cmake not found"; exit 1; }

# ---- 0. 拷贝到无空格的暂存目录（NDK/部分工具链对含空格路径敏感） ----
# Windows(Git Bash) 下用项目同盘的无空格目录，且取 cygpath -m 混合形式
# （"E:/..." 对 Windows 原生工具 javac/d8/cmake 与 bash 均可用）；Linux 直接用 /tmp
if command -v cygpath >/dev/null 2>&1; then
  STAGE="$(dirname "$(cygpath -m "$ROOT")")/clipshield-build"
else
  STAGE="${TMPDIR:-/tmp}/clipshield-build"
fi
rm -rf "$STAGE"
mkdir -p "$STAGE/src"
tar -C "$ROOT" --exclude=.git --exclude=build --exclude=dist --exclude=.cxx \
    --exclude='third_party/*/build' --exclude='third_party/aapt-prebuilt/work' \
    -cf - . | tar -C "$STAGE/src" -xf -
echo "== staged to $STAGE"

# ---- 1. Java hooker -> classes.dex ----
JAVA_OBJ="$STAGE/java-obj"; DEX_OUT="$STAGE/dex-out"
mkdir -p "$JAVA_OBJ" "$DEX_OUT"
find "$STAGE/src/java" -name '*.java' > "$STAGE/sources.txt"
javac -source 11 -target 11 -Xlint:-options -encoding UTF-8 \
      -cp "$PLATFORM_JAR" -d "$JAVA_OBJ" @"$STAGE/sources.txt"
mapfile -t CLASSES < <(find "$JAVA_OBJ" -name '*.class')
D8="$BT/d8"
[ -e "$D8" ] || D8="$BT/d8.bat"   # Windows build-tools 只有 d8.bat
"$D8" --release --lib "$PLATFORM_JAR" --min-api 33 \
      --output "$DEX_OUT" "${CLASSES[@]}"
[ -f "$DEX_OUT/classes.dex" ] || { echo "ERROR: d8 produced no dex"; exit 1; }
echo "== dex built: $(wc -c < "$DEX_OUT/classes.dex") bytes"

# 注意：不要尝试 app_process + 自定义 CLASSPATH 方案（ListApps 取应用名）。
# 实测 HyperOS Android 16 上会导致 app_process/系统应用 native 崩溃甚至重启。

# ---- 2. dex 嵌入为 C++ 头 ----
python "$ROOT/tools/gen_dex_header.py" \
       "$DEX_OUT/classes.dex" "$STAGE/src/native/src/embed/dex_bytes.h"

# ---- 3. NDK CMake 构建 ----
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
[ -f "$TOOLCHAIN" ] || { echo "ERROR: toolchain missing: $TOOLCHAIN"; exit 1; }
cmake -G Ninja -S "$STAGE/src" -B "$STAGE/build" \
      -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
      -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-33 \
      -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$STAGE/build" -j "$(nproc 2>/dev/null || echo 4)"

SO="$STAGE/build/libclipshield.so"
[ -f "$SO" ] || { echo "ERROR: libclipshield.so not produced"; exit 1; }

# strip
HOST_TAG="$(ls "$NDK/toolchains/llvm/prebuilt" | head -1)"
STRIP="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin/llvm-strip"
[ -x "$STRIP" ] || STRIP="$(command -v llvm-strip || true)"
if [ -n "${STRIP:-}" ]; then "$STRIP" --strip-all "$SO"; fi
echo "== .so built: $(wc -c < "$SO") bytes"

# ---- 4. 打包模块 zip ----
# 内置 aapt（读真实应用名）及其 .so 依赖
if [ -f "$ROOT/third_party/aapt-prebuilt/aapt" ]; then
  mkdir -p "$STAGE/src/module/bin/libs"
  cp "$ROOT/third_party/aapt-prebuilt/aapt" "$STAGE/src/module/bin/aapt"
  cp "$ROOT"/third_party/aapt-prebuilt/libs/*.so* "$STAGE/src/module/bin/libs/"
else
  echo "WARN: aapt-prebuilt 缺失（跳过），WebUI 将只显示包名（先跑 tools/fetch_deps.sh）"
fi
mkdir -p "$STAGE/src/module/zygisk"
cp "$SO" "$STAGE/src/module/zygisk/$ABI.so"
mkdir -p "$ROOT/dist"
VERSION="$(grep '^version=' "$ROOT/module/module.prop" | cut -d= -f2)"
VERSION_CODE="$(grep '^versionCode=' "$ROOT/module/module.prop" | cut -d= -f2)"
ZIP_NAME="clipshield-${VERSION_CODE}-${VERSION}.zip"
python - "$ROOT/dist/$ZIP_NAME" "$STAGE/src/module" <<'PY'
import os, sys, zipfile
out, module_dir = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for base, _, files in os.walk(module_dir):
        for f in sorted(files):
            if f == ".gitkeep":
                continue
            full = os.path.join(base, f)
            rel = os.path.relpath(full, module_dir).replace("\\", "/")
            z.write(full, rel)
print("zip:", out)
PY

echo ""
echo "== DONE: $ROOT/dist/$ZIP_NAME"
