#!/usr/bin/env python3
"""纯 Python（无第三方依赖）生成模块图标 PNG：深色圆角底 + 白色剪贴板 + 青色盾牌对勾。"""
import struct
import sys
import zlib

W = H = 512


def in_rounded_rect(x, y, x0, y0, x1, y1, r):
    if not (x0 <= x < x1 and y0 <= y < y1):
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def dist_to_segment(px, py, ax, ay, bx, by):
    vx, vy = bx - ax, by - ay
    wx, wy = px - ax, py - ay
    t = (vx * wx + vy * wy) / (vx * vx + vy * vy)
    t = max(0.0, min(1.0, t))
    dx = (ax + t * vx) - px
    dy = (ay + t * vy) - py
    return (dx * dx + dy * dy) ** 0.5


def in_clipboard(x, y):
    if in_rounded_rect(x, y, 150, 118, 362, 402, 22):
        return True
    if in_rounded_rect(x, y, 216, 100, 296, 140, 12):  # 顶部夹子
        return True
    return False


def in_shield(x, y):
    cx, cy, r = 322, 310, 96
    if (x - cx) ** 2 + (y - cy) ** 2 <= r * r and y <= cy:
        return True
    if cy < y <= cy + 100:  # 下半收窄
        half = r * (1 - (y - cy) / 118.0)
        return abs(x - cx) <= half
    return False


def in_clip_lines(x, y):
    for ly0, lw in ((190, 10), (222, 10), (254, 10)):
        if 186 <= x <= 326 and ly0 <= y < ly0 + lw:
            return True
    return False


def in_check(x, y):
    # 盾牌内白色对勾：两段线，线宽 12
    if dist_to_segment(x, y, 286, 312, 312, 338) <= 6.5:
        return True
    if dist_to_segment(x, y, 312, 338, 358, 288) <= 6.5:
        return True
    return False


def pixel(x, y):
    if not in_rounded_rect(x, y, 16, 16, W - 16, H - 16, 96):
        return (0, 0, 0, 0)
    if in_shield(x, y):
        if in_check(x, y):
            return (255, 255, 255, 255)
        return (20, 184, 166, 255)  # teal
    if in_clipboard(x, y):
        if in_clip_lines(x, y):
            return (148, 163, 184, 255)  # 灰蓝线条
        return (241, 245, 249, 255)  # 近白
    return (15, 23, 42, 255)  # slate-900


def main() -> None:
    rows = []
    for y in range(H):
        row = bytearray([0])  # filter type 0
        for x in range(W):
            row.extend(pixel(x, y))
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(
            ">I", zlib.crc32(tag + data) & 0xFFFFFFFF
        )

    ihdr = struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )
    out = sys.argv[1] if len(sys.argv) > 1 else "assets/icon.png"
    with open(out, "wb") as f:
        f.write(png)
    print(f"icon written: {out} ({len(png)} bytes)")


if __name__ == "__main__":
    main()
