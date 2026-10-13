#!/usr/bin/env bash
# 拉取 pinned 的构建依赖（本地与 CI 共用；幂等，已存在则跳过）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP="$ROOT/third_party"

LSPLANT_REPO="https://github.com/LSPosed/LSPlant.git"
LSPLANT_SHA="7217ac6f41e2bda549e4acb54e632abb03e5ccaf"     # v6.4（2024-04，最后一代传统 C++ 结构；master 已改 C++20 modules）
DEXBUILDER_REPO="https://github.com/LSPosed/DexBuilder.git"
DEXBUILDER_SHA="9d57844a301077abf4c29e061b2458c56a363c8f"  # LSPlant v6.4 子模块 pin
PHMAP_SHA="65775fa09fecaa65d0b0022ab6bf091c0e509445"       # DexBuilder v6.4 pin 的子模块
DOBBY_AAR="https://repo1.maven.org/maven2/io/github/vvb2060/ndk/dobby/1.2/dobby-1.2.aar"
# 另有两处不走此脚本：
# - native/src/elfimg/（ElfImg + XZ Embedded）：直接 vendored 在仓库里
#   来源：https://github.com/Aliucord/LSPlant @ aliucord 分支 test/src/main/jni（GPL-3.0 / public domain）
#   —— 2024-08 起 ART 主线更新将内部符号移入 .gnu_debugdata，必须用支持该段的解析器
fetch() { # repo sha dest
  local repo=$1 sha=$2 dest=$3
  if [ -d "$dest/.git" ]; then
    echo "== $dest exists, skip"
    return
  fi
  echo "== cloning $repo @ ${sha:0:8}"
  git init -q "$dest"
  git -C "$dest" remote add origin "$repo"
  git -C "$dest" fetch -q --depth 1 origin "$sha"
  git -C "$dest" checkout -q FETCH_HEAD
}

fetch_tarball() { # repo sha dest  （git fetch 不通时用 codeload tarball，可含 gitlink 内容但不含子模块）
  local repo=$1 sha=$2 dest=$3
  if [ -f "$dest/CMakeLists.txt" ]; then
    echo "== $dest exists, skip"
    return
  fi
  local owner=${repo#https://github.com/} name
  name=${owner%.git}; owner=${name%%/*}; name=${name#*/}
  echo "== tarball $repo @ ${sha:0:8}"
  local tmp; tmp="$(mktemp -d)"
  curl -sL --retry 3 --retry-delay 2 -o "$tmp/src.tar.gz" "https://codeload.github.com/$owner/$name/tar.gz/$sha"
  mkdir -p "$dest"
  tar --force-local -xzf "$tmp/src.tar.gz" -C "$dest" --strip-components=1
  rm -rf "$tmp"
}

fetch "$LSPLANT_REPO" "$LSPLANT_SHA" "$TP/LSPlant"
fetch_tarball "$DEXBUILDER_REPO" "$DEXBUILDER_SHA" "$TP/LSPlant/lsplant/src/main/jni/external/dex_builder"
fetch_tarball "https://github.com/greg7mdp/parallel-hashmap.git" "$PHMAP_SHA" \
  "$TP/LSPlant/lsplant/src/main/jni/external/dex_builder/external/parallel_hashmap"

# aapt（Termux 构建，Apache-2.0）：WebUI 扫描时从 APK 读取真实应用名
# pinned 于 Termux main 仓库 2026-10-13 索引；运行需同名 .so 依赖随行
AAPT_BASE="https://packages.termux.dev/apt/termux-main/pool/main"
AAPT_DEBS="
aapt/aapt_16.0.0.4-3_aarch64.deb
z/zlib/zlib_1.3.2_aarch64.deb
libp/libpng/libpng_1.6.59_aarch64.deb
libe/libexpat/libexpat_2.9.0_aarch64.deb
libc/libc++/libc++_30_aarch64.deb
"

fetch_aapt() {
  local dest="$TP/aapt-prebuilt"
  if [ -f "$dest/aapt" ]; then
    echo "== aapt-prebuilt exists, skip"
    return
  fi
  echo "== fetching aapt + deps (Termux)"
  local work; work="$(mktemp -d)"
  for rel in $AAPT_DEBS; do
    curl -sL --retry 3 --retry-delay 2 -o "$work/$(basename $rel)" "$AAPT_BASE/$rel"
  done
  python "$ROOT/tools/extract_aapt.py" "$work" "$dest"
  rm -rf "$work"
}

fetch_aapt

echo "== deps ready"

# Dobby：从 Maven Central 提取预编译静态库（arm64-v8a）
if [ -f "$TP/dobby-prebuilt/android.arm64-v8a/libdobby.a" ]; then
  echo "== dobby-prebuilt exists, skip"
else
  echo "== fetching $DOBBY_AAR"
  TMPAAR="$(mktemp -d)"
  curl -sL --retry 3 --retry-delay 2 -o "$TMPAAR/dobby.aar" "$DOBBY_AAR"
  mkdir -p "$TP/dobby-prebuilt/android.arm64-v8a"
  unzip -o -q "$TMPAAR/dobby.aar" \
    "prefab/modules/dobby/include/dobby.h" \
    "prefab/modules/dobby/libs/android.arm64-v8a/libdobby.a" \
    -d "$TMPAAR/x"
  cp "$TMPAAR/x/prefab/modules/dobby/include/dobby.h" "$TP/dobby-prebuilt/"
  cp "$TMPAAR/x/prefab/modules/dobby/libs/android.arm64-v8a/libdobby.a" "$TP/dobby-prebuilt/android.arm64-v8a/"
  rm -rf "$TMPAAR"
fi

echo "== deps ready"
