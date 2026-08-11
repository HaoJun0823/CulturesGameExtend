// TitleOverrideFeature.cpp
// ===================================================================
// [TitleOverride]
// DLL 挂载后替换游戏主窗口标题（窗口标题栏）。
//
// 背景：游戏只导入 CreateWindowExA / SendMessageA，未导入 SetWindowText，
// 窗口标题在创建时传入（lpWindowName），创建后可能再通过
// SendMessageA(hwnd, WM_SETTEXT, ...) 覆盖（Cultures_Title 项目正是
// 拦截 SendMessageA 的 WM_SETTEXT 生效）。
//
// 本 Feature 采用双 IAT hook（比直接 hook 导出函数入口更安全、可逆）：
//   1. CreateWindowExA：首个顶层窗口（hWndParent==NULL）创建后
//      SetWindowTextW(hwnd, 配置标题) —— 覆盖创建参数里的原标题；
//   2. SendMessageA：主窗口收到 WM_SETTEXT(0xC) 时，改走
//      SendMessageW 并替换为配置标题（宽字符），防止游戏后续覆盖。
//      其它窗口/其它消息原样转发，零副作用。
// 标题全程 UTF-16（宽字符），完美支持中文；配置为 UTF-8 文本。
//
// 配置（plugins/config/CulturesGameExtend_Global.ini，可用 Game.ini 覆盖）：
//   [TitleOverride]
//   Enabled = 1
//   Title = 文化：萨迦 汉化整合版        <- 留空 = 不启用替换
// ===================================================================
#include "pch.h"
#include "Core/Feature.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Patch.h"
#include "Core/Logger.h"
#include "Core/GameApi.h"
#include <Windows.h>

namespace fe_title {

const char* kName = "TitleOverride";
const char* kCat  = "[TitleOverride]";

// ---- IAT 槽（VA 0x4F31D4 / 0x4F31B8，RVA = VA - 0x400000）----
constexpr uintptr_t R_IatCreateWindowExA = 0xF31D4;
constexpr uintptr_t R_IatSendMessageA    = 0xF31B8;

// ---- 运行时状态 ----
static HWND     g_mainHwnd = nullptr;   // 游戏主窗口（首个顶层窗口）
static LONG     g_done     = 0;         // 只处理一次主窗口
static wchar_t  g_title[128] = L"";     // 配置标题（UTF-16）

static decltype(&CreateWindowExA) pRealCreateWindowExA = nullptr;
static decltype(&SendMessageA)    pRealSendMessageA    = nullptr;

// ===================================================================
// HookCreateWindowExA —— 创建窗口时接管
//   仅对"顶层窗口"（hWndParent==NULL）生效：记住主窗口句柄并设置标题。
//   子窗口（按钮/控件等）一律原样放行，绝不改动。
// ===================================================================
static HWND WINAPI HookCreateWindowExA(
    DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle,
    int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu,
    HINSTANCE hInstance, LPVOID lpParam) {
    HWND hw = pRealCreateWindowExA(dwExStyle, lpClassName, lpWindowName, dwStyle,
                                   X, Y, nWidth, nHeight, hWndParent, hMenu,
                                   hInstance, lpParam);
    if (hw && !hWndParent && !g_done && g_title[0]) {
        g_done   = 1;
        g_mainHwnd = hw;
        SetWindowTextW(hw, g_title);
    }
    return hw;
}

// ===================================================================
// HookSendMessageA —— 拦截对主窗口的 WM_SETTEXT（0xC）
//   游戏创建后若再用 SendMessageA(WM_SETTEXT) 覆盖标题（ANSI 路径），
//   这里改走 SendMessageW + 宽字符标题，保证中文不乱码。
//   仅当 hwnd == 主窗口 时替换；其余消息/窗口原样转发。
// ===================================================================
static LRESULT WINAPI HookSendMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    if (Msg == WM_SETTEXT && hWnd == g_mainHwnd && g_title[0]) {
        return SendMessageW(hWnd, Msg, wParam, (LPARAM)g_title);
    }
    return pRealSendMessageA(hWnd, Msg, wParam, lParam);
}

// ===================================================================
// 安装
// ===================================================================
class TitleOverrideFeature : public Feature {
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }

    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        if (!cfg.GetBool(kName, "Enabled", false)) {
            LOG_INFO(kCat, "Disabled (Enabled=0)");
            return true;
        }
        gameapi::Init(ver.GetBaseAddress());
        const DWORD b = ver.GetBaseAddress();

        // 读取标题（UTF-8）。空 = 不替换，仅记录。
        std::string title = cfg.GetString(kName, "Title", "");
        if (title.empty()) {
            LOG_INFO(kCat, "no 'Title' configured, title override inactive");
            return true;
        }

        // UTF-8 -> UTF-16（宽字符标题，中文安全）
        int n = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, g_title,
                                    (int)(sizeof(g_title) / sizeof(wchar_t)));
        if (n <= 0) {
            LOG_ERROR(kCat, "invalid UTF-8 Title, override inactive");
            return false;
        }

        // 备份原 IAT 值并接管（IAT hook：只改导入表槽，不动导出函数入口）
        if (!Patch::ReadMemory(b + R_IatCreateWindowExA, &pRealCreateWindowExA, 4) ||
            !pRealCreateWindowExA) {
            LOG_ERROR(kCat, "read IAT CreateWindowExA failed");
            return false;
        }
        if (!Patch::ReadMemory(b + R_IatSendMessageA, &pRealSendMessageA, 4) ||
            !pRealSendMessageA) {
            LOG_ERROR(kCat, "read IAT SendMessageA failed");
            return false;
        }
        if (!Patch::WriteU32(b + R_IatCreateWindowExA, (uint32_t)(uintptr_t)&HookCreateWindowExA) ||
            !Patch::WriteU32(b + R_IatSendMessageA,    (uint32_t)(uintptr_t)&HookSendMessageA)) {
            LOG_ERROR(kCat, "write IAT hook failed");
            return false;
        }

        LOG_INFO(kCat, "installed: main-window title -> L\"%ls\" (IAT@0x%X/0x%X)",
                 g_title,
                 (unsigned)(b + R_IatCreateWindowExA), (unsigned)(b + R_IatSendMessageA));
        return true;
    }
};

REGISTER_FEATURE(TitleOverrideFeature)

} // namespace fe_title
