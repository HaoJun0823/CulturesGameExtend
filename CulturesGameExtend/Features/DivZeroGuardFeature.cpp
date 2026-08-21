// DivZeroGuardFeature.cpp
// ===================================================================
// [CustomSafety] — 除零防护（per-map logic 重载派生表索引失配的兜底）
// DivZeroGuard — divide-by-zero guard (a safety net for the per-map
//   logic reload index-mismatch family of crashes).
//
// 背景（崩溃复盘，Game.exe.42700.dmp）：
//   per-map logic 重载重排了类型/平衡表索引，而实体创建按「全局表」固定索引引用，
//   导致类型索引 0 被传入构造函数 0x42DD46，其除数 = 静态类型表[0x510f2c][类型索引]
//   （即类型索引 0 读到表项 0 = 0），在 0x42DE1F 的 `idiv ebx` 触发
//   EXCEPTION_INT_DIVIDE_BY_ZERO (0xC0000094)。
//   同表另一 `idiv` 点 0x446EB2（经 0x446EAF 的 imul+idiv 序列）使用同一除数表，
//   同样风险；0x446E87 那处则已有原生 `test esi,esi; jbe skip` 守卫，无需再补。
//
// 修复策略（与用户拍板「三处除零加守卫」一致，仅对缺口的两处下守卫）：
//   在 0x42DE1F 与 0x446EAF 两处用 5 字节 E9 近跳转重定向到运行时申请的独立
//   可执行页（代码洞穴），洞穴内做「divisor==0 则跳过除法（不写商/余数）」的
//   条件守卫；除数非 0 时走原指令序列，行为与原版逐字节一致，绝不污染合法实体。
//   这与 0x446E87 已有的 `test esi,esi; jbe skip` 守卫语义完全相同。
//
// 与触发开关的绑定（同 CustomSafetyPatchesFeature）：
//   仅在 [PerMapLogic] 启用（崩溃路径存活）且 [CustomSafety] 启用时安装守卫。
//
// 配置：无额外 .ini；门控见上方。[CustomSafety] Enabled 默认 1，可单独置 0 复现崩溃。
// ===================================================================
// DivZeroGuardFeature.cpp
// ===================================================================
// [CustomSafety] -- divide-by-zero guard.
//
// Crash recap (Game.exe.42700.dmp): per-map logic reload reindexes the type/balance
//   tables, but entity creation references them by fixed global index, so a type index
//   of 0 reaches ctor 0x42DD46 whose divisor = static type table [0x510f2c][typeIndex]
//   (index 0 reads entry 0 = 0), and `idiv ebx` at 0x42DE1F raises #DE.
//   A sibling `idiv` at 0x446EB2 (via the imul+idiv sequence at 0x446EAF) uses the same
//   table and carries the same risk; the 0x446E87 site already has a native
//   `test esi,esi; jbe skip` guard and needs no extra work.
//
// Fix (consistent with the chosen "guard the three idiv points"): at 0x42DE1F and
//   0x446EAF, redirect via a 5-byte E9 near jump into a runtime-allocated independent
//   executable page (code cave). The cave performs a conditional guard
//   ("divisor==0 -> skip the division, write nothing"), identical in semantics to the
//   existing `test esi,esi; jbe skip` at 0x446E87. When the divisor is non-zero the
//   cave executes the original instruction sequence byte-for-byte, so valid entities
//   are never affected.
//
// Switch binding (same as CustomSafetyPatchesFeature): installed only when [PerMapLogic]
//   is enabled (crash path live) and [CustomSafety] is enabled.
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

namespace fe_divzero {

const char* kName = "DivZeroGuard";
const char* kCat  = "[CustomSafety]";

// ---- 重定向目标（RVA，无 ASLR，基址恒 0x400000；base+RVA=真实 VA） ----
// Redirect sites (RVA; no ASLR, base always 0x400000; base+RVA = real VA).
// 注意：本文件早期版本误把全 VA(0x42DE1F)当 RVA 用，导致 base+0x42DE1F=0x82DE1F
//       越界，VerifyBytes 静默跳过两处守卫（Game.exe.42700.dmp 仍崩于 0x42DE1F 即证明）。
//       现统一为 RVA：base+0x2DE1F=0x42DE1F（正确站点）。
// NOTE: an earlier build wrongly used the full VA (0x42DE1F) as RVA, producing
//       base+0x42DE1F=0x82DE1F (OOB) so VerifyBytes silently skipped both guards
//       (Game.exe.42700.dmp still crashed at 0x42DE1F, proving it). Now RVA:
//       base+0x2DE1F = 0x42DE1F (correct site).
static const DWORD kSite1 = 0x2DE1F;   // idiv ebx  (5B: F7 FB 89 5F 44)
static const DWORD kSite3 = 0x46EAF;   // imul ecx,ecx,0x58 ; idiv esi (5B: 6B C9 58 F7 FE)

// ---- 洞穴返回点（原指令序列的接续 VA） ----
// Cave return points (where execution resumes in Game.exe).
static const DWORD kSite1BackNormal = 0x42DE24; // 2nd store `mov [edi+0x48],eax`
static const DWORD kSite1BackSkip   = 0x42DE27; // epilogue: skip both stores
static const DWORD kSite3BackNormal = 0x446EB4; // add ecx,[0x510bf0]
static const DWORD kSite3BackSkip   = 0x446EBA; // 1st store `mov [ecx+0x44],esi`

// ---- 重定向点原字节（写入前校验，防 build 漂移写坏代码） ----
// Expected original bytes at the redirect sites (verified before writing; guards
// against build drift corrupting code).
static const uint8_t kSite1Prologue[5] = {0xF7,0xFB,0x89,0x5F,0x44};
static const uint8_t kSite3Prologue[5] = {0x6B,0xC9,0x58,0xF7,0xFE};

// ---- 校验原字节 ----
// verify original bytes
bool VerifyBytes(DWORD base, DWORD va, const uint8_t* exp, size_t n) {
    std::vector<uint8_t> cur = Patch::ReadBytes(base + va, n);
    if (cur.size() != n) return false;
    return memcmp(cur.data(), exp, n) == 0;
}

// ---- 构建洞穴 1（站点 1） ----
// 偏移：0x00 test ebx,ebx / 0x02 jz skip1(+0xF) / 0x04 lea eax,[ebx+0x270f]
//      0x0A cdq / 0x0B idiv ebx / 0x0D mov [edi+0x44],ebx
//      0x10 E9 -> kSite1BackNormal / 0x13(skip1) E9 -> kSite1BackSkip
void BuildCave1(std::vector<uint8_t>& b, DWORD caveBase) {
    b.clear();
    auto emit  = [&](uint8_t v){ b.push_back(v); };
    auto emit4 = [&](uint32_t v){ b.push_back((uint8_t)(v&0xFF)); b.push_back((uint8_t)((v>>8)&0xFF));
                                  b.push_back((uint8_t)((v>>16)&0xFF)); b.push_back((uint8_t)((v>>24)&0xFF)); };
    emit(0x85); emit(0xDB);                                  // test ebx,ebx
    emit(0x74); emit(0x0F);                                  // jz skip1  (-> 0x13)
    emit(0x8D); emit(0x83); emit(0x0F); emit(0x27); emit(0x00); emit(0x00); // lea eax,[ebx+0x270f]
    emit(0x99);                                              // cdq
    emit(0xF7); emit(0xFB);                                  // idiv ebx
    emit(0x89); emit(0x5F); emit(0x44);                      // mov [edi+0x44],ebx
    emit(0xE9); { int32_t rel = (int32_t)(kSite1BackNormal - (caveBase + 0x10 + 5)); emit4((uint32_t)rel); }
    emit(0xE9); { int32_t rel = (int32_t)(kSite1BackSkip   - (caveBase + 0x13 + 5)); emit4((uint32_t)rel); }
}

// ---- 构建洞穴 2（站点 3） ----
// 偏移：0x00 test esi,esi / 0x02 jz skip2(+0xF) / 0x04 imul ecx,ecx,0x58
//      0x07 idiv esi / 0x09 E9 -> kSite3BackNormal
//      0x0E pad(5) / 0x13(skip2) imul ; add ecx,[0x510bf0] ; xor eax,eax
//      0x1E E9 -> kSite3BackSkip
void BuildCave2(std::vector<uint8_t>& b, DWORD caveBase) {
    b.clear();
    auto emit  = [&](uint8_t v){ b.push_back(v); };
    auto emit4 = [&](uint32_t v){ b.push_back((uint8_t)(v&0xFF)); b.push_back((uint8_t)((v>>8)&0xFF));
                                  b.push_back((uint8_t)((v>>16)&0xFF)); b.push_back((uint8_t)((v>>24)&0xFF)); };
    emit(0x85); emit(0xF6);                                  // test esi,esi
    emit(0x74); emit(0x0F);                                  // jz skip2  (-> 0x13)
    emit(0x6B); emit(0xC9); emit(0x58);                      // imul ecx,ecx,0x58
    emit(0xF7); emit(0xFE);                                  // idiv esi
    emit(0xE9); { int32_t rel = (int32_t)(kSite3BackNormal - (caveBase + 0x09 + 5)); emit4((uint32_t)rel); }
    for (int i = 0; i < 5; ++i) emit(0x90);                  // pad 0x0E..0x12
    emit(0x6B); emit(0xC9); emit(0x58);                      // imul ecx,ecx,0x58  (skip2)
    emit(0x03); emit(0x0D); emit(0xF0); emit(0x0B); emit(0x51); emit(0x00); // add ecx,[0x510bf0]
    emit(0x33); emit(0xC0);                                  // xor eax,eax
    emit(0xE9); { int32_t rel = (int32_t)(kSite3BackSkip   - (caveBase + 0x1E + 5)); emit4((uint32_t)rel); }
}

class DivZeroGuardFeature : public Feature {
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }
    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        // 仅在崩溃路径（[PerMapLogic] 启用）且安全网启用时安装。
        // Only install when the crash path ([PerMapLogic]) is live and the safety net is on.
        if (!cfg.GetBool("PerMapLogic", "Enabled", false)) {
            LOG_INFO(kCat, "PerMapLogic disabled -> div-zero guard inactive (no crash path).");
            return true;
        }
        if (!cfg.GetBool(kName, "Enabled", true)) {
            LOG_INFO(kCat, "Disabled by [CustomSafety] Enabled=0.");
            return true;
        }

        DWORD base = ver.GetBaseAddress();

        // build-drift 校验：重定向点原字节必须逐字节匹配，否则跳过（避免写坏代码）。
        // build-drift guard: the redirect site bytes must match exactly, else skip.
        if (!VerifyBytes(base, kSite1, kSite1Prologue, 5)) {
            LOG_WARN(kCat, "Site1 prologue mismatch @0x%X, skipped (likely build drift)", base + kSite1);
            return true;
        }
        if (!VerifyBytes(base, kSite3, kSite3Prologue, 5)) {
            LOG_WARN(kCat, "Site3 prologue mismatch @0x%X, skipped (likely build drift)", base + kSite3);
            return true;
        }

        // 运行时申请独立可执行页放置两个洞穴（不占用映像地址，规避 ACCESS_DENIED）。
        // Allocate an independent executable page for the two caves.
        BYTE* page = (BYTE*)VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!page) {
            LOG_ERROR(kCat, "Cave alloc failed (err=%u)", (unsigned)GetLastError());
            return false;
        }
        DWORD cave1 = (DWORD)(uintptr_t)page;            // 洞穴 1 位于页首
        DWORD cave2 = (DWORD)(uintptr_t)page + 0x40;    // 洞穴 2 偏移 0x40

        std::vector<uint8_t> c1, c2;
        BuildCave1(c1, cave1);
        BuildCave2(c2, cave2);
        memcpy(page,            c1.data(), c1.size());
        memcpy(page + 0x40,     c2.data(), c2.size());
        FlushInstructionCache(GetCurrentProcess(), page, 0x1000);

        // 5 字节 E9 近跳转重定向到洞穴。
        // 5-byte E9 near jump redirect into the cave.
        if (!Patch::WriteJmp(base + kSite1, (uintptr_t)cave1)) {
            LOG_ERROR(kCat, "WriteJmp site1 failed");
            return false;
        }
        if (!Patch::WriteJmp(base + kSite3, (uintptr_t)cave2)) {
            LOG_ERROR(kCat, "WriteJmp site3 failed");
            return false;
        }

        LOG_INFO(kCat, "Div-zero guards installed: site1@0x%X -> cave 0x%X, site3@0x%X -> cave 0x%X",
                 base + kSite1, cave1, base + kSite3, cave2);
        return true;
    }
};
REGISTER_FEATURE(DivZeroGuardFeature)
}
