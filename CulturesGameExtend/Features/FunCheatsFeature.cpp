// FunCheatsFeature.cpp
// ===================================================================
// [FunCheats] -- 移植 C3CD.exe 资料盘的 7 个 fun* 反转作弊码到 Game.exe
//
// 作弊码一览（正向键入 → 滚动缓冲中的反序串 → 动作 / 门控）：
//   funexplore     erolpxenuf      sub_49FEC3 全图探索        !多人标志(byte_510B9C)
//   funhigh        hgihnuf         速度对象+20 = 150          速度对象(dword_50F87C)非零
//   funnorm        mronnuf         速度对象+20 = 100          同上
//   funlow         wolnuf          速度对象+20 = 75           同上
//   funmapsmall    llamspamnuf     mkdir mapshots + small_%4.4d.bmp 25%   !sub_405A79()
//   funmapverybig  gibyrevpamnuf   mkdir mapshots + verybig_%4.4d.bmp 100% !sub_405A79()
//   funcolors      srolocnuf       玩家颜色轮换(容器+0x238 环形+1)  无门控
//
// 安全机制（完整复刻 C3CD 原版，多人游戏不破坏公平）：
//   - funexplore 在多人模式（byte_510B9C != 0）下被跳过（原版同款检查）
//   - 速度码仅当游戏速度对象存在时生效（菜单/前台弹窗时 dword_50F87C==0）
//   - 截图码沿用原版许可检查（sub_405A79() != 0 则跳过，与德语码
//     pamagem/paminim 共用同一门控与计数器 dword_569610/0x569614）
//   - funcolors 沿用原版无附加检查
//
// Hook 设计（零 stolen bytes）：
//   Game.exe sub_4BB0E9（WM_CHAR 键入处理）在 pamagem 块里：
//       4bb172: push edi            ; Str2 = "pamagem"
//       4bb173: push esi            ; Str1 = 滚动缓冲 byte_5695F8（新字符在[0]）
//       4bb174: call _strncmp       ; E8 D7 C0 02 00 → 0x4E7250
//       4bb179: add esp, 10h        ; 同时清 strlen+strncmp 共 4 个 push
//   把 0x4BB174 处 E8 的 rel32 重定向到本模块 naked 中继：
//       中继: call FunCheats_ProcessBuffer（cdecl，从全局地址自取数据）
//             jmp [g_realStrncmp]   （尾跳真 strncmp 0x4E7250）
//   栈上 strncmp 的 3 个参数（esi/edi/eax 已 push）原封不动；call 语义保持
//   （返回地址 0x4BB179 不变），eax/ecx/edx 允许被中继破坏（cdecl 约定），
//   ebx/esi/edi 由 C++ 函数的编译器序言自动保存。
//
//   时序：此刻滚动缓冲已完成本字符更新（memcpy 后移 + 新字符入 [0]）、
//   nalamkcug（探索码）检查已处理完。fun 码若命中：
//     1) 执行动作 + 命中后处理（音效 sub_4E2367 + 清缓冲[0] +
//        状态条提示 sub_4127EF —— 完整复刻原版 v71 命中分支）；
//     2) 缓冲[0] 已清零 → 中继尾跳的 strncmp 做 pamagem 匹配时自然失败，
//        后续 paminim/netsaklam 同样失败，游戏原生码零干扰；
//     3) 游戏自己的 v71 标志（[ebp+0xB]）保持 0，不会重复播音效/提示。
//   fun 码未命中：缓冲不动，strncmp 照常执行，完全原版行为。
//
//   注意：pamagem 的 strncmp 不在多人 if 内 —— 每个键入字符（含多人聊天）
//   都会进入中继，因此多人门控由 FunCheats_ProcessBuffer 自查
//   （funexplore 的 byte_510B9C 检查），与 C3CD 原版一致。
//
// RVA 约定：本代码库 hook 常量用 RVA（VA - 0x400000），运行时 base+RVA。
// ===================================================================
#include "pch.h"
#include "Core/Feature.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Patch.h"
#include "Core/Logger.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace fe_funcheats {

const char* kName = "FunCheats";
const char* kCat  = "[FunCheats]";

// ---- 目标地址（RVA = VA - 0x400000；运行时 base+RVA = 真实 VA）----
static const DWORD kRvaHookSite     = 0xBB174;  // pamagem 块 call _strncmp（E8 D7 C0 02 00）
static const DWORD kRvaStrncmp      = 0xE7250;  // 游戏 CRT strncmp（中继尾跳目标）
static const DWORD kRvaKeyBuf       = 0x1695F8; // byte_5695F8 滚动缓冲[21]，新字符在[0]
static const DWORD kRvaMultiplayer  = 0x110B9C; // byte_510B9C 多人标志（0=单人）
static const DWORD kRvaSpeedObj     = 0x10F87C; // dword_50F87C 速度对象指针（+20 = 速度 ms）
static const DWORD kRvaGlobalCfg    = 0x169274; // dword_569274 引擎配置对象指针
static const DWORD kRvaRevealFn     = 0x9FEC3;  // sub_49FEC3 thiscall(ecx=cfg) 全图探索
static const DWORD kRvaShotFn       = 0x90241;  // sub_490241 cdecl(name, pct) 截图
static const DWORD kRvaMkdirFn      = 0x66C1;   // sub_4066C1 cdecl(path) 建目录
static const DWORD kRvaShotChkFn    = 0x5A79;   // sub_405A79 cdecl() -> al 截图许可
static const DWORD kRvaColorFn      = 0xBA3B0;  // sub_4BA3B0 thiscall(ecx=容器, idx) 换色
static const DWORD kRvaSndFn        = 0xE2367;  // sub_4E2367 cdecl() 确认音效
static const DWORD kRvaTipFn        = 0x127EF;  // sub_4127EF thiscall(ecx, 5 栈参) 提示
static const DWORD kRvaTipThis      = 0x110C0C; // dword_510C0C 提示分发器对象指针
static const DWORD kRvaShotCntSmall = 0x169610; // small_%4.4d 计数器（与 pamagem 共享）
static const DWORD kRvaShotCntBig   = 0x169614; // verybig_%4.4d 计数器（与 paminim 共享）

// ---- 速度值（与 C3CD 原版一致）----
static const DWORD kSpeedHigh = 150;
static const DWORD kSpeedNorm = 100;
static const DWORD kSpeedLow  = 75;

// ---- 反转码（用户正向键入 fun*，滚动缓冲中呈现为反序串）----
static const char* kCodeExplore    = "erolpxenuf";    // funexplore
static const char* kCodeHigh       = "hgihnuf";       // funhigh
static const char* kCodeNorm       = "mronnuf";       // funnorm
static const char* kCodeLow        = "wolnuf";        // funlow
static const char* kCodeMapSmall   = "llamspamnuf";   // funmapsmall
static const char* kCodeMapVeryBig = "gibyrevpamnuf"; // funmapverybig
static const char* kCodeColors     = "srolocnuf";     // funcolors

// ---- 安装时固化的运行时状态 ----
// 被 __declspec(naked) 内联汇编直接引用，必须 C 链接（不重整名）
extern "C" static uintptr_t g_realStrncmp = 0; // 真 strncmp 地址（尾跳目标）
static DWORD g_base = 0;                       // Game.exe 运行时基址

// ---- 游戏函数原型 ----
// thiscall 用 __fastcall 模拟：首参进 ecx、次参占位 edx，栈参数由被调方
// 清理（与 thiscall 的 retn N 语义一致）。均按 IDA 权威反汇编确认。
typedef void (__fastcall *Fn_this0)(void* self, int edx);                       // sub_49FEC3
typedef void (__fastcall *Fn_this1)(void* self, int edx, int a1);               // sub_4BA3B0
typedef void (__fastcall *Fn_this5)(void* self, int edx, int a1, int a2,
                                    int a3, int a4, int a5);                    // sub_4127EF
typedef void (__cdecl *Fn_cdecl0)();                                            // sub_4E2367
typedef void (__cdecl *Fn_cdecl1)(const char* path);                            // sub_4066C1
typedef void (__cdecl *Fn_cdecl2)(const char* name, int pct);                   // sub_490241
typedef unsigned char (__cdecl *Fn_chk0)();                                     // sub_405A79

// ---------------------------------------------------------------------------
// fun 码匹配 + 动作执行（中继调用；cdecl，从全局地址自取全部数据）
// 调用时机：滚动缓冲已含新字符、nalamkcug 已检查（见文件头注释）
// ---------------------------------------------------------------------------
extern "C" void __cdecl FunCheats_ProcessBuffer() {
    if (!g_base || !g_realStrncmp) return; // 未安装（防御，不应发生）

    const char* buf = (const char*)(g_base + kRvaKeyBuf);
    bool hit = false;

    // ---- funexplore：全图探索（多人模式禁用 —— 原版同款门控）----
    if (!*(BYTE*)(g_base + kRvaMultiplayer)) {
        if (!strncmp(buf, kCodeExplore, (size_t)strlen(kCodeExplore))) {
            DWORD cfg = *(DWORD*)(g_base + kRvaGlobalCfg);
            if (cfg) {
                ((Fn_this0)(g_base + kRvaRevealFn))((void*)cfg, 0);
                hit = true;
            }
        }
    }

    // ---- funhigh / funnorm / funlow：游戏速度（速度对象存在才生效 —— 原版门控）----
    DWORD speedObj = *(DWORD*)(g_base + kRvaSpeedObj);
    if (speedObj) {
        if      (!strncmp(buf, kCodeHigh, (size_t)strlen(kCodeHigh)))
            { *(DWORD*)(speedObj + 20) = kSpeedHigh; hit = true; }
        else if (!strncmp(buf, kCodeNorm, (size_t)strlen(kCodeNorm)))
            { *(DWORD*)(speedObj + 20) = kSpeedNorm; hit = true; }
        else if (!strncmp(buf, kCodeLow,  (size_t)strlen(kCodeLow)))
            { *(DWORD*)(speedObj + 20) = kSpeedLow;  hit = true; }
    }

    // ---- funmapsmall / funmapverybig：地图截图（沿用原版许可门控）----
    if (!((Fn_chk0)(g_base + kRvaShotChkFn))()) {
        if (!strncmp(buf, kCodeMapSmall, (size_t)strlen(kCodeMapSmall))) {
            ((Fn_cdecl1)(g_base + kRvaMkdirFn))("mapshots");
            DWORD n = *(DWORD*)(g_base + kRvaShotCntSmall);
            *(DWORD*)(g_base + kRvaShotCntSmall) = n + 1; // 原版 dword_569610++
            char name[64];
            _snprintf(name, sizeof(name) - 1, "mapshots\\small_%4.4d.bmp", (int)n);
            name[sizeof(name) - 1] = 0;
            ((Fn_cdecl2)(g_base + kRvaShotFn))(name, 25);
            hit = true;
        }
        else if (!strncmp(buf, kCodeMapVeryBig, (size_t)strlen(kCodeMapVeryBig))) {
            ((Fn_cdecl1)(g_base + kRvaMkdirFn))("mapshots");
            DWORD n = *(DWORD*)(g_base + kRvaShotCntBig);
            *(DWORD*)(g_base + kRvaShotCntBig) = n + 1;   // 原版 dword_569614++
            char name[64];
            _snprintf(name, sizeof(name) - 1, "mapshots\\verybig_%4.4d.bmp", (int)n);
            name[sizeof(name) - 1] = 0;
            ((Fn_cdecl2)(g_base + kRvaShotFn))(name, 100);
            hit = true;
        }
    }

    // ---- funcolors：玩家颜色轮换（无门控；容器+0x238 环形 +1，4 回绕 0）----
    if (!strncmp(buf, kCodeColors, (size_t)strlen(kCodeColors))) {
        DWORD cfg = *(DWORD*)(g_base + kRvaGlobalCfg);
        if (cfg) {
            DWORD container = *(DWORD*)(cfg + 4);
            if (container) {
                DWORD idx = *(DWORD*)(container + 0x238) + 1;
                if (idx == 4) idx = 0;
                ((Fn_this1)(g_base + kRvaColorFn))((void*)container, 0, (int)idx);
                hit = true;
            }
        }
    }

    // ---- 命中后处理（复刻原版 v71 分支：音效 + 清缓冲 + 状态条提示）----
    if (hit) {
        ((Fn_cdecl0)(g_base + kRvaSndFn))();          // 确认音效（内部自检速度对象）
        *(BYTE*)(g_base + kRvaKeyBuf) = 0;            // 清缓冲：防原生码/后续 fun 码误匹配
        DWORD cfg     = *(DWORD*)(g_base + kRvaGlobalCfg);
        DWORD tipThis = *(DWORD*)(g_base + kRvaTipThis);
        if (cfg && tipThis) {
            // sub_4127EF(this=提示分发器, 0x18, 1, *(DWORD*)cfg, 9, 0)
            ((Fn_this5)(g_base + kRvaTipFn))((void*)tipThis, 0,
                                             0x18, 1, (int)(*(DWORD*)cfg), 9, 0);
        }
        LOG_DEBUG(kCat, "fun code hit (buffer=%s)", buf);
    }
}

// ---------------------------------------------------------------------------
// 中继：0x4BB174 的 call _strncmp 被重定向到这里。
// 栈上 strncmp 的 3 个参数原封不动；先做 fun 码匹配再尾跳真 strncmp。
// eax/ecx/edx 可自由破坏（cdecl）；esi（Str1=缓冲指针）与 ebx/edi 由
// FunCheats_ProcessBuffer 的编译器序言自动保存，尾跳时完好。
// ---------------------------------------------------------------------------
extern "C" void __declspec(naked) FunCheats_StrncmpRelay() {
    __asm {
        call FunCheats_ProcessBuffer
        jmp  dword ptr [g_realStrncmp]
    }
}

// ---------------------------------------------------------------------------
// Feature：安装 = 校验原字节 → 固化地址 → 重写 E8 rel32 → 回读验证
// ---------------------------------------------------------------------------
class FunCheatsFeature : public Feature {
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }
    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        if (!cfg.GetBool(kName, "Enabled", false)) {
            LOG_INFO(kCat, "Disabled by [%s] Enabled=0.", kName);
            return true;
        }
        DWORD base = ver.GetBaseAddress();
        uintptr_t hookVA = base + kRvaHookSite;

        // build-drift 校验：必须是原版 call _strncmp（E8 D7 C0 02 00 → 0x4E7250）
        static const uint8_t kHookOrig[5] = {0xE8, 0xD7, 0xC0, 0x02, 0x00};
        std::vector<uint8_t> cur = Patch::ReadBytes(hookVA, 5);
        if (cur.size() != 5 || memcmp(cur.data(), kHookOrig, 5) != 0) {
            LOG_WARN(kCat, "Hook site 0x%X prologue mismatch (build drift?), feature skipped",
                     (unsigned)hookVA);
            return false;
        }

        // 先固化运行时地址（写补丁后中继可能立即被任意键入触发）
        g_realStrncmp = base + kRvaStrncmp;
        g_base        = base;

        // 重写 E8 的 rel32 → 中继（保持 call 语义：返回地址 0x4BB179 不变）
        int32_t rel = (int32_t)((intptr_t)FunCheats_StrncmpRelay - (intptr_t)(hookVA + 5));
        uint8_t patch[5] = {0xE8,
                            (uint8_t)(rel & 0xFF),
                            (uint8_t)((rel >> 8) & 0xFF),
                            (uint8_t)((rel >> 16) & 0xFF),
                            (uint8_t)((rel >> 24) & 0xFF)};
        if (!Patch::WriteBytes(hookVA, patch, 5)) {
            LOG_ERROR(kCat, "WriteBytes failed at 0x%X", (unsigned)hookVA);
            return false;
        }

        // 回读验证
        cur = Patch::ReadBytes(hookVA, 5);
        if (cur.size() != 5 || memcmp(cur.data(), patch, 5) != 0) {
            LOG_ERROR(kCat, "Hook verify failed at 0x%X", (unsigned)hookVA);
            return false;
        }

        LOG_INFO(kCat, "fun* cheat codes active: funexplore / funhigh / funnorm / funlow / "
                       "funmapsmall / funmapverybig / funcolors (hook @0x%X, strncmp relay @0x%X)",
                 (unsigned)hookVA, (unsigned)g_realStrncmp);
        return true;
    }
};

REGISTER_FEATURE(FunCheatsFeature)

} // namespace fe_funcheats
