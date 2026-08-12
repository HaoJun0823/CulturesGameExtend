// test_gdifont.cpp (debug build) —— 验证 GDI 字形引擎两条加载路径：
//   A. 系统已装字体（Create）
//   B. 从 .ttf 文件加载（CreateFromFile，自包含，不依赖系统字体）
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "../../CulturesGameExtend/Core/GdiFont.h"

using namespace ge::text;

#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while(0)

// 用给定光栅器渲染一段 UTF-8 文本，返回蓝色字形像素数，并落盘 bmp
long renderAndCount(GdiFontRasterizer& r, const char* utf8, const char* bmpPath, uint32_t color) {
    std::vector<uint32_t> cps = GdiFontRasterizer::DecodeUtf8(utf8, strlen(utf8));
    const int pad = 8;
    int lineH = r.Height() + 8;
    int imgW = 720, imgH = lineH + pad * 2;
    std::vector<uint8_t> img((size_t)imgW * imgH * 4, 0xFF);
    int penX = pad;
    int baseY = pad + r.Ascent();
    for (uint32_t cp : cps) {
        const Glyph* g = r.GetGlyph(cp);
        if (!g) continue;
        GdiFontRasterizer::BlitGlyph(img.data(), imgW * 4, 4, penX + g->originX, baseY + g->originY, g, color, imgW, imgH);
        penX += g->advance;
    }
    long nonWhite = 0, blue = 0;
    for (size_t i = 0; i + 3 < img.size(); i += 4) {
        uint8_t R = img[i+2], G = img[i+1], B = img[i+0];
        if (R < 250 || G < 250 || B < 250) ++nonWhite;
        if (B > 80 && B > R + 20 && B > G + 20) ++blue;
    }
    GdiFontRasterizer::SaveRGBAAsBMP(bmpPath, imgW, imgH, img.data());
    LOG("    -> %s: 非白=%ld 蓝色字形=%ld\n", bmpPath, nonWhite, blue);
    return nonWhite;
}

int main() {
    using namespace ge::text;
    const char* txt = u8"繁體中文測試 多語言渲染 ABC 123";

    // ---------- 路径 A：系统字体 ----------
    LOG("=== 路径 A：系统已装字体 ===\n");
    const wchar_t* candidates[] = { L"Microsoft JhengHei", L"PMingLiU", L"MingLiU", L"SimSun", L"Microsoft YaHei" };
    GdiFontRasterizer r;
    bool created = false; const wchar_t* used = nullptr;
    for (auto f : candidates) {
        if (r.Create(f, 48, 400, false)) { created = true; used = f; break; }
    }
    if (created) { LOG("OK 系统字体=%ls\n", used); renderAndCount(r, txt, "test_gdifont_sys.bmp", 0x1A3C8C); }
    else LOG("[WARN] 路径A 无可用系统字体\n");

    // ---------- 路径 B：从 bundled .ttf 文件加载 ----------
    LOG("=== 路径 B：从 plugins/fonts/*.ttf 文件加载 ===\n");
    const wchar_t* fontDir = L"G:/Projects/CulturesGameExtend/Resource/plugins/fonts/";
    struct { const wchar_t* file; const char* out; } files[] = {
        { L"l10.ttf", "test_gdifont_l10.bmp" },  // 思源黑体 SC（CJK）
        { L"eng.ttf", "test_gdifont_eng.bmp" },  // Roboto（Latin）
    };
    for (auto& e : files) {
        std::wstring path = std::wstring(fontDir) + e.file;
        GdiFontRasterizer f;
        LOG("B1 try CreateFromFile %ls\n", path.c_str());
        if (f.CreateFromFile(path.c_str(), 48, 400, false)) {
            LOG("OK CreateFromFile %ls (族名已解析)\n", e.file);
            renderAndCount(f, txt, e.out, 0x8C1A3C);
        } else {
            LOG("[FAIL] CreateFromFile %ls 失败\n", e.file);
        }
    }

    LOG("=== 完成 ===\n");
    return 0;
}
