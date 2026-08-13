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
// TitleOverrideFeature.cpp
// ===================================================================
// [TitleOverride]
// Replace the game's main window title (window title bar) after the DLL is loaded.
//
// Background: the game only imports CreateWindowExA / SendMessageA, not SetWindowText; the window
// title is passed at creation time (lpWindowName), and may later be overwritten via
// SendMessageA(hwnd, WM_SETTEXT, ...) (the Cultures_Title project works precisely by intercepting
// SendMessageA's WM_SETTEXT).
//
// This Feature uses a dual IAT hook (safer and reversible than hooking an export-function entry
// directly):
//   1. CreateWindowExA: after the first top-level window (hWndParent==NULL) is created,
//      SetWindowTextW(hwnd, configured title) — overrides the original title from the creation args;
//   2. SendMessageA: when the main window receives WM_SETTEXT(0xC), redirect to
//      SendMessageW with the configured title (wide char) to prevent later game-side overwrites.
//      All other windows / messages are forwarded as-is — zero side effects.
// The title is UTF-16 (wide char) throughout, perfectly supporting Chinese; config is UTF-8 text.
//
// Config (plugins/config/CulturesGameExtend_Global.ini, overridable by Game.ini):
//   [TitleOverride]
//   Enabled = 1
//   Title = Cultures: Saga localized build        <- empty = no replacement
// ===================================================================
#include "pch.h"
#include "Core/Feature.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Patch.h"
#include "Core/Logger.h"
#include "Core/GameApi.h"
#include <Windows.h>


#ifndef CGE_VERSION_STR
#define CGE_VERSION_STR "0.0.0"
// 构建脚本未注入时的兜底
// fallback when the build script doesn't inject it
#endif
namespace fe_title {


const char* kName = "TitleOverride";


const char* kCat  = "[TitleOverride]";
constexpr uintptr_t R_IatCreateWindowExA = 0xF31D4;

// ---- IAT 槽（VA 0x4F31D4 / 0x4F31B8，RVA = VA - 0x400000）----

#pragma region IAT Slots & Runtime State
// ---- IAT slots (VA 0x4F31D4 / 0x4F31B8, RVA = VA - 0x400000) ----
constexpr uintptr_t R_IatSendMessageA    = 0xF31B8;
static HWND     g_mainHwnd = nullptr;

// ---- 运行时状态 ----

// ---- runtime state ----
static LONG     g_logOnce  = 0;
// 记录到的主窗口（仅日志）
// recorded main window (log only)
static wchar_t  g_title[256] = L"";
static decltype(&CreateWindowExA) pRealCreateWindowExA = nullptr;
// 调试日志只打一次
// debug log printed once
static decltype(&SendMessageA)    pRealSendMessageA    = nullptr;
static void FindMainWindow() {
// 完整标题：配置 Title + 追加 DLL 信息
// full title: configured Title + appended DLL info
    if (g_mainHwnd && IsWindow(g_mainHwnd)) return;


    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        if (!IsWindowVisible(hwnd)) return TRUE;

// ===================================================================
// 轮询兜底：游戏若在 IAT hook 之前缓存了 SendMessageA 地址（见
// sub_401000 `mov edi, ds:SendMessageA; call edi`），或通过其它途径
// 设置标题，hook 可能拦不到。此线程每 0.5s 检查主窗口标题，不符即修正。
// ===================================================================
#pragma endregion

#pragma region Title Watcher Thread
// ===================================================================
// Polling fallback: if the game cached the SendMessageA address before our IAT hook (see
// sub_401000 `mov edi, ds:SendMessageA; call edi`), or sets the title through another path, the hook
// may miss it. This thread checks the main window title every 0.5s and corrects it if it differs.
// ===================================================================
        if (GetWindow(hwnd, GW_OWNER)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId()) return TRUE;
        *(HWND*)lp = hwnd;
// 跳过 owned（对话框等）
// skip owned (dialogs etc.)
        return FALSE;
    }, (LPARAM)&g_mainHwnd);
}
static DWORD WINAPI TitleWatcher(LPVOID) {
// 只认本进程
// only our own process
    while (g_title[0]) {
        FindMainWindow();
        if (g_mainHwnd && IsWindow(g_mainHwnd)) {
// 第一个可见顶层窗口
// first visible top-level window
            wchar_t cur[128] = L"";
            GetWindowTextW(g_mainHwnd, cur, 128);
            if (wcscmp(cur, g_title) != 0) {


                SetWindowTextW(g_mainHwnd, g_title);
                if (!g_logOnce) {
                    g_logOnce = 1;
                    LOG_INFO(kCat, "watcher: corrected title of 0x%X",
                             (unsigned)(uintptr_t)g_mainHwnd);
                }
            }
        }
        Sleep(500);
    }
    return 0;
}
static HWND WINAPI HookCreateWindowExA(
    DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle,
    int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu,
    HINSTANCE hInstance, LPVOID lpParam) {
    HWND hw = pRealCreateWindowExA(dwExStyle, lpClassName, lpWindowName, dwStyle,
                                   X, Y, nWidth, nHeight, hWndParent, hMenu,
                                   hInstance, lpParam);

// ===================================================================
// HookCreateWindowExA —— 创建窗口时接管
//   仅对"顶层窗口"（hWndParent==NULL）生效：记住主窗口句柄并设置标题。
//   子窗口（按钮/控件等）一律原样放行，绝不改动。
// ===================================================================
#pragma endregion

#pragma region Hook Functions
// ===================================================================
// HookCreateWindowExA —— take over at window creation
//   Only affects "top-level windows" (hWndParent==NULL): remember the main window handle and set the
//   title. Child windows (buttons/controls etc.) are passed through untouched.
// ===================================================================
    if (hw && !hWndParent && g_title[0]) {
        if (!g_mainHwnd) g_mainHwnd = hw;
        SetWindowTextW(hw, g_title);
        if (!g_logOnce) {
            g_logOnce = 1;
            LOG_INFO(kCat, "top-level window created 0x%X, title applied",
                     (unsigned)(uintptr_t)hw);
        }
    }
    return hw;
}
static LRESULT WINAPI HookSendMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    if (Msg == WM_SETTEXT && g_title[0] && hWnd) {
        DWORD style = GetWindowLongW(hWnd, GWL_STYLE);
        if (!(style & WS_CHILD)) {
            if (!g_mainHwnd) g_mainHwnd = hWnd;
            return SendMessageW(hWnd, Msg, wParam, (LPARAM)g_title);
        }

// ===================================================================
// HookSendMessageA —— 拦截对顶层窗口的 WM_SETTEXT（0xC）
//   游戏创建后通过 SendMessageA(hwnd, WM_SETTEXT, ..., "Saga") 覆盖标题
//   （ANSI 路径）。这里对**任意顶层窗口**（无 WS_CHILD 样式）的 WM_SETTEXT
//   改走 SendMessageW + 宽字符标题，防覆盖且中文不乱码。
//   子窗口（按钮/编辑框等）的 WM_SETTEXT 原样放行，零副作用。
// ===================================================================

// ===================================================================
// HookSendMessageA —— intercept WM_SETTEXT (0xC) to top-level windows
//   The game overwrites the title after creation via SendMessageA(hwnd, WM_SETTEXT, ..., "Saga")
//   (ANSI path). Here, for WM_SETTEXT on **any top-level window** (no WS_CHILD style) we redirect to
//   SendMessageW + wide-char title, preventing overwrite and avoiding Chinese mojibake.
//   WM_SETTEXT on child windows (buttons/edit boxes etc.) is passed through untouched — zero side effects.
// ===================================================================
    }
    return pRealSendMessageA(hWnd, Msg, wParam, lParam);
}
class TitleOverrideFeature : public Feature {
// 顶层窗口（主窗口/对话框）
// top-level window (main window / dialog)
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }
    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        if (!cfg.GetBool(kName, "Enabled", false)) {
            LOG_INFO(kCat, "Disabled (Enabled=0)");
            return true;

// ===================================================================
// 安装
// ===================================================================
#pragma endregion

#pragma region Feature Install
// ===================================================================
// Install
// ===================================================================
        }
        gameapi::Init(ver.GetBaseAddress());
        const DWORD b = ver.GetBaseAddress();
        std::string title = cfg.GetString(kName, "Title", "");


        if (title.empty()) {
            LOG_INFO(kCat, "no 'Title' configured, title override inactive");
            return true;
        }
        wchar_t base[128];
        int n = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, base,
                                    (int)(sizeof(base) / sizeof(wchar_t)));

        // 读取标题（UTF-8）。空 = 不替换，仅记录。

        // read title (UTF-8). Empty = no replacement, just record.
        if (n <= 0) {
            LOG_ERROR(kCat, "invalid UTF-8 Title, override inactive");
            return false;
        }
        wchar_t verW[32], dateW[48];

        // 配置标题（UTF-8 -> UTF-16）

        // config title (UTF-8 -> UTF-16)
        MultiByteToWideChar(CP_UTF8, 0, CGE_VERSION_STR, -1, verW, 32);
        MultiByteToWideChar(CP_UTF8, 0, __DATE__, -1, dateW, 48);
        swprintf_s(g_title, 256, L"%ls  CulturesGameExtend v%ls (Build %ls %hs)",
                   base, verW, dateW, __TIME__);
        if (!Patch::ReadMemory(b + R_IatCreateWindowExA, &pRealCreateWindowExA, 4) ||
            !pRealCreateWindowExA) {
            LOG_ERROR(kCat, "read IAT CreateWindowExA failed");

        // 完整标题 = 配置 Title + 追加 DLL 信息（硬编码，不受 ini 控制）：
        //   <配置标题>  CulturesGameExtend v<版本> (Build <构建日期> <构建时间>)

        // full title = configured Title + appended DLL info (hard-coded, not ini-controlled):
        //   <config title>  CulturesGameExtend v<version> (Build <date> <time>)
            return false;
        }
        if (!Patch::ReadMemory(b + R_IatSendMessageA, &pRealSendMessageA, 4) ||
            !pRealSendMessageA) {
            LOG_ERROR(kCat, "read IAT SendMessageA failed");

        // 备份原 IAT 值并接管（IAT hook：只改导入表槽，不动导出函数入口）

        // back up the original IAT values and take over (IAT hook: only change the import-table slot,
        // not the export-function entry)
            return false;
        }
        if (!Patch::WriteU32(b + R_IatCreateWindowExA, (uint32_t)(uintptr_t)&HookCreateWindowExA) ||
            !Patch::WriteU32(b + R_IatSendMessageA,    (uint32_t)(uintptr_t)&HookSendMessageA)) {
            LOG_ERROR(kCat, "write IAT hook failed");
            return false;
        }
        LOG_INFO(kCat, "installed: title override active (utf8len=%d wide=%d)",
                 (int)title.size(), n);
        HANDLE h = CreateThread(nullptr, 0, TitleWatcher, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
        return true;
    }
};
REGISTER_FEATURE(TitleOverrideFeature)


}
#pragma endregion
