// GdiFont.cpp
// 见 GdiFont.h 的设计说明。本文件实现 GDI 光栅化与混合。
//
// 字形光栅化采用 DIB + TextOutW 路线（而非 GetGlyphOutline）：在无显示会话
// （服务/沙盒）里 GetGlyphOutline 的灰度缓冲格式容易崩溃，而 TextOut 到内存
// DIB 是最稳健、兼容性最好的取字方式，同样"从 TTF 动态生成到内存"。
#include "pch.h"
#include "GdiFont.h"
#include <cstring>
#include <cstdio>

namespace ge {
namespace text {

namespace {

inline uint16_t To565(uint32_t c) {
    uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
inline void From565(uint16_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = (uint8_t)(((v >> 11) & 0x1F) << 3);
    g = (uint8_t)(((v >> 5)  & 0x3F) << 2);
    b = (uint8_t)((v & 0x1F) << 3);
}

} // namespace

GdiFontRasterizer::~GdiFontRasterizer() {
    Reset();
}

void GdiFontRasterizer::Reset() {
    if (m_hbmp)  { if (m_hdc) SelectObject(m_hdc, GetStockObject(SYSTEM_FONT)); DeleteObject(m_hbmp); m_hbmp = nullptr; }
    if (m_hfont) { DeleteObject(m_hfont); m_hfont = nullptr; }
    if (m_hdc)   { DeleteDC(m_hdc); m_hdc = nullptr; }
    if (m_fontRes && !m_fontFile.empty()) {
        RemoveFontResourceExW(m_fontFile.c_str(), FR_PRIVATE, 0);
    }
    m_fontRes = 0; m_fontFile.clear();
    m_bits = nullptr; m_height = 0; m_ascent = 0; m_descent = 0;
    m_cellW = m_cellH = m_baseY = 0;
    m_cache.clear();
}

// 读 TTF name 表，取族名（nameID 1/4/6）。失败返回空串。
// 仅依赖 windows.h，纯手工解析 TrueType 偏移表 + name 表。
std::wstring GdiFontRasterizer::ReadTtfFamilyName(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    DWORD sz = GetFileSize(h, nullptr);
    if (sz < 12) { CloseHandle(h); return {}; }
    std::vector<uint8_t> buf(sz);
    DWORD rd = 0;
    ReadFile(h, buf.data(), sz, &rd, nullptr);
    CloseHandle(h);
    if (rd < 12) return {};

    uint16_t numTables = (uint16_t)((buf[4] << 8) | buf[5]);
    uint32_t nameOff = 0;
    for (uint16_t i = 0; i < numTables; ++i) {
        uint32_t base = 12u + (uint32_t)i * 16u;
        if (buf[base] == 'n' && buf[base+1] == 'a' && buf[base+2] == 'm' && buf[base+3] == 'e') {
            nameOff = ((uint32_t)buf[base+8] << 24) | ((uint32_t)buf[base+9] << 16) |
                      ((uint32_t)buf[base+10] << 8) | (uint32_t)buf[base+11];
            break;
        }
    }
    if (!nameOff || nameOff + 6 > buf.size()) return {};

    uint16_t count  = (uint16_t)((buf[nameOff+2] << 8) | buf[nameOff+3]);
    uint16_t strOff = (uint16_t)((buf[nameOff+4] << 8) | buf[nameOff+5]);
    uint32_t so = nameOff + strOff;
    std::wstring best;
    for (uint16_t i = 0; i < count; ++i) {
        uint32_t rb = nameOff + 6u + (uint32_t)i * 12u;
        if (rb + 12 > buf.size()) break;
        uint16_t pid = (uint16_t)((buf[rb] << 8) | buf[rb+1]);
        uint16_t nid = (uint16_t)((buf[rb+6] << 8) | buf[rb+7]);
        uint16_t len = (uint16_t)((buf[rb+8] << 8) | buf[rb+9]);
        uint16_t noff= (uint16_t)((buf[rb+10] << 8) | buf[rb+11]);
        if (nid != 1 && nid != 4 && nid != 6) continue;
        bool okPid = (pid == 0 || pid == 3 || pid == 1);
        if (!okPid) continue;
        uint32_t start = so + noff;
        if (start + len > buf.size()) continue;
        std::wstring s;
        if (pid == 0 || pid == 3) {
            if (len % 2) continue;
            s.resize(len / 2);
            for (uint16_t k = 0; k < len / 2; ++k)
                s[k] = (wchar_t)((buf[start + 2*k] << 8) | buf[start + 2*k + 1]);
        } else { // pid == 1 (Mac, ASCII/Latin)
            s.resize(len);
            for (uint16_t k = 0; k < len; ++k) s[k] = (wchar_t)buf[start + k];
        }
        if (s.empty()) continue;
        if (nid == 1) { best = s; break; }       // 族名优先
        if (best.empty()) best = s;
    }
    return best;
}

bool GdiFontRasterizer::CreateFromFile(const wchar_t* filePath, int heightPx, int weight, bool italic,
                                       bool antiAlias) {
    if (m_hdc) return false;
    // FR_PRIVATE：仅本进程可见，不写注册表、不需管理员权限；卸载用 RemoveFontResourceEx。
    m_fontRes = AddFontResourceExW(filePath, FR_PRIVATE, 0);
    std::wstring face = ReadTtfFamilyName(filePath);
    if (face.empty()) {
        // 读不到族名：无法按文件建字体（文件损坏/非 TTF）
        if (m_fontRes) { RemoveFontResourceExW(filePath, FR_PRIVATE, 0); m_fontRes = 0; }
        return false;
    }
    m_fontFile = filePath;
    bool ok = Create(face.c_str(), heightPx, weight, italic, antiAlias);
    if (!ok && m_fontRes) { RemoveFontResourceExW(filePath, FR_PRIVATE, 0); m_fontRes = 0; m_fontFile.clear(); }
    return ok;
}

bool GdiFontRasterizer::Create(const wchar_t* fontName, int heightPx, int weight, bool italic,
                               bool antiAlias) {
    if (m_hdc) return false;
    m_hdc = CreateCompatibleDC(nullptr);
    if (!m_hdc) return false;

    // ★ 10:3x 抗锯齿开关：ANTIALIASED=灰度抗锯齿(平滑/发虚)；NONANTIALIASED=硬边(清晰/锯齿)
    int quality = antiAlias ? ANTIALIASED_QUALITY : NONANTIALIASED_QUALITY;
    m_hfont = CreateFontW(-heightPx, 0, 0, 0, weight, italic ? 1 : 0, 0, 0,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          quality, DEFAULT_PITCH | FF_DONTCARE, fontName);
    if (!m_hfont) { DeleteDC(m_hdc); m_hdc = nullptr; return false; }
    SelectObject(m_hdc, m_hfont);

    m_height = heightPx;
    TEXTMETRICW tm = {};
    if (GetTextMetricsW(m_hdc, &tm)) { m_ascent = tm.tmAscent; m_descent = tm.tmDescent; }
    else { m_ascent = heightPx * 4 / 5; m_descent = heightPx / 5; }

    // 内存 DIB（32bpp，自上而下）作为逐字形渲染画布。
    // 关键：用 TA_BASELINE 对齐，把 (penX, baseY) 当作"基线"落点；
    // 画布高度必须覆盖基线上方(ascent)与下方(descent)，否则字形会被裁掉。
    m_cellW = heightPx * 2 + 6;     // 宽：覆盖全宽 CJK(~height) + 余量
    m_cellH = m_ascent + m_descent + 6; // 高：ascent+descent + 上下各 3 余量
    m_baseY = m_ascent + 3;         // 基线在 DIB 中的 y（顶部留 3 余量）
    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi);
    bi.biWidth = m_cellW;
    bi.biHeight = -m_cellH;         // 负 = 自上而下
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    m_hbmp = CreateDIBSection(m_hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, (void**)&m_bits, nullptr, 0);
    if (!m_hbmp) { DeleteDC(m_hdc); m_hdc = nullptr; DeleteObject(m_hfont); m_hfont = nullptr; return false; }
    SelectObject(m_hdc, m_hbmp);
    SetTextAlign(m_hdc, TA_LEFT | TA_BASELINE); // 落点 (x,y) = 字形基线
    SetBkMode(m_hdc, OPAQUE);
    SetBkColor(m_hdc, RGB(255, 255, 255));
    SetTextColor(m_hdc, RGB(0, 0, 0));
    return true;
}

const Glyph* GdiFontRasterizer::GetGlyph(uint32_t codepoint) {
    auto it = m_cache.find(codepoint);
    if (it != m_cache.end()) return &it->second;
    Glyph g;
    if (!Rasterize(codepoint, g)) {
        g.w = g.h = g.pitch = g.originX = g.originY = g.advance = 0;
    }
    auto res = m_cache.emplace(codepoint, std::move(g));
    return &res.first->second;
}

bool GdiFontRasterizer::Rasterize(uint32_t cp, Glyph& out) {
    if (!m_hdc || !m_bits) return false;
    // 清白底
    memset(m_bits, 0xFF, (size_t)m_cellW * m_cellH * 4);
    // 画黑字
    wchar_t wc = (wchar_t)cp;
    TextOutW(m_hdc, 3, m_baseY, &wc, 1);
    // 量宽高（逻辑步进）
    SIZE sz = {};
    GetTextExtentPoint32W(m_hdc, &wc, 1, &sz);

    // 扫描墨迹 bbox + 反算覆盖度
    int minX = m_cellW, minY = m_cellH, maxX = -1, maxY = -1;
    for (int y = 0; y < m_cellH; ++y) {
        const uint8_t* row = m_bits + (size_t)y * m_cellW * 4;
        for (int x = 0; x < m_cellW; ++x) {
            const uint8_t* p = row + x * 4;
            // 接近白（覆盖<阈值）视为空白
            if (p[0] < 250 || p[1] < 250 || p[2] < 250) {
                if (x < minX) minX = x;
                if (y < minY) minY = y;
                if (x > maxX) maxX = x;
                if (y > maxY) maxY = y;
            }
        }
    }
    if (maxX < 0) {
        // 纯空白字形（空格类）：给合法 advance，零尺寸
        out.w = out.h = out.pitch = 0;
        out.originX = 0; out.originY = 0; out.advance = (int)sz.cx;
        return true;
    }
    int bw = maxX - minX + 1, bh = maxY - minY + 1;
    out.w = bw; out.h = bh; out.pitch = bw;
    out.originX = minX - 3;          // 相对 pen(3, baseY) 的偏移
    out.originY = minY - m_baseY;
    out.advance = (int)sz.cx;
    out.alpha.resize((size_t)bw * bh);
    for (int y = 0; y < bh; ++y) {
        const uint8_t* src = m_bits + (size_t)(minY + y) * m_cellW * 4 + minX * 4;
        uint8_t* dst = out.alpha.data() + (size_t)y * bw;
        for (int x = 0; x < bw; ++x) {
            const uint8_t* p = src + x * 4;
            // 覆盖度 = 255 - 亮度（白=0，黑=255）
            int lum = (p[2] * 77 + p[1] * 150 + p[0] * 29) / 256; // 近似 0.299/0.587/0.114
            dst[x] = (uint8_t)(255 - lum);
        }
    }
    return true;
}

std::vector<uint32_t> GdiFontRasterizer::DecodeUtf8(const char* s, size_t n) {
    std::vector<uint32_t> cps;
    cps.reserve(n);
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp; int len;
        if (c < 0x80)       { cp = c;        len = 1; }
        else if ((c>>5)==6) { cp = c & 0x1F; len = 2; }
        else if ((c>>4)==0xE){ cp = c & 0x0F; len = 3; }
        else if ((c>>3)==0x1E){cp = c & 0x07; len = 4; }
        else { ++i; continue; }
        if (i + (size_t)len > n) { ++i; continue; }
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            unsigned char cc = (unsigned char)s[i+k];
            if ((cc>>6) != 0x2) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (ok) cps.push_back(cp);
        i += (size_t)len;
    }
    return cps;
}

void GdiFontRasterizer::BlitGlyph(uint8_t* fb, int pitch, int bpp,
                                  int dx, int dy, const Glyph* g, uint32_t color,
                                  int fbW, int fbH, bool idempotent,
                                  int clipX, int clipY, int clipW, int clipH) {
    if (!fb || !g || g->w == 0 || g->h == 0) return;
    if (fbW <= 0 || fbH <= 0) return;
    // ★ 00:4x 引擎 UI 框裁剪（原行为 Rect::Intersect）：clipW/H > 0 时字形像素超出该矩形不画
    //   （修垂直列表最后一行溢出——原版/LG4 都按 clip 裁，我们之前只按表面边界裁 → 溢出可见）
    bool doClip = (clipW > 0 && clipH > 0);
    uint8_t tr = (uint8_t)((color >> 16) & 0xFF);
    uint8_t tg = (uint8_t)((color >> 8)  & 0xFF);
    uint8_t tb = (uint8_t)( color        & 0xFF);
    for (int j = 0; j < g->h; ++j) {
        int fy = dy + j;
        if (fy < 0 || fy >= fbH) continue;   // ★ 上下边界裁剪（防越界写）
        if (doClip && (fy < clipY || fy >= clipY + clipH)) continue;   // ★ UI 框上下裁剪
        const uint8_t* srcRow = g->alpha.data() + (size_t)j * g->pitch;
        for (int i = 0; i < g->w; ++i) {
            int fx = dx + i;
            if (fx < 0 || fx >= fbW) continue; // ★ 左右边界裁剪（防越界写）
            if (doClip && (fx < clipX || fx >= clipX + clipW)) continue; // ★ UI 框左右裁剪
            int a = srcRow[i];
            if (a == 0) continue;
            if (bpp == 2) {
                uint16_t* p = (uint16_t*)(fb + (size_t)fy * pitch + (size_t)fx * 2);
                if (idempotent) {
                    // RGB565 通道容差（5/6/5 位 ≈ 8 位值 24/32/24）
                    uint8_t r, gg, b; From565(*p, r, gg, b);
                    int dr = (int)r - tr, dg = (int)gg - tg, db = (int)b - tb;
                    if (dr > -4 && dr < 4 && dg > -5 && dg < 5 && db > -4 && db < 4) continue;
                }
                uint8_t r, gg, b; From565(*p, r, gg, b);
                r = (uint8_t)(r + ((tr - r) * a) / 255);
                gg = (uint8_t)(gg + ((tg - gg) * a) / 255);
                b = (uint8_t)(b + ((tb - b) * a) / 255);
                *p = To565((uint32_t)((r << 16) | (gg << 8) | b));
            } else if (bpp == 4) {
                uint8_t* p = fb + (size_t)fy * pitch + (size_t)fx * 4;
                if (idempotent) {
                    // 32bpp 通道容差 24：抗锯齿中心墨迹≈纯前景色，重绘直接跳过（防累积）
                    int dr = (int)p[2] - tr, dg = (int)p[1] - tg, db = (int)p[0] - tb;
                    if (dr > -24 && dr < 24 && dg > -24 && dg < 24 && db > -24 && db < 24) continue;
                }
                p[0] = (uint8_t)(p[0] + ((tb - p[0]) * a) / 255);
                p[1] = (uint8_t)(p[1] + ((tg - p[1]) * a) / 255);
                p[2] = (uint8_t)(p[2] + ((tr - p[2]) * a) / 255);
                p[3] = 0xFF;
            }
        }
    }
}

bool GdiFontRasterizer::SaveRGBAAsBMP(const char* path, int w, int h, const uint8_t* rgba) {
    int stride = (w * 4 + 3) & ~3;
    int filesize = 54 + stride * h;
    std::vector<uint8_t> hdr(54, 0);
    hdr[0]='B'; hdr[1]='M';
    *(uint32_t*)&hdr[2] = (uint32_t)filesize;
    *(uint32_t*)&hdr[10] = 54;
    *(uint32_t*)&hdr[14] = 40;
    *(int32_t*)&hdr[18] = w;
    *(int32_t*)&hdr[22] = h;  // 正高度 = 自下而上(bottom-up)；像素按 y=h-1..0 写出，与 bottom-up 一致（纠正原先 -h 与写出顺序矛盾导致的上下颠倒）
    *(uint16_t*)&hdr[26] = 1;
    *(uint16_t*)&hdr[28] = 32;
    *(uint32_t*)&hdr[34] = (uint32_t)(stride * h);
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(hdr.data(), 1, 54, f);
    std::vector<uint8_t> row(stride, 0);
    for (int y = h - 1; y >= 0; --y) {
        const uint8_t* src = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            row[(size_t)x*4+0] = src[(size_t)x*4+2];
            row[(size_t)x*4+1] = src[(size_t)x*4+1];
            row[(size_t)x*4+2] = src[(size_t)x*4+0];
            row[(size_t)x*4+3] = 0xFF;
        }
        fwrite(row.data(), 1, stride, f);
    }
    fclose(f);
    return true;
}

} // namespace text
} // namespace ge
