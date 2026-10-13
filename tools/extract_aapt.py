#!/usr/bin/env python3
"""从 Termux .deb（ar + tar.xz）中提取 aapt 二进制及其 .so 依赖。

用法: extract_aapt.py <work_dir 放 deb> <输出目录>
输出:
  <输出目录>/aapt
  <输出目录>/libs/<libz.so.1, libpng16.so, libexpat.so.1, libc++_shared.so>
"""
import io
import lzma
import os
import sys
import tarfile

# 库文件名（deb 内） -> 交付名（与 aapt 的 DT_NEEDED 对齐）
LIB_MAP = {
    "libz.so.1.3.2": "libz.so.1",
    "libz.so.1": "libz.so.1",
    "libpng16.so": "libpng16.so",
    "libexpat.so.1": "libexpat.so.1",
    "libexpat.so.1.13.0": "libexpat.so.1",
    "libc++_shared.so": "libc++_shared.so",
}


def unpack_ar(data: bytes):
    assert data[:8] == b"!<arch>\n", "not a deb"
    pos = 8
    while pos + 60 <= len(data):
        hdr = data[pos:pos + 60]
        name = hdr[0:16].decode().strip()
        size = int(hdr[48:58].decode().strip())
        body = data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size % 2)
        if name.startswith("data.tar"):
            yield body


def extract(deb_path: str, dest: str):
    data = open(deb_path, "rb").read()
    for tar_xz in unpack_ar(data):
        raw = lzma.open(io.BytesIO(tar_xz)).read()
        tf = tarfile.open(fileobj=io.BytesIO(raw))
        for m in tf.getmembers():
            if not m.isfile():
                continue
            base = os.path.basename(m.name)
            target = None
            if base == "aapt":
                target = os.path.join(dest, "aapt")
            elif base in LIB_MAP:
                target = os.path.join(dest, "libs", LIB_MAP[base])
            if target:
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "wb") as f:
                    f.write(tf.extractfile(m).read())
                print(f"extract: {target} ({m.size} bytes)")


def main() -> None:
    work, dest = sys.argv[1], sys.argv[2]
    os.makedirs(os.path.join(dest, "libs"), exist_ok=True)
    for name in sorted(os.listdir(work)):
        if name.endswith(".deb"):
            extract(os.path.join(work, name), dest)
    # 交付名校验
    for f in ("aapt", "libs/libz.so.1", "libs/libpng16.so",
              "libs/libexpat.so.1", "libs/libc++_shared.so"):
        p = os.path.join(dest, f)
        if not os.path.exists(p):
            print(f"ERROR: missing {p}", file=sys.stderr)
            sys.exit(1)
    print("aapt-prebuilt ready")


if __name__ == "__main__":
    main()
