// GdiFont.h
// ===================================================================
// GDI 字形光栅化引擎（多语言文本渲染重做的核心，游戏无关、可独立测试）
//
// 设计要点（对照 LG4Northland.dll + 你的决策 UTF-8 / 不用 FreeType / 游戏不跨平台）：
//   1. 输入是 UTF-8 字节流；先解码成 Unicode 码点（与磁盘文件编码彻底解耦）。
//   2. 每个码点用 Windows GDI 从任意已安装 TTF（繁中=微軟正黑體 / 細明體等）
//      动态光栅化成内存灰度位图 —— 这就是"从 TTF 动态生成到内存"，零外部
//      依赖（已链接 gdi32.lib）。
//      ★ 光栅化走 DIB + TextOutW 路线（不是 GetGlyphOutline）：无显示会话里
//        GetGlyphOutline 的灰度缓冲格式易崩溃，TextOutW 最稳、兼容最好。
//      ★ 必须用 SetTextAlign(TA_BASELINE) —— TextOut 默认 TA_TOP 会把 (x,y)
//        当成字形"顶边"，导致整字落在画布下方被裁掉（曾卡住数轮排查）。
//   3. 字形带缓存（码点 -> 灰度图），避免每帧重绘重复光栅化。
//   4. BlitGlyph 把灰度字形按文本色 alpha 混合进 32bpp(RGBA/RGB) 或 16bpp(RGB565)
//      目标缓冲，算法与 LG4 sub_10002D40 一致（编码无关，只认灰度+颜色）。
//
// 该模块不 hook 任何游戏函数；它只负责"码点 -> 内存里的字形位图"。
// 真正的游戏内接通（hook 文本绘制入口、把游戏字形源替换成这里）是另一个步骤。
// ===================================================================
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

namespace ge {
namespace text {

// 单个字形：灰度 alpha 位图（8 位每像素，0=透明，255=不透明）。
// 坐标约定同 GDI：y 向下；基线(baseline)是文字落地的水平线。
struct Glyph {
    int w = 0;        // 黑框宽度  gmBlackBoxX
    int h = 0;        // 黑框高度  gmBlackBoxY
    int pitch = 0;    // 每行字节数（4 字节对齐，GGO_GRAY8 规定）
    int originX = 0;  // 黑框左上角相对"笔位(pen)"的 x 偏移 gmptGlyphOrigin.x
    int originY = 0;  // 黑框左上角相对"笔位"的 y 偏移 gmptGlyphOrigin.y（通常为负，向上）
    int advance = 0;  // 光标水平步进 gmCellIncX
    std::vector<uint8_t> alpha; // 灰度数据，长度 = pitch * h
};

class GdiFontRasterizer {
public:
    GdiFontRasterizer() = default;
    ~GdiFontRasterizer();

    GdiFontRasterizer(const GdiFontRasterizer&) = delete;
    GdiFontRasterizer& operator=(const GdiFontRasterizer&) = delete;

    // 用指定字体名 / 像素高度创建光栅器。失败返回 false。
    // fontName 例：L"Microsoft JhengHei"(繁中) / L"Microsoft YaHei"(简中) / L"SimSun"
    // antiAlias: true=灰度抗锯齿(平滑但可能发虚) / false=硬边(NONANTIALIASED，清晰但锯齿)。
    bool Create(const wchar_t* fontName, int heightPx, int weight = 400, bool italic = false,
                bool antiAlias = true);

    // 从 .ttf 文件加载字体（自包含，不依赖系统是否安装该字体）。
    // 内部用 AddFontResourceEx(FR_PRIVATE) 把文件注册到本进程，再按文件内的
    // 族名创建。filePath 例：L"plugins/fonts/l10.ttf"。
    bool CreateFromFile(const wchar_t* filePath, int heightPx, int weight = 400, bool italic = false,
                        bool antiAlias = true);

    // 释放当前字体与画布，回到初始态（便于按游戏语言热切换字体）。
    void Reset();

    // 取码点字形（带缓存）。取不到返回 nullptr（调用方用空格/豆腐占位）。
    const Glyph* GetGlyph(uint32_t codepoint);

    int Height()  const { return m_height; }
    int Ascent()  const { return m_ascent; }
    int Descent() const { return m_descent; }

    // ---------- 纯工具（静态，便于独立测试）----------

    // UTF-8 字节流 -> Unicode 码点序列（符合 RFC 3629，越界字节跳过）。
    static std::vector<uint32_t> DecodeUtf8(const char* s, size_t n);

    // 把灰度字形按文本色混合进帧缓冲。
    //   fb    : 目标缓冲首字节
    //   pitch : 每行字节数
    //   bpp   : 2 = 16bpp RGB565；4 = 32bpp（内存布局 B,G,R[,A]）
    //   dx,dy : 字形黑框左上角在 fb 中的像素位置（调用方应已加 originX/originY 校正）
    //   color : 0xRRGGBB（16bpp 时自动转 RGB565）
    //   fbW,fbH : 目标表面像素宽/高（用于边界裁剪，防止写出缓冲外导致崩溃）
    //   idempotent : 像素级幂等（1=推荐）：目标像素已≈前景色则跳过该像素，避免
    //     不清空表面上的跨帧重绘累积（LG4 tga 位图不透明直写无累积，GDI 抗锯齿
    //     alpha 混合会累积变浓；幂等 = 保留抗锯齿 + 同位置重绘像素不变）。
    //   clipX/Y/W/H : 引擎 UI 框裁剪区（DrawContext +0x08..0x14）。W/H > 0 时启用，
    //     字形像素超出该矩形的不画（引擎原行为 Rect::Intersect，修垂直列表最后一行溢出）。
    static void BlitGlyph(uint8_t* fb, int pitch, int bpp,
                          int dx, int dy, const Glyph* g, uint32_t color,
                          int fbW, int fbH, bool idempotent = true,
                          int clipX = -1, int clipY = -1, int clipW = -1, int clipH = -1);

    // 把 RGBA 缓冲（白底文字）保存为 32bpp BMP（用于独立验证 / 自测落盘）。
    static bool SaveRGBAAsBMP(const char* path, int w, int h, const uint8_t* rgba);

private:
    bool Rasterize(uint32_t cp, Glyph& out);

    HDC   m_hdc   = nullptr;
    HFONT m_hfont = nullptr;
    HBITMAP m_hbmp = nullptr;   // 逐字形渲染画布（内存 DIB）
    uint8_t* m_bits = nullptr; // DIB 像素指针
    int    m_fontRes = 0;        // AddFontResourceEx 返回值（新增字体数，>0 表示已注册）
    std::wstring m_fontFile;     // 注册过的字体文件路径（用于 RemoveFontResourceEx）

    // 读取 TTF 的族名（name 表 nameID 1/4/6，UTF-16BE），用于 CreateFromFile。
    static std::wstring ReadTtfFamilyName(const wchar_t* path);
    int   m_height = 0;
    int   m_ascent = 0;
    int   m_descent = 0;
    int   m_cellW = 0, m_cellH = 0, m_baseY = 0; // DIB 画布尺寸 / 基线 y
    std::unordered_map<uint32_t, Glyph> m_cache; // 简单哈希缓存（可换 LRU）
};

} // namespace text
} // namespace ge
