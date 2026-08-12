#!/usr/bin/env python3
# 纯标准库：32bpp BMP -> PNG（用于在不装 Pillow 的环境查看字形自测图）
import sys, struct, zlib

def bmp_to_png(src, dst):
    with open(src, "rb") as f:
        data = f.read()
    # BITMAPFILEHEADER: 14 bytes. 后面 BITMAPINFOHEADER 从偏移14开始。
    bfSize = struct.unpack_from("<I", data, 2)[0]
    # 信息头
    biSize = struct.unpack_from("<I", data, 14)[0]
    w = struct.unpack_from("<i", data, 18)[0]
    h = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    assert bpp == 32, f"only 32bpp supported, got {bpp}"
    # 顶/底向上：height 为正=底向上（先存最后一行）
    topdown = h < 0
    h = abs(h)
    stride = (w * 4 + 3) & ~3
    off = 14 + biSize
    # 逐行读（自底向上存，需要翻转）
    rows = []
    for y in range(h):
        file_row = (h - 1 - y) if not topdown else y
        base = off + file_row * stride
        rows.append(data[base:base + w*4])
    # 构造 PNG：每扫描行前加 filter byte 0，像素 RGBA
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for x in range(w):
            b = row[x*4+0]; g = row[x*4+1]; r = row[x*4+2]
            raw += bytes((r, g, b, 255))  # RGBA：IHDR 是 color type 6，必须 4 字节/像素
    # PNG 块
    def chunk(typ, body):
        c = struct.pack(">I", len(body)) + typ + body
        crc = zlib.crc32(typ + body) & 0xffffffff
        return c + struct.pack(">I", crc)
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)  # 8-bit, RGBA
    idat = zlib.compress(bytes(raw), 9)
    png = sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")
    with open(dst, "wb") as f:
        f.write(png)
    print(f"wrote {dst} ({w}x{h}, {len(png)} bytes)")

if __name__ == "__main__":
    bmp_to_png(sys.argv[1], sys.argv[2])
