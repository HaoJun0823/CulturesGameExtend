// AsgardCampaignFeature.cpp
// ===================================================================
// [AsgardCampaign]
// 在主菜单"单人游戏"界面（MainMenuUI_Build case 3）的"北国风云(Nordland)"
// 按钮之上，插入"文化II：阿斯加德之门"战役入口按钮。
//
// 原理：case 3 每个战役按钮都是内联的"new(0x60) → GetSagaText(文本对) →
// UI_CreateButton → UI_AddButton → 坐标累加"序列。本 Feature hook
// Nordland 按钮对象创建点（0x4D295E call operator new，5 字节 E8），
// 跳转到本 DLL 的 AsgardButtonStub；stub 在同一栈帧内（EBP/ESI 均由
// MainMenuUI_Build 继承）先用游戏自身 API 完整创建 Asgard 按钮 +
// 坐标累加，再跳回 0x4D2963 继续原流程创建 Nordland。
//
// 实现方式说明（2026-08 重构）：
//   旧版把这段逻辑编码为 121 字节机器码数组写进 game.exe 的 .text 填充区
//   （code cave），需手算 8 处 rel32 且不可调试。现改为 __declspec(naked)
//   内联汇编函数，直接驻留本 DLL：
//     - game.exe 无 ASLR（RELOCS_STRIPPED）且非 LARGE_ADDRESS_AWARE，
//       用户态上限 0x7FFFFFFF，E9 rel32(±2GB) 必定可达本 DLL，无距离风险；
//     - 所有游戏函数调用一律 call dword ptr [全局指针]，跳回用
//       jmp dword ptr [g_ret]，全程零手算 rel32；
//     - 唯一需要计算的 rel32 是 hook 点那条 E9，属标准 hook 写入。
//   已不再占用 game.exe 的 code cave，CodeCaveStart 配置项对本 Feature 作废。
//
// 按钮文本：saga 表（表13, saga001.ini）ID 25/26 —— 已在
//   Data\text\l10\strings\saga\saga001.ini 添加（l10 汉化版）。
// 控件 ID：0x138E (5006)。注：原 0x1397(5015) 原版已占 = UserCampaign00 屏，
// 这正是「点了跳到别处」的根因；现改由 Cultures2CampaignFeature 接管 5006
// 跳表项，本按钮只负责「画在单人游戏界面上」这一件事。
//
// 配置（plugins/config/CulturesGameExtend_Game.ini）：
//   [AsgardCampaign]
//   Enabled = 1
//   ; AddButton = 1   （是否插入按钮；0=禁用插入）
// ===================================================================
#include "pch.h"
#include "Core/Feature.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Patch.h"
#include "Core/Logger.h"
#include "Core/GameApi.h"
#include <cstring>

namespace fe_asgard {

const char* kName = "AsgardCampaign";
const char* kCat  = "[AsgardCampaign]";

// ---- hook 点（RVA = VA - 0x400000；gameapi::Va 约定）----
constexpr uintptr_t HOOK = 0xD295E;  // VA 0x4D295E：call operator new（Nordland 按钮对象）
constexpr uintptr_t RET  = 0xD2963;  // VA 0x4D2963：跳回点（cmp eax, ebx 继续原流程）

// ---- stub 引用的游戏函数 / 全局（RVA）----
constexpr uintptr_t F_New        = 0xE55BD; // VA 0x4E55BD operator new(uint)
constexpr uintptr_t F_GetSagaTxt = 0xE169E; // VA 0x4E169E StringTable_GetSagaText(id) = 表13
constexpr uintptr_t F_CreateBtn  = 0xB7C2E; // VA 0x4B7C2E UI_CreateButton
constexpr uintptr_t F_Sub439B9C  = 0x39B9C; // VA 0x439B9C 按钮挂到界面（thiscall ecx=edi）
constexpr uintptr_t F_AddBtn     = 0x768DA; // VA 0x4768DA UI_AddButton（thiscall ecx=dword_554F20）
constexpr uintptr_t D_UiRoot     = 0x154F20;// VA 0x554F20 UI 根对象槽位

// ---- 运行时绑定的跳转目标（stub 通过间接寻址引用，免手算 rel32）----
static void* p_operatorNew   = nullptr;
static void* p_getSagaText   = nullptr;
static void* p_createButton  = nullptr;
static void* p_sub439B9C     = nullptr;
static void* p_uiAddButton   = nullptr;
static void* p_ret           = nullptr;   // 0x4D2963
static void* p_uiRootSlot    = nullptr;   // &dword_554F20（取址，非取值）

// ---- 常量（内联汇编中作为立即数引用）----
enum : int {
    kAsgardCtlId = 0x138E,  // 5006，MainMenu_OnCommand 跳表 slot[6] 空闲控件 ID
    kTextName    = 0x19,    // saga 表 ID 25：战役名
    kTextHint    = 0x1A,    // saga 表 ID 26：提示
    kBtnObjSize  = 0x60     // 按钮对象大小，与 Nordland 一致
};

// ===================================================================
// AsgardButtonStub —— 寄生在 MainMenuUI_Build 栈帧内执行
//   进入条件：由 0x4D295E 的 E9 跳入，此时
//     EBP = MainMenuUI_Build 栈帧      ESI = 主菜单 UI 对象(this)
//     EDI = 按钮容器                    EBX = 0
//   栈帧槽位沿用原函数：[ebp-4]=临时按钮对象  [ebp-8]/[ebp-10h]=布局坐标
//   [ebp-14h]=UI_CreateButton 输出缓冲
//   退出：jmp 0x4D2963，EAX 必须为 Nordland 按钮对象（原指令的返回值）
// ===================================================================
__declspec(naked) void AsgardButtonStub() {
    __asm {
        // ---- 重放被 hook 覆盖的指令：创建 Nordland 按钮对象 ----
        call dword ptr [p_operatorNew]
        push eax                            // 暂存 Nordland 对象，退出前还原

        // ---- 创建 Asgard 按钮对象 ----
        push kBtnObjSize
        call dword ptr [p_operatorNew]
        pop  ecx                            // 清 cdecl 参数（同原 0x4D2965）
        mov  [ebp-4], eax
        cmp  eax, ebx
        jz   L_newFailed

        // ---- 取两条 saga 文本并创建按钮 ----
        lea  eax, [esi+216Ch]
        push eax
        push kAsgardCtlId
        push kTextHint
        call dword ptr [p_getSagaText]
        pop  ecx
        push eax
        push kTextName
        call dword ptr [p_getSagaText]
        pop  ecx
        mov  ecx, [ebp-4]
        push eax
        lea  eax, [ebp-14h]
        push eax
        call dword ptr [p_createButton]
        mov  [ebp-4], eax
        jmp  L_attach

    L_newFailed:
        mov  [ebp-4], ebx

    L_attach:
        // ---- 挂进界面并注册 ----
        push dword ptr [ebp-4]
        mov  ecx, edi
        call dword ptr [p_sub439B9C]
        mov  ecx, [p_uiRootSlot]
        mov  ecx, [ecx]                     // ecx = dword_554F20
        push 1
        push dword ptr [ebp-4]
        call dword ptr [p_uiAddButton]

        // ---- 布局坐标累加（与 Nordland 的 0x4D29B7 同款）----
        mov  eax, [ebp-10h]
        mov  ecx, [ebp-8]
        lea  eax, [eax+ecx+0Ah]
        mov  [ebp-10h], eax

        pop  eax                            // 还原 Nordland 对象
        jmp  dword ptr [p_ret]
    }
}

static bool VerifyBytes(DWORD base, DWORD off, const uint8_t* exp, size_t n) {
    std::vector<uint8_t> cur = Patch::ReadBytes(base + off, n);
    return cur.size() == n && memcmp(cur.data(), exp, n) == 0;
}

class AsgardCampaignFeature : public Feature {
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }

    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        if (!cfg.GetBool(kName, "Enabled", false)) {
            LOG_INFO(kCat, "Disabled (Enabled=0)");
            return true;
        }
        gameapi::Init(ver.GetBaseAddress());
        if (!cfg.GetBool(kName, "AddButton", true)) {
            LOG_INFO(kCat, "AddButton=0, skip button injection");
            return true;
        }

        DWORD base = ver.GetBaseAddress();

        // 1) 绑定 stub 内所有间接跳转目标
        p_operatorNew  = (void*)(base + F_New);
        p_getSagaText  = (void*)(base + F_GetSagaTxt);
        p_createButton = (void*)(base + F_CreateBtn);
        p_sub439B9C    = (void*)(base + F_Sub439B9C);
        p_uiAddButton  = (void*)(base + F_AddBtn);
        p_ret          = (void*)(base + RET);
        p_uiRootSlot   = (void*)(base + D_UiRoot);

        // 2) hook 0x4D295E：原 5 字节 call operator new -> E9 跳向本 DLL 的 stub
        static const uint8_t hookVerify[] = { 0xE8, 0x5A, 0x2C, 0x01, 0x00 };
        if (!VerifyBytes(base, HOOK, hookVerify, sizeof(hookVerify))) {
            LOG_WARN(kCat, "hook verify mismatch @0x%X (already patched?)", base + HOOK);
            return false;
        }

        DWORD stub = (DWORD)(void*)&AsgardButtonStub;
        int32_t rel = (int32_t)(stub - (base + HOOK + 5));
        uint8_t jmp[5] = { 0xE9, 0, 0, 0, 0 };
        memcpy(&jmp[1], &rel, 4);
        if (!Patch::WriteBytes(base + HOOK, jmp, 5)) {
            LOG_ERROR(kCat, "hook write failed @0x%X", base + HOOK);
            return false;
        }

        LOG_INFO(kCat, "Asgard button injected (stub@0x%X, rel=%d, text saga 25/26, ctl 0x1397)",
                 stub, rel);
        return true;
    }
};

REGISTER_FEATURE(AsgardCampaignFeature)

} // namespace fe_asgard
