// AsgardCampaignFeature.cpp
// ===================================================================
// [Cultures2Campaign]   (合并原 AsgardCampaign 入口按钮 + Cultures2Campaign 战役屏加载)
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
// 配置（plugins/config/CulturesGameExtend_Game.ini，与 Cultures2Campaign 共用同一 section）：
//   [Cultures2Campaign]
//   Enabled = 1
//   AddButton = 1   （是否插入按钮；0=禁用插入）
// ===================================================================
// AsgardCampaignFeature.cpp
// ===================================================================
// [Cultures2Campaign]   (merges the original AsgardCampaign entry button + Cultures2Campaign campaign-screen loading)
// On the main menu "Single Player" screen (MainMenuUI_Build case 3), insert a
// "Cultures II: Gates of Asgard" campaign entry button above the "Nordland" button.
//
// Principle: in case 3, every campaign button is built by an inline sequence of
// "new(0x60) -> GetSagaText(text pair) -> UI_CreateButton -> UI_AddButton -> coordinate accumulation".
// This Feature hooks the Nordland button object creation site (0x4D295E call operator new, 5-byte E8)
// and redirects to this DLL's AsgardButtonStub; the stub runs inside the same stack frame
// (EBP/ESI are both inherited from MainMenuUI_Build), first fully creates the Asgard button using the
// game's own APIs + accumulates coordinates, then jumps back to 0x4D2963 to continue the original
// Nordland creation flow.
//
// Implementation note (2026-08 refactor):
//   The old version encoded this logic as a 121-byte machine-code array written into a .text padding
//   region of game.exe (code cave), requiring 8 hand-computed rel32 offsets and being undebuggable.
//   It is now a __declspec(naked) inline-assembly function that lives directly in this DLL:
//     - game.exe has no ASLR (RELOCS_STRIPPED) and is not LARGE_ADDRESS_AWARE, so the user-mode
//       ceiling is 0x7FFFFFFF; an E9 rel32 (±2GB) can always reach this DLL — no distance risk;
//     - all game-function calls use "call dword ptr [global pointer]" and returns use
//       "jmp dword ptr [g_ret]" — zero hand-computed rel32 throughout;
//     - the only rel32 that must be computed is the single E9 at the hook site, a standard hook write.
//   This no longer occupies a game.exe code cave; the CodeCaveStart config option is void for this Feature.
//
// Button text: saga table (table 13, saga001.ini) IDs 25/26 — already added in
//   Data\text\l10\strings\saga\saga001.ini (l10 localized build).
// Control ID: 0x138E (5006). Note: the original 0x1397 (5015) is already taken by the vanilla game
//   = UserCampaign00 screen, which was exactly the root cause of "clicking jumps elsewhere"; now
//   Cultures2CampaignFeature takes over the slot-6 jump-table entry for 5006, and this button is only
//   responsible for "being drawn on the Single Player screen".
//
// Config (plugins/config/CulturesGameExtend_Game.ini, same section as Cultures2Campaign):
//   [Cultures2Campaign]
//   Enabled = 1
//   AddButton = 1   (whether to insert the button; 0 = disable insertion)
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


const char* kName = "Cultures2Campaign";
const char* kCat  = "[Cultures2Campaign]";

// ---- hook 点（RVA = VA - 0x400000；gameapi::Va 约定）----

#pragma region Hook Points & Game-Function References
// ---- hook site (RVA = VA - 0x400000; gameapi::Va convention) ----
constexpr uintptr_t HOOK = 0xD295E;
// VA 0x4D295E：call operator new（Nordland 按钮对象）
// VA 0x4D295E: call operator new (Nordland button object)
constexpr uintptr_t RET  = 0xD2963;
constexpr uintptr_t F_New        = 0xE55BD;
// VA 0x4D2963：跳回点（cmp eax, ebx 继续原流程）
// VA 0x4D2963: return point (cmp eax, ebx, continue original flow)
constexpr uintptr_t F_GetSagaTxt = 0xE169E;

// ---- stub 引用的游戏函数 / 全局（RVA）----

// ---- game functions / globals referenced by the stub (RVA) ----
constexpr uintptr_t F_CreateBtn  = 0xB7C2E;
// VA 0x4E55BD operator new(uint)
// VA 0x4E55BD operator new(uint)
constexpr uintptr_t F_Sub439B9C  = 0x39B9C;
constexpr uintptr_t F_AddBtn     = 0x768DA;
// VA 0x4E169E StringTable_GetSagaText(id) = 表13
// VA 0x4E169E StringTable_GetSagaText(id) = table 13
constexpr uintptr_t D_UiRoot     = 0x154F20;
static void* p_operatorNew   = nullptr;
// VA 0x4B7C2E UI_CreateButton
// VA 0x4B7C2E UI_CreateButton
static void* p_getSagaText   = nullptr;
static void* p_createButton  = nullptr;
// VA 0x439B9C 按钮挂到界面（thiscall ecx=edi）
// VA 0x439B9C attach button to UI (thiscall ecx=edi)
static void* p_sub439B9C     = nullptr;
static void* p_uiAddButton   = nullptr;
// VA 0x4768DA UI_AddButton（thiscall ecx=dword_554F20）
// VA 0x4768DA UI_AddButton (thiscall ecx=dword_554F20)
static void* p_ret           = nullptr;
static void* p_uiRootSlot    = nullptr;
// VA 0x554F20 UI 根对象槽位
// VA 0x554F20 UI root object slot
enum : int {

// ---- 运行时绑定的跳转目标（stub 通过间接寻址引用，免手算 rel32）----

// ---- runtime-bound jump targets (stub references via indirect addressing, no hand-computed rel32) ----
    kAsgardCtlId = 0x138E,
    kTextName    = 0x19,
    kTextHint    = 0x1A,
    kBtnObjSize  = 0x60
};
__declspec(naked) void AsgardButtonStub() {
// 0x4D2963
// 0x4D2963
    __asm {
        call dword ptr [p_operatorNew]
// &dword_554F20（取址，非取值）
// &dword_554F20 (address-of, not value-of)
        push eax

// ---- 常量（内联汇编中作为立即数引用）----

// ---- constants (referenced as immediates in inline asm) ----
        push kBtnObjSize
        call dword ptr [p_operatorNew]
// 5006，MainMenu_OnCommand 跳表 slot[6] 空闲控件 ID
// 5006, free control ID in MainMenu_OnCommand jump-table slot[6]
        pop  ecx
        mov  [ebp-4], eax
// saga 表 ID 25：战役名
// saga table ID 25: campaign name
        cmp  eax, ebx
        jz   L_newFailed
// saga 表 ID 26：提示
// saga table ID 26: hint
        lea  eax, [esi+216Ch]
        push eax
// 按钮对象大小，与 Nordland 一致
// button object size, same as Nordland
        push kAsgardCtlId
        push kTextHint

// ===================================================================
// AsgardButtonStub —— 寄生在 MainMenuUI_Build 栈帧内执行
//   进入条件：由 0x4D295E 的 E9 跳入，此时
//     EBP = MainMenuUI_Build 栈帧      ESI = 主菜单 UI 对象(this)
//     EDI = 按钮容器                    EBX = 0
//   栈帧槽位沿用原函数：[ebp-4]=临时按钮对象  [ebp-8]/[ebp-10h]=布局坐标
//   [ebp-14h]=UI_CreateButton 输出缓冲
//   退出：jmp 0x4D2963，EAX 必须为 Nordland 按钮对象（原指令的返回值）
// ===================================================================
#pragma endregion

#pragma region AsgardButtonStub (naked inline asm)
// ===================================================================
// AsgardButtonStub —— runs parasitically inside the MainMenuUI_Build stack frame
//   Entry: jumped in by the E9 at 0x4D295E, at which point
//     EBP = MainMenuUI_Build stack frame      ESI = main-menu UI object (this)
//     EDI = button container                  EBX = 0
//   Stack-frame slots reused from the original function:
//     [ebp-4]  = temporary button object      [ebp-8]/[ebp-10h] = layout coordinates
//     [ebp-14h] = UI_CreateButton output buffer
//   Exit: jmp 0x4D2963; EAX must hold the Nordland button object (return value of the original instruction)
// ===================================================================
        call dword ptr [p_getSagaText]
        pop  ecx
        // ---- 重放被 hook 覆盖的指令：创建 Nordland 按钮对象 ----
        // ---- replay the hooked instruction: create the Nordland button object ----
        push eax
        push kTextName
// 暂存 Nordland 对象，退出前还原
// stash Nordland object, restore before exit
        call dword ptr [p_getSagaText]

        // ---- 创建 Asgard 按钮对象 ----

        // ---- create the Asgard button object ----
        pop  ecx
        mov  ecx, [ebp-4]
        push eax
// 清 cdecl 参数（同原 0x4D2965）
// clear cdecl arg (same as original 0x4D2965)
        lea  eax, [ebp-14h]
        push eax
        call dword ptr [p_createButton]
        mov  [ebp-4], eax

        // ---- 取两条 saga 文本并创建按钮 ----

        // ---- fetch the two saga texts and create the button ----
        jmp  L_attach
    L_newFailed:
        mov  [ebp-4], ebx
    L_attach:
        push dword ptr [ebp-4]
        mov  ecx, edi
        call dword ptr [p_sub439B9C]
        mov  ecx, [p_uiRootSlot]
        mov  ecx, [ecx]
        push 1
        push dword ptr [ebp-4]
        call dword ptr [p_uiAddButton]
        mov  eax, [ebp-10h]
        mov  ecx, [ebp-8]
        lea  eax, [eax+ecx+0Ah]
        mov  [ebp-10h], eax
        pop  eax


        jmp  dword ptr [p_ret]
    }


}
        // ---- 挂进界面并注册 ----
        // ---- attach to UI and register ----
static bool VerifyBytes(DWORD base, DWORD off, const uint8_t* exp, size_t n) {
    std::vector<uint8_t> cur = Patch::ReadBytes(base + off, n);
    return cur.size() == n && memcmp(cur.data(), exp, n) == 0;
}
class AsgardCampaignFeature : public Feature {
// ecx = dword_554F20
// ecx = dword_554F20
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }
    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {

        // ---- 布局坐标累加（与 Nordland 的 0x4D29B7 同款）----

        // ---- layout coordinate accumulation (same as Nordland's 0x4D29B7) ----
        if (!cfg.GetBool(kName, "Enabled", false)) {
            LOG_INFO(kCat, "Disabled (Enabled=0)");
            return true;
        }


        gameapi::Init(ver.GetBaseAddress());
// 还原 Nordland 对象
// restore Nordland object
        if (!cfg.GetBool(kName, "AddButton", true)) {
            LOG_INFO(kCat, "AddButton=0, skip button injection");
            return true;
        }

#pragma endregion

#pragma region Feature Install & Verification
        DWORD base = ver.GetBaseAddress();
        p_operatorNew  = (void*)(base + F_New);
        p_getSagaText  = (void*)(base + F_GetSagaTxt);
        p_createButton = (void*)(base + F_CreateBtn);


        p_sub439B9C    = (void*)(base + F_Sub439B9C);
        p_uiAddButton  = (void*)(base + F_AddBtn);
        p_ret          = (void*)(base + RET);
        p_uiRootSlot   = (void*)(base + D_UiRoot);


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

        // 1) 绑定 stub 内所有间接跳转目标

        // 1) bind all indirect jump targets referenced inside the stub
            return false;
        }
        LOG_INFO(kCat, "Asgard button injected (stub@0x%X, rel=%d, text saga 25/26, ctl 0x1397)",
                 stub, rel);
        return true;
    }
};

        // 2) hook 0x4D295E：原 5 字节 call operator new -> E9 跳向本 DLL 的 stub

        // 2) hook 0x4D295E: original 5-byte "call operator new" -> E9 jump to this DLL's stub
REGISTER_FEATURE(AsgardCampaignFeature)
}
#pragma endregion
