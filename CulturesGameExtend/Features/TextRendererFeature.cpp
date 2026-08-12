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
#include "pch.h"
#include "Core/Feature.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Patch.h"
#include "Core/Logger.h"
#include "Core/GameApi.h"
#include "Core/GdiFont.h"
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace fe_text {

const char* kName = "TextRenderer";
const char* kCat  = "[TextRenderer]";

// ---- 默认配置 ----
const wchar_t* kDefFont = L"Microsoft JhengHei"; // 繁中（微軟正黑體）；简中改 Microsoft YaHei
const int      kDefSize = 16;

// ---- 引擎实例（进程内唯一）----
static ge::text::GdiFontRasterizer g_raster;
static bool g_ready = false;

// ---- 配置 ----
static std::wstring g_cfgFontName;
static std::wstring g_cfgFontFile;
static int          g_fontSize = kDefSize;
static bool         g_enabled  = false;
static bool         g_selfTest = false;
static bool         g_hookOn   = false;
static uint32_t     g_textColor  = 0xFFFFFF;
static int          g_textYOffset = 0;
static int          g_textXOffset = 0;   // 2026-08-12 新增：水平微调（修"整体右偏"）
static uintptr_t    g_base = 0;

// UTF-8 重组缓冲（文本绘制单线程顺序进行，足够）
static int      s_buf  = 0;     // 1=正在缓冲一个多字节码点
static int      s_need = 0;     // 还需要的 continuation 数
static int      s_got  = 0;     // 已收到的 continuation 数
static uint32_t s_cp   = 0;     // 当前码点累加值
static int      s_lx = 0, s_ly = 0; // lead 字节时的 (x,y)，渲染以此为准
static const uint8_t* s_color = nullptr; // lead 字节时的引擎颜色上下文（多字节码点跨两次调用）
static const void*     s_font  = nullptr; // lead 字节时的字体对象（垂直居中行高）

static bool  g_loggedOnce = false;

// ---- 语言驱动字体选择 ----
static const int LANG_TABLE_VA = 0x4F338C;

static int ReadGameLangIdFromIni() {
    wchar_t exepath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exepath, MAX_PATH);
    std::wstring dir(exepath);
    auto pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir = dir.substr(0, pos);
    std::wstring iniPath = dir + L"\\Game.ini";
    FILE* f = nullptr;
    if (_wfopen_s(&f, iniPath.c_str(), L"r") != 0 || !f) return 0;
    int id = 0;
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
        ok = g_raster.CreateFromFile(filePath.c_str(), g_fontSize, 400, false);
    if (!ok)
        ok = g_raster.Create(faceName.c_str(), g_fontSize, 400, false);
    if (ok) { g_ready = true; LOG_INFO(kCat, "engine ready: font key=%ls size=%d", wantKey.c_str(), g_fontSize); }
    else    { g_ready = false; LOG_ERROR(kCat, "failed to create font: %ls", wantKey.c_str()); }
}

// ===================================================================
// SelfTest：UTF-8 繁中 + 混合 ASCII 渲染到 logs/TextRenderer_selftest.bmp
// ===================================================================
static void RunSelfTest() {
    EnsureFont(g_base);
    LOG_INFO(kCat, "SelfTest: font %ls", g_ready ? L"ok" : L"FAIL");
    const char* utf8 = u8"繁體中文測試 多語言渲染 ABC 123";
    std::vector<uint32_t> cps = ge::text::GdiFontRasterizer::DecodeUtf8(utf8, strlen(utf8));
    int lineH = g_fontSize + 8;
    int imgW = 720, imgH = lineH + 16;
    std::vector<uint8_t> img((size_t)imgW * imgH * 4, 0xFF);
    int penX = 8, baseY = 8 + g_raster.Ascent();
    for (uint32_t cp : cps) {
        const ge::text::Glyph* g = g_raster.GetGlyph(cp);
        if (!g) continue;
        ge::text::GdiFontRasterizer::BlitGlyph(img.data(), imgW * 4, 4,
                                               penX + g->originX, baseY + g->originY, g, 0x1A3C8C, imgW, imgH);
        penX += g->advance;
    }
    char path[MAX_PATH] = {};
    snprintf(path, sizeof(path), "%s/TextRenderer_selftest.bmp", "logs");
    if (ge::text::GdiFontRasterizer::SaveRGBAAsBMP(path, imgW, imgH, img.data()))
        LOG_INFO(kCat, "SelfTest: wrote %s (%zu codepoints)", path, cps.size());
    else
        LOG_WARN(kCat, "SelfTest: failed to write BMP");
}

// ===================================================================
// 字形层 hook 实现
//   DrawContext(this) 结构（static 逆向 sub_4658C7 实证）：
//     +0x08=clip.x +0x0C=clip.y +0x10=clip.w(表面宽) +0x14=clip.h(表面高)
//     +0x2C=像素基址   +0x30=pitch(像素)   文本路径硬编码 *4 => 32bpp
// ===================================================================

static uintptr_t g_thunkGlyph = 0;   // sub_439610 跳板
static uintptr_t g_thunkWidth = 0;   // sub_439633 跳板

// ★ 2026-08-12 08:5x 引擎颜色接管（Tooltip 残留重影根因）：
//   sub_4658C7 反编译实锤：颜色上下文 = sub_439610 的 [ebp+8](a3)，结构 = {B,G,R} 字节数组
//   （32bpp 分支 v145=*a6/B, HIBYTE(v15)=a6[1]/G, v147=a6[2]<<16/R；16bpp 分支
//   sub_410029(a1)=sub_40FFEA(a1[2],a1[1],*a1) 同源）。0xRRGGBB = c[2]<<16 | c[1]<<8 | c[0]。
//   Tooltip 画多 pass 时各 pass 颜色不同（阴影/描边深色 + 主体白色），旧代码统一用
//   g_textColor(白) → 两遍白色叠加 → 残留重影。改用引擎颜色 → 还原原版颜色/阴影，重影消失。
static bool g_useEngineColor = true;
static bool g_wordSplitPatch = true;   // LG4/LG0 同款分词补丁（引擎按单字符分词，CJK 布局根源）
static bool g_blendIdempotent = true;  // BlitGlyph 像素级幂等（防不清空表面跨帧累积）
static uint32_t EngineColor(const uint8_t* c) {
    if (!c || !g_useEngineColor) return g_textColor;
    return ((uint32_t)c[2] << 16) | ((uint32_t)c[1] << 8) | c[0];
}

// 渲染一个码点到 DrawContext 表面（引擎已在 (x,y) 给出该字形的左上角）
static bool g_dumpedDrawCtx = false;

// ★ 2026-08-12 08:3x 重复绘制检测：信息框"模糊"疑似同一字符被引擎多次绘制叠加。
//   08:49 诊断定案：dt=31ms = **跨帧重绘叠加**（Tooltip 每帧重绘到持久表面不清空）。
//   原版硬边位图叠加无变化；GDI 抗锯齿字形边缘 alpha 渐变叠加 → 重影。
//   普通文字表面每帧清空，所以不叠加。
//   ★ 修复方案：绘制前检查字形中心像素是否已是目标色——是 → 表面未清空(重绘) → 跳过；
//     否 → 正常画。对"清空/不清空"两种表面都正确。
static bool ShouldSkipRepeat(uint8_t* fb, int pitchBytes, int bpp, int dx, int dy,
                             const ge::text::Glyph* g, uint32_t color, int fbW, int fbH) {
    if (!fb || !g || g->w < 2 || g->h < 2) return false;
    int cx = dx + g->w / 2;
    int cy = dy + g->h / 2;
    if (cx < 0 || cx >= fbW || cy < 0 || cy >= fbH) return false;
    const uint8_t* p = fb + (size_t)cy * pitchBytes + (size_t)cx * bpp;
    if (bpp == 4) {
        uint8_t tr = (uint8_t)((color >> 16) & 0xFF);
        uint8_t tg = (uint8_t)((color >> 8) & 0xFF);
        uint8_t tb = (uint8_t)(color & 0xFF);
        // 容差 24：抗锯齿中心墨迹≈纯色；背景色差异明显
        return (p[2] > tr - 24 && p[2] < tr + 24) &&
               (p[1] > tg - 24 && p[1] < tg + 24) &&
               (p[0] > tb - 24 && p[0] < tb + 24);
    } else if (bpp == 2) {
        uint16_t v = *(const uint16_t*)p;
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

// ★ 09:2x 引擎行高自适应垂直居中：不同 UI 用不同字体对象（sub_439610 的 ecx），
//   行距 = 字体对象+8 的值 + 2（sub_40ECF9 反编译实锤 `a5 += a2[2] + 2`）。
//   主界面字体行高≈20 → 居中偏移 (20-13)/2≈3（正是用户手工调出的 TextYOffset=3）；
//   Tooltip 字体行高紧凑 → 偏移≈0（不再溢出引擎画的方框 → 叠加消失）。
//   TextYOffset 降级为纯微调（默认 0）。
static int FontLineHeight(const void* font) {
    if (!font) return g_fontSize;
    int lh = *(const int*)((uintptr_t)font + 8) + 2;   // 行距 = font[8] + 2
    if (lh < 8 || lh > 64) lh = g_fontSize;            // 防御：异常回退
    return lh;
}

static void RenderCodepoint(void* self, uint32_t cp, int x, int y, const uint8_t* colorCtx, const void* font) {
    if (!self || cp <= 0x20) return;
    uint32_t color = EngineColor(colorCtx);
    uintptr_t fb = *(uintptr_t*)((uintptr_t)self + 0x2C);
    if (!fb) fb = *(uintptr_t*)((uintptr_t)self + 0x28);   // 兜底读 +0x28（旧误判字段，尽量不用）
    int pitch_px = *(int*)((uintptr_t)self + 0x30);
    int clipX = *(int*)((uintptr_t)self + 0x08);
    int clipY = *(int*)((uintptr_t)self + 0x0C);
    int clipW = *(int*)((uintptr_t)self + 0x10);
    int clipH = *(int*)((uintptr_t)self + 0x14);
    // ★ 2026-08-12 07:5x 修正：pitch_px 是帧缓冲真实行宽（如 800），clip 只是绘制子区域
    //   （如 280x20）。用 clip 推 fbW 会把超出子区域的字形裁掉（"字被吃掉"）。
    //   fbW 必须用 pitch_px；fbH 候选 = +0x18（先 dump 验证），回退 600。
    int fbW = pitch_px;
    int fbH = *(int*)((uintptr_t)self + 0x18);
    if (fbH <= 0 || fbH > 4096) fbH = 600;
    if (fbW <= 0 || fbW > 4096) fbW = 800;
    // ★ 09:2x 改为"每新表面 dump 一次"（最多 8 个）：区分 Tooltip 表面 vs 主界面表面
    //   （clip 尺寸/偏移语义不同——3,3 偏移对 Tooltip 溢出框 → 叠加；0 则正常）
    {
        static uintptr_t s_dumpedFb[8];
        static int s_dumpedN = 0;
        bool isNew = true;
        for (int i = 0; i < s_dumpedN; ++i)
            if (s_dumpedFb[i] == fb) { isNew = false; break; }
        if (isNew && s_dumpedN < 8) {
            s_dumpedFb[s_dumpedN++] = fb;
            uint32_t* d = (uint32_t*)self;
            LOG_INFO(kCat, "SURFACE#%d: self=%p fb=%p pitch_px=%d clip=(%d,%d %dx%d) "
                     "fbW=%d fbH=%d cp=U+%04X x=%d y=%d color=0x%X",
                     s_dumpedN, self, (void*)fb, pitch_px, clipX, clipY, clipW, clipH,
                     fbW, fbH, cp, x, y, (unsigned)color);
            LOG_INFO(kCat, "DrawCtx dump +0x00..0x54: %08X %08X %08X %08X | %08X %08X %08X %08X | "
                     "%08X %08X %08X %08X | %08X %08X %08X %08X | %08X %08X %08X %08X | %08X",
                     d[0x00/4], d[0x04/4], d[0x08/4], d[0x0C/4],
                     d[0x10/4], d[0x14/4], d[0x18/4], d[0x1C/4],
                     d[0x20/4], d[0x24/4], d[0x28/4], d[0x2C/4],
                     d[0x30/4], d[0x34/4], d[0x38/4], d[0x3C/4],
                     d[0x40/4], d[0x44/4], d[0x48/4], d[0x4C/4],
                     d[0x50/4]);
        }
    }
    // ★ 防御：帧缓冲/步长异常一律跳过渲染，绝不往野指针写像素（防踩坏游戏内存 → 连锁崩溃）
    if (!fb || pitch_px <= 0 || pitch_px > 4096) { LOG_WARN(kCat, "RenderCodepoint skip: bad fb/pitch"); return; }
    if (fb < 0x10000 || fb > 0x7FFFFFFF) { LOG_WARN(kCat, "RenderCodepoint skip: fb out of range"); return; }

    const ge::text::Glyph* g = g_raster.GetGlyph(cp);
    if (!g || g->w == 0 || g->h == 0) return;
    // Glyph metric 独立于 DrawCtx dump 各打印一次（旧版共用 g_dumpedDrawCtx 导致 metric 永不输出）
    {
        static bool s_dumpedGlyph = false;
        if (!s_dumpedGlyph) {
            s_dumpedGlyph = true;
            LOG_INFO(kCat, "Glyph metric: cp=U+%04X w=%d h=%d origin=(%d,%d) advance=%d",
                     cp, g->w, g->h, g->originX, g->originY, g->advance);
        }
    }
    // ★★ 2026-08-12 08:2x 崩溃根因修复（DrawCtx dump 实锤）：
    //   +0x30=800(pitch像素)、+0x38=1600(pitch字节) → 每像素 2 字节 = **16bpp**！
    //   旧代码硬编码 bpp=4 → 每像素写 4 字节(实际 2) → 字形横向拉伸 2 倍("字很大")
    //   + 行步进 3200 字节写进 1600 字节行 → **越界写 → 崩溃**。
    //   引擎 sub_4658C7 有两条路径：32bpp 用 +0x2C 基址，16bpp 用 +0x28 基址（dump 有效值）。
    //   修复：bpp 由 pitchBytesHint/pitch_px 推导（1600/800=2；32bpp 表面则为 4）。
    int bpp = 4;
    {
        int pitchBytesHint = *(int*)((uintptr_t)self + 0x38);
        if (pitchBytesHint > 0 && pitch_px > 0 && (pitchBytesHint % pitch_px) == 0) {
            int cand = pitchBytesHint / pitch_px;   // 每像素字节数 2 或 4
            if (cand == 2 || cand == 4) bpp = cand;
        }
    }
    int pitchBytes = pitch_px * bpp;
    // ★★ 方案A（2026-08-12 08:1x 定案，mirror LG tga 等宽格模型，用户拍板）：
    //   LG sub_10002D40 的坐标模型 = (x,y) 是引擎算好的"格子左上角"，直接把整个固定格子
    //   blit 过去，零缩放零 bearing。右偏的根因不是 origin（实测不加 origin 已完整显示），
    //   而是 sub_439633 返回 GDI advance 与引擎排版推进步长不一致 → 每个字符 x 累积偏差，
    //   偏移量随位置变化，单个 TextXOffset 无法统一矫正。
    //   方案A：字宽固定（ASCII=半格=cellW/2，CJK=全格=cellW，与引擎排版完全同步），
    //   字形墨迹在格内居中，从 (x,y) 直画。cellW=cellH=g_fontSize。
    int cellW = g_fontSize;
    // ★★ 2026-08-12 08:4x 位置基准定案（用户实测三轮）：
    //   垂直居中基准 = cellH(g_fontSize) 即可（"居中是对的"）；不要用 lineH=clip.h，
    //   否则字形再下沉 3px + TextYOffset=3 → 太靠下。
    //   TextYOffset/TextXOffset 现在是纯平移微调（用户用 3,3 完美）。
    // ★★ 09:2x 升级：垂直居中基准改用**字体行高**（font[8]+2，sub_40ECF9 行距推进值），
    //   替代固定 cellH——不同 UI 字体行高不同：主界面≈20 → dy=y+3（等效旧 TextYOffset=3），
    //   Tooltip 字体紧凑 → dy≈y+0（不再溢出引擎方框 → 叠加消失）。TextYOffset 归零微调。
    int cellH = g_fontSize;
    int lineH = FontLineHeight(font);
    if (lineH < g->h) lineH = g->h;                // 行高小于字形时不平移（贴顶，防溢出）
    int slotW = (cp < 0x80) ? (cellW / 2) : cellW;   // 半角 ASCII 占半格，全角占全格
    int dx = x + (slotW - g->w) / 2 + g_textXOffset;
    int dy = y + (lineH - g->h) / 2 + g_textYOffset;
    // ★ 09:2x 首帧确认行高：主界面应≈20（居中偏移≈3），Tooltip 应紧凑（偏移≈0）
    {
        static bool s_loggedLine = false;
        if (!s_loggedLine) {
            s_loggedLine = true;
            LOG_INFO(kCat, "LineHeight: font=%p font[8]=%d lineH=%d glyphH=%d dyOffset=%d",
                     font, font ? *(const int*)((uintptr_t)font + 8) : -1, lineH, g->h,
                     (lineH - g->h) / 2);
        }
    }
    // ★ 08:49 跨帧重绘去重：Tooltip 表面不清空，每帧重绘 → 中心像素已是目标色 → 跳过（防叠加模糊）
    //   08:5x 用引擎颜色（多 pass 时各 pass 颜色不同，检测色必须与本次要画的颜色一致）
    if (ShouldSkipRepeat((uint8_t*)fb, pitchBytes, bpp, dx, dy, g, color, fbW, fbH)) return;
    ge::text::GdiFontRasterizer::BlitGlyph((uint8_t*)fb, pitchBytes, bpp, dx, dy, g, color, fbW, fbH,
                                           g_blendIdempotent);
}

// 字形层绘制 hook：sub_439610(__thiscall, ecx=字体对象; DrawContext=[ebp+0x0C]) 的 C 处理
// ★ 全字符接管：ASCII(<0x80) 是完整码点直接渲染；≥0x80 走 UTF-8 重组。
//   colorCtx = sub_439610 的 [ebp+8](a3)，{B,G,R} 字节数组（引擎颜色，还原多 pass 阴影/描边）。
//   font = sub_439610 的 ecx（字体对象；+8=行高，用于垂直居中自适应）。
//   返回 true=已接管绘制；false=未就绪（stub 会 relay 回引擎原函，防空字/崩）。
static bool OnGlyphDraw(void* dc, int ch, int x, int y, const uint8_t* colorCtx, const void* font) {
    if (!g_ready) return false;
    if (!g_loggedOnce) {
        g_loggedOnce = true;
        LOG_INFO(kCat, "OnGlyphDraw active: dc=%p ch=0x%X x=%d y=%d size=%d cfgColor=0x%X engColor=0x%X font=%p",
                 dc, (unsigned)ch, x, y, g_fontSize, (unsigned)g_textColor,
                 (unsigned)EngineColor(colorCtx), font);
    }
    if (ch < 0x80) {
        // ASCII：单字节即完整码点，直接 GDI 渲染（用户拍板全接管）
        RenderCodepoint(dc, (uint32_t)ch, x, y, colorCtx, font);
        return true;
    }
    if ((ch & 0xC0) == 0x80) {
        // continuation (0x80..0xBF)
        if (!s_buf) return true;            // 游离 continuation，忽略（已接管，不 relay）
        s_cp = (s_cp << 6) | (ch & 0x3F);
        if (++s_got < s_need) return true;  // 还需更多
        uint32_t cp = s_cp; s_buf = 0; s_got = 0; s_need = 0;
        RenderCodepoint(dc, cp, s_lx, s_ly, s_color, s_font);
    } else {
        // lead (0xC0..0xFF)：开新码点，记录起始 (x,y) 与颜色/字体
        if (ch >= 0xF0)         { s_cp = ch & 0x07; s_need = 3; }
        else if (ch >= 0xE0)    { s_cp = ch & 0x0F; s_need = 2; }
        else                    { s_cp = ch & 0x1F; s_need = 1; }
        s_got = 0; s_buf = 1; s_lx = x; s_ly = y; s_color = colorCtx; s_font = font;
    }
    return true;
}

// 字宽 hook：sub_439633(__thiscall) 的 C 处理
// ★★ 方案A：固定格子宽（mirror LG sub_10002D40 等宽模型）——
//   ASCII(单字节, 含空格) = 半格 cellW/2；CJK lead = 全格 cellW；continuation = 0。
//   引擎排版循环用本返回值推进 x，与渲染的格子完全同步 → 消除累积偏移。
//   返回 -1 = 未就绪（stub relay 回引擎原函）。
static int OnGlyphWidth(int /*font*/, int ch) {
    if (!g_ready) return -1;
    if (ch < 0x80) {
        // ASCII / 空格：半格（LG: v19==32 -> a5 += 格宽>>1；其他 ASCII 同半格）
        return g_fontSize / 2;
    }
    if ((ch & 0xC0) == 0x80) return 0;      // continuation：不前进
    return g_fontSize;                      // lead：前进一个 CJK 全格
}

// ---- sub_439610(__thiscall, ecx=字体对象; DrawContext=[ebp+0x0C]) 替换 stub ----
//   全部字符 -> OnGlyphDraw（GDI 渲染）；false -> relay 回引擎原函（安全兜底）
extern "C" void __declspec(naked) GlyphDrawStub() {
    __asm {
        push ebp
        mov  ebp, esp
        push ebx            // ★ 暂存 ecx（font=this）到非易失 ebx：C 函数会破坏 ecx
        mov  ebx, ecx
        push esi
        push edi
        // OnGlyphDraw(dc=[ebp+0x0C]=DrawContext, ch=[ebp+0x10], x=[ebp+0x14], y=[ebp+0x18],
        //             colorCtx=[ebp+8]=a3 {B,G,R}, font=ecx=ebx)
        push ebx          // font（sub_439610 的 ecx=字体对象；+8=行高）
        push [ebp+8]      // 颜色上下文（sub_4658C7 的 a6/arg_10；sub_410029(a1) 读 a1[0..2]）
        push [ebp+0x18]   // y
        push [ebp+0x14]   // x
        movzx eax, byte ptr [ebp+0x10]
        push eax          // ch
        push [ebp+0x0C]   // DrawContext = sub_439610 的 arg1（styleCtx 槽位，实为 DrawContext）
        call OnGlyphDraw
        add  esp, 24
        test eax, eax
        jnz  L_done
        // 未就绪：relay 回引擎原函（★必须先还原 ecx=this）
        mov  ecx, ebx
        pop  edi
        pop  esi
        pop  ebx
        mov  esp, ebp
        pop  ebp
        jmp  g_thunkGlyph
    L_done:
        pop  edi
        pop  esi
        pop  ebx
        mov  esp, ebp
        pop  ebp
        ret  0x14         // __thiscall：callee 清理 5 个栈参数（ecx=font 不走栈）
    }
}

// ---- sub_439633(__thiscall) 替换 stub ----
//   全部字符 -> OnGlyphWidth（GDI 字宽）；-1 -> relay 回引擎原函
extern "C" void __declspec(naked) GlyphWidthStub() {
    __asm {
        push ebx            // ★ 暂存 ecx（font=this）
        mov  ebx, ecx
        // OnGlyphWidth(font=[esp+0x0C] 原 ecx, ch=[esp+0x08] 原 [esp+4])
        movzx eax, byte ptr [esp+8]   // ch（push ebx 后 +4）
        push eax
        push ecx
        call OnGlyphWidth
        add  esp, 8
        cmp  eax, -1
        jne  L_wdone
        // 未就绪：relay 回引擎原函（★先还原 ecx=this；ch 原样留在 [esp+4]）
        mov  ecx, ebx
        pop  ebx
        jmp  g_thunkWidth
    L_wdone:
        pop  ebx
        ret  4             // __thiscall：被调方清 4 字节参数
    }
}

// 抄 copyLen 原字节 + E9 跳回 entry+copyLen
static uintptr_t MakeTrampoline(uintptr_t entry, int copyLen) {
    uintptr_t t = (uintptr_t)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    auto orig = Patch::ReadBytes(entry, copyLen);
    if (orig.size() != (size_t)copyLen) { VirtualFree((void*)t, 0, MEM_RELEASE); return 0; }
    memcpy((void*)t, orig.data(), copyLen);
    uintptr_t back = entry + copyLen;
    *(uint8_t*)(t + copyLen) = 0xE9;
    *(int32_t*)(t + copyLen + 1) = (int32_t)(back - (t + copyLen + 5));
    return t;
}

static void InstallHooks(uintptr_t base) {
    // sub_439610：抄 6 字节（落点 0x439616）
    uintptr_t eGlyph = base + (0x439610 - 0x400000);
    g_thunkGlyph = MakeTrampoline(eGlyph, 6);
    if (!g_thunkGlyph) { LOG_ERROR(kCat, "InstallHooks: glyph trampoline failed"); return; }
    if (!Patch::WriteJmp(eGlyph, (uintptr_t)&GlyphDrawStub, 3)) {
        LOG_ERROR(kCat, "InstallHooks: WriteJmp failed @0x%X", (unsigned)eGlyph); return;
    }
    // sub_439633：抄 5 字节（落点 0x439638）
    uintptr_t eWidth = base + (0x439633 - 0x400000);
    g_thunkWidth = MakeTrampoline(eWidth, 5);
    if (!g_thunkWidth) { LOG_ERROR(kCat, "InstallHooks: width trampoline failed"); return; }
    if (!Patch::WriteJmp(eWidth, (uintptr_t)&GlyphWidthStub, 3)) {
        LOG_ERROR(kCat, "InstallHooks: WriteJmp failed @0x%X", (unsigned)eWidth); return;
    }
    LOG_INFO(kCat, "InstallHooks: hooked glyph chokepoint sub_439610 + width sub_439633 "
                    "(ASCII relays to engine; CJK -> GDI).");
}

// ★ 2026-08-12 09:1x 分词补丁（LG4/LG0 同款思路，★只打补丁①）：
//   引擎文本处理默认"按空格分词"，中文无空格 → 整段中文被引擎当一个"超长词"→
//   换行/宽度/Tooltip 框布局计算异常。
//   ① RVA 0xD053B `jmp short loc_4D0516`(EB D9) → NOP：sub_4CFE93 富文本解析器
//      词扫描循环单字符化。★已验证安全：NOP 后各路径（普通词/空白/'\n'/标签）
//      edi 均前进，无死循环。
//   ★★ ② RVA 0xE2251 `jnz short loc_4E222F`(75 DC) —— **不可用**！
//      Saga sub_4E21B7 外层循环(0x4E21E3)不自行推进 String1，依赖收集循环
//      (0x4E222F)消费整词推进；NOP 后只收集 1 字符，0x4E225D `dec edx` 把
//      String1 拉回当前字符 → 外层无限重读同字符 → **死循环卡死**（选地图界面实测）。
//      C3CD sub_4DCE33 外层自行推进，故 LG0 在 C3CD 有效；Saga 结构不同不可照搬。
//      ⇒ 换行排版的中文适配改由 hook sub_43967D 实现（LG4 hook1=0x4396A5 同款）。
//   写入前校验原字节，防版本不符写坏。
static void InstallWordSplitPatches(uintptr_t base) {
    const uint8_t nops[2] = { 0x90, 0x90 };
    struct Pt { uint32_t rva; uint8_t b0, b1; const char* what; };
    static const Pt pts[] = {
        { 0xD053B, 0xEB, 0xD9, "rich-text parser word-loop" },
        // { 0xE2251, 0x75, 0xDC, "layout word-collect loop" },  // ★ Saga 上 NOP 死循环，禁用
    };
    for (const auto& p : pts) {
        uintptr_t va = base + p.rva;
        auto cur = Patch::ReadBytes(va, 2);
        if (cur.size() != 2 || cur[0] != p.b0 || cur[1] != p.b1) {
            LOG_WARN(kCat, "WordSplitPatch SKIP @RVA 0x%X: bytes %02X %02X (expect %02X %02X) - %s",
                     (unsigned)p.rva, cur.size() == 2 ? cur[0] : 0, cur.size() == 2 ? cur[1] : 0,
                     p.b0, p.b1, p.what);
            continue;
        }
        if (!Patch::WriteBytes(va, nops, 2)) {
            LOG_ERROR(kCat, "WordSplitPatch FAIL @RVA 0x%X - %s", (unsigned)p.rva, p.what);
        } else {
            LOG_INFO(kCat, "WordSplitPatch OK @RVA 0x%X (%s) -> 90 90 (single-char word split)",
                     (unsigned)p.rva, p.what);
        }
    }
}

// 解析 0xRRGGBB 文本色
static uint32_t ParseColor(const std::string& s, uint32_t def) {
    if (s.empty()) return def;
    const char* p = s.c_str();
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    char* end = nullptr;
    unsigned long v = strtoul(p, &end, 16);
    if (end == p) return def;
    return (uint32_t)v;
}

class TextRendererFeature : public Feature {
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }

    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        g_enabled = cfg.GetBool(kName, "Enabled", false);
        if (!g_enabled) { LOG_INFO(kCat, "Disabled (Enabled=0)"); return true; }

        {
            std::string s = cfg.GetString(kName, "FontName", "");
            g_cfgFontName.clear();
            for (char c : s) g_cfgFontName += (wchar_t)(unsigned char)c;
            s = cfg.GetString(kName, "FontFile", "");
            g_cfgFontFile.clear();
            for (char c : s) g_cfgFontFile += (wchar_t)(unsigned char)c;
        }
        g_fontSize     = cfg.GetInt(kName, "FontSize", kDefSize);
        g_selfTest     = cfg.GetBool(kName, "SelfTest", false);
        g_hookOn       = cfg.GetBool(kName, "HookEnabled", false);
        g_textColor    = ParseColor(cfg.GetString(kName, "TextColor", "FFFFFF"), 0xFFFFFF);
        g_textYOffset  = cfg.GetInt(kName, "TextYOffset", 0);
        g_textXOffset  = cfg.GetInt(kName, "TextXOffset", 0);
        g_useEngineColor = cfg.GetBool(kName, "UseEngineColor", true);
        g_wordSplitPatch = cfg.GetBool(kName, "WordSplitPatch", true);
        g_blendIdempotent = cfg.GetBool(kName, "BlendIdempotent", true);

        g_base = ver.GetBaseAddress();
        gameapi::Init(g_base);

        EnsureFont(g_base);
        if (!g_ready) return false;

        if (g_selfTest) RunSelfTest();
        if (g_hookOn)  InstallHooks(g_base);
        else           LOG_INFO(kCat, "HookEnabled=0: glyph hooks not installed.");
        if (g_wordSplitPatch) InstallWordSplitPatches(g_base);
        return true;
    }
};

REGISTER_FEATURE(TextRendererFeature)

} // namespace fe_text
