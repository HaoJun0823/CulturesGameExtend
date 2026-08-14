#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Merge bilingual comments: keep original Chinese + inject English/#pragma region from src_en.
Code lines (identical between original and src_en) are emitted unchanged -> guarantees code integrity.
"""
import os, re, glob

ROOT = r"G:/Projects/CulturesGameExtend"

def map_srcen(orig):
    # original -> src_en counterpart (normalize separators)
    o = orig.replace("\\", "/")
    ROOTn = ROOT.replace("\\", "/")
    for pref, rep in [
        (ROOTn + "/CulturesGameExtend", ROOTn + "/src_en"),
        (ROOTn + "/CulturesProxyDLL", ROOTn + "/src_en/CulturesProxyDLL"),
        (ROOTn + "/tools", ROOTn + "/src_en/tools"),
    ]:
        if o.startswith(pref):
            return (o.replace(pref, rep, 1)).replace("/", "\\")
    return None

def tokenize(path):
    lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    tokens = []  # ('noncode', [raw...]) or ('code', text)
    buf = []
    in_block = False
    for raw in lines:
        s = raw.strip()
        low = s.lower()
        if low.startswith("#pragma") and ("region" in low):
            buf.append(raw); continue
        if in_block:
            buf.append(raw)
            if "*/" in raw: in_block = False
            continue
        if s.startswith("/*"):
            buf.append(raw)
            if "*/" not in raw: in_block = True
            continue
        if s.startswith("//"):
            buf.append(raw); continue
        if "//" in raw:
            idx = raw.index("//")
            code_part = raw[:idx]
            if code_part.strip() == "":
                buf.append(raw)
            else:
                tokens.append(("noncode", buf)); buf = []
                tokens.append(("code", code_part.rstrip()))
                tokens.append(("noncode", ["//" + raw[idx+2:]]))
            continue
        if s == "":
            buf.append(raw); continue
        tokens.append(("noncode", buf)); buf = []
        tokens.append(("code", raw.rstrip()))
    tokens.append(("noncode", buf))
    C = [t[1] for t in tokens if t[0] == "code"]
    N = [t[1] for t in tokens if t[0] == "noncode"]
    return C, N

def merge(orig_txt, en_txt):
    C, N = tokenize_str(orig_txt)
    _, Ne = tokenize_str(en_txt)
    if len(Ne) != len(N):
        # structural mismatch (shouldn't happen) -> fallback: just concat noncode blocks
        pass
    out = []
    m = len(C)
    for i in range(m + 1):
        out += N[i]
        if i < len(Ne):
            out += Ne[i]
        if i < m:
            out.append(C[i])
    return "\n".join(out)

def main():
    files = []
    for ext in ("*.cpp", "*.h"):
        files += glob.glob(os.path.join(ROOT, "CulturesGameExtend", "**", ext), recursive=True)
        files += glob.glob(os.path.join(ROOT, "CulturesProxyDLL", "**", ext), recursive=True)
        files += glob.glob(os.path.join(ROOT, "tools", "_rev", ext), recursive=True)
    files = [f for f in files if "/src_en/" not in f.replace("\\", "/")]
    done = 0
    for f in sorted(files):
        en = map_srcen(f)
        if not en or not os.path.exists(en):
            print("SKIP (no src_en):", f); continue
        orig_txt = open(f, encoding="utf-8", errors="replace").read()
        en_txt = open(en, encoding="utf-8", errors="replace").read()
        merged = merge(orig_txt, en_txt)
        # safety: code lines must equal original code lines
        C, N = tokenize(f)
        Cm, Nm = tokenize_str(merged)
        if C != Cm:
            print("CODE MISMATCH:", f); continue
        open(f, "w", encoding="utf-8").write(merged)
        done += 1
    print(f"Merged {done} files.")

def tokenize_str(txt):
    # reuse tokenize via temp
    import tempfile, os
    p = os.path.join(tempfile.gettempdir(), "_mg_tmp.txt")
    open(p, "w", encoding="utf-8").write(txt)
    r = tokenize(p)
    os.remove(p)
    return r

if __name__ == "__main__":
    main()
