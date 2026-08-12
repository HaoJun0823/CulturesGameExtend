#!/usr/bin/env python3
# 纯标准库读取 TTF 的 name 表（字体族名/全名），用于确认 plugins/fonts 下各 ttf 身份
import sys, struct

def read_name(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 12:
        return {}
    num = struct.unpack_from(">H", data, 4)[0]
    name_off = None
    for i in range(num):
        base = 12 + i * 16
        tag = data[base:base+4]
        if tag == b"name":
            name_off = struct.unpack_from(">I", data, base+8)[0]
            break
    if name_off is None:
        return {}
    fmt, count, str_off = struct.unpack_from(">HHH", data, name_off)
    records = []
    so = name_off + str_off
    for i in range(count):
        rb = name_off + 6 + i * 12
        pid, eid, lid, nid, length, noff = struct.unpack_from(">HHHHHH", data, rb)
        start = so + noff
        raw = data[start:start+length]
        if pid == 3 or pid == 0:  # UTF-16BE (Microsoft/Unicode)
            try:
                s = raw.decode("utf-16-be")
            except Exception:
                s = raw.decode("latin-1", "ignore")
        else:
            s = raw.decode("latin-1", "ignore")
        records.append((nid, s))
    out = {}
    for nid, s in records:
        if nid in (1, 4, 6) and nid not in out:
            out[nid] = s
    return out

if __name__ == "__main__":
    import os
    d = r"G:/Projects/CulturesGameExtend/Resource/plugins/fonts"
    for fn in sorted(os.listdir(d)):
        if fn.lower().endswith(".ttf"):
            names = read_name(os.path.join(d, fn))
            fam = names.get(1, "?")
            full = names.get(4, "?")
            print(f"{fn:10s} family={fam!r}  full={full!r}")
