// UnlockAllCampaignsFeature.cpp
// ===================================================================
// [UnlockAllCampaigns]
// 无视 campaign.ini 的存档进度，强制游戏"解锁所有战役 / 所有关卡"。
//
// 背景（逆向结论）：
//   游戏里"某个关卡能不能玩"由 sub_416E20（IsUnlocked）决定。它是
//   __thiscall（ecx = 进度对象），内部在一张 8 字节条目表
//   {campaignId, nodeId} 上做线性查找，命中返回 1、否则返回 0，
//   用 `ret 8` 清理两个栈参数（thiscall 的 this 在 ecx，不进栈）。
//   这张表的内容来自存档 campaign.ini 的 [campaign_sub_missions] 段——
//   也就是说，原版任何关卡是否开放，都由"你之前打通过什么"决定。
//
//   注意：该表只有 {campaignId, nodeId}，没有任何"完成 / 未完成"标志位。
//   通关完成的判定走的是另一张表（AddUnlock 的 flag 参数另有去处），
//   因此本功能把 IsUnlocked 强行恒返回 1，**只会解除"能否游玩"的锁，
//   不会破坏通关 / 完成判定**。
//
//   调用面：IsUnlocked 全程序只有 6 个调用点，全部位于 0x4Dxxxx 的
//   战役屏 handler / painter（关卡按钮创建、路线绘制把关）。主菜单战役
//   是否列出由地图数据文件（map 记录的 0x384 隐藏标志 / 0x120 槽位位图）
//   决定，与 campaign.ini 进度无关——主菜单建按钮区没有任何 IsUnlocked
//   调用。故 hook 此一处即可让"所有战役的所有关卡"变为可选。
//
//   实现：把 0x416E20 函数入口改写为 E9 跳到本 DLL 的 IsUnlockedStub，
//   stub 直接 `mov eax,1 / ret 8`（平衡栈，等价于"永远解锁"）。
//   用标准 Patch::WriteJmp，无需手算 rel32；游戏无 ASLR（RELOCS_STRIPPED），
//   E9 ±2GB 必定可达本 DLL。
//
//   默认关闭（Enabled = 0）。需要"全解锁"体验时把配置改成 1 即可，
//   无需重新编译。
//
// 配置（plugins/config/CulturesGameExtend_Game.ini）：
//   [UnlockAllCampaigns]
//   Enabled = 0        ; 1 = 无视进度，解锁全部战役 / 关卡
// ===================================================================
#include "pch.h"
#include "Core/Feature.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Patch.h"
#include "Core/Logger.h"
#include "Core/GameApi.h"
#include <cstring>

namespace fe_unlockall {

const char* kName = "UnlockAllCampaigns";
const char* kCat  = "[UnlockAllCampaigns]";

// ---- hook 点（RVA = VA - 0x400000；gameapi::Va 约定）----
constexpr uintptr_t R_IsUnlocked = 0x16E20;  // VA 0x416E20：sub_416E20 / IsUnlocked

// ---- 原入口前 8 字节（用于校验未被其它补丁改过）----
//  8b 81 08 02 00 00   mov eax, [ecx+0x208]   ; 读解锁表条目数
//  33 d2              xor edx, edx           ; 索引清零（循环变量）
static const uint8_t kPrologue[8] = {
    0x8b, 0x81, 0x08, 0x02, 0x00, 0x00, 0x33, 0xd2
};

// ===================================================================
// IsUnlockedStub —— 永远返回"已解锁"（1）
//   IsUnlocked 是 __thiscall：this 在 ecx，两个栈参数（campaignId, nodeId），
//   原函数用 `ret 8` 返回。我们跳过整张表的查找，直接置 eax=1 并以 ret 8
//   平衡栈。ecx 是易失寄存器，调用方不期望保留，无需还原。
// ===================================================================
__declspec(naked) void IsUnlockedStub() {
    __asm {
        mov eax, 1
        ret 8
    }
}

static bool VerifyBytes(DWORD base, DWORD off, const uint8_t* exp, size_t n) {
    std::vector<uint8_t> cur = Patch::ReadBytes(base + off, n);
    return cur.size() == n && memcmp(cur.data(), exp, n) == 0;
}

class UnlockAllCampaignsFeature : public Feature {
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }

    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        if (!cfg.GetBool(kName, "Enabled", false)) {
            LOG_INFO(kCat, "Disabled (Enabled=0)");
            return true;
        }
        gameapi::Init(ver.GetBaseAddress());

        DWORD base = ver.GetBaseAddress();

        // 1) 校验原入口未被改过（避免重复 hook 或版本不符）
        if (!VerifyBytes(base, R_IsUnlocked, kPrologue, sizeof(kPrologue))) {
            LOG_WARN(kCat, "prologue verify mismatch @0x%X (already patched or version mismatch?)",
                     base + R_IsUnlocked);
            return false;
        }

        // 2) 改写入口为 E9 -> IsUnlockedStub（覆盖 8 字节：E9 占 5，剩余 3 字节 NOP）
        DWORD stub = (DWORD)(void*)&IsUnlockedStub;
        int32_t rel = (int32_t)(stub - (base + R_IsUnlocked + 5));
        uint8_t jmp[5] = { 0xE9, 0, 0, 0, 0 };
        memcpy(&jmp[1], &rel, 4);
        if (!Patch::WriteBytes(base + R_IsUnlocked, jmp, 5)) {
            LOG_ERROR(kCat, "hook write failed @0x%X", base + R_IsUnlocked);
            return false;
        }
        // 把被覆盖指令的后 3 字节补成 NOP，保证入口区无半截指令
        Patch::WriteU8(base + R_IsUnlocked + 5, 0x90);
        Patch::WriteU8(base + R_IsUnlocked + 6, 0x90);
        Patch::WriteU8(base + R_IsUnlocked + 7, 0x90);

        LOG_INFO(kCat, "IsUnlocked hooked -> always return 1 (stub@0x%X, rel=%d). "
                       "All campaigns/missions now unlocked, ignoring campaign.ini progress.",
                 stub, rel);
        return true;
    }
};

REGISTER_FEATURE(UnlockAllCampaignsFeature)

} // namespace fe_unlockall
