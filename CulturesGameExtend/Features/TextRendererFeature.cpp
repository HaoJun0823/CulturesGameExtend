// TextRendererFeature.cpp
// ===================================================================
// [TextRenderer] 多语言文本渲染（取代 LG4Northland.dll 的渲染半边）
//
// 决策（用户拍板 2026-08-12，hy3 重架构 2026-08-12）：
//   - 输入文本编码 = UTF-8；引擎逐字节喂给字形 blit，hook 内按 UTF-8 重组码点。
//   - 字形来源 = 系统 GDI（DIB + TextOutW），不用 FreeType。
//   - 语言驱动字体选择：读 Game.ini set_language -> plugins/fonts/<code>.ttf。
//   - ★ 2026-08-12 07:4x 用户拍板：**ASCII 也由我们接管，所有文字由 GDI 统一重绘**
//     （不再 relay 回引擎位图字体，保证中英混排风格/基线/字宽完全一致）。
//
// ★ 架构（对照 LG4Northland 的完整逆向结论，2026-08-12）：
//   LG4 在 Northland 引擎的"逐字形"层 hook（sub_10001520/16D0/1870 都调用
//   sub_10002D40 这一原子 blit），而不是在排版层 hook。原因：
//
//   Saga 文本链 = sub_40ECF9(排版循环) ─┐
//                                     ├─> sub_439610(薄壳) -> sub_4658C7(原子字形 blit)
//                 sub_4E1E3F(格式化文本)┘   （sub_4E1E3F 是第二个文本入口，带 gfxgood/
//                                            gfxhouse/setfontcolor/setx/sety 等标记语言，
//                                            它同样只通过 sub_439610 画每个可见字形）
//
//   sub_4658C7 的唯一调用者是 sub_439610；sub_439610 仅 3 个调用者：
//   sub_40ECF9（×2）与 sub_4E1E3F（×1）。所以 **sub_439610 是所有文本的唯一汇合点**。
//
//   旧版只 hook sub_40ECF9（排版层 ReplaceMode 接管）→
//     (a) sub_4E1E3F 路径完全没被 hook => "一些文字完全没被 hook 到"；
//     (b) 自己重排版、固定 GDI 字号、基线假设 => 位置/大小不对。
//
//   新版在 **字形层** hook 两个函数，mirror LG4：
//     - sub_439610（原子 blit 汇合点）：**全部字节（含 ASCII<0x80）进入 GDI 渲染**
//       —— ASCII 是完整码点直接画；≥0x80 进入 UTF-8 缓冲，码点完整时用 GDI 落字。
//       仅当光栅器未就绪/防御失败时 relay 回引擎原函（安全兜底，绝不让游戏空字/崩）。
//     - sub_439633（字宽查询）：全部字节返回 GDI 字宽 —— ASCII=GetGlyph(ch)->advance；
//       ≥0xC0 lead 返回 CJK 代表字宽；0x80..0xBF continuation 返回 0，使 UTF-8 多字节
//       只前进一次（与 LG4 双字节处理一致）。未就绪返回 -1 触发 relay。
//   这样：位置由引擎算（正确）、两种文本入口都覆盖（修 (a)）、字号统一 GDI（修 (b)）、
//   ASCII 与 CJK 同一套 GDI 字形（用户拍板全接管）。
//
//   调用约定（已用 va.py capstone 反汇编确认指令边界 + 实读字节，07:1x 定案）：
//     sub_439610: __thiscall，ecx = 字体对象(font)；栈 5 参：
//                 [ebp+8]=颜色上下文(a3)  [ebp+0xC]=DrawContext(★含 +0x2C 帧缓冲基址 / +0x30 pitch
//                 / +0x08..0x14 裁剪区)  [ebp+0x10]=ch  [ebp+0x14]=x  [ebp+0x18]=y；
//                 跳板抄 6 字节 -> 0x439616。
//                 铁证：0x439613 `push [ebp+8]` + sub_4658C7@0x46590E `mov edi,[ebp+0xC]`（帧缓冲
//                 edi[+0x2C]）+ sub_40ECF9@0x40ED6D `push edi`(this=DrawContext 作为第 2 个栈参)。
//                 ★ stub 必须把 [ebp+0x0C] 当 DrawContext 传给 OnGlyphDraw。
//     sub_439633: __thiscall, ecx=font, ch=[esp+4]; 跳板抄 5 字节 -> 0x439638
//
//   ★ 全接管后 naked stub 调 C 函数必破坏 ecx（font=this）：relay 分支必须先用非易失
//     寄存器(ebx)暂存 ecx 再还原（铁律，见 2026-08-12 记忆 05:51 崩溃）。
// ===================================================================
#pragma region "Header & Architecture Notes"
// TextRendererFeature.cpp
// ===================================================================
// [TextRenderer] Multilingual text rendering (replaces the rendering half of LG4Northland.dll)
//
// Decision (user decision 2026-08-12, hy3 re-architected 2026-08-12):
//   - Input text encoding = UTF-8; the engine feeds bytes one by one to the glyph blit;
//     the hook reassembles codepoints from UTF-8.
//   - Glyph source = system GDI (DIB + TextOutW), not FreeType.
//   - Language-driven font selection: read Game.ini set_language -> plugins/fonts/<code>.ttf.
//   - ★ 2026-08-12 07:4x user decision: **ASCII is also taken over by us; all text is
//     redrawn uniformly by GDI** (no longer relay back to the engine's bitmap font, to
//     guarantee identical CJK/English mixing style / baseline / glyph width).
//
// ★ Architecture (mapped against LG4Northland's full reverse-engineering conclusions, 2026-08-12):
//   LG4 hooks at the engine's "per-glyph" layer (sub_10001520/16D0/1870 all call
//   sub_10002D40, the atomic blit) instead of at the layout layer. Reason:
//
//   Saga text chain = sub_40ECF9 (layout loop) ─┐
//                                               ├─> sub_439610 (thin shell) -> sub_4658C7 (atomic glyph blit)
//                     sub_4E1E3F (formatted text)┘   (sub_4E1E3F is a second text entry point, with gfxgood/
//                                                      gfxhouse/setfontcolor/setx/sety markup; it also only
//                                                      draws each visible glyph through sub_439610)
//
//   sub_4658C7 is called only by sub_439610; sub_439610 has only 3 callers:
//   sub_40ECF9 (×2) and sub_4E1E3F (×1). So **sub_439610 is the single convergence point of all text**.
//
//   Old version only hooked sub_40ECF9 (layout-layer ReplaceMode takeover) ->
//     (a) the sub_4E1E3F path was never hooked => "some text was completely not hooked";
//     (b) self re-layout, fixed GDI font size, baseline assumption => wrong position/size.
//
//   New version hooks two functions at the **glyph layer**, mirroring LG4:
//     - sub_439610 (atomic blit convergence point): **all bytes (including ASCII<0x80) go into GDI rendering**
//       —— ASCII is a complete codepoint drawn directly; ≥0x80 enters the UTF-8 buffer, and once the
//          codepoint is complete it is drawn via GDI.
//       Only when the rasterizer is not ready / defense fails do we relay back to the engine's original
//       function (safe fallback; never let the game show blank text or crash).
//     - sub_439633 (glyph-width query): all bytes return the GDI glyph width —— ASCII=GetGlyph(ch)->advance;
//       ≥0xC0 lead returns a CJK representative glyph width; 0x80..0xBF continuation returns 0, so a UTF-8
//       multi-byte sequence only advances once (consistent with LG4's double-byte handling). Returns -1
//       when not ready to trigger relay.
//   This way: position is computed by the engine (correct), both text entry points are covered (fixes (a)),
//   font size unified under GDI (fixes (b)), ASCII and CJK share the same set of GDI glyphs (user decision
//   to take over everything).
//
//   Calling convention (confirmed via va.py capstone disassembly of instruction boundaries + direct byte
//   reading, finalized 07:1x):
//     sub_439610: __thiscall, ecx = font object; 5 stack args:
//                 [ebp+8]=color context (a3)  [ebp+0xC]=DrawContext (★contains +0x2C framebuffer base /
//                 +0x30 pitch / +0x08..0x14 clip region)  [ebp+0x10]=ch  [ebp+0x14]=x  [ebp+0x18]=y;
//                 trampoline copies 6 bytes -> 0x439616.
//                 Hard evidence: 0x439613 `push [ebp+8]` + sub_4658C7@0x46590E `mov edi,[ebp+0xC]` (framebuffer
//                 edi[+0x2C]) + sub_40ECF9@0x40ED6D `push edi` (this=DrawContext as 2nd stack arg).
//                 ★ stub must pass [ebp+0x0C] as DrawContext to OnGlyphDraw.
//     sub_439633: __thiscall, ecx=font, ch=[esp+4]; trampoline copies 5 bytes -> 0x439638
//
//   ★ After full takeover, a naked stub calling a C function necessarily clobbers ecx (font=this): the
//     relay branch must first save ecx in a non-volatile register (ebx) and restore it (hard rule; see
//     2026-08-12 memory 05:51 crash).
// ===================================================================
#pragma endregion

#pragma region "Includes & Namespace"
#include "pch.h"
#include "Core/Feature.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Patch.h"
#include "Core/Logger.h"
#include "Core/GameApi.h"
#include "Core/GdiFont.h"
#include "Core/Paths.h"
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>


namespace fe_text {


#pragma endregion

#pragma region "Global Config & Engine State"
const char* kName = "TextRenderer";
const char* kCat  = "[TextRenderer]";

// ---- 默认配置 ----

// ---- Default config ----
const wchar_t* kDefFont = L"Microsoft JhengHei";
// 繁中（微軟正黑體）；简中改 Microsoft YaHei
// Traditional Chinese (Microsoft JhengHei); for Simplified Chinese use Microsoft YaHei
const int      kDefSize = 16;
static ge::text::GdiFontRasterizer g_raster;

// ---- 引擎实例（进程内唯一）----

// ---- Engine instance (process-unique) ----
static bool g_ready = false;
static std::wstring g_cfgFontName;

// ---- 配置 ----

// ---- Config ----
static std::wstring g_cfgFontFile;
static int          g_fontSize = kDefSize;
static bool         g_enabled  = false;
static bool         g_selfTest = false;
static bool         g_hookOn   = false;
static uint32_t     g_textColor  = 0xFFFFFF;
static int          g_textYOffset = 0;
static int          g_textXOffset = 0;
static bool         g_antiAlias = true;
// 2026-08-12 新增：水平微调（修"整体右偏"）
// 2026-08-12 added: horizontal fine-tune (fixes "overall rightward shift")
static int          g_halfCellExtra = 1;
static uintptr_t    g_base = 0;
// 抗锯齿开关（1=灰度抗锯齿；0=硬边清晰）
// anti-alias toggle (1=grayscale AA; 0=hard edges, crisp)
static int      s_buf  = 0;
static int      s_need = 0;
// 半角格加宽（西文间距偏近 → +N px）
// half-width cell widening (Latin spacing too tight -> +N px)
static int      s_got  = 0;
static uint32_t s_cp   = 0;

// UTF-8 重组缓冲（文本绘制单线程顺序进行，足够）

// UTF-8 reassembly buffer (text drawing is single-threaded and sequential, so this is sufficient)
static int      s_lx = 0, s_ly = 0;
// 1=正在缓冲一个多字节码点
// 1=currently buffering a multi-byte codepoint
static const uint8_t* s_color = nullptr;
static const void*     s_font  = nullptr;
// 还需要的 continuation 数
// remaining continuation bytes needed
static bool  g_loggedOnce = false;
static const int LANG_TABLE_VA = 0x4F338C;
// 已收到的 continuation 数
// continuation bytes already received
static int ReadGameLangIdFromIni() {
    wchar_t exepath[MAX_PATH] = {};
// 当前码点累加值
// current codepoint accumulator
    GetModuleFileNameW(nullptr, exepath, MAX_PATH);
    std::wstring dir(exepath);
// lead 字节时的 (x,y)，渲染以此为准
// (x,y) at the lead byte; rendering uses these
    auto pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir = dir.substr(0, pos);
// lead 字节时的引擎颜色上下文（多字节码点跨两次调用）
// engine color context at the lead byte (multi-byte codepoint spans two calls)
    std::wstring iniPath = dir + L"\\Game.ini";
    FILE* f = nullptr;
// lead 字节时的字体对象（垂直居中行高）
// font object at the lead byte (for vertical-centering line height)
    if (_wfopen_s(&f, iniPath.c_str(), L"r") != 0 || !f) return 0;


    int id = 0;

// ---- 语言驱动字体选择 ----

#pragma endregion

#pragma region "Language-Driven Font Selection"
// ---- Language-driven font selection ----
    char line[512];


    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (_strnicmp(p, "set_language", 12) != 0) continue;
        p += 12;
        while (*p == ' ' || *p == '\t' || *p == '=') p++;
        int v = atoi(p);
        if (v >= 0 && v <= 19) { id = v; break; }
    }
    fclose(f);
    return id;
}
static std::string LangCodeForId(int id) {
    if (id == 0) return "ger";
    if (id < 1 || id > 19) return {};
    char** table = (char**)LANG_TABLE_VA;
    char* code = table[id - 1];
    if (!code || !code[0]) return {};
    return std::string(code, strnlen(code, 8));
}
static std::wstring FontPathForCode(const std::string& code) {
    wchar_t exepath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exepath, MAX_PATH);


    std::wstring root(exepath);
    auto p2 = root.find_last_of(L"\\/");
    if (p2 != std::wstring::npos) root = root.substr(0, p2);
    std::wstring wcode;
    for (char c : code) wcode += (wchar_t)(unsigned char)c;
    return root + L"\\plugins\\fonts\\" + wcode + L".ttf";
}
static void MergeLangOverrides(IniConfig& cfg) {


    int langId = ReadGameLangIdFromIni();
    std::string code = LangCodeForId(langId);
    if (code.empty()) { LOG_INFO(kCat, "Lang override: no lang code (id=%d), skip", langId); return; }
    std::string path = ge_paths::ExeDir() + "plugins/fonts/" + code + ".ini";
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LOG_INFO(kCat, "Lang override: %s not found (lang=%s), use Game.ini only",
                 path.c_str(), code.c_str());
        return;
    }
    IniConfig langCfg;

// ★ 多语言配置覆盖：plugins/fonts/<langcode>.ini 的 [TextRenderer] 段覆盖
//   CulturesGameExtend_Game.ini，实现"每个语言一套渲染配置"。
//   语言代码与字体同源（Game.ini set_language -> 0x4F338C 表）。
//   IniConfig::Merge(other)：other 覆盖当前 → cfg.Merge(langCfg)。

// ★ Per-language config override: the [TextRenderer] section of plugins/fonts/<langcode>.ini overrides
//   CulturesGameExtend_Game.ini, achieving "one rendering config per language".
//   The language code shares its source with the font (Game.ini set_language -> 0x4F338C table).
//   IniConfig::Merge(other): other overrides current -> cfg.Merge(langCfg).
    if (!langCfg.Load(path)) {
        LOG_WARN(kCat, "Lang override: failed to load %s", path.c_str());
        return;
    }
    cfg.Merge(langCfg);
    LOG_INFO(kCat, "Lang override: %s loaded (lang=%s) -> overrides [%s] keys",
             path.c_str(), code.c_str(), kName);
}
static void EnsureFont(uintptr_t base) {
    std::wstring wantKey;
    std::wstring filePath;
    std::wstring faceName;
    if (!g_cfgFontName.empty()) {
        wantKey = L"S:" + g_cfgFontName; faceName = g_cfgFontName;
    } else if (!g_cfgFontFile.empty()) {
        wantKey = L"F:" + g_cfgFontFile; filePath = g_cfgFontFile;
    } else {
        int langId = ReadGameLangIdFromIni();
        std::string code = LangCodeForId(langId);


        if (!code.empty()) {
            filePath = FontPathForCode(code);
            if (GetFileAttributesW(filePath.c_str()) != INVALID_FILE_ATTRIBUTES)
                wantKey = L"F:" + filePath;


        }
        if (wantKey.empty()) { faceName = kDefFont; wantKey = L"S:" + std::wstring(kDefFont); }
    }
    g_raster.Reset();
    bool ok = false;
    if (!filePath.empty())
        ok = g_raster.CreateFromFile(filePath.c_str(), g_fontSize, 400, false, g_antiAlias);
    if (!ok)
        ok = g_raster.Create(faceName.c_str(), g_fontSize, 400, false, g_antiAlias);
    if (ok) { g_ready = true; LOG_INFO(kCat, "engine ready: font key=%ls size=%d aa=%d", wantKey.c_str(), g_fontSize, g_antiAlias ? 1 : 0); }
    else    { g_ready = false; LOG_ERROR(kCat, "failed to create font: %ls", wantKey.c_str()); }
}
static void RunSelfTest(const std::string& cfgText) {
    EnsureFont(g_base);


    LOG_INFO(kCat, "SelfTest: font %ls", g_ready ? L"ok" : L"FAIL");
    std::string txt = cfgText;
    if (txt.empty()) txt = u8"繁體中文測試 多語言渲染 ABC 123";
    std::vector<std::string> lines;
    std::string cur;
    for (size_t i = 0; i < txt.size(); ++i) {
        if (txt[i] == '\\' && i + 1 < txt.size() && txt[i + 1] == 'n') {
            lines.push_back(cur); cur.clear(); ++i;
        } else {

// ===================================================================
// SelfTest：渲染 ini 配置的 SelfTestText（UTF-8，支持 \n 换行）到
//   logs/TextRenderer_selftest.bmp。空配置用默认繁中样例。
//   多语言测试：在 <code>.ini 里写对应语言的 SelfTestText 即可。
// ===================================================================

#pragma endregion

#pragma region "Self-Test Rendering"
// ===================================================================
// SelfTest: render the ini-configured SelfTestText (UTF-8, supports \n line breaks) to
//   logs/TextRenderer_selftest.bmp. Empty config uses the default Traditional Chinese sample.
//   For multilingual testing: just write the corresponding language's SelfTestText in <code>.ini.
// ===================================================================
            cur += txt[i];
        }
    }


    lines.push_back(cur);
    std::vector<std::vector<uint32_t>> rows;
    // 拆分 \n 转义为多行（ini 值单行书写）
    // Split \n escapes into multiple lines (ini values are written on a single line)
    int maxW = 0;
    for (const auto& line : lines) {
        auto cps = ge::text::GdiFontRasterizer::DecodeUtf8(line.data(), line.size());
        int w = 8;
        for (uint32_t cp : cps) {
            const ge::text::Glyph* g = g_raster.GetGlyph(cp);
            w += g ? g->advance : g_fontSize;
        }
        if (w > maxW) maxW = w;
        rows.push_back(std::move(cps));

    // 逐行解码 + 动态画布尺寸（max 行宽）

    // Decode line by line + dynamic canvas size (max line width)
    }
    int lineH = g_fontSize + 8;
    int imgW = maxW + 16;
    int imgH = (int)rows.size() * lineH + 16;
    std::vector<uint8_t> img((size_t)imgW * imgH * 4, 0xFF);
    int penX = 8, baseY = 8 + g_raster.Ascent();
    for (size_t r = 0; r < rows.size(); ++r) {
        int rowX = 8;
        for (uint32_t cp : rows[r]) {
            const ge::text::Glyph* g = g_raster.GetGlyph(cp);
            if (!g) continue;
            ge::text::GdiFontRasterizer::BlitGlyph(img.data(), imgW * 4, 4,


                                                   rowX + g->originX, baseY + g->originY, g, 0x1A3C8C, imgW, imgH);
            rowX += g->advance;
        }
        baseY += lineH;
    }
    char path[MAX_PATH] = {};
    snprintf(path, sizeof(path), "%s/TextRenderer_selftest.bmp", "logs");
    size_t total = 0;
    for (auto& r : rows) total += r.size();
    if (ge::text::GdiFontRasterizer::SaveRGBAAsBMP(path, imgW, imgH, img.data()))
        LOG_INFO(kCat, "SelfTest: wrote %s (%zu lines, %zu codepoints)", path, rows.size(), total);
    else
        LOG_WARN(kCat, "SelfTest: failed to write BMP");
}
static uintptr_t g_thunkGlyph = 0;
static uintptr_t g_thunkWidth = 0;
static bool g_useEngineColor = true;
static bool g_wordSplitPatch = false;
static bool g_blendIdempotent = true;
static volatile int g_inLink = 0;
static uint32_t EngineColor(const uint8_t* c) {
    if (!c || !g_useEngineColor) return g_textColor;
    return ((uint32_t)c[2] << 16) | ((uint32_t)c[1] << 8) | c[0];
}
static bool g_dumpedDrawCtx = false;

// ===================================================================
// 字形层 hook 实现
//   DrawContext(this) 结构（static 逆向 sub_4658C7 实证）：
//     +0x08=clip.x +0x0C=clip.y +0x10=clip.w(表面宽) +0x14=clip.h(表面高)
//     +0x2C=像素基址   +0x30=pitch(像素)   文本路径硬编码 *4 => 32bpp
// ===================================================================


#pragma endregion

#pragma region "Glyph-Layer Hook: DrawContext & Engine Color"
// ===================================================================
// Glyph-layer hook implementation
//   DrawContext(this) structure (confirmed via static reverse-engineering of sub_4658C7):
//     +0x08=clip.x +0x0C=clip.y +0x10=clip.w (surface width) +0x14=clip.h (surface height)
//     +0x2C=pixel base   +0x30=pitch (pixels)   text path hardcodes *4 => 32bpp
// ===================================================================

static bool ShouldSkipRepeat(uint8_t* fb, int pitchBytes, int bpp, int dx, int dy,
// sub_439610 跳板
// sub_439610 trampoline
                             const ge::text::Glyph* g, uint32_t color, int fbW, int fbH) {
    if (!fb || !g || g->w < 2 || g->h < 2) return false;
// sub_439633 跳板
// sub_439633 trampoline
    int cx = dx + g->w / 2;

// ★ 2026-08-12 08:5x 引擎颜色接管（Tooltip 残留重影根因）：
//   sub_4658C7 反编译实锤：颜色上下文 = sub_439610 的 [ebp+8](a3)，结构 = {B,G,R} 字节数组
//   （32bpp 分支 v145=*a6/B, HIBYTE(v15)=a6[1]/G, v147=a6[2]<<16/R；16bpp 分支
//   sub_410029(a1)=sub_40FFEA(a1[2],a1[1],*a1) 同源）。0xRRGGBB = c[2]<<16 | c[1]<<8 | c[0]。
//   Tooltip 画多 pass 时各 pass 颜色不同（阴影/描边深色 + 主体白色），旧代码统一用
//   g_textColor(白) → 两遍白色叠加 → 残留重影。改用引擎颜色 → 还原原版颜色/阴影，重影消失。

// ★ 2026-08-12 08:5x engine color takeover (root cause of Tooltip ghosting artifacts):
//   sub_4658C7 decompilation confirms: the color context = [ebp+8] (a3) of sub_439610, structured as a {B,G,R} byte array
//   (32bpp branch v145=*a6/B, HIBYTE(v15)=a6[1]/G, v147=a6[2]<<16/R; 16bpp branch
//   sub_410029(a1)=sub_40FFEA(a1[2],a1[1],*a1) is the same source). 0xRRGGBB = c[2]<<16 | c[1]<<8 | c[0].
//   When the Tooltip draws in multiple passes, each pass has a different color (dark shadow/outline + white body);
//   the old code used g_textColor (white) uniformly -> two white passes overlaid -> residual ghosting.
//   Using the engine color restores the original colors/shadows, and the ghosting disappears.
    int cy = dy + g->h / 2;
    if (cx < 0 || cx >= fbW || cy < 0 || cy >= fbH) return false;
// ★23:4x 最终定案：**WordSplitPatch=1 启用中**（ini 控制，代码默认 false 防缺键）。
// ★23:4x final decision: **WordSplitPatch=1 is active** (controlled by ini; code defaults to false to avoid missing keys).
    const uint8_t* p = fb + (size_t)cy * pitchBytes + (size_t)cx * bpp;
                                       //   语义（23:34 用户实测正常）：补丁①(0xD053B) 让 sub_4CFE93 词扫描单字节化
                                       //   → 富文本词=单字节（GBK 字拆碎）→ 每词不超宽 → 长文本完整显示；
                                       //   副作用"每字节后空格宽"由 0x4CA612 NOP 消除 → 中文紧凑。
                                       //   ⚠ 旧弃用原因（10:41 justify 分散）已随 NOP 组合解决，勿再回退到 0
                                       //   （WordSplitPatch=0 → 富文本词=整段 → 长文本超宽溢出/消失，用户 23:32 实测）。
                                       //   Semantics (verified working by user at 23:34): patch ① (0xD053B) makes sub_4CFE93's word scan single-byte
                                       //   -> rich-text words become single-byte (GBK chars split apart) -> each word never exceeds width -> long text displays fully;
                                       //   the side effect "space width after each byte" is eliminated by the 0x4CA612 NOP -> Chinese becomes compact.
                                       //   WARNING: the old reason for deprecation (10:41 justify spreading) is already solved by the NOP combination; do not revert to 0
                                       //   (WordSplitPatch=0 -> rich-text words become the whole paragraph -> long text overflows/disappears, verified by user at 23:32).
    if (bpp == 4) {
// BlitGlyph 像素级幂等（防不清空表面跨帧累积）
// BlitGlyph is pixel-idempotent (prevents cross-frame accumulation when the surface is not cleared)
        uint8_t tr = (uint8_t)((color >> 16) & 0xFF);
        uint8_t tg = (uint8_t)((color >> 8) & 0xFF);
// ★ sub_4CFE93 解析期链接状态：<anch 标签内=1、其他/结束标签=0
// ★ sub_4CFE93 parse-time link state: 1 inside an <anch> tag, 0 otherwise / on closing tag
        uint8_t tb = (uint8_t)(color & 0xFF);
                                        //   （TagEntryStub 跟踪；整句变亮实际依赖 token+36 的 linkId 传播）
                                        //   (tracked by TagEntryStub; whole-sentence brightening actually depends on linkId propagation at token+36)
        return (p[2] > tr - 24 && p[2] < tr + 24) &&
               (p[1] > tg - 24 && p[1] < tg + 24) &&
               (p[0] > tb - 24 && p[0] < tb + 24);
    } else if (bpp == 2) {

// 渲染一个码点到 DrawContext 表面（引擎已在 (x,y) 给出该字形的左上角）

#pragma endregion

#pragma region "Glyph-Layer Hook: Defensive Helpers"
// Render one codepoint onto the DrawContext surface (the engine has already given the glyph's top-left at (x,y))
        uint16_t v = *(const uint16_t*)p;

// ★ 2026-08-12 08:3x 重复绘制检测：信息框"模糊"疑似同一字符被引擎多次绘制叠加。
//   08:49 诊断定案：dt=31ms = **跨帧重绘叠加**（Tooltip 每帧重绘到持久表面不清空）。
//   原版硬边位图叠加无变化；GDI 抗锯齿字形边缘 alpha 渐变叠加 → 重影。
//   普通文字表面每帧清空，所以不叠加。
//   ★ 修复方案：绘制前检查字形中心像素是否已是目标色——是 → 表面未清空(重绘) → 跳过；
//     否 → 正常画。对"清空/不清空"两种表面都正确。

// ★ 2026-08-12 08:3x duplicate-draw detection: info boxes looked "blurry", suspected the same character was drawn multiple times and overlaid by the engine.
//   08:49 diagnosis: dt=31ms = **cross-frame redraw overlay** (Tooltip redraws to a persistent surface every frame without clearing).
//   The original hard-edged bitmap overlay showed no change; but GDI anti-aliased glyph edges with alpha gradients overlay -> ghosting.
//   Normal text surfaces are cleared every frame, so they do not overlay.
//   ★ Fix: before drawing, check whether the glyph's center pixel is already the target color -- if yes -> surface was not cleared (redraw) -> skip;
//     if no -> draw normally. Correct for both "cleared" and "uncleared" surfaces.
        uint16_t tc = (uint16_t)((((color >> 16) & 0xFF) >> 3) << 11 |
                                 (((color >> 8) & 0xFF) >> 2) << 5 |
                                 ((color & 0xFF) >> 3));
        int dr = ((v >> 11) & 0x1F) - ((tc >> 11) & 0x1F);
        int dg = ((v >> 5) & 0x3F) - ((tc >> 5) & 0x3F);
        int db = (v & 0x1F) - (tc & 0x1F);
        return dr > -3 && dr < 3 && dg > -4 && dg < 4 && db > -3 && db < 3;
    }
    return false;
}
static bool IsWideCodepoint(uint32_t cp) {
        // 容差 24：抗锯齿中心墨迹≈纯色；背景色差异明显
        // Tolerance 24: anti-aliased center ink ≈ solid color; background differs clearly
    if (cp < 0x80) return false;
    if (cp >= 0x2E80 && cp <= 0x9FFF) return true;
    if (cp >= 0xF900 && cp <= 0xFAFF) return true;
    if (cp >= 0xFF00 && cp <= 0xFF60) return true;
    if (cp >= 0x3000 && cp <= 0x303F) return true;
    return false;
}
static bool IsRangeMapped(const void* p, size_t n) {
    if (!p) return false;
    const uint8_t* cur = (const uint8_t*)p;
    const uint8_t* end = cur + n;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(cur, &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;

// ★ 半角/全角分类：只有 CJK 及全角形式才是"全格"；西语/希腊/西里尔等 Latin 扩展
//   字母是半角（≈ASCII 宽）。旧逻辑 cp>=0x80 一律全格 → 西语字母占中文宽度（间距怪）。

// ★ Half-width / full-width classification: only CJK and full-width forms are "full cell"; Latin extensions like
//   Western/Greek/Cyrillic letters are half-width (≈ASCII width). The old logic treated cp>=0x80 as full cell -> Western letters took Chinese width (odd spacing).
        DWORD prot = mbi.Protect & 0xFF;
        switch (prot) {
// ASCII：半角
// ASCII: half-width
            case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
            case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
// CJK 部首 + 统一汉字
// CJK radicals + Unified Han
                break;
            default: return false;
// CJK 兼容汉字
// CJK compatibility Han
        }
        const uint8_t* next = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
// 全角形式
// Full-width forms
        if (next <= cur) return false;
        cur = next;
// CJK 标点（U+3000 全角空格）
// CJK punctuation (U+3000 full-width space)
    }
    return true;
// Latin/希腊/西里尔等：半角
// Latin/Greek/Cyrillic etc.: half-width
}
struct FbState { uintptr_t fb; DWORD t; int mode; int realH; };

// ★ 防野指针/越界：逐区域校验 [p, p+n) 全部"已提交 + 可读"。

// ★ Guard against wild pointers / out-of-bounds: validate each region that [p, p+n) is fully "committed + readable".
static FbState s_fbState[64];
static FbState* FindFbState(uintptr_t fb) {
    DWORD now = GetTickCount();
    for (int i = 0; i < 64; ++i) {
        if (s_fbState[i].fb == fb) {
            if (now - s_fbState[i].t > 2000) { s_fbState[i].mode = 0; s_fbState[i].realH = 0; }
            s_fbState[i].t = now;
            return &s_fbState[i];
        }
    }
    int oldest = 0; DWORD ot = GetTickCount();
    for (int i = 0; i < 64; ++i) if (s_fbState[i].t < ot) { ot = s_fbState[i].t; oldest = i; }
    s_fbState[oldest] = { fb, now, 0, 0 };
    return &s_fbState[oldest];
}
static int ProbeFbHeight(uintptr_t fb, int pitchBytes, int maxH) {
    if (maxH <= 0 || maxH > 4096) return 0;
    int lo = 0, hi = maxH;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (IsRangeMapped((const void*)fb, (size_t)mid * pitchBytes)) lo = mid;

// ★ 探测 fb 实际可写行数（二分 + per-fb 缓存）：
//   DrawContext +0x18 是"宽-1"非高度（全屏 800x600 → 799），直接当高度裁剪会越界；
//   按 pitchBytes 探测到分配边界即真实可写高度 → 越界像素被 BlitGlyph 裁掉（不崩），
//   g 等带 descender 的字形只要主体合法就正常画（不整字跳过 → 不缺失）。

// ★ Probe the framebuffer's actually-writable row count (binary search + per-fb cache):
//   DrawContext +0x18 is "width-1", not height (fullscreen 800x600 -> 799); treating it as height for clipping would go out of bounds;
//   probing by pitchBytes up to the allocation boundary yields the true writable height -> out-of-bounds pixels are clipped by BlitGlyph (no crash),
//   glyphs with descenders like 'g' draw normally as long as their body is valid (no whole-glyph skip -> no missing glyphs).
        else hi = mid - 1;
// mode: 0=普通, 1=持久(Tooltip)
// mode: 0=normal, 1=persistent (Tooltip)
    }
    return lo;
}
static int FontLineHeight(const void* font) {
    if (!font) return g_fontSize;
    int lh = *(const int*)((uintptr_t)font + 8) + 2;
    if (lh < 8 || lh > 64) lh = g_fontSize;
    return lh;
}
static void RenderCodepoint(void* self, uint32_t cp, int x, int y, const uint8_t* colorCtx, const void* font) {
    if (!self || cp <= 0x20) return;
    uint32_t color = EngineColor(colorCtx);
    uintptr_t fb = *(uintptr_t*)((uintptr_t)self + 0x2C);
    if (!fb) fb = *(uintptr_t*)((uintptr_t)self + 0x28);
    int pitch_px = *(int*)((uintptr_t)self + 0x30);
    int clipX = *(int*)((uintptr_t)self + 0x08);
    int clipY = *(int*)((uintptr_t)self + 0x0C);
    int clipW = *(int*)((uintptr_t)self + 0x10);
    int clipH = *(int*)((uintptr_t)self + 0x14);
    int fbW = pitch_px;
    int fbH = *(int*)((uintptr_t)self + 0x18);
    if (fbH <= 0 || fbH > 4096) fbH = 600;
    if (fbW <= 0 || fbW > 4096) fbW = 800;
    {
        static uintptr_t s_dumpedFb[8];
        static int s_dumpedN = 0;

// ★ 引擎行高自适应垂直居中：不同 UI 用不同字体对象（sub_439610 的 ecx），
//   行距 = 字体对象+8 的值 + 2（sub_40ECF9 反编译实锤 `a5 += a2[2] + 2`）。
//   主界面字体行高≈20 → 居中偏移 (20-13)/2≈3（正是用户手工调出的 TextYOffset=3）；
//   Tooltip 字体行高紧凑 → 偏移≈0（不再溢出引擎画的方框 → 叠加消失）。
//   TextYOffset 降级为纯微调（默认 0）。

// ★ Engine line-height adaptive vertical centering: different UIs use different font objects (sub_439610's ecx),
//   line spacing = font object's +8 value + 2 (confirmed by sub_40ECF9 decompilation: `a5 += a2[2] + 2`).
//   Main UI font line height ≈20 -> centering offset (20-13)/2≈3 (exactly the TextYOffset=3 the user tuned by hand);
//   Tooltip font line height is tight -> offset≈0 (no longer overflows the engine-drawn box -> overlay disappears).
//   TextYOffset is demoted to a pure fine-tune (default 0).
#pragma endregion

#pragma region "RenderCodepoint"
        bool isNew = true;
        for (int i = 0; i < s_dumpedN; ++i)
            if (s_dumpedFb[i] == fb) { isNew = false; break; }
// 行距 = font[8] + 2
// line spacing = font[8] + 2
        if (isNew && s_dumpedN < 8) {
            s_dumpedFb[s_dumpedN++] = fb;
// 防御：异常回退
// defense: fall back on anomaly
            uint32_t* d = (uint32_t*)self;
            LOG_INFO(kCat, "SURFACE#%d: self=%p fb=%p pitch_px=%d clip=(%d,%d %dx%d) "
                     "fbW=%d fbH=%d cp=U+%04X x=%d y=%d color=0x%X",
                     s_dumpedN, self, (void*)fb, pitch_px, clipX, clipY, clipW, clipH,
                     fbW, fbH, cp, x, y, (unsigned)color);
            LOG_INFO(kCat, "DrawCtx dump +0x00..0x54: %08X %08X %08X %08X | %08X %08X %08X %08X | "
                     "%08X %08X %08X %08X | %08X %08X %08X %08X | %08X %08X %08X %08X | %08X",
                     d[0x00/4], d[0x04/4], d[0x08/4], d[0x0C/4],
// 兜底读 +0x28（旧误判字段，尽量不用）
// fallback read of +0x28 (a previously misjudged field; avoid using if possible)
                     d[0x10/4], d[0x14/4], d[0x18/4], d[0x1C/4],
                     d[0x20/4], d[0x24/4], d[0x28/4], d[0x2C/4],
                     d[0x30/4], d[0x34/4], d[0x38/4], d[0x3C/4],
                     d[0x40/4], d[0x44/4], d[0x48/4], d[0x4C/4],
                     d[0x50/4]);
        }
    // ★ 2026-08-12 07:5x 修正：pitch_px 是帧缓冲真实行宽（如 800），clip 只是绘制子区域
    //   （如 280x20）。用 clip 推 fbW 会把超出子区域的字形裁掉（"字被吃掉"）。
    //   fbW 必须用 pitch_px；fbH 上界用 +0x18（★09:5x 定案：+0x18 = clip.w-1 = **宽-1 非高度**！
    //   全屏 800x600 → 799；按它当高度裁剪会越界 → 真实高度靠 ProbeFbHeight 探测，见下）。
    // ★ 2026-08-12 07:5x fix: pitch_px is the framebuffer's real row width (e.g. 800); clip is only the drawing sub-region
    //   (e.g. 280x20). Deriving fbW from clip would clip glyphs outside the sub-region ("glyphs eaten").
    //   fbW must use pitch_px; fbH upper bound uses +0x18 (★09:5x decision: +0x18 = clip.w-1 = **width-1, NOT height**!
    //   fullscreen 800x600 -> 799; clipping by it as height would go out of bounds -> true height comes from ProbeFbHeight probing, see below).
    }
    if (!fb || pitch_px <= 0 || pitch_px > 4096) { LOG_WARN(kCat, "RenderCodepoint skip: bad fb/pitch"); return; }
    if (fb < 0x10000 || fb > 0x7FFFFFFF) { LOG_WARN(kCat, "RenderCodepoint skip: fb out of range"); return; }
    const ge::text::Glyph* g = g_raster.GetGlyph(cp);
    // ★ 09:2x 改为"每新表面 dump 一次"（最多 8 个）：区分 Tooltip 表面 vs 主界面表面
    //   （clip 尺寸/偏移语义不同——3,3 偏移对 Tooltip 溢出框 → 叠加；0 则正常）
    // ★ 09:2x changed to "dump once per new surface" (max 8): distinguish Tooltip surface vs main-UI surface
    //   (clip size/offset semantics differ -- a 3,3 offset overflows the Tooltip box -> overlay; 0 is normal)
    if (!g || g->w == 0 || g->h == 0) return;
    {
        static bool s_dumpedGlyph = false;
        if (!s_dumpedGlyph) {
            s_dumpedGlyph = true;
            LOG_INFO(kCat, "Glyph metric: cp=U+%04X w=%d h=%d origin=(%d,%d) advance=%d",
                     cp, g->w, g->h, g->originX, g->originY, g->advance);
        }
    }
    int bpp = 4;
    {
        int pitchBytesHint = *(int*)((uintptr_t)self + 0x38);
        if (pitchBytesHint > 0 && pitch_px > 0 && (pitchBytesHint % pitch_px) == 0) {
            int cand = pitchBytesHint / pitch_px;
            if (cand == 2 || cand == 4) bpp = cand;
        }
    }
    int pitchBytes = pitch_px * bpp;
    int cellW = g_fontSize;
    int cellH = g_fontSize;
    int lineH = FontLineHeight(font);
    static const int kBearingX = 3;
    FbState* st = FindFbState(fb);
    // ★ 防御：帧缓冲/步长异常一律跳过渲染，绝不往野指针写像素（防踩坏游戏内存 → 连锁崩溃）
    // ★ Defense: any abnormal framebuffer/stride -> skip rendering; never write pixels to a wild pointer (prevents corrupting game memory -> chain crash)
    int slotW = IsWideCodepoint(cp) ? cellW : (cellW / 2 + g_halfCellExtra);
    int dx, dy;


    if (st->mode == 1) {
        int lh = lineH; if (lh < g->h) lh = g->h;
    // Glyph metric 独立于 DrawCtx dump 各打印一次（旧版共用 g_dumpedDrawCtx 导致 metric 永不输出）
    // Glyph metric is printed independently from the DrawCtx dump (the old version shared g_dumpedDrawCtx, so the metric was never output)
        dx = x + (slotW - g->w) / 2 + kBearingX + g_textXOffset;
        dy = y + (lh - g->h) / 2 + g_textYOffset;
    } else {
        int fontH = g_raster.Ascent() + g_raster.Descent();
        int centerOff = (lineH - fontH) / 2; if (centerOff < 0) centerOff = 0;
        dx = x + (slotW - g->w) / 2 + kBearingX + g_textXOffset;
        dy = y + centerOff + g_raster.Ascent() + g->originY + g_textYOffset;
    }
    // ★★ 2026-08-12 08:2x 崩溃根因修复（DrawCtx dump 实锤）：
    //   +0x30=800(pitch像素)、+0x38=1600(pitch字节) → 每像素 2 字节 = **16bpp**！
    //   旧代码硬编码 bpp=4 → 每像素写 4 字节(实际 2) → 字形横向拉伸 2 倍("字很大")
    //   + 行步进 3200 字节写进 1600 字节行 → **越界写 → 崩溃**。
    //   引擎 sub_4658C7 有两条路径：32bpp 用 +0x2C 基址，16bpp 用 +0x28 基址（dump 有效值）。
    //   修复：bpp 由 pitchBytesHint/pitch_px 推导（1600/800=2；32bpp 表面则为 4）。
    // ★★ 2026-08-12 08:2x crash root-cause fix (confirmed by DrawCtx dump):
    //   +0x30=800 (pitch pixels), +0x38=1600 (pitch bytes) -> 2 bytes per pixel = **16bpp**!
    //   The old code hard-coded bpp=4 -> writing 4 bytes per pixel (actually 2) -> glyph stretched 2x horizontally ("huge text")
    //   + row stepping of 3200 bytes written into 1600-byte rows -> **out-of-bounds write -> crash**.
    //   The engine's sub_4658C7 has two paths: 32bpp uses the +0x2C base, 16bpp uses the +0x28 base (valid dump values).
    //   Fix: derive bpp from pitchBytesHint/pitch_px (1600/800=2; 4 for 32bpp surfaces).
    if (st->realH <= 0) st->realH = ProbeFbHeight(fb, pitchBytes, fbH);
    int fbHReal = st->realH;
    if (fbHReal <= 0) return;
    if (fbHReal > fbH) fbHReal = fbH;
    if (ShouldSkipRepeat((uint8_t*)fb, pitchBytes, bpp, dx, dy, g, color, fbW, fbHReal)) {
// 每像素字节数 2 或 4
// bytes per pixel: 2 or 4
        st->mode = 1;
        return;
    }
    if (clipW > 0 && clipH > 0) {
        if (dx + g->w <= clipX || dx >= clipX + clipW ||
    // ★★ 方案A（2026-08-12 08:1x 定案，mirror LG tga 等宽格模型，用户拍板）：
    //   LG sub_10002D40 的坐标模型 = (x,y) 是引擎算好的"格子左上角"，直接把整个固定格子
    //   blit 过去，零缩放零 bearing。右偏的根因不是 origin（实测不加 origin 已完整显示），
    //   而是 sub_439633 返回 GDI advance 与引擎排版推进步长不一致 → 每个字符 x 累积偏差，
    //   偏移量随位置变化，单个 TextXOffset 无法统一矫正。
    //   方案A：字宽固定（ASCII=半格=cellW/2，CJK=全格=cellW，与引擎排版完全同步），
    //   字形墨迹在格内居中，从 (x,y) 直画。cellW=cellH=g_fontSize。
    // ★★ Plan A (decided 2026-08-12 08:1x, mirroring LG's tga equal-cell model, user-approved):
    //   LG sub_10002D40's coordinate model = (x,y) is the "cell top-left" computed by the engine; blit the whole
    //   fixed cell directly, zero scaling zero bearing. The rightward shift is NOT caused by origin (testing shows full
    //   display without origin), but by sub_439633 returning a GDI advance inconsistent with the engine's layout advance
    //   step -> each character's x accumulates drift, and the offset varies with position, so a single TextXOffset
    //   cannot uniformly correct it.
    //   Plan A: fixed cell width (ASCII=half cell=cellW/2, CJK=full cell=cellW, fully synchronized with engine layout),
    //   glyph ink centered within the cell, drawn straight from (x,y). cellW=cellH=g_fontSize.
            dy + g->h <= clipY || dy >= clipY + clipH)
            return;
    }
    // ★★ 10:2x 水平系统偏差内置修正（与垂直 FontLineHeight 自动居中同理）：
    //   引擎 x = 格子左边界，但原版字形有 xBearing（≈3px）→ 原版视觉位置 = x + bearing；
    //   我们格内居中 (slotW-w)/2≈1px → 整体偏左 ~3px（用户实测：主界面与 Tooltip 都偏 3）。
    //   内置 kBearingX=3 补回原版 bearing，**所有表面统一**（含持久 Tooltip）。
    //   TextXOffset 仅作微调（默认 0）。
    // ★★ 10:2x built-in horizontal systematic-bias correction (same idea as vertical FontLineHeight auto-centering):
    //   The engine's x = cell left edge, but the original glyph has an xBearing (≈3px) -> the original visual position = x + bearing;
    //   our in-cell centering (slotW-w)/2≈1px -> overall shifted left ~3px (user testing: both main UI and Tooltip shift left by 3).
    //   The built-in kBearingX=3 compensates for the original bearing, **uniformly across all surfaces** (including persistent Tooltip).
    //   TextXOffset is only a fine-tune (default 0).
    ge::text::GdiFontRasterizer::BlitGlyph((uint8_t*)fb, pitchBytes, bpp, dx, dy, g, color, fbW, fbHReal,
                                           g_blendIdempotent, clipX, clipY, clipW, clipH);
}
// ★半格+Extra
// ★ half cell + Extra
static bool OnGlyphDraw(void* dc, int ch, int x, int y, const uint8_t* colorCtx, const void* font) {
    if (!g_ready) return false;
    if (!g_loggedOnce) {
        g_loggedOnce = true;
        LOG_INFO(kCat, "OnGlyphDraw active: dc=%p ch=0x%X x=%d y=%d size=%d cfgColor=0x%X engColor=0x%X font=%p",
                 dc, (unsigned)ch, x, y, g_fontSize, (unsigned)g_textColor,
// 持久表面：墨迹居中（09:40 公式）
// persistent surface: ink centered (09:40 formula)
                 (unsigned)EngineColor(colorCtx), font);
    }
    if (ch < 0x80) {
        RenderCodepoint(dc, (uint32_t)ch, x, y, colorCtx, font);
        return true;
    }
    if ((ch & 0xC0) == 0x80) {
    // ★ 10:1x 实际可写高度探测（per-fb 缓存）：防越界（+0x18 是宽-1）+ g 不缺失
    // ★ 10:1x actual writable-height probe (per-fb cache): prevent OOB (+0x18 is width-1) + no missing 'g'
        if (!s_buf) return true;
        s_cp = (s_cp << 6) | (ch & 0x3F);
        if (++s_got < s_need) return true;
        uint32_t cp = s_cp; s_buf = 0; s_got = 0; s_need = 0;
    // ★ 08:49 跨帧重绘去重：Tooltip 表面不清空，每帧重绘 → 中心像素已是目标色 → 跳过（防叠加模糊）
    //   08:5x 用引擎颜色（多 pass 时各 pass 颜色不同，检测色必须与本次要画的颜色一致）
    //   ★ 10:1x 命中即标记该 fb 为持久表面（下帧起零偏移）
    // ★ 08:49 cross-frame redraw deduplication: Tooltip surface is not cleared, redrawn every frame -> center pixel already target color -> skip (prevents overlay blur)
    //   08:5x uses engine color (each pass has a different color in multi-pass; the test color must match the color about to be drawn)
    //   ★ 10:1x on a hit, mark this fb as a persistent surface (zero offset from next frame on)
        RenderCodepoint(dc, cp, s_lx, s_ly, s_color, s_font);
    } else {
        if (ch >= 0xF0)         { s_cp = ch & 0x07; s_need = 3; }
        else if (ch >= 0xE0)    { s_cp = ch & 0x0F; s_need = 2; }
    // ★ 00:4x UI 框裁剪（引擎原行为）：完全在 clip 外的字形直接跳过；
    //   部分在外的由 BlitGlyph 像素级裁剪（修垂直列表最后一行溢出）
    // ★ 00:4x UI box clipping (engine's original behavior): glyphs fully outside the clip are skipped directly;
    //   partially-outside ones are pixel-clipped by BlitGlyph (fixes last-row overflow of vertical lists)
        else                    { s_cp = ch & 0x1F; s_need = 1; }
        s_got = 0; s_buf = 1; s_lx = x; s_ly = y; s_color = colorCtx; s_font = font;
    }
    return true;
}
static int OnGlyphWidth(int /*font*/, int ch) {
    if (!g_ready) return -1;
    if (ch <= 0) return 0;

// 字形层绘制 hook：sub_439610(__thiscall, ecx=字体对象; DrawContext=[ebp+0x0C]) 的 C 处理
// ★ 全字符接管：ASCII(<0x80) 是完整码点直接渲染；≥0x80 走 UTF-8 重组。
//   colorCtx = sub_439610 的 [ebp+8](a3)，{B,G,R} 字节数组（引擎颜色，还原多 pass 阴影/描边）。
//   font = sub_439610 的 ecx（字体对象；+8=行高，用于垂直居中自适应）。
//   返回 true=已接管绘制；false=未就绪（stub 会 relay 回引擎原函，防空字/崩）。

#pragma endregion

#pragma region "OnGlyphDraw (UTF-8 Reassembly)"
// Glyph-layer draw hook: the C handler for sub_439610 (__thiscall, ecx=font object; DrawContext=[ebp+0x0C])
// ★ Full character takeover: ASCII (<0x80) is a complete codepoint drawn directly; ≥0x80 goes through UTF-8 reassembly.
//   colorCtx = [ebp+8] (a3) of sub_439610, a {B,G,R} byte array (engine color, restores multi-pass shadows/outlines).
//   font = ecx of sub_439610 (font object; +8=line height, for adaptive vertical centering).
//   Returns true=takeover drawing; false=not ready (stub relays back to the engine's original function, to avoid blank text/crash).
    if (ch < 0x80) {
        return g_fontSize / 2 + g_halfCellExtra;
    }
    if ((ch & 0xC0) == 0x80) return 0;
    if (ch < 0xE0) return g_fontSize / 2 + g_halfCellExtra;
    return g_fontSize;
}
extern "C" void __declspec(naked) GlyphDrawStub() {
    __asm {
        // ASCII：单字节即完整码点，直接 GDI 渲染（用户拍板全接管）
        // ASCII: a single byte is a complete codepoint, render directly via GDI (user-approved full takeover)
        push ebp
        mov  ebp, esp
        push ebx
        mov  ebx, ecx
        // continuation (0x80..0xBF)
        // continuation (0x80..0xBF)
        push esi
// 游离 continuation，忽略（已接管，不 relay）
// stray continuation, ignore (already taken over, do not relay)
        push edi
        push ebx
        push [ebp+8]
// 还需更多
// need more
        push [ebp+0x18]
        push [ebp+0x14]
        movzx eax, byte ptr [ebp+0x10]
        push eax
        // lead (0xC0..0xFF)：开新码点，记录起始 (x,y) 与颜色/字体
        // lead (0xC0..0xFF): start a new codepoint, record starting (x,y) and color/font
        push [ebp+0x0C]
        call OnGlyphDraw
        add  esp, 24
        test eax, eax
        jnz  L_done
        mov  ecx, ebx
        pop  edi

// 字宽 hook：sub_439633(__thiscall) 的 C 处理
// ★★ 方案A：固定格子宽（mirror LG sub_10002D40 等宽模型）——
//   ASCII(单字节, 含空格) = 半格 cellW/2；CJK lead = 全格 cellW；continuation = 0。
//   引擎排版循环用本返回值推进 x，与渲染的格子完全同步 → 消除累积偏移。
//   返回 -1 = 未就绪（stub relay 回引擎原函）。

#pragma endregion

#pragma region "OnGlyphWidth"
// Glyph-width hook: the C handler for sub_439633 (__thiscall)
// ★★ Plan A: fixed cell width (mirroring LG sub_10002D40's equal-width model) --
//   ASCII (single byte, including space) = half cell cellW/2; CJK lead = full cell cellW; continuation = 0.
//   The engine layout loop advances x by this return value, fully synchronized with the rendered cell -> eliminates accumulated drift.
//   Returns -1 = not ready (stub relays back to the engine's original function).
        pop  esi
        pop  ebx
        mov  esp, ebp
// ★21:2x NUL/负：引擎 sub_43967D 测宽循环会先处理词尾 \0 再判断停止，
// ★21:2x NUL/negative: the engine sub_43967D width loop processes the word-end \0 before deciding to stop,
        pop  ebp
                                        //   若按 ASCII 返回 9px → 每词尾 +9px 假间距（WordSplitPatch=1 每字=词时
                                        //   → "你(14+9) 好(14+9)" 分散；LG4 对 NUL 走原逻辑返回 0）
                                        //   if we returned 9px as ASCII -> +9px fake spacing at every word end (when WordSplitPatch=1 makes each char a word
                                        //   -> "ni(14+9) hao(14+9)" spread; LG4 follows the original logic for NUL and returns 0)
        jmp  g_thunkGlyph
        // ASCII / 空格：半格 + HalfCellExtra（★10:3x 西文间距偏近 → 加宽）
        // ASCII / space: half cell + HalfCellExtra (★10:3x Latin spacing too tight -> widen)
    L_done:
        pop  edi
        pop  esi
// continuation：不前进
// continuation: do not advance
        pop  ebx
        mov  esp, ebp
// 2 字节 lead（Latin 等）：半格
// 2-byte lead (Latin etc.): half cell
        pop  ebp
        ret  0x14
// 3/4 字节 lead（CJK/全角）：全格
// 3/4-byte lead (CJK/full-width): full cell
    }
}

// ---- sub_439610(__thiscall, ecx=字体对象; DrawContext=[ebp+0x0C]) 替换 stub ----
//   全部字符 -> OnGlyphDraw（GDI 渲染）；false -> relay 回引擎原函（安全兜底）

#pragma endregion

#pragma region "Replacement Stub: GlyphDrawStub"
// ---- sub_439610 (__thiscall, ecx=font object; DrawContext=[ebp+0x0C]) replacement stub ----
//   All characters -> OnGlyphDraw (GDI rendering); false -> relay back to engine original (safe fallback)
extern "C" void __declspec(naked) GlyphWidthStub() {
    __asm {
        push ebx
        mov  ebx, ecx
        movzx eax, byte ptr [esp+8]
// ★ 暂存 ecx（font=this）到非易失 ebx：C 函数会破坏 ecx
// ★ Save ecx (font=this) to the non-volatile ebx: a C function clobbers ecx
        push eax
        push ecx
        call OnGlyphWidth
        add  esp, 8
        // OnGlyphDraw(dc=[ebp+0x0C]=DrawContext, ch=[ebp+0x10], x=[ebp+0x14], y=[ebp+0x18],
        //             colorCtx=[ebp+8]=a3 {B,G,R}, font=ecx=ebx)
        // OnGlyphDraw(dc=[ebp+0x0C]=DrawContext, ch=[ebp+0x10], x=[ebp+0x14], y=[ebp+0x18],
        //             colorCtx=[ebp+8]=a3 {B,G,R}, font=ecx=ebx)
        cmp  eax, -1
// font（sub_439610 的 ecx=字体对象；+8=行高）
// font (sub_439610's ecx=font object; +8=line height)
        jne  L_wdone
        mov  ecx, ebx
// 颜色上下文（sub_4658C7 的 a6/arg_10；sub_410029(a1) 读 a1[0..2]）
// color context (sub_4658C7's a6/arg_10; sub_410029(a1) reads a1[0..2])
        pop  ebx
        jmp  g_thunkWidth
// y
// y
    L_wdone:
        pop  ebx
// x
// x
        ret  4
    }
}
// ch
// ch
static uintptr_t g_thunkWordCol = 0;
extern "C" void __declspec(naked) WordColStub() {
// DrawContext = sub_439610 的 arg1（styleCtx 槽位，实为 DrawContext）
// DrawContext = sub_439610's arg1 (styleCtx slot, actually the DrawContext)
    __asm {
        cmp  cl, 0x80
        jb   L_ascii
        mov  edi, eax
        mov  [edi+0x569A38], cl
        // 未就绪：relay 回引擎原函（★必须先还原 ecx=this）
        // Not ready: relay back to engine original (★ must restore ecx=this first)
        inc  edi
        mov  cl, [edx]
        and  cl, 0xC0
        cmp  cl, 0x80
        jne  L_done
        mov  cl, [edx]
        inc  edx
        mov  [edi+0x569A38], cl
        inc  edi
        mov  cl, [edx]
        and  cl, 0xC0
        cmp  cl, 0x80
        jne  L_done
        mov  cl, [edx]
// __thiscall：callee 清理 5 个栈参数（ecx=font 不走栈）
// __thiscall: callee cleans 5 stack args (ecx=font does not go on the stack)
        inc  edx
        mov  [edi+0x569A38], cl
        inc  edi

// ---- sub_439633(__thiscall) 替换 stub ----
//   全部字符 -> OnGlyphWidth（GDI 字宽）；-1 -> relay 回引擎原函

#pragma endregion

#pragma region "Replacement Stub: GlyphWidthStub"
// ---- sub_439633 (__thiscall) replacement stub ----
//   All characters -> OnGlyphWidth (GDI glyph width); -1 -> relay back to engine original
        mov  cl, [edx]
        and  cl, 0xC0
        cmp  cl, 0x80
// ★ 暂存 ecx（font=this）
// ★ Save ecx (font=this)
        jne  L_done
        mov  cl, [edx]
        // OnGlyphWidth(font=[esp+0x0C] 原 ecx, ch=[esp+0x08] 原 [esp+4])
        // OnGlyphWidth(font=[esp+0x0C] original ecx, ch=[esp+0x08] original [esp+4])
        inc  edx
// ch（push ebx 后 +4）
// ch (after push ebx, +4)
        mov  [edi+0x569A38], cl
        inc  edi
    L_done:
        mov  byte ptr [edi+0x569A38], 0
        mov  eax, edi
        add  edx, 1
        mov  eax, 4E225Ah
        // 未就绪：relay 回引擎原函（★先还原 ecx=this；ch 原样留在 [esp+4]）
        // Not ready: relay back to engine original (★ restore ecx=this first; ch left intact at [esp+4])
        jmp  eax
    L_ascii:
        jmp  g_thunkWordCol
    }
}
static int FixOverflowWord(char* buf, int curX, int lineW, char** strPtr, int wordW) {
// __thiscall：被调方清 4 字节参数
// __thiscall: callee cleans 4 bytes of args
    if (wordW <= 0 || !buf || !strPtr || !*strPtr) return wordW;
    if (curX + wordW < lineW) return wordW;
    int L = (int)strlen(buf);

// ===================================================================
// ★ 2026-08-12 11:0x sub_4E21B7 词收集循环 CJK 单码点 hook
//   问题：引擎换行排版（sub_4E21B7）按空格收集"词"；中文无空格 → 整段一个超长词
//   → 超宽换行时整段换行、x 重置到起始（a2）→ "下一行从屏幕最左边出来"（用户实测）。
//   修复：hook 词收集循环 0x4E2242（cmp cl,7Bh = 80 F9 7B 74 0C），
//   CJK 字节（≥0x80）按 **整个码点**（lead+continuation）收集为一个"词"→
//   引擎按码点换行（词宽=全格），中文排版正常；不动 sub_4CFE93（简报 justify 不受影响）。
//   寄存器契约（收集循环内）：eax=缓冲索引, edx=String1(当前字节), cl=当前字节。
//   byte_569A38 = 词缓冲（绝对地址）。edi 用作临时（0x4E226A mov edi,eax 会覆盖，安全）。
//   尾部 add edx,1 补偿引擎 0x4E225D 的 dec edx（我们已消费整个码点）。
// ===================================================================

#pragma endregion

#pragma region "Word-Collect CJK Hook (sub_4E21B7)"
// ===================================================================
// ★ 2026-08-12 11:0x sub_4E21B7 word-collect loop CJK single-codepoint hook
//   Problem: the engine's line-wrap layout (sub_4E21B7) collects "words" by spaces; Chinese has no spaces -> the whole paragraph becomes one over-long word
//   -> on over-width wrap, the whole paragraph wraps and x resets to the start (a2) -> "the next line comes out from the far left of the screen" (user-observed).
//   Fix: hook the word-collect loop at 0x4E2242 (cmp cl,7Bh = 80 F9 7B 74 0C),
//   collect CJK bytes (≥0x80) as **whole codepoints** (lead+continuation) into a single "word" ->
//   the engine wraps by codepoint (word width = full cell), Chinese layout is normal; sub_4CFE93 is untouched (briefing justify unaffected).
//   Register contract (inside the collect loop): eax=buffer index, edx=String1 (current byte), cl=current byte.
//   byte_569A38 = word buffer (absolute address). edi is used as a temporary (0x4E226A mov edi,eax overwrites it, safe).
//   The trailing add edx,1 compensates for the engine's 0x4E225D dec edx (we already consumed the whole codepoint).
// ===================================================================
    if (L <= 1) return wordW;
// 0x4E2242 跳板（原 5 字节）
// 0x4E2242 trampoline (original 5 bytes)
    uint8_t b0 = (uint8_t)buf[0];


    int c1 = 1;
    if (b0 >= 0x80) {
        // 入口：eax=索引, edx=String1, cl=当前字节
        // Entry: eax=index, edx=String1, cl=current byte
        c1 = (b0 >= 0xF0) ? 4 : (b0 >= 0xE0) ? 3 : 2;
        while (c1 > 1 && c1 <= L && ((uint8_t)buf[c1-1] & 0xC0) != 0x80) c1--;
// ASCII → 原逻辑（trampoline）
// ASCII -> original logic (trampoline)
    }
        // CJK：整体收集码点
        // CJK: collect the whole codepoint
    if (c1 >= L) return wordW;
    buf[c1] = 0;
    int nw = (b0 < 0xE0) ? (g_fontSize / 2 + g_halfCellExtra) : g_fontSize;
        // cont1
        // cont1
    if (nw <= 0) nw = 1;
    *strPtr = *strPtr - (L - c1);
    return nw;
}
static int g_containerLeftFix = 0;
static char g_curText[160] = {0};
static void CaptureText(const char* s) {
    if (!s) return;
        // cont2
        // cont2
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(s, &mbi, sizeof(mbi)) == 0) return;
    if (mbi.State != MEM_COMMIT) return;
    if ((uintptr_t)s + 160 > (uintptr_t)mbi.BaseAddress + mbi.RegionSize) return;
    char buf[160] = {0};
    int bi = 0;
    for (int i = 0; i < 159 && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        // cont3（4 字节码点）
        // cont3 (4-byte codepoint)
        if (c == '\n') { if (bi < 156) { buf[bi++] = '\\'; buf[bi++] = 'n'; } }
        else if (c >= 0x20 && c != 0x7F) buf[bi++] = (char)c;
        else buf[bi++] = '.';
        if (bi >= 158) break;
    }
    buf[bi] = 0;
    memcpy(g_curText, buf, 160);
}
static int FixLayoutXStart(void* ctx, int lineW, int origX) {
    if (!g_containerLeftFix) return origX;
// 结束符（= 0x4E2253 的 and）
// terminator (= 0x4E2253's and)
    if (!ctx) return origX;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ctx, &mbi, sizeof(mbi)) == 0) return origX;
// 补偿 0x4E225D dec edx（已消费整个码点）
// compensate 0x4E225D dec edx (whole codepoint consumed)
    if (mbi.State != MEM_COMMIT) return origX;
    if ((uintptr_t)ctx + 0x44 > (uintptr_t)mbi.BaseAddress + mbi.RegionSize) return origX;
// 跳过 0x4E2253 直接测宽（MSVC 内联 asm 不支持 jmp 立即数）
// skip 0x4E2253 straight to width measurement (MSVC inline asm does not support jmp with immediate)
    int right = *(const int*)((uintptr_t)ctx + 0x40);
    if (right <= 0 || right > 8000) return origX;
    if (lineW <= 0 || lineW > 8000) return origX;
    int left = right - lineW;
    if (left < 0 || left > 8000) return origX;
    return left;

// ===================================================================
// ★ 2026-08-12 11:4x 超宽词字符级折行（hook 0x4E2267 测宽后，LG4 同款点）
//   问题（用户实测 11:36）：动态容器内文本渲染异常——
//   ① 过长文本（无空格长词：IP 地址/长英文）超宽时引擎**整词换行**到 x=起始(4)
//      → 溢出屏幕左侧 / 超出容器不显示；
//   ② 换行排版按空格分词，中文无空格 → 整段 1 超长词 → 无法按容器折行。
//   诊断（LayDiag/LayMeas 11:41）：xStart=4、行宽=346、词宽=23×6 连续词。
//   修复：测宽后（0x4E2267，edi=词宽前）检查"词宽+当前x >= 行宽"（超宽）：
//     → 把词缓冲 byte_569A38 截断为**第一个完整字符**（ASCII 1B / UTF-8 码点），
//       新词宽 = 单字符宽（不超宽 → 引擎正常绘制）；
//     → String1([ebp+0x20]) 回退到第二个字符 → 外层循环继续收集剩余 → 逐字符排版。
//   效果：长词按字符折行（像 LG4 单字符分词，但只在超宽时触发 → 英文单词正常
//   整词换行不受影响，briefings justify 也不受影响）。
// ===================================================================

#pragma endregion

#pragma region "Layout & Word-Wrap Helpers"
// ===================================================================
// ★ 2026-08-12 11:4x over-wide word character-level wrapping (hook after width measurement at 0x4E2267, same point as LG4)
//   Problem (user-observed 11:36): abnormal text rendering inside dynamic containers --
//   ① Over-long text (space-less long words: IP addresses / long English) wraps as a **whole word** to x=start (4) when over width
//      -> overflows off the left of the screen / exceeds the container and is not shown;
//   ② The wrap layout splits words by spaces; Chinese has no spaces -> the whole paragraph becomes 1 over-long word -> cannot wrap by container.
//   Diagnosis (LayDiag/LayMeas 11:41): xStart=4, line width=346, word width=23×6 consecutive words.
//   Fix: after width measurement (0x4E2267, before edi=word width), check "word width + current x >= line width" (over width):
//     -> truncate the word buffer byte_569A38 to the **first complete character** (ASCII 1B / UTF-8 codepoint),
//        new word width = single char width (not over width -> engine draws normally);
//     -> String1 ([ebp+0x20]) falls back to the second character -> the outer loop continues collecting the rest -> per-character layout.
//   Effect: long words wrap by character (like LG4's single-char word splitting, but only triggered when over width -> English words wrap
//   as whole words normally, and briefing justify is unaffected).
// ===================================================================
}
static void MeasLog(int curX, int wordW, int lineW) {
    static int n = 0;
// 未超宽：不干预
// not over width: do not intervene
    if (n >= 24) return;
    n++;
    char buf[64] = {0};
// 单字符词（已最小）
// single-char word (already minimal)
    {
        const char* src = (const char*)0x569A38;
        int bi = 0;
        for (int i = 0; i < 63 && src[i]; i++) {
// UTF-8 lead：取完整码点
// UTF-8 lead: take the complete codepoint
            unsigned char c = (unsigned char)src[i];
            if (c >= 0x20 && c != 0x7F) buf[bi++] = (char)c;
            else if (bi > 0) break;
        }
        buf[bi] = 0;
// 整词就一个字符
// the whole word is just one character
    }
    LOG_INFO(kCat, "Meas[%d]: text='%s' | curX=%d wordW=%d lineW=%d '%s' -> %s",
// 截断词缓冲为首字符
// truncate word buffer to first character
             n, g_curText, curX, wordW, lineW, buf,
             (curX + wordW >= lineW) ? "OVERFLOW(换行)" : "ok");
}
static void WrapLog(int xBefore, int yBefore) {
// String1 回退到第二个字符
// String1 falls back to the second character
    static int n = 0;
    if (n >= 12) return;
    n++;

// ★ 容器左修复：sub_4E21B7 的 x 起始(a2) 引擎硬编码=4（屏幕左），动态容器内文本整体错位左侧。
//   布局上下文(a1)+0x40 = 容器右边界；行宽(a4) = 容器宽；容器左 = +0x40 - 行宽。
//   （实测 IP 场景：+0x40=440, 行宽=346 → 容器左=94。全屏表面 +0x40=0 → 不启用保持原值。）
//   ★ 11:57 用户反馈"缩进很多" → 改配置控制（ContainerLeftFix，默认 0=关，先修换行）。

// ★ Container-left fix: sub_4E21B7's x start (a2) is hard-coded to 4 (screen left) by the engine, so text inside dynamic containers is shifted entirely to the left.
//   Layout context (a1)+0x40 = container right edge; line width (a4) = container width; container left = +0x40 - line width.
//   (Measured IP scenario: +0x40=440, line width=346 -> container left=94. Fullscreen surface +0x40=0 -> not enabled, keep original value.)
//   ★ 11:57 user feedback "too much indent" -> changed to config control (ContainerLeftFix, default 0=off, fix wrapping first).
    char buf[64] = {0};
// 配置 ContainerLeftFix
// config ContainerLeftFix
    {

// ★ 已废弃：CaptureText 随 DrawTextStub 停用（20:2x 曾有 [ebp+0x20] 越界读崩溃史：
//   Game.exe.34112.dmp EIP 跳 0xCE500C50 = 栈破坏 → 该入口读法永久弃用）。
// 当前正在排版的完整文本（sub_4E21B7 入口捕获，供 Meas/Wrap 日志对照界面）

// ★ Deprecated: CaptureText was retired along with DrawTextStub (20:2x had a history of [ebp+0x20] out-of-bounds read crashes:
//   Game.exe.34112.dmp EIP jumped to 0xCE500C50 = stack corruption -> this entry's read method is permanently deprecated).
// The full text currently being laid out (captured at sub_4E21B7 entry, for cross-referencing Meas/Wrap logs with the UI)
        const char* src = (const char*)0x569A38;


        int bi = 0;
        for (int i = 0; i < 63 && src[i]; i++) {
            unsigned char c = (unsigned char)src[i];
            if (c >= 0x20 && c != 0x7F) buf[bi++] = (char)c;
            else if (bi > 0) break;
        }
        buf[bi] = 0;
    }
    LOG_INFO(kCat, "Wrap[%d]: text='%s' | xBefore=%d yBefore=%d '%s' (line break)",
             n, g_curText, xBefore, yBefore, buf);
}
extern "C" void __declspec(naked) DrawTextStub() {
    __asm {
        pushad
        mov  eax, [ebp + 0x18]
        push eax
        call CaptureText
        add  esp, 4


        popad
        push ecx
// 默认关闭（11:57 用户反馈缩进过头）
// default off (11:57 user feedback: too much indent)
        cmp  dword ptr [ebp + 0x18], 0
        mov  eax, 40ED01h
        jmp  eax
    }
}
static uintptr_t g_thunkWrap = 0;
extern "C" void __declspec(naked) WrapStub() {
    __asm {
// 无容器右/异常 → 保持
// no container right / anomaly -> keep
        pushad
        mov  eax, [esp + 16]
        push dword ptr [ebp - 4]
        push eax
        call WrapLog
// 容器左
// container left
        add  esp, 8
        popad

// ★ 换行诊断：记录每个词的排版参数（测宽点）+ 换行事件（0x4E2280）

// ★ Wrap diagnostics: record each word's layout params (at the width-measure point) + wrap events (0x4E2280)
        jmp  g_thunkWrap
    }
}
static uintptr_t g_thunkRepaint = 0;
    // 词缓冲 0x569A38（引擎 .data 绝对地址，NUL 结尾，≤100 字节）——打印当前词内容
    // Word buffer 0x569A38 (engine .data absolute address, NUL-terminated, ≤100 bytes) -- print the current word content
static int      g_inLinkRepaint = 0;
static int      g_linkLog = 0;
typedef void* (__thiscall* IterFn)(void* state);
static int RepaintLinkTokens(void* self, void* surface, void* layoutObj, int linkId, bool matchExact) {
    IterFn first = (IterFn)0x416CA0;
    IterFn next  = (IterFn)0x4D0BE0;
    typedef void(__thiscall* RepaintFn)(void*, void*, void*, int);
    RepaintFn repaint = (RepaintFn)0x4C9FC4;
// 遇控制符截断
// break on control char
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!layoutObj || !VirtualQuery((char*)layoutObj + 0x24, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (uintptr_t)layoutObj + 0x28 > (uintptr_t)mbi.BaseAddress + mbi.RegionSize)
        return 0;
    uint32_t listHead = *(const uint32_t*)((const char*)layoutObj + 0x24);
    if (listHead < 0x10000 || listHead >= 0x7FFFFFFF ||
        !VirtualQuery((const void*)listHead, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (uintptr_t)listHead + 12 > (uintptr_t)mbi.BaseAddress + mbi.RegionSize)
        return 0;
    int rows = 0, count = 0;
    uint8_t stRow[12] = { 0 };
    *(uint32_t*)&stRow[0] = listHead;
    // 词缓冲内容（触发换行的词）
    // Word buffer content (the word that triggered the wrap)
    for (void* row = first((void*)stRow); row && rows < 64 && count < 256; row = next((void*)stRow)) {
        if (!VirtualQuery(row, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (uintptr_t)row + 12 > (uintptr_t)mbi.BaseAddress + mbi.RegionSize) break;
        rows++;
        uint32_t subHead = *(const uint32_t*)((const char*)row + 8);
        if (subHead < 0x10000 || subHead >= 0x7FFFFFFF ||
            !VirtualQuery((const void*)subHead, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (uintptr_t)subHead + 12 > (uintptr_t)mbi.BaseAddress + mbi.RegionSize) continue;
        uint8_t stTok[12] = { 0 };
        *(uint32_t*)&stTok[0] = subHead;
        for (void* tok = first((void*)stTok); tok && count < 256; tok = next((void*)stTok)) {
            if (!VirtualQuery(tok, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
                (uintptr_t)tok + 40 > (uintptr_t)mbi.BaseAddress + mbi.RegionSize) break;
            if (!*(const uint8_t*)((const char*)tok + 32)) continue;

// ★ 已废弃（20:5x 起不再安装，用户放弃日志诊断"看不懂"）：sub_40ECF9 绘制入口捕获文本
//   （[ebp+0x18] 字符串参数）。保留作参考；g_curText 不再更新（日志 text='' 属预期）。

#pragma endregion

#pragma region "Deprecated Stub: DrawTextStub"
// ★ Deprecated (not installed since 20:5x; user gave up on log diagnostics "can't understand them"): sub_40ECF9 draw-entry text capture
//   ([ebp+0x18] string param). Kept for reference; g_curText is no longer updated (log text='' is expected).
            if (matchExact && *(const int*)((const char*)tok + 36) != linkId) continue;
            repaint(self, surface, tok, 1);
            count++;
        }
// 字符串（词缓冲/完整文本）
// string (word buffer / full text)
    }
    return count;
}
static void HoverLinkRepaint(void* self, void* surface, void* token) {
    if (g_inLinkRepaint) return;
    int linkId = *(const int*)((const char*)token + 36);
// 原 5 字节：push ecx
// original 5 bytes: push ecx
    if (linkId < 0) {
        typedef void(__thiscall* RepaintFn)(void*, void*, void*, int);
// cmp [ebp+0x18],0
// cmp [ebp+0x18],0
        ((RepaintFn)0x4C9FC4)(self, surface, token, 1);
        return;
// 跳过已执行，回 0x40ED01（push ebx）
// skip already-executed, return to 0x40ED01 (push ebx)
    }
    g_inLinkRepaint = 1;
    int repainted = RepaintLinkTokens(self, surface, self, linkId, /*matchExact=*/true);
    if (repainted == 0) {

// sub_4E21B7 换行分支 hook（0x4E2280，mov ebx,[ebp+0xC]; mov [ebp-4],eax = 6 字节）

#pragma endregion

#pragma region "Wrap-Branch Hook: WrapStub"
// sub_4E21B7 wrap-branch hook (0x4E2280, mov ebx,[ebp+0xC]; mov [ebp-4],eax = 6 bytes)
        repainted = RepaintLinkTokens(self, surface, self, linkId, /*matchExact=*/false);


    }
    if (repainted == 0) {
        typedef void(__thiscall* RepaintFn)(void*, void*, void*, int);
        ((RepaintFn)0x4C9FC4)(self, surface, token, 1);
// pushad 保存的 ebx = 换行前 x
// ebx saved by pushad = x before wrap
        repainted = 1;
    }
// 换行前 y（先 push）
// y before wrap (push first)
    if (g_linkLog < 6) {
        g_linkLog++;
// x（最后 push = 参数1）
// x (pushed last = param 1)
        LOG_INFO(kCat, "LinkHover[%d]: linkId=%d words=%d (repaint linked words)",
                 g_linkLog, linkId, repainted);
// WrapLog(xBefore, yBefore)
// WrapLog(xBefore, yBefore)
    }
    g_inLinkRepaint = 0;
}
extern "C" void __declspec(naked) HoverRepaintStub() {
// trampoline：原 6 字节 → 0x4E2286
// trampoline: original 6 bytes -> 0x4E2286
    __asm {
        pushad
        mov  eax, [esp + 44]

// ===================================================================
// ★ 富文本"词后空格"修复（最终方案 23:4x = **NOP 0x4CA612**，见 InstallHooks）
//   问题（用户实测 19:43）：WordSplitPatch=1 后所有内容完整但"你    好     世     界"分散。
//   机制（IDA 反编译 sub_4CA3C6 定案）：富文本排版遍历词 token，**每个词后固定加空格宽**
//   （0x4CA60D push 0x69 → sub_439633('i'宽=9) → 0x4CA612 add [esi+0x38],eax，x += 空格宽）。
//   原版英文词间距正常；补丁① 让词=单字节（SpaceHex 实测：词缓冲 = 单字节 GBK 首字节+NUL，
//   如 E8 00）→ 每个字节后 +9px → "分散/表格"（= 11:41 LayMeas wordW=23 之谜的成因之一）。
//   ★ 演进史（勿再走回头路）：
//     - 0x4CA556（case 3 图片 token）E9 hook：从未触发（任务界面走 case 2）→ 无效；
//     - 0x4CA612（case 2 词 token）E9 hook：连"纯执行原指令"的 stub 都导致**文字全消失**
//       （用户 22:38-23:01 四版实测；机制未明，推测 E9 跳转破坏某状态）→ 弃用；
//     - ★ 0x4CA612 **3 字节 NOP**（add -> 90 90 90）：不跳转/不碰寄存器/栈 → 安全，
//       x += 空格宽 取消 → 中文紧凑（用户 23:41 实测本版正常）。sub_4C9DAD 的 `x-=9`
//       回退在左对齐（this+21=0）时无害。
// ===================================================================
// ===================================================================
// ★ 00:5x hover 变亮重绘诊断（sub_4C9FC4 = token 重绘函数，10 字节 trampoline）
//   ★ 00:5x 定案：sub_4C9FC4 是**通用重绘**——两个调用者：
//     - sub_4C9F47（普通重绘，每帧遍历 token：sub_4C9FC4(this, a2, i, **0**)）
//     - sub_4CA218（hover 检测，命中链接 +32=1 → sub_4C9FC4(a2, result, **1**)）
//   a4=1 才走变亮路径（sub_40F884(0,1.5,1.5,1.5) → sub_40FABA 调色板 → sub_40ECF9 重绘）。
//   ★ 诊断策略：stub 内先判断 a4==1（hover）才记录——普通重绘（a4=0）高频刷屏会
//     占满限次挡住 hover 调用（00:58 日志 20 条 hover=0 全是 sub_4C9F47 的教训）。
// ===================================================================

#pragma endregion

#pragma region "Rich-Text Word-Space NOP (doc)"
// ===================================================================
// ★ Rich-text "space-after-word" fix (final solution 23:4x = **NOP 0x4CA612**, see InstallHooks)
//   Problem (user-observed 19:43): after WordSplitPatch=1 everything is complete but the glyphs "ni hao shi jie" are spread out.
//   Mechanism (confirmed via IDA decompilation of sub_4CA3C6): rich-text layout iterates word tokens and **adds a fixed space width after every word**
//   (0x4CA60D push 0x69 -> sub_439633('i' width=9) -> 0x4CA612 add [esi+0x38],eax, x += space width).
//   The original English word spacing is normal; patch ① makes words single-byte (SpaceHex test: word buffer = single-byte GBK lead byte + NUL,
//   e.g. E8 00) -> +9px after every byte -> "spread/table" (one cause of the 11:41 LayMeas wordW=23 mystery).
//   ★ Evolution history (do not go back):
//     - 0x4CA556 (case 3 image token) E9 hook: never triggered (the quest UI uses case 2) -> ineffective;
//     - 0x4CA612 (case 2 word token) E9 hook: even a stub that "just executed the original instruction" caused **all text to disappear**
//       (user tested 4 versions 22:38-23:01; mechanism unclear, suspected E9 jump corrupts some state) -> deprecated;
//     - ★ 0x4CA612 **3-byte NOP** (add -> 90 90 90): no jump / no register or stack touched -> safe,
//       x += space width is cancelled -> Chinese becomes compact (user verified this version works at 23:41). sub_4C9DAD's `x-=9`
//       fallback is harmless under left alignment (this+21=0).
// ===================================================================
#pragma endregion

#pragma region "Hover Whole-Sentence Brighten"
// ===================================================================
// ★ 00:5x hover-brighten redraw diagnostics (sub_4C9FC4 = token redraw function, 10-byte trampoline)
//   ★ 00:5x decision: sub_4C9FC4 is **general redraw** -- two callers:
//     - sub_4C9F47 (normal redraw, iterates tokens every frame: sub_4C9FC4(this, a2, i, **0**))
//     - sub_4CA218 (hover detection, on link hit +32=1 -> sub_4C9FC4(a2, result, **1**))
//   Only a4=1 takes the brighten path (sub_40F884(0,1.5,1.5,1.5) -> sub_40FABA palette -> sub_40ECF9 redraw).
//   ★ Diagnostic strategy: the stub first checks a4==1 (hover) before logging -- normal redraw (a4=0) is high-frequency screen refresh that would
//     fill up the limited log count and block hover calls (lesson from 00:58: all 20 log entries had hover=0, from sub_4C9F47).
// ===================================================================
// ★ 08-13 whole-sentence brighten (hover hits a link word -> redraw all words of the link, the whole sentence lights up).
//   When the engine's hover detection calls sub_4C9FC4 (__thiscall this=layout object, surface, token, a4=1):
//   if the token is a link word (+32 link flag=1), iterate the layout object (row list at +0x24) to find tokens with the same linkId (+36)
//   and repaint each (self, surface, tok, 1) -> the whole sentence lights up.
//   ★ Calling convention (confirmed via IDA disassembly of sub_4CA218): repaint must take 4 params -- (this, surface, token, brighten=1);
//     only a4=1 takes the brighten path, and surface must be the same object as the original call, otherwise it draws to the wrong target.
//   ★ Crash protection: g_inLinkRepaint prevents reentrancy; the row/word iterators each have their own independent 12-byte state buffer;
//     VirtualQuery guards throughout; caps rows≤64 / words≤256; on linkId match failure, fall back to iterating the whole sentence / single char.
        test eax, eax

// ★ 08-13 整句变亮（hover 命中链接词 → 重绘整链接所有词，整句一起亮）。
//   sub_4C9FC4(__thiscall this=排版对象, surface, token, a4=1) 被引擎 hover 检测调用时：
//   若 token 是链接词（+32 链接标志=1），遍历排版对象（行列表在 +0x24）找同 linkId（+36）
//   的所有词逐个 repaint(self, surface, tok, 1) → 整句一起亮。
//   ★ 调用约定（IDA 反汇编 sub_4CA218 定案）：repaint 必须 4 参——(this, surface, token, brighten=1)；
//     a4=1 才走变亮路径，surface 须与原始调用同一对象，否则画到错误目标。
//   ★ 防崩溃：g_inLinkRepaint 防重入；行/词迭代器各自独立 12 字节 state buffer；
//     VirtualQuery 全程守卫；rows≤64 / words≤256 上限；linkId 匹配失败时兜底遍历整句/单字亮。

        je   L_hr_tramp
        mov  edx, OFFSET g_inLinkRepaint
        cmp  dword ptr [edx], 1

// 遍历排版对象所有行→词，对链接词（token+32 标志=1）调用 repaint(self, surface, tok, 1)。
// matchExact=true 仅重绘 linkId 完全相等的词；false 重绘所有链接词（兜底：linkId 未传播时整段亮）。
// 返回重绘词数。任何越界/野指针经 VirtualQuery 守卫立即停止，绝不崩。

// Iterate all rows->words of the layout object, calling repaint(self, surface, tok, 1) on link words (token+32 flag=1).
// matchExact=true repaints only words with an exactly equal linkId; false repaints all link words (fallback: when linkId was not propagated, the whole paragraph lights up).
// Returns the number of repainted words. Any OOB / wild pointer stops immediately under VirtualQuery guard; never crashes.
        je   L_hr_tramp
        mov  ebx, [esp + 40]
        test ebx, ebx
        je   L_hr_tramp
        cmp  byte ptr [ebx + 32], 0
        je   L_hr_tramp
    // 行列表头 = 排版对象 +0x24（sub_4C9F47 反汇编定案）
    // Row list head = layout object +0x24 (confirmed via sub_4C9F47 disassembly)
        mov  ecx, [esp + 4]
        mov  eax, [esp + 36]
        push ebx
        push eax
        push ecx
        call HoverLinkRepaint
        add  esp, 12
    L_hr_tramp:
        popad
    // ★ 行/词迭代器用各自独立的 12 字节 state（仿 sub_4C9F47：v6[2] 行、v8[2] 词）。
    //   共享同一块 buffer 会让内层 first/next 覆盖外层的 current/flag，破坏外层 next。
    // ★ The row/word iterators each use their own independent 12-byte state (after sub_4C9F47: v6[2] for rows, v8[2] for words).
    //   Sharing one buffer would let the inner first/next overwrite the outer current/flag, breaking the outer next.
        jmp  g_thunkRepaint
    }
// 仅填 head；[4]/[8] 由 {0} 初始化为 0
// only fill head; [4]/[8] are zero-initialized by {0}
}
static uintptr_t g_thunkLayEntry = 0;
extern "C" void __declspec(naked) LayoutEntryStub() {
    __asm {
        pushad
        mov  eax, [ebp + 0x0C]
// 行+8 = 词列表头（sub_4C9F47: v8[0]=row[2]）
// row+8 = word list head (sub_4C9F47: v8[0]=row[2])
        push eax
        mov  eax, [ebp + 0x14]
        push eax
        mov  eax, [ebp + 0x08]
        push eax
        call FixLayoutXStart
        add  esp, 12
        mov  [ebp + 0x0C], eax
        popad
        jmp  g_thunkLayEntry
// 非链接词（+0x20 标志，sub_4CA218 判定依据）
// non-link word (+0x20 flag, the basis sub_4CA218 checks)
    }
}
extern "C" void __declspec(naked) OverflowStub() {
    __asm {
        pushad
        mov  eax, [esp + 28]
        push dword ptr [ebp + 0x14]
        push eax


        push ebx
        call MeasLog
// 重入保护（repaint 内部再进 stub → 直接 trampoline）
// reentrancy guard (if repaint re-enters the stub internally -> go straight to trampoline)
        add  esp, 12
        mov  eax, [esp + 28]
    // 非链接词（linkId<0）→ 引擎原逻辑：单字变亮
    // non-link word (linkId<0) -> engine original logic: single char brightens
        push eax
        lea  ecx, [ebp + 0x20]
        push ecx
        mov  eax, [ebp + 0x14]
        push eax
        push ebx
    // ★ self = sub_4C9FC4 的 this = 排版对象（行列表在 +0x24），同时作为 repaint 的 this。
    // ★ self = sub_4C9FC4's this = layout object (row list at +0x24), also used as repaint's this.
        push 0x569A38
        call FixOverflowWord
        // linkId 未传播/不一致 → 兜底重绘本排版对象内所有链接词（整句/整段亮，绝优于逐字）
        // linkId not propagated / inconsistent -> fallback repaint all link words in this layout object (whole sentence/paragraph lights up, strictly better than per-char)
        add  esp, 20
        mov  [esp + 28], eax
        popad
        // 仍无链接词 → 退化单字亮（兜底，不崩）
        // still no link words -> degrade to single-char brighten (fallback, no crash)
        mov  ecx, [ebp + 8]
        mov  edi, eax
        mov  eax, 4E226Ch
        jmp  eax
    }
}
static uintptr_t MakeTrampoline(uintptr_t entry, int copyLen) {
    uintptr_t t = (uintptr_t)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    auto orig = Patch::ReadBytes(entry, copyLen);
    if (orig.size() != (size_t)copyLen) { VirtualFree((void*)t, 0, MEM_RELEASE); return 0; }


    memcpy((void*)t, orig.data(), copyLen);
    uintptr_t back = entry + copyLen;
    *(uint8_t*)(t + copyLen) = 0xE9;
        // ★ sub_4C9FC4 入口 ESP（pushad 前）：[esp+0]=返回地址、[esp+4]=a2(绘制surface)、
        //   [esp+8]=a3(token)、[esp+0xC]=a4(hover)。ECX=this(排版对象)。
        //   pushad 后偏移 +32：this=[esp+4]、a2(surface)=[esp+36]、token=[esp+40]、a4=[esp+44]。
        // ★ sub_4C9FC4 entry ESP (before pushad): [esp+0]=return addr, [esp+4]=a2 (draw surface),
        //   [esp+8]=a3 (token), [esp+0xC]=a4 (hover). ECX=this (layout object).
        //   after pushad offset +32: this=[esp+4], a2 (surface)=[esp+36], token=[esp+40], a4=[esp+44].
    *(int32_t*)(t + copyLen + 1) = (int32_t)(back - (t + copyLen + 5));
// a4（hover 标志）
// a4 (hover flag)
    return t;
}
static void InstallHooks(uintptr_t base) {
// ★ 普通重绘（a4=0）：直接 trampoline
// ★ normal redraw (a4=0): go straight to trampoline
    uintptr_t eGlyph = base + (0x439610 - 0x400000);
    g_thunkGlyph = MakeTrampoline(eGlyph, 6);
    if (!g_thunkGlyph) { LOG_ERROR(kCat, "InstallHooks: glyph trampoline failed"); return; }
    if (!Patch::WriteJmp(eGlyph, (uintptr_t)&GlyphDrawStub, 3)) {
// ★ 重入（repaint 内部再进本 stub）：直接 trampoline 重绘该字
// ★ reentrancy (repaint re-enters this stub internally): go straight to trampoline and redraw this char
        LOG_ERROR(kCat, "InstallHooks: WriteJmp failed @0x%X", (unsigned)eGlyph); return;
    }
// a3（token）
// a3 (token)
    uintptr_t eWidth = base + (0x439633 - 0x400000);
    g_thunkWidth = MakeTrampoline(eWidth, 5);
    if (!g_thunkWidth) { LOG_ERROR(kCat, "InstallHooks: width trampoline failed"); return; }
    if (!Patch::WriteJmp(eWidth, (uintptr_t)&GlyphWidthStub, 3)) {
// token+32 = 链接标志
// token+32 = link flag
        LOG_ERROR(kCat, "InstallHooks: WriteJmp failed @0x%X", (unsigned)eWidth); return;
    }
// 非链接词 → 引擎原逻辑（重绘单词，单字亮）
// non-link word -> engine original logic (redraw single word, single char brightens)
    LOG_INFO(kCat, "InstallHooks: hooked glyph chokepoint sub_439610 + width sub_439633 "
        // 链接词 → 整句变亮：遍历同 linkId 所有词逐个 repaint（传 surface 保证画到同一目标）。
        // ★ self = sub_4C9FC4 的 this（= 排版对象，行列表在 +0x24），位于 [esp+4]（ECX）。
        //   surface = sub_4C9FC4 的 a2（绘制surface），位于 [esp+36]。二者都要传给 HoverLinkRepaint。
        // link word -> whole-sentence brighten: iterate all words with the same linkId and repaint each (pass surface to ensure drawing to the same target).
        // ★ self = sub_4C9FC4's this (= layout object, row list at +0x24), located at [esp+4] (ECX).
        //   surface = sub_4C9FC4's a2 (draw surface), located at [esp+36]. Both must be passed to HoverLinkRepaint.
                    "(ASCII relays to engine; CJK -> GDI).");
// self（排版对象）
// self (layout object)
    uintptr_t eWordCol = base + (0x4E2242 - 0x400000);
    auto curBytes = Patch::ReadBytes(eWordCol, 5);
// surface（绘制surface）
// surface (draw surface)
    if (curBytes.size() == 5 && curBytes[0] == 0x80 && curBytes[1] == 0xF9 &&
        curBytes[2] == 0x7B && curBytes[3] == 0x74) {
// 参数3：token
// param3: token
        g_thunkWordCol = MakeTrampoline(eWordCol, 5);
        if (!g_thunkWordCol) { LOG_ERROR(kCat, "InstallHooks: wordcol trampoline failed"); return; }
// 参数2：surface
// param2: surface
        if (!Patch::WriteJmp(eWordCol, (uintptr_t)&WordColStub)) {
            LOG_ERROR(kCat, "InstallHooks: WriteJmp failed @0x%X (wordcol)", (unsigned)eWordCol); return;
// 参数1：self
// param1: self
        }
        LOG_INFO(kCat, "InstallHooks: hooked sub_4E21B7 word-collect (CJK single-codepoint word).");
    } else {
        LOG_WARN(kCat, "InstallHooks: wordcol bytes mismatch @0x%X (skip)", (unsigned)eWordCol);
    }
    uintptr_t eLayEntry = base + (0x4E21C7 - 0x400000);
// trampoline：抄 10 字节 → 0x4C9FCE（push esi）
// trampoline: copied 10 bytes -> 0x4C9FCE (push esi)
    auto eb1 = Patch::ReadBytes(eLayEntry, 5);
    if (eb1.size() == 5 && eb1[0] == 0x8B && eb1[1] == 0x45 && eb1[2] == 0x10 && eb1[3] == 0x53) {
        g_thunkLayEntry = MakeTrampoline(eLayEntry, 5);

// sub_4E21B7 入口 hook（0x4E21C7，mov eax,[ebp+0x10]; push ebx; push esi）

#pragma endregion

#pragma region "Layout-Entry Hook: LayoutEntryStub"
// sub_4E21B7 entry hook (0x4E21C7, mov eax,[ebp+0x10]; push ebx; push esi)
        if (g_thunkLayEntry && Patch::WriteJmp(eLayEntry, (uintptr_t)&LayoutEntryStub))


            LOG_INFO(kCat, "InstallHooks: hooked sub_4E21B7 entry (container-left x-fix, %s).",
                     g_containerLeftFix ? "ON" : "OFF");
    } else LOG_WARN(kCat, "InstallHooks: layEntry bytes mismatch (skip)");
        // ★ 20:2x 崩溃根因：sub_4E21B7 参数可能 <7，[ebp+0x20] 越界读 → 栈破坏（EIP 跳 0xCE500C50）
        //   CaptureText/DrawTextStub 均已停用（20:5x）；这里只做 FixLayoutXStart（容器左修复）
        // FixLayoutXStart(ctx=[ebp+8], lineW=[ebp+14], origX=[ebp+0C])
        // ★ 20:2x crash root cause: sub_4E21B7 args may be <7, [ebp+0x20] out-of-bounds read -> stack corruption (EIP jumped to 0xCE500C50)
        //   CaptureText/DrawTextStub are both retired (20:5x); here we only do FixLayoutXStart (container-left fix)
        // FixLayoutXStart(ctx=[ebp+8], lineW=[ebp+14], origX=[ebp+0C])
    uintptr_t eWrap = base + (0x4E2280 - 0x400000);
    auto ew = Patch::ReadBytes(eWrap, 6);
// origX
// origX
    if (ew.size() == 6 && ew[0] == 0x8B && ew[1] == 0x5D && ew[2] == 0x0C && ew[3] == 0x89) {
        g_thunkWrap = MakeTrampoline(eWrap, 6);
        if (g_thunkWrap && Patch::WriteJmp(eWrap, (uintptr_t)&WrapStub))
// lineW
// lineW
            LOG_INFO(kCat, "InstallHooks: hooked sub_4E21B7 wrap branch (line-break diag).");
    } else LOG_WARN(kCat, "InstallHooks: wrap bytes mismatch (skip)");
    uintptr_t eSpace = base + (0x4CA612 - 0x400000);
// ctx
// ctx
    auto espc = Patch::ReadBytes(eSpace, 6);
    if (espc.size() == 6 && espc[0] == 0x01 && espc[1] == 0x46 && espc[2] == 0x38 &&
        espc[4] == 0x45 && espc[5] == 0xF8) {
        const uint8_t nop3[3] = { 0x90, 0x90, 0x90 };
// 改 a2（x 起始）
// modify a2 (x start)
        if (Patch::WriteBytes(eSpace, nop3, 3))
            LOG_INFO(kCat, "InstallHooks: NOP'd rich-text word-space (0x4CA612 add[esi+38h],eax -> NOP, CJK compact).");
        else
// trampoline：原 5 字节 → 0x4E21CC
// trampoline: original 5 bytes -> 0x4E21CC
            LOG_ERROR(kCat, "InstallHooks: NOP 0x4CA612 FAIL");
    } else LOG_WARN(kCat, "InstallHooks: space-fix bytes mismatch @0x4CA612 (skip)");
    uintptr_t eLink = base + (0x4CA604 - 0x400000);


#pragma endregion

#pragma region "Overflow Wrap Hook: OverflowStub"
    auto el = Patch::ReadBytes(eLink, 4);
    if (el.size() == 4 && el[0] == 0x80 && el[1] == 0x66 && el[2] == 0x48 && el[3] == 0x00) {
        const uint8_t nop4[4] = { 0x90, 0x90, 0x90, 0x90 };
        // MeasLog(curX=ebx, wordW=原eax, lineW=[ebp+14]) —— 参数顺序修正（19:5x 曾错位）
        // MeasLog(curX=ebx, wordW=original eax, lineW=[ebp+14]) -- parameter order corrected (was misordered at 19:5x)
        if (Patch::WriteBytes(eLink, nop4, 4))
// 原 eax = 词宽
// original eax = word width
            LOG_INFO(kCat, "InstallHooks: NOP'd link-state reset (0x4CA604, link spans all words).");
        else
// lineW（先 push）
// lineW (push first)
            LOG_ERROR(kCat, "InstallHooks: NOP 0x4CA604 FAIL");
    } else LOG_WARN(kCat, "InstallHooks: link-state bytes mismatch @0x4CA604 (skip)");
// wordW
// wordW
    uintptr_t eRepaint = base + (0x4C9FC4 - 0x400000);
    auto er = Patch::ReadBytes(eRepaint, 10);
// curX（最后 push = 参数1）
// curX (pushed last = param 1)
    if (er.size() == 10 && er[0] == 0x55 && er[1] == 0x8B && er[2] == 0xEC &&
        er[3] == 0x81 && er[4] == 0xEC && er[9] == 0x53) {
        g_thunkRepaint = MakeTrampoline(eRepaint, 10);
        // 参数：FixOverflowWord(buf, curX, lineW, &[ebp+0x20], wordW)
        // Args: FixOverflowWord(buf, curX, lineW, &[ebp+0x20], wordW)
        if (g_thunkRepaint && Patch::WriteJmp(eRepaint, (uintptr_t)&HoverRepaintStub))
// 原 eax = 词宽（sub_43967D 返回值）
// original eax = word width (return value of sub_43967D)
            LOG_INFO(kCat, "InstallHooks: hooked sub_4C9FC4 hover-repaint (whole-sentence brighten).");
    } else LOG_WARN(kCat, "InstallHooks: repaint bytes mismatch @0x4C9FC4 (skip)");
// wordW
// wordW
    uintptr_t eLayMeas = base + (0x4E2267 - 0x400000);
    auto eb2 = Patch::ReadBytes(eLayMeas, 5);
// &String1
// &String1
    if (eb2.size() == 5 && eb2[0] == 0x8B && eb2[1] == 0x4D && eb2[2] == 0x08 &&
        eb2[3] == 0x8B && eb2[4] == 0xF8) {
// &strPtr
// &strPtr
        if (Patch::WriteJmp(eLayMeas, (uintptr_t)&OverflowStub))
            LOG_INFO(kCat, "InstallHooks: hooked sub_4E21B7 measure-after (overflow word-wrap).");
// 行宽
// line width
    } else LOG_WARN(kCat, "InstallHooks: layMeas bytes mismatch (skip)");
}
// lineW
// lineW
extern "C" void __declspec(naked) SepStub() {
    __asm {
// curX（pushad 后 ebx 未动 = 当前 x）
// curX (after pushad ebx is unchanged = current x)
        push 1Ch
        mov  eax, 4D053Fh
// 词缓冲（绝对地址）
// word buffer (absolute address)
        jmp  eax
    }
}
extern "C" void __declspec(naked) CJKWordColStub() {
// 新词宽 → pushad 保存的 eax
// new word width -> eax saved by pushad
    __asm {
        cmp  al, 0x80
        // 原 5 字节逻辑：mov ecx,[ebp+8]; mov edi,eax（edi = 词宽）
        // original 5-byte logic: mov ecx,[ebp+8]; mov edi,eax (edi = word width)
        jae  L_wc_cjk
        mov  ecx, edi
    L_wc_scan:
// 跳过已执行部分，回 0x4E226C（超宽判断）
// skip already-executed part, return to 0x4E226C (over-width check)
        mov  al, byte ptr [ecx]
        cmp  al, 0
        je   L_wc_end
        cmp  al, 32

// 抄 copyLen 原字节 + E9 跳回 entry+copyLen

#pragma endregion

#pragma region "Trampoline Builder"
// Copy copyLen original bytes + E9 jump back to entry+copyLen
        je   L_wc_space
        cmp  al, 9
        je   L_wc_end
        cmp  al, 10
        je   L_wc_end
        cmp  al, 13
        je   L_wc_end
        cmp  al, 60
        je   L_wc_end
        cmp  al, 92
        je   L_wc_bs


#pragma endregion

#pragma region "InstallHooks"
        cmp  al, 0x80
    // sub_439610：抄 6 字节（落点 0x439616）
    // sub_439610: copy 6 bytes (landing at 0x439616)
        jae  L_wc_end
        inc  ecx
        jmp  L_wc_scan
    L_wc_bs:
        cmp  byte ptr [ecx + 1], 0x6E
        je   L_wc_end
    // sub_439633：抄 5 字节（落点 0x439638）
    // sub_439633: copy 5 bytes (landing at 0x439638)
        inc  ecx
        jmp  L_wc_scan
    L_wc_space:
        inc  ecx
    L_wc_end:
        mov  edi, ecx
        push 1Ch
        mov  eax, 4D053Fh

    // ★ 11:0x sub_4E21B7 词收集循环 CJK 单码点 hook（中文按码点换行）

    // ★ 11:0x sub_4E21B7 word-collect loop CJK single-codepoint hook (Chinese wraps by codepoint)
        jmp  eax
    L_wc_cjk:
        cmp  al, 0xC0
        jb   L_wc_cjk1
        cmp  al, 0xE0
        jb   L_wc_cjk2
        cmp  al, 0xF0
        jb   L_wc_cjk3
        add  edi, 4
        jmp  L_wc_cjk_fin
    L_wc_cjk3:
        add  edi, 3
        jmp  L_wc_cjk_fin

    // ★ 11:5x 容器左修复 hook（sub_4E21B7 入口 0x4E21C7）：
    //   x 起始(a2) 引擎硬编码 4 → 改为 容器右(+0x40) - 行宽（动态容器内文本不溢出左侧）

    // ★ 11:5x container-left fix hook (sub_4E21B7 entry 0x4E21C7):
    //   x start (a2) hard-coded to 4 by engine -> change to container right (+0x40) - line width (text inside dynamic containers won't overflow left)
    L_wc_cjk2:
        add  edi, 2
        jmp  L_wc_cjk_fin
    L_wc_cjk1:
        inc  edi
        jmp  L_wc_cjk_fin
    L_wc_cjk_fin:
        push 1Ch

    // ★ 11:57 换行诊断 hook（sub_4E21B7 换行分支 0x4E2280，6 字节）——记录换行事件

    // ★ 11:57 wrap-diagnostic hook (sub_4E21B7 wrap branch 0x4E2280, 6 bytes) -- record wrap events
        mov  eax, 4D053Fh
        jmp  eax
    }
}
extern "C" void __declspec(naked) TagEntryStub() {
    __asm {
        pushad

    // ★ 20:2x 绘制文本捕获 hook（sub_40ECF9 入口 0x40ECFC，5 字节）——安全版（[ebp+0x18] 参数可靠）
    // ★★ 20:5x 停用：用户已放弃日志诊断（"日志看不懂"），DrawTextStub 不再安装（避免无谓拦截）
    // uintptr_t eDrawTxt = base + (0x40ECFC - 0x400000);
    // auto edt = Patch::ReadBytes(eDrawTxt, 5);
    // if (edt.size() == 5 && edt[0] == 0x51 && edt[1] == 0x83 && edt[2] == 0x7D && edt[3] == 0x18) {
    //     if (Patch::WriteJmp(eDrawTxt, (uintptr_t)&DrawTextStub))
    //         LOG_INFO(kCat, "InstallHooks: hooked sub_40ECF9 draw-entry (text capture, safe).");
    // } else LOG_WARN(kCat, "InstallHooks: drawtext bytes mismatch (skip)");

    // ★ 23:0x 定案：0x4CA612 E9 hook 连纯跳转 stub 都导致文字消失 → **改用 3 字节 NOP**：
    //   把 `add [esi+38h],eax`（x += 空格宽）NOP 掉 → 词后空格=0 → 中文紧凑。
    //   ★ NOP 不跳转、不碰寄存器/栈，规避 E9 hook 的消失问题（用户 23:32 实测短文本正常=引擎原逻辑 OK）。
    //   ★ 配合 WordSplitPatch=1（词=单字节 → 不超宽 → 长文本完整）+ sub_4C9DAD 的 x-=9 在
    //   左对齐(this+21=0)时无害。

    // ★ 20:2x draw-text capture hook (sub_40ECF9 entry 0x40ECFC, 5 bytes) -- safe version ([ebp+0x18] param reliable)
    // ★★ 20:5x retired: the user gave up on log diagnostics ("can't understand the logs"), DrawTextStub is no longer installed (avoid pointless interception)
    // uintptr_t eDrawTxt = base + (0x40ECFC - 0x400000);
    // auto edt = Patch::ReadBytes(eDrawTxt, 5);
    // if (edt.size() == 5 && edt[0] == 0x51 && edt[1] == 0x83 && edt[2] == 0x7D && edt[3] == 0x18) {
    //     if (Patch::WriteJmp(eDrawTxt, (uintptr_t)&DrawTextStub))
    //         LOG_INFO(kCat, "InstallHooks: hooked sub_40ECF9 draw-entry (text capture, safe).");
    // } else LOG_WARN(kCat, "InstallHooks: drawtext bytes mismatch (skip)");

    // ★ 23:0x decision: the 0x4CA612 E9 hook caused text to disappear even with a pure-jump stub -> **switched to a 3-byte NOP**:
    //   NOP out `add [esi+38h],eax` (x += space width) -> space after word = 0 -> Chinese compact.
    //   ★ NOP does not jump and touches no registers/stack, avoiding the disappearance problem of the E9 hook (user verified at 23:32 that short text is normal = engine original logic OK).
    //   ★ Combined with WordSplitPatch=1 (word=single byte -> not over width -> long text complete) + sub_4C9DAD's x-=9 is
    //   harmless under left alignment (this+21=0).
        movzx eax, byte ptr [edi + 1]
        cmp  al, 2Fh
        jne  L_tg_notend
        mov  eax, OFFSET g_inLink
        mov  byte ptr [eax], 0
        jmp  L_tg_done
    L_tg_notend:
        cmp  al, 61h
        jne  L_tg_clear
        cmp  byte ptr [edi + 2], 6Eh

    // ★ 00:3x 超链接修复：0x4CA604 `and byte ptr [esi+48h],0`（case 2 词后**链接状态重置**）→ NOP。
    //   机制（IDA 逆向）：<anch=ID> 标签 → case 7/8 设 this+72=1（链接状态）+ this+19=ID；
    //   case 2 词 token 若 this+72 → 词带链接标志(+32=1,+36=ID)，然后 0x4CA604 **重置** this+72=0
    //   → 只有锚点后**第一个词**是链接。命中检测 sub_4CA334：行垂直命中 → 词 token +4 矩形
    //   （sub_4397C0 点-矩形）→ 返回命中词 → sub_4CA240 看 +36 激活。
    //   WordSplitPatch=1（词=单字节）→ 链接文字拆成 N 词 → 只有第 1 词带链接标志 → 命中矩形
    //   =1 字符 → 点击其余链接文字无效（用户 00:1x 实测"超链接无法触发"；=0 时词=整段正常）。
    //   ★ NOP 后：链接状态保持到 case 7（this+19=-1 结束，普通词 +36=-1 → sub_4CA240 判非链接）
    //   或排版结束（sub_4CA3C6 开头 this+72=0 重置）→ 链接内所有词都带标志 → 命中任意字符激活。

    // ★ 00:3x hyperlink fix: 0x4CA604 `and byte ptr [esi+48h],0` (case 2 word-after **link-state reset**) -> NOP.
    //   Mechanism (IDA reverse): the <anch=ID> tag -> case 7/8 set this+72=1 (link state) + this+19=ID;
    //   case 2 word token, if this+72 -> the word carries the link flag (+32=1, +36=ID), then 0x4CA604 **resets** this+72=0
    //   -> only the **first word** after the anchor is a link. Hit detection sub_4CA334: vertical row hit -> word token +4 rectangle
    //   (sub_4397C0 point-rect) -> returns hit word -> sub_4CA240 checks +36 to activate.
    //   WordSplitPatch=1 (word=single byte) -> link text splits into N words -> only the 1st word carries the link flag -> hit rectangle
    //   =1 char -> clicking the rest of the link text does nothing (user verified at 00:1x "hyperlinks cannot trigger"; =0 makes word=whole paragraph, normal).
    //   ★ After NOP: link state persists until case 7 (this+19=-1 ends, normal word +36=-1 -> sub_4CA240 judges non-link)
    //   or layout end (sub_4CA3C6 sets this+72=0 at the start) -> all words inside the link carry the flag -> hitting any char activates.
        jne  L_tg_clear
        cmp  byte ptr [edi + 3], 63h
        jne  L_tg_clear
        cmp  byte ptr [edi + 4], 68h
        jne  L_tg_clear
        mov  eax, OFFSET g_inLink
        mov  byte ptr [eax], 1
        jmp  L_tg_done
    L_tg_clear:

    // ★ 08-13 hover 整句变亮（sub_4C9FC4 = token 重绘函数，10 字节 trampoline）——
    //   引擎 hover 检测命中链接词后调用 sub_4C9FC4(this, surface, token, a4=1)；
    //   HoverRepaintStub 在此拦截，遍历同 linkId 所有词逐个 repaint（整句一起亮）。

    // ★ 08-13 hover whole-sentence brighten (sub_4C9FC4 = token redraw function, 10-byte trampoline) --
    //   after the engine's hover detection hits a link word it calls sub_4C9FC4(this, surface, token, a4=1);
    //   HoverRepaintStub intercepts here, iterating all words with the same linkId and repainting each (whole sentence lights up).
        mov  eax, OFFSET g_inLink
        mov  byte ptr [eax], 0
    L_tg_done:
        popad
        lea  eax, [edi + 1]
        push 3Eh
        mov  eax, 4CFF05h
        jmp  eax

    // ★ 11:4x 超宽词字符级折行 hook（0x4E2267 测宽后，LG4 同款点）：
    //   超宽词截断为单字符 + String1 推进 → 引擎逐字符排版（修 IP/长词溢出、不显示）

    // ★ 11:4x over-wide word character-level wrap hook (after width measurement at 0x4E2267, same point as LG4):
    //   Truncate over-wide word to a single char + advance String1 -> engine lays out character by character (fixes IP/long-word overflow, not shown)
    }
}
static void InstallWordSplitPatches(uintptr_t base) {
    uintptr_t eWc = base + (0x4D053A - 0x400000);
    auto cur = Patch::ReadBytes(eWc, 5);
    if (cur.size() == 5 && cur[0] == 0x47 && cur[1] == 0xEB && cur[2] == 0xD9 &&
        cur[3] == 0x6A && cur[4] == 0x1C) {
        if (Patch::WriteJmp(eWc, (uintptr_t)&CJKWordColStub))

// ===================================================================
// ★ 08-13 修复"文字全部消失"（07:09 之后仍消失）：双跳转岛方案（不依赖 code cave）
//   根因：0x4D053A 的 5 字节 E9 覆盖 0x4D053D（分隔符→词结束 push 1Ch），而原循环 7 个
//   `je 0x4d053d`（空格/边界bl/tab/换行/CR/'<'/'\n'）在真实文本（含空格/标签）下跳进垃圾
//   字节 → 整段解析崩坏 → 文字全消失。01:5x 仅因测"你好世界"（无空格）侥幸未触发。
//   修复：用 0x4D0530 / 0x4D053A 两个 5 字节点做跳转岛：
//     - 0x4D0530 → CJKCharStub：非分隔符字符路径（码点/链接整段收集）
//     - 0x4D053A → SepStub      ：push 1Ch; jmp 0x4D053F（词结束，分隔符安全收尾）
//   6 个 je 的 1 字节位移改跳 0x4D053A（均在 ±127，rel8 够）。0x4D053D 虽被覆盖但永
//   不可达 → 安全。CJK 词=1 码点（不超宽+词缓冲完整），链接内词=整段（整句变亮）。
//   寄存器契约（入口 0x4D0530 时）：al=[edi]（0x4D0516 已读）、esi=词起始、edi=扫描指针。
// ===================================================================

#pragma endregion

#pragma region "Word-Split Patches"
// ===================================================================
// ★ 08-13 fix for "all text disappears" (still disappearing after 07:09): two-jump-island approach (no code cave dependency)
//   Root cause: the 5-byte E9 at 0x4D053A overwrote 0x4D053D (delimiter->word-end push 1Ch), and the original loop's 7
//   `je 0x4d053d` (space/boundary bl/tab/newline/CR/'<'/'\n') jump into garbage under real text (with spaces/tags)
//   bytes -> the whole parse breaks -> all text disappears. 01:5x only got lucky because it tested "nihao shijie" (no spaces).
//   Fix: use two 5-byte jump islands at 0x4D0530 / 0x4D053A:
//     - 0x4D0530 -> CJKCharStub: non-delimiter char path (codepoint / link whole-segment collect)
//     - 0x4D053A -> SepStub      : push 1Ch; jmp 0x4D053F (word end, delimiter safely closed)
//   6 je 1-byte displacements changed to jump to 0x4D053A (all within ±127, rel8 suffices). 0x4D053D is overwritten but never
//   reachable -> safe. CJK word=1 codepoint (not over width + complete word buffer), link-internal word=whole segment (whole-sentence brighten).
//   Register contract (at entry 0x4D0530): al=[edi] (already read at 0x4D0516), esi=word start, edi=scan pointer.
// ===================================================================
            LOG_INFO(kCat, "WordSplitPatch OK @RVA 0xD053A (UTF-8 codepoint word-collect, E9 -> CJKWordColStub).");
        else
            LOG_ERROR(kCat, "WordSplitPatch FAIL @RVA 0xD053A (write)");
// 原 0x4D053D: push 1Ch（operator new 参数）
// original 0x4D053D: push 1Ch (operator new arg)
    } else {
        LOG_WARN(kCat, "WordSplitPatch SKIP @RVA 0xD053A: bytes %02X %02X %02X %02X %02X (expect 47 EB D9 6A 1C)",
// 跳 0x4D053F（call operator new，完整保留）
// jump to 0x4D053F (call operator new, fully preserved)
                 cur.size() >= 1 ? cur[0] : 0, cur.size() >= 2 ? cur[1] : 0,
                 cur.size() >= 3 ? cur[2] : 0, cur.size() >= 4 ? cur[3] : 0,
                 cur.size() >= 5 ? cur[4] : 0);
    }

// ===================================================================
// ★ 08-13 整句变亮修复：恢复 02:01 纯 UTF-8 码点词收集（hook 0x4D053A，5 字节 E9）。
//   背景：单字节词缓冲（E4 00）无法渲染 → hover 变亮失效（用户 01:57 数据实锤）；
//   整段词（g_inLink/TagEntryStub，02:17/07:09/07:23）连续空白，已弃用。
//   本 stub：分两条路径——
//     · ASCII：整词收集（扫描到分隔符止），并把尾随空格(0x20)纳入元素文本 →
//       sub_43967D 对 0x20 按 sub_439633(0x69)='i' 宽(≈9px) 计量 → 元素自带词距（英文词间有空格）。
//     · CJK：1 个完整 UTF-8 码点（≤4 字节）逐字 → 保任意位置折行 + 词缓冲完整（变亮可渲染）。
//   任一路径都 push 1Ch; jmp 0x4D053F 收尾，**绝不跳回 0x4D0516 循环头** →
//   0x4D053D（被 E9 覆盖为垃圾）永不可达 → 分隔符路径安全。
//   寄存器契约（0x4D053A 时）：al=[edi]（0x4D0516 已读当前字符），esi=词起始，edi=扫描指针。
//   反斜杠转义（\n 等）由 0x4D0530 原逻辑处理（本 hook 点 0x4D053A 在其后，见不到已结束的转义）。
// ===================================================================

// ===================================================================
// ★ 08-13 whole-sentence brighten fix: restore the 02:01 pure UTF-8 codepoint word collection (hook 0x4D053A, 5-byte E9).
//   Background: the single-byte word buffer (E4 00) cannot be rendered -> hover brighten fails (hard data from user at 01:57);
//   whole-segment words (g_inLink/TagEntryStub, 02:17/07:09/07:23) are continuous blanks and have been deprecated.
//   This stub: two paths --
//     · ASCII: collect the whole word (scan until delimiter) and include the trailing space (0x20) in the element text ->
//       sub_43967D measures 0x20 as sub_439633(0x69)='i' width (≈9px) -> the element carries its own word spacing (spaces between English words).
//     · CJK: one complete UTF-8 codepoint (≤4 bytes) per char -> enables wrapping at any position + complete word buffer (brighten is renderable).
//   Both paths end with push 1Ch; jmp 0x4D053F, **never jump back to the 0x4D0516 loop head** ->
//   0x4D053D (overwritten as garbage by E9) is never reachable -> the delimiter path is safe.
//   Register contract (at 0x4D053A): al=[edi] (current char already read at 0x4D0516), esi=word start, edi=scan pointer.
//   Backslash escapes (\n etc.) are handled by the original logic at 0x4D0530 (this hook point 0x4D053A is after it, so it never sees an already-ended escape).
// ===================================================================
}
static uint32_t ParseColor(const std::string& s, uint32_t def) {
        // 入口：al = [edi]（当前字符）；esi = 词起点（= 当前字符，由外层循环 0x4D0514 设置）。
        // ★ 英文词距方案（绝不碰 0x4CA612）：ASCII 整词收集，把尾随空格纳入元素文本 →
        //   sub_43967D 对 0x20 按 sub_439633(0x69)='i' 宽(≈9px) 计量 → 元素自带词距，与原始引擎一致。
        //   CJK 仍逐码点（保任意位置折行），不加尾随空格 → 紧凑。
        //   元素文本由 sub_4CFDD5 拷贝为 Count+1 空终止缓冲，吞空格不越界。
        // Entry: al = [edi] (current char); esi = word start (= current char, set by outer loop 0x4D0514).
        // ★ English word-spacing approach (never touches 0x4CA612): collect ASCII whole word, include trailing space in element text ->
        //   sub_43967D measures 0x20 as sub_439633(0x69)='i' width (≈9px) -> the element carries its own word spacing, consistent with the original engine.
        //   CJK still goes per-codepoint (enables wrapping at any position), with no trailing space -> compact.
        //   The element text is copied by sub_4CFDD5 into a Count+1 NUL-terminated buffer, swallowing spaces without OOB.
    if (s.empty()) return def;
    const char* p = s.c_str();
// >=0x80 → CJK 码点路径（逐字，无尾随空格）
// >=0x80 -> CJK codepoint path (per char, no trailing space)
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    // ---- ASCII 整词扫描（含尾随空格）----
    // ---- ASCII whole-word scan (including trailing space) ----
    char* end = nullptr;
// 扫描指针从当前字符起
// scan pointer starts at current char
    unsigned long v = strtoul(p, &end, 16);
    if (end == p) return def;
    return (uint32_t)v;
}
class TextRendererFeature : public Feature {
// 字符串结束 → 词尾（无空格）
// string end -> word end (no space)
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }
// 空格 → 词尾，并把空格纳入元素（自带 9px 词距）
// space -> word end, and include the space in the element (carries its own 9px spacing)
    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        MergeLangOverrides(cfg);
        g_enabled = cfg.GetBool(kName, "Enabled", false);
// tab → 词尾
// tab -> word end
        if (!g_enabled) { LOG_INFO(kCat, "Disabled (Enabled=0)"); return true; }
        {
            std::string s = cfg.GetString(kName, "FontName", "");
// LF
// LF
            g_cfgFontName.clear();
            for (char c : s) g_cfgFontName += (wchar_t)(unsigned char)c;
            s = cfg.GetString(kName, "FontFile", "");
// CR
// CR
            g_cfgFontFile.clear();
            for (char c : s) g_cfgFontFile += (wchar_t)(unsigned char)c;
        }
// '<' 标签
// '<' tag
        g_fontSize     = cfg.GetInt(kName, "FontSize", kDefSize);
        g_selfTest     = cfg.GetBool(kName, "SelfTest", false);
        g_hookOn       = cfg.GetBool(kName, "HookEnabled", false);
// '\' 转义
// '\' escape
        g_textColor    = ParseColor(cfg.GetString(kName, "TextColor", "FFFFFF"), 0xFFFFFF);
        g_textYOffset  = cfg.GetInt(kName, "TextYOffset", 0);
        g_textXOffset  = cfg.GetInt(kName, "TextXOffset", 0);
// CJK（>=0x80）→ 词尾（中文后不强行加距）
// CJK (>=0x80) -> word end (do not force spacing after Chinese)
        g_useEngineColor = cfg.GetBool(kName, "UseEngineColor", true);
        g_antiAlias      = cfg.GetBool(kName, "AntiAlias", true);
// ASCII 非分隔符 → 属同一词
// ASCII non-delimiter -> part of the same word
        g_halfCellExtra  = cfg.GetInt(kName, "HalfCellExtra", 1);
        g_containerLeftFix = cfg.GetBool(kName, "ContainerLeftFix", false);
        g_wordSplitPatch = cfg.GetBool(kName, "WordSplitPatch", false);
        g_blendIdempotent = cfg.GetBool(kName, "BlendIdempotent", true);
// 下一字节是 'n'？
// is the next byte 'n'?
        g_base = ver.GetBaseAddress();
        gameapi::Init(g_base);
// '\n' → 词尾
// '\n' -> word end
        EnsureFont(g_base);
        if (!g_ready) return false;
// 孤立反斜杠 → 视为词内字符
// lone backslash -> treat as in-word character
        if (g_selfTest) RunSelfTest(cfg.GetString(kName, "SelfTestText", ""));
        if (g_hookOn)  InstallHooks(g_base);
        else           LOG_INFO(kCat, "HookEnabled=0: glyph hooks not installed.");
        if (g_wordSplitPatch) InstallWordSplitPatches(g_base);
// 空格纳入元素长度（元素文本含尾随空格）
// include space in element length (element text contains the trailing space)
        return true;
    }
};
// edi = 元素尾（空格情形已含空格；其余止于分隔符）
// edi = element tail (space case already includes the space; others stop at the delimiter)
REGISTER_FEATURE(TextRendererFeature)
}
// 原 0x4D053D 语义：push 1Ch（operator new 参数）
// original 0x4D053D semantics: push 1Ch (operator new arg)
#pragma endregion
