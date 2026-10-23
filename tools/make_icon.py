#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_icon.py — 生成 src/app.ico（多尺寸 32bpp 图标）

设计：品牌红 #C9242B 圆角方块 + 白色 "B64" 字样。
大尺寸显示 B64，小尺寸（<48px）退化为 "64" 以保证可辨认。

用法：
    python3 tools/make_icon.py            # 输出 src/app.ico
"""
import os
import struct
import sys

BRAND_RED = (0xC9, 0x24, 0x2B)
WHITE = (0xFF, 0xFF, 0xFF)
SIZES = [16, 24, 32, 48, 64, 128, 256]
SS = 4  # 每个像素的超采样边长（抗锯齿）

# 5x7 点阵字形
GLYPHS = {
    "B": ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    "6": ["01110", "10000", "10000", "11110", "10001", "10001", "01110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
}


def rounded_rect_hit(px, py, size, radius):
    """点 (px,py) 是否落在圆角方块内（坐标以像素为单位，可为小数）"""
    if px < 0 or py < 0 or px > size or py > size:
        return False

    right = size - radius
    bottom = size - radius
    # 四个被切掉的角
    if px < radius and py < radius:
        return (px - radius) ** 2 + (py - radius) ** 2 <= radius * radius
    if px > right and py < radius:
        return (px - right) ** 2 + (py - radius) ** 2 <= radius * radius
    if px < radius and py > bottom:
        return (px - radius) ** 2 + (py - bottom) ** 2 <= radius * radius
    if px > right and py > bottom:
        return (px - right) ** 2 + (py - bottom) ** 2 <= radius * radius
    return True


def build_text_mask(size):
    """返回 (text, tx, ty, scale)，text 为点阵行列表"""
    text = "B64" if size >= 48 else "64"
    gw, gh, gap = 5, 7, 1
    units = gw * len(text) + gap * (len(text) - 1)
    ratio = 0.60 if size >= 48 else 0.72
    scale = max(1, int(round(size * ratio / units)))
    while units * scale > size - 2 and scale > 1:
        scale -= 1
    tw, th = units * scale, gh * scale
    tx = (size - tw) // 2
    ty = (size - th) // 2
    return text, tx, ty, scale


def render(size):
    """渲染一个尺寸，返回 (bgra_rows_bottom_up, alpha_rows)"""
    radius = max(1.0, size * 0.18)
    text, tx, ty, scale = build_text_mask(size)

    # 每个输出像素的超采样累积
    pixels = []
    for y in range(size):
        row = []
        for x in range(size):
            r_acc = g_acc = b_acc = a_acc = 0.0
            for sy in range(SS):
                for sx in range(SS):
                    px = x + (sx + 0.5) / SS
                    py = y + (sy + 0.5) / SS
                    if not rounded_rect_hit(px, py, size, radius):
                        continue
                    # 判断是否落在文字点阵上
                    in_text = False
                    gx = int((px - tx) // scale)
                    gy = int((py - ty) // scale)
                    if 0 <= gy < 7 and 0 <= gx < len(text) * 6 - 1:
                        gi, gi_x = divmod(gx, 6)
                        if gi_x < 5:
                            in_text = GLYPHS[text[gi]][gy][gi_x] == "1"
                    c = WHITE if in_text else BRAND_RED
                    r_acc += c[0]
                    g_acc += c[1]
                    b_acc += c[2]
                    a_acc += 1.0
            total = SS * SS
            if a_acc <= 0:
                row.append((0, 0, 0, 0))
            else:
                # 反预乘：颜色取覆盖区域的平均色，alpha 取覆盖率
                row.append((int(b_acc / a_acc + 0.5),
                            int(g_acc / a_acc + 0.5),
                            int(r_acc / a_acc + 0.5),
                            int(a_acc / total * 255 + 0.5)))
        pixels.append(row)

    # BMP 为自下而上
    bgra = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            b, g, r, a = pixels[y][x]
            bgra += bytes((b, g, r, a))

    # AND 掩码：32bpp 有 alpha 通道，掩码全 0
    mask_row_bytes = ((size + 31) // 32) * 4
    mask = bytes(mask_row_bytes * size)

    return bgra, mask


def build_ico(path):
    images = []
    for size in SIZES:
        bgra, mask = render(size)
        header = struct.pack("<IiiHHIIiiII",
                             40,          # biSize
                             size,        # biWidth
                             size * 2,    # biHeight（含掩码）
                             1,           # biPlanes
                             32,          # biBitCount
                             0,           # biCompression
                             len(bgra) + len(mask),
                             0, 0, 0, 0)
        images.append((size, header + bytes(bgra) + mask))

    out = bytearray()
    out += struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for size, data in images:
        w = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in images:
        out += data

    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "src", "app.ico")
    n = build_ico(out)
    print("已生成 %s（%d 字节，尺寸 %s）" % (out, n, SIZES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
