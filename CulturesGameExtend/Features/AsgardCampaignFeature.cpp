// AsgardCampaignFeature.cpp
// ===================================================================
// [AsgardCampaign]
// 在主菜单"单人游戏"界面（MainMenuUI_Build case 3）的"北国风云(Nordland)"
// 按钮之上，插入"文化II：阿斯加德之门"战役入口按钮。
//
// 原理：case 3 每个战役按钮都是内联的"new(0x60) → GetSagaText(文本对) →
// UI_CreateButton → UI_AddButton → 坐标累加"序列。本 Feature hook
// Nordland 按钮对象创建点（0x4D295E call operator new，5 字节 E8），
// 跳转到 code cave；cave 在栈帧内（EBP 未变）先用游戏自身 API 完整创建
// Asgard 按钮 + 坐标累加，再跳回 0x4D2963 继续原流程创建 Nordland。
//
// 按钮文本：saga 表（表13, saga001.ini）ID 25/26 —— 已在
//   Data\text\l10\strings\saga\saga001.ini 添加（l10 汉化版）。
// 控件 ID：0x1397 (5015)，case 3 中空闲（5010-5014/5017/5018 已用）。
//
// 配置（plugins/config/CulturesGameExtend_Game.ini）：
//   [AsgardCampaign]
//   Enabled = 1
//   ; AddButton = 1   （是否插入按钮；0=禁用插入）
//   ; CodeCaveStart = 0xF22CB  （默认在 CulturesPatches 的 cave 区之后）
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

// ---- cave 引用的游戏函数（RVA）----
constexpr uintptr_t F_New        = 0xE55BD; // VA 0x4E55BD operator new
constexpr uintptr_t F_GetSagaTxt = 0xE169E; // VA 0x4E169E StringTable_GetSagaText(id) = 表13
constexpr uintptr_t F_CreateBtn  = 0xB7C2E; // VA 0x4B7C2E UI_CreateButton
constexpr uintptr_t F_Sub439B9C  = 0x39B9C; // VA 0x439B9C 按钮挂到界面（thiscall ecx=esi+8）
constexpr uintptr_t F_AddBtn     = 0x768DA; // VA 0x4768DA UI_AddButton（thiscall ecx=dword_554F20）
constexpr int kAsgardCtlId = 0x1397;         // 5015（空闲控件 ID）
constexpr int kAsgardTextA = 0x19;           // saga 表 ID 25（战役名）
constexpr int kAsgardTextB = 0x1A;           // saga 表 ID 26（提示）

// ---- cave 机器码（121 字节）----
// 布局（offset: 指令）：
//   0   call operator new        （重放被覆盖指令；eax = Nordland 对象）
//   5   push eax                 （暂存 Nordland 对象）
//   6   push 60h; 8 call new     （Asgard 按钮对象 → eax）
//   13  pop ecx                  （清 cdecl 参数 60h —— 原代码 0x4D2965 同款）
//   14  mov [ebp-4], eax
//   17  cmp eax, ebx; 19 jz +0x2F（new 失败跳 asgard_skip@68）
//   21  lea eax,[esi+216Ch]; push eax; push 1397h; push 1Ah
//   35  call GetSagaText; pop ecx; push eax; push 19h
//   44  call GetSagaText; pop ecx
//   50  mov ecx,[ebp-4]; push eax; lea eax,[ebp-14h]; push eax
//   58  call UI_CreateButton; mov [ebp-4], eax
//   66  jmp +3 -> 71
//   68  asgard_skip: mov [ebp-4], ebx
//   71  asgard_add: push [ebp-4]
//   74  mov ecx, edi
//   76  call sub_439B9C; mov ecx, dword_554F20(81); push 1(87); push [ebp-4](89)
//   92  call UI_AddButton
//   97  mov eax,[ebp-10h]; mov ecx,[ebp-8](100); lea eax,[eax+ecx+0Ah](103)
//   107 mov [ebp-10h], eax      （坐标累加，同 Nordland 的 0x4D29B7）
//   110 pop eax                 （恢复 Nordland 对象）
//   111 jmp RET
static const uint8_t kCave[121] = {
    0xE8,0,0,0,0,                          // 0   call ??2@YAPAXI@Z
    0x50,                                  // 5   push eax
    0x6A,0x60,                             // 6   push 60h
    0xE8,0,0,0,0,                          // 8   call ??2@YAPAXI@Z
    0x59,                                  // 13  pop ecx
    0x89,0x45,0xFC,                        // 14  mov [ebp-4], eax
    0x3B,0xC3,                             // 17  cmp eax, ebx
    0x74,0x2F,                             // 19  jz +0x2F (->68)
    0x8D,0x86,0x6C,0x21,0x00,0x00,         // 21  lea eax,[esi+216Ch]
    0x50,                                  // 27  push eax
    0x68,0x97,0x13,0x00,0x00,              // 28  push 1397h
    0x6A,0x1A,                             // 33  push 1Ah
    0xE8,0,0,0,0,                          // 35  call GetSagaText
    0x59,                                  // 40  pop ecx
    0x50,                                  // 41  push eax
    0x6A,0x19,                             // 42  push 19h
    0xE8,0,0,0,0,                          // 44  call GetSagaText
    0x59,                                  // 49  pop ecx
    0x8B,0x4D,0xFC,                        // 50  mov ecx,[ebp-4]
    0x50,                                  // 53  push eax
    0x8D,0x45,0xEC,                        // 54  lea eax,[ebp-14h]
    0x50,                                  // 57  push eax
    0xE8,0,0,0,0,                          // 58  call UI_CreateButton
    0x89,0x45,0xFC,                        // 63  mov [ebp-4], eax
    0xEB,0x03,                             // 66  jmp +3 (->71)
    0x89,0x5D,0xFC,                        // 68  mov [ebp-4], ebx
    0xFF,0x75,0xFC,                        // 71  push [ebp-4]
    0x8B,0xCF,                             // 74  mov ecx, edi
    0xE8,0,0,0,0,                          // 76  call sub_439B9C
    0x8B,0x0D,0x20,0x4F,0x55,0x00,         // 81  mov ecx, dword_554F20
    0x6A,0x01,                             // 87  push 1
    0xFF,0x75,0xFC,                        // 89  push [ebp-4]
    0xE8,0,0,0,0,                          // 92  call UI_AddButton
    0x8B,0x45,0xF0,                        // 97  mov eax,[ebp-10h]
    0x8B,0x4D,0xF8,                        // 100 mov ecx,[ebp-8]
    0x8D,0x44,0x08,0x0A,                   // 103 lea eax,[eax+ecx+0Ah]
    0x89,0x45,0xF0,                        // 107 mov [ebp-10h], eax
    0x58,                                  // 110 pop eax
    0xE9,0,0,0,0                           // 111 jmp RET
};

static void FillRel(uint8_t* buf, size_t off, uintptr_t base,
                    uintptr_t fromOff, uintptr_t toOff) {
    // E8/E9 均为 5 字节（1 操作码 + 4 rel32），rel32 写在 opcode 之后：off+1
    // rel32 = to - (from + 5)
    int32_t rel = (int32_t)((base + toOff) - (base + fromOff + 5));
    memcpy(buf + off + 1, &rel, 4);
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
        DWORD caveStart = (DWORD)cfg.GetInt(kName, "CodeCaveStart", 0xF22CB);

        // 组装 cave（动态填 8 个 rel32）
        uint8_t buf[sizeof(kCave)];
        memcpy(buf, kCave, sizeof(kCave));
        // FillRel 的 off 必须与 kCave 数组的实际字节位置一致（见数组注释）
        FillRel(buf, 0,   base, caveStart + 0,    F_New);
        FillRel(buf, 8,   base, caveStart + 8,    F_New);
        FillRel(buf, 35,  base, caveStart + 35,   F_GetSagaTxt);
        FillRel(buf, 44,  base, caveStart + 44,   F_GetSagaTxt);
        FillRel(buf, 58,  base, caveStart + 58,   F_CreateBtn);
        FillRel(buf, 76,  base, caveStart + 76,   F_Sub439B9C);
        FillRel(buf, 92,  base, caveStart + 92,   F_AddBtn);
        FillRel(buf, 111, base, caveStart + 111,  RET);

        if (!Patch::WriteBytes(base + caveStart, buf, sizeof(buf))) {
            LOG_ERROR(kCat, "cave write failed @0x%X", caveStart);
            return false;
        }

        // hook 0x4D295E：E9 rel32 -> cave
        static const uint8_t hookVerify[] = { 0xE8, 0x5A, 0x2C, 0x01, 0x00 };
        if (!VerifyBytes(base, HOOK, hookVerify, sizeof(hookVerify))) {
            LOG_WARN(kCat, "hook verify mismatch @0x%X (already patched?)", base + HOOK);
            return false;
        }
        uint8_t jmp[5] = { 0xE9, 0, 0, 0, 0 };
        int32_t rel = (int32_t)((base + caveStart) - (base + HOOK + 5));
        memcpy(&jmp[1], &rel, 4);
        if (!Patch::WriteBytes(base + HOOK, jmp, 5)) {
            LOG_ERROR(kCat, "hook write failed @0x%X", base + HOOK);
            return false;
        }

        LOG_INFO(kCat, "Asgard button injected (cave@0x%X, text saga 25/26, ctl 0x1397)",
                 caveStart);
        return true;
    }
};

REGISTER_FEATURE(AsgardCampaignFeature)

} // namespace fe_asgard
