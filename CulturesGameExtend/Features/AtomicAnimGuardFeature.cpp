// AtomicAnimGuardFeature.cpp
// ===================================================================
// [CustomSafety] — atomicanimations 表索引越界守卫
// AtomicAnimGuard — atomicanimations table index-out-of-bounds guard.
//
// 背景（崩溃复盘，Game.exe.32664.dmp，0xC0000005 @0x42E3D5）：
//   per-map logic 重载（[PerMapLogic] 启用）重建 atomicanimations 表
//   （dword_5112FC = 表基址，dword_5112F8 = 条目计数，单条 0x138=312B）。
//   实体在重载前已按「旧表」固定索引引用（this->0x78 = 表索引），
//   重载后该索引对「新表」越界，使 sub_42E38A 中
//     v2 = dword_5112FC[312 * this->0x78]
//   指向垃圾内存，其 *(v2+0x3c) 子元素计数被读成数万，
//   内层 `mov ecx,[eax-4]; ... ; eax+=0xC` 循环越界读无效堆 → 先卡死后 AV。
//   计算推得循环迭代 ~37880 次（差值 0x6F0A0 / 步长 0xC），远超真实子元素数组。
//
// 修复策略（与 DivZeroGuard 同族、运行时代码洞穴）：
//   在 sub_42E38A 的索引装载点 0x42E3A8（RVA，6B：mov edi,[esi+0x78]; imul edi,edi,0x138）
//   用 5 字节 E9 + 1 字节 nop 重定向到独立可执行页。洞穴内：
//     1) 还原原 `mov edi,[esi+0x78]`（this->0x78 = 表索引）；
//     2) 校验 index >= dword_5112F8（原子动画计数）则跳到游戏自有早退点 0x42E646
//        （*(this+0x70)==0 分支，语义等价：标记实体未就绪并干净返回，绝不污染合法实体）；
//     3) 索引合法时还原 `imul edi,edi,0x138` 并跳回 0x42E3B1，行为与原版逐字节一致。
//
// 与触发开关的绑定（同 DivZeroGuard）：
//   仅在 [PerMapLogic] 启用（崩溃路径存活）且 [AtomicAnimGuard] 启用时安装守卫。
//
// 配置：无额外 .ini；门控见上方。[AtomicAnimGuard] Enabled 默认 1，可单独置 0 复现崩溃。
// ===================================================================
// AtomicAnimGuardFeature.cpp
// ===================================================================
// [CustomSafety] -- atomicanimations index-out-of-bounds guard.
//
// Crash recap (Game.exe.32664.dmp, 0xC0000005 @0x42E3D5): per-map logic reload
//   ( [PerMapLogic] enabled) rebuilds the atomicanimations table
//   (dword_5112FC = base, dword_5112F8 = entry count, stride 0x138/312B).
//   Entities created before the reload still reference it by a fixed index
//   (this->0x78 = table index); after reload that index is out of range for the
//   NEW table, so in sub_42E38A `v2 = dword_5112FC[312 * this->0x78]` points at
//   garbage, its *(v2+0x3c) sub-element count is read as tens of thousands, and
//   the inner `mov ecx,[eax-4]; ...; eax+=0xC` loop overruns invalid heap ->
//   freeze then AV. ~37880 iterations inferred (0x6F0A0 / 0xC), far beyond any
//   real sub-array.
//
// Fix (same family as DivZeroGuard; runtime code cave): at the index-load site
//   0x42E3A8 (RVA, 6B: mov edi,[esi+0x78]; imul edi,edi,0x138), redirect via a
//   5-byte E9 + 1 nop into an independent executable page. The cave:
//     1) restores the original `mov edi,[esi+0x78]` (this->0x78 = table index);
//     2) if index >= dword_5112F8 (atomicanimations count) jumps to the game's
//        own early-out 0x42E646 (the *(this+0x70)==0 branch, semantically
//        identical: marks the entity not-ready and returns cleanly, never
//        touching valid entities);
//     3) if the index is valid, restores `imul edi,edi,0x138` and jumps back to
//        0x42E3B1, byte-for-byte identical to the original.
//
// Switch binding (same as DivZeroGuard): installed only when [PerMapLogic] is
//   enabled (crash path live) and [AtomicAnimGuard] is enabled.
// ===================================================================
#include "pch.h"
#include "Core/Feature.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Patch.h"
#include "Core/Logger.h"
#include "Core/fs_compat.h"
#include "Core/Paths.h"
#include <windows.h>
#include <vector>
#include <cstdint>
#include <cstring>

namespace fe_atomicanim {

const char* kName = "AtomicAnimGuard";
const char* kCat  = "[CustomSafety]";

// ---- 重定向目标（RVA，无 ASLR，基址恒 0x400000；base+RVA=真实 VA） ----
// Redirect site (RVA; no ASLR, base always 0x400000; base+RVA = real VA).
static const DWORD kSite = 0x2E3A8;   // mov edi,[esi+0x78] ; imul edi,edi,0x138
                                      // (6B: 8B 7E 78 6B FF 38)

// ---- 表计数 / 早退点 / 续跑点（全 VA） ----
// Table count / early-out / resume (full VAs).
static const DWORD kCountVA    = 0x5112F8;   // dword_5112F8 = atomicanimations 条目计数
static const DWORD kEarlyRetVA = 0x42E646;   // *(this+0x70)==0 早退：设 this->0x1c=1 / this->0x20=0 后干净返回
static const DWORD kResumeVA   = 0x42E3B1;   // `add edi,[0x5112fc]` 续跑点（原 imul 之后）

// ---- 重定向点原字节（6B，写入前校验，防 build 漂移写坏代码） ----
// Expected original bytes at the redirect site (6B, verified before writing).
static const uint8_t kPrologue[6] = {0x8B, 0x7E, 0x78, 0x6B, 0xFF, 0x38};

// ---- 校验原字节 ----
// verify original bytes
bool VerifyBytes(DWORD base, DWORD rva, const uint8_t* exp, size_t n) {
    std::vector<uint8_t> cur = Patch::ReadBytes(base + rva, n);
    if (cur.size() != n) return false;
    return memcmp(cur.data(), exp, n) == 0;
}

// ---- 构建洞穴（sub_42E38A 索引越界守卫） ----
// 偏移：
//   0x00 mov edi,[esi+0x78]        (3B) 还原原指令①
//   0x03 mov eax,[0x5112F8]        (5B) eax = 原子动画计数
//   0x08 cmp edi,eax               (2B)
//   0x0A jae skip (+0x0E)          (6B) index>=count -> 早退
//   0x10 imul edi,edi,0x138        (3B) 还原原指令②
//   0x13 E9 -> kResumeVA           (5B) 索引合法 -> 续跑
//   0x18(skip) E9 -> kEarlyRetVA   (5B) 越界 -> 游戏自有早退
void BuildCave(std::vector<uint8_t>& b, DWORD caveBase) {
    b.clear();
    auto emit  = [&](uint8_t v){ b.push_back(v); };
    auto emit4 = [&](uint32_t v){ b.push_back((uint8_t)(v&0xFF)); b.push_back((uint8_t)((v>>8)&0xFF));
                                  b.push_back((uint8_t)((v>>16)&0xFF)); b.push_back((uint8_t)((v>>24)&0xFF)); };
    emit(0x8B); emit(0x7E); emit(0x78);                        // mov edi,[esi+0x78]
    emit(0xA1); emit(0xFC); emit(0x12); emit(0x51); emit(0x00); // mov eax,[0x5112F8]
    emit(0x3B); emit(0xF8);                                   // cmp edi,eax
    emit(0x0F); emit(0x83); { int32_t rel = (int32_t)((caveBase + 0x18) - (caveBase + 0x0A + 6)); emit4((uint32_t)rel); } // jae skip
    emit(0x6B); emit(0xFF); emit(0x38);                        // imul edi,edi,0x138
    emit(0xE9); { int32_t rel = (int32_t)(kResumeVA - (caveBase + 0x13 + 5)); emit4((uint32_t)rel); } // jmp kResumeVA
    emit(0xE9); { int32_t rel = (int32_t)(kEarlyRetVA - (caveBase + 0x18 + 5)); emit4((uint32_t)rel); } // jmp kEarlyRetVA (skip)
}

class AtomicAnimGuardFeature : public Feature {
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }
    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        // 仅在崩溃路径（[PerMapLogic] 启用）且安全网启用时安装。
        // Only install when the crash path ([PerMapLogic]) is live and the safety net is on.
        if (!cfg.GetBool("PerMapLogic", "Enabled", false)) {
            LOG_INFO(kCat, "PerMapLogic disabled -> atomicanim guard inactive (no crash path).");
            return true;
        }
        if (!cfg.GetBool(kName, "Enabled", true)) {
            LOG_INFO(kCat, "Disabled by [%s] Enabled=0.", kName);
            return true;
        }

        DWORD base = ver.GetBaseAddress();

        // build-drift 校验：重定向点原字节必须逐字节匹配，否则跳过（避免写坏代码）。
        // build-drift guard: the redirect site bytes must match exactly, else skip.
        if (!VerifyBytes(base, kSite, kPrologue, 6)) {
            LOG_WARN(kCat, "Site prologue mismatch @0x%X, skipped (likely build drift)", base + kSite);
            return true;
        }

        // 运行时申请独立可执行页放置洞穴（不占用映像地址，规避 ACCESS_DENIED）。
        // Allocate an independent executable page for the cave.
        BYTE* page = (BYTE*)VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!page) {
            LOG_ERROR(kCat, "Cave alloc failed (err=%u)", (unsigned)GetLastError());
            return false;
        }
        DWORD cave = (DWORD)(uintptr_t)page;                  // 洞穴位于页首

        std::vector<uint8_t> c;
        BuildCave(c, cave);
        memcpy(page, c.data(), c.size());
        FlushInstructionCache(GetCurrentProcess(), page, 0x1000);

        // 6 字节重定向（5B E9 + 1B nop）到洞穴。
        // 6-byte redirect (5B E9 + 1B nop) into the cave.
        if (!Patch::WriteJmp(base + kSite, (uintptr_t)cave, 1)) {
            LOG_ERROR(kCat, "WriteJmp site failed");
            return false;
        }

        LOG_INFO(kCat, "AtomicAnim guard installed: site@0x%X -> cave 0x%X (count@0x%X, earlyret@0x%X)",
                 base + kSite, cave, kCountVA, kEarlyRetVA);
        return true;
    }
};
REGISTER_FEATURE(AtomicAnimGuardFeature)
}
