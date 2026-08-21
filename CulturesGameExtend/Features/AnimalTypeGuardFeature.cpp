// AnimalTypeGuardFeature.cpp
// ===================================================================
// [AnimalTypeGuard] -- 实体创建期校验早退（根治除数索引=0 的 #DE 崩溃）
//
// 崩溃复盘（Game.exe.42700 / 24464 / 22816 / 31572.dmp，同根因家族）:
//   在 sub_42DD46（animaltype/实体构造函数，thiscall）里：
//       42dd71: mov  ebx, [ebp+arg_8]          ; arg_8 = 除数索引
//       42de0e: shl  ebx, 6
//       42de11: mov  ebx, [ebx + dword_510F2C - 0x1D8]
//       42de1f: idiv ebx                       ; #DE @ 0x42DE1F（除数 0）
//   即除数索引 = 0 时，读 dword_510F2C[(0-8)*64+40] = dword_510F2C-0x1D8
//   （低界越界）→ 值 0 → EXCEPTION_INT_DIVIDE_BY_ZERO (0xC0000094)。
//
// 参数溯源（IDA 权威反汇编，逐链确认）:
//   sub_41D290（StaticObjects 实体解析，animaltype 分支）:
//       v65 = sub_425315(v1)           // 实体引用的 animaltype 名字
//       v66 = sub_40AF22(v65)          // -> sub_40ADB1.a2  = tribetype
//       v67 = sub_425315(v1)           // 同一名字
//       v68 = sub_40AF62(v67)          // -> sub_40ADB1.a3  = movespeed 索引
//       sub_40ADB1(v150, v66, v69=v68, &v136, v148, v70)
//   sub_40ADB1(a1,a2,a3,...) -> sub_41C2E7(dword_511724, a1,a2,a3,...)
//   sub_41C2E7 把 a2 作为第 4 个 push -> sub_42DD46.arg_8（除数索引）
//   ⇒ 除数索引 = sub_42DD46.arg_8 = sub_41C2E7.arg_4 = sub_40ADB1.a2
//     = sub_40AF22(name)（tribetype）。
//   sub_40AF22 在全局 tribetype 名表 dword_511200（其加载不随 per-map 重载）里
//   按名字查找，查不到返回 0。⇒ 地图实体引用的 animaltype 名字不在全局名表 ->
//   tribetype=0 -> 除数索引 0 -> #DE。这是 per-map 逻辑重载 + 静态名表的固有错配。
//
// 与既有 DivZeroGuard 的区别（这是根治，不是 SKIP）:
//   DivZeroGuard 在 0x42DE1F 处“除数为 0 则跳过除法”——把 movespeed=0 的残废实体
//   留在游戏里，且未触及根因（名字解析失败）。这是用户不认可的 SKIP 思路。
//   本 Feature 做两层干净早退：
//     (1) sub_40ADB1 入口校验 a2（tribetype / 除数索引）== 0 -> return -1，
//         游戏自有链路 sub_41C2E7 -> sub_41D290 据此干净跳过该实体（不创建、不留残废）。
//     (2) sub_42DD46 入口校验 arg_8（除数索引）== 0 -> 直接 return 0（构造失败），
//         sub_41C2E7 据此 return -1 -> 实体干净跳过。
//   sub_42DD46 只有 1 个调用者 sub_41C2E7，而 sub_41C2E7 有 4 个调用者
//   （sub_40ADB1 / sub_40A332 / sub_44CB26×2），故钩 sub_42DD46 能覆盖
//   【所有】到达 #DE 站点的路径，保证 0x42DE1F 永不复发（第 (1) 层仅覆盖
//   animaltype 路径、并负责把非法名字写进日志做诊断）。
//
// 诊断：relay sub_40AF62 记录最近查询的 animaltype 名字到 g_lastAF62Name；
//   第 (1) 层早退时把该名字写进日志，便于定位“哪个地图实体引用了非法 animaltype”。
//
// 配置：[AnimalTypeGuard] Enabled（默认 1）。纯字节防护、无需改 Game.exe。
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
#include <cstdint>
#include <cstring>
#include <vector>

namespace fe_animtypeguard {

const char* kName = "AnimalTypeGuard";
const char* kCat  = "[AnimalTypeGuard]";

// 目标地址：本代码库约定 hook 常量用 RVA（相对映像基址 0x400000）；运行时 base+RVA = 真实 VA。
// 早期版本误把完整 VA(0x40ADB1) 当 RVA，致 base+0x40ADB1=0x80ADB1 越界、VerifyBytes 静默跳过
// 整组守卫（与早年 DivZeroGuard 双 base 错误同型，正是"崩溃反复出现"的根因）。现统一为 RVA：
static const DWORD kSub40ADB1 = 0xADB1;     // __cdecl 包装（animaltype 专属），a2 = 除数索引（VA=0x40ADB1）
static const DWORD kSub40AF62 = 0xAF62;     // movespeed 名表查找（诊断 relay）（VA=0x40AF62）
static const DWORD kSub42DD46 = 0x2DD46;    // 唯一 #DE 站点（thiscall 构造器）（VA=0x42DD46）

// ---- 以下符号被 __declspec(naked) 内联汇编直接引用，必须 C 链接（不重整名）----
extern "C" static uintptr_t g_tramp_40ADB1 = 0;
extern "C" static uintptr_t g_tramp_40AF62 = 0;
extern "C" static uintptr_t g_tramp_42DD46 = 0;
// 最近一次 sub_40AF62 查询的 animaltype 名字（诊断用）
extern "C" static char g_lastAF62Name[256] = {0};

// 前向声明：内联汇编中 call 引用的 C 函数必须在 asm 块之前可见，
// 否则 MSVC 报 C2094（标签未定义）。
extern "C" void __cdecl RecordAF62Name(const char* name);
extern "C" void __cdecl LogBadAnimalType(const char* name);
extern "C" void __cdecl LogDD46Skip(uintptr_t caller);

// ---- sub_40AF62 的 relay：记录最近查询的名字 ----
extern "C" void __declspec(naked) AF62Stub() {
    __asm {
        push ebp
        mov  ebp, esp
        push ebx
        push esi
        push edi
        mov  eax, [ebp+0x08]        // String2（sub_40AF62 的唯一参数）
        push eax
        call RecordAF62Name
        add  esp, 4
        pop  edi
        pop  esi
        pop  ebx
        mov  esp, ebp
        pop  ebp
        jmp  g_tramp_40AF62
    }
}

extern "C" void __cdecl RecordAF62Name(const char* name) {
    if (name) {
        strncpy(g_lastAF62Name, name, sizeof(g_lastAF62Name) - 1);
        g_lastAF62Name[sizeof(g_lastAF62Name) - 1] = 0;
    }
}

// ---- sub_40ADB1 的 guard（第 (1) 层，animaltype 路径 + 名字诊断）----
// 校验 a2（tribetype / 除数索引 = sub_40AF22(name) 的结果）== 0 则干净早退。
extern "C" void __declspec(naked) ADB1Stub() {
    __asm {
        push ebp
        mov  ebp, esp
        mov  eax, [ebp+0x0C]        // a2 = tribetype / 除数索引
        test eax, eax
        jnz  proceed
        // 记录诊断日志（名字已由 sub_40AF62 relay 捕获）
        push offset g_lastAF62Name
        call LogBadAnimalType
        add  esp, 4
        // 干净早退：return -1（__cdecl，调用方清理栈；栈已恢复 entry 状态）
        mov  esp, ebp
        pop  ebp
        mov  eax, -1
        ret
    proceed:
        mov  esp, ebp
        pop  ebp
        jmp  g_tramp_40ADB1
    }
}

extern "C" void __cdecl LogBadAnimalType(const char* name) {
    LOG_WARN(kCat, "Dropped animaltype entity: tribetype=0 (sub_40AF22('%s') unresolved; divisor index 0) -> entity skipped to avoid #DE @0x42DE1F",
             name ? name : "(null)");
}

// ---- sub_42DD46 的 guard（第 (2) 层，覆盖全部 4 个调用路径的兜底）----
// 校验 arg_8（除数索引）== 0 则直接 return 0（构造失败）。sub_42DD46 用 retn 1Ch
// 清理 7 个栈参数，故早退也必须 ret 1Ch。ecx(this) 全程未动。
extern "C" void __declspec(naked) DD46Stub() {
    __asm {
        push ebp
        mov  ebp, esp
        // 除数索引 = sub_42DD46.arg_8 = [ebp+0x10]（IDA 命名 arg_8 = [ebp+0x10]，
        // 等价于入口帧 [esp+0xC]）。注意：不是 [ebp+0x1C]（那是 arg_14，错槽）。
        // 证明：42700.dmp 故障帧 [esp_fault+0x1C]=0 即除数；stub 帧 ebp=entry_esp-4，
        // 故除数在 stub 帧 [ebp+0x10]；读 [ebp+0x1C] 会误判 arg_14 而漏守。
        mov  eax, [ebp+0x10]        // arg_8 = 除数索引（真实槽位）
        test eax, eax
        jnz  proceed
        // 诊断：记录 sub_42DD46 的返回地址（位于 sub_41C2E7 内），定位是 sub_40A332 /
        //       sub_44CB26 等哪条路径触发（[ebp+0x04] = 本函数返回地址 = sub_41C2E7 内位置）
        mov  eax, [ebp+0x04]
        push eax
        call LogDD46Skip
        add  esp, 4
        // 干净早退：return 0（al=0，构造失败），sub_41C2E7 据 test al,al 走失败分支
        mov  esp, ebp
        pop  ebp
        xor  eax, eax
        ret  1Ch
    proceed:
        mov  esp, ebp
        pop  ebp
        jmp  g_tramp_42DD46
    }
}

extern "C" void __cdecl LogDD46Skip(uintptr_t caller) {
    LOG_WARN(kCat, "sub_42DD46 divisor index (arg_8) = 0 -> constructor aborted (entity skipped), via sub_41C2E7 @caller 0x%X", (unsigned)caller);
}

// ---- 构造 N 字节 trampoline：复制原 prologue + E9 跳回 entry+N ----
static uintptr_t MakeTrampoline(uintptr_t entry, int copyLen) {
    uintptr_t t = (uintptr_t)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    auto orig = Patch::ReadBytes(entry, copyLen);
    if (orig.size() != (size_t)copyLen) { VirtualFree((void*)t, 0, MEM_RELEASE); return 0; }
    memcpy((void*)t, orig.data(), copyLen);
    uintptr_t back = entry + copyLen;
    *(uint8_t*)(t + copyLen) = 0xE9;
    *(int32_t*)(t + copyLen + 1) = (int32_t)(back - (t + copyLen + 5));
    FlushInstructionCache(GetCurrentProcess(), (void*)t, 64);
    return t;
}

static bool VerifyBytes(DWORD base, DWORD va, const uint8_t* exp, size_t n) {
    std::vector<uint8_t> cur = Patch::ReadBytes(base + va, n);
    if (cur.size() != n) return false;
    return memcmp(cur.data(), exp, n) == 0;
}

// 独立安装单个守卫：prologue 校验失败仅跳过该层（不连坐其它层），
// trampoline/WriteJmp 失败视为致命（返回 false）。
static bool InstallOneGuard(DWORD base, DWORD va, const uint8_t* prologue, size_t n,
                            int nopCount, uintptr_t& tramp, uintptr_t stub,
                            const char* name) {
    if (!VerifyBytes(base, va, prologue, n)) {
        LOG_WARN(kCat, "%s prologue mismatch @0x%X, skipped (build drift?)", name, base + va);
        return false;
    }
    tramp = MakeTrampoline(base + va, (int)n);
    if (!tramp) {
        LOG_ERROR(kCat, "%s trampoline alloc failed (err=%u)", name, (unsigned)GetLastError());
        return false;
    }
    if (!Patch::WriteJmp(base + va, stub, nopCount)) {
        LOG_ERROR(kCat, "%s WriteJmp failed", name);
        return false;
    }
    return true;
}

class AnimalTypeGuardFeature : public Feature {
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }
    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        if (!cfg.GetBool(kName, "Enabled", true)) {
            LOG_INFO(kCat, "Disabled by [%s] Enabled=0.", kName);
            return true;
        }
        DWORD base = ver.GetBaseAddress();

        // build-drift 校验：prologue 原字节必须逐字节匹配。三层守卫【相互独立】安装——
        // 任一层 prologue 漂移只禁用那一层，关键的第 (2) 层 sub_42DD46（真正兜 #DE 的兜底）
        // 不受第 (1) 层影响。注意：sub_42DD46 的 prologue 是 7 字节（mov esi,ecx = 8B F1）。
        static const uint8_t kADB1Prologue[6] = {0x55,0x8B,0xEC,0xFF,0x75,0x1C};
        static const uint8_t kAF62Prologue[6] = {0x56,0x57,0x33,0xFF,0x33,0xF6};
        static const uint8_t kDD46Prologue[7] = {0x55,0x8B,0xEC,0x53,0x56,0x8B,0xF1}; // push ebp; mov ebp,esp; push ebx; push esi; mov esi,ecx

        int installed = 0;
        if (InstallOneGuard(base, kSub40ADB1, kADB1Prologue, 6, /*nopCount=*/1,
                            g_tramp_40ADB1, (uintptr_t)ADB1Stub, "sub_40ADB1")) installed++;
        if (InstallOneGuard(base, kSub40AF62, kAF62Prologue, 6, /*nopCount=*/1,
                            g_tramp_40AF62, (uintptr_t)AF62Stub, "sub_40AF62")) installed++;
        if (InstallOneGuard(base, kSub42DD46, kDD46Prologue, 7, /*nopCount=*/2,
                            g_tramp_42DD46, (uintptr_t)DD46Stub, "sub_42DD46")) installed++;

        LOG_INFO(kCat, "AnimalType guard: %d/3 hooks installed (sub_40ADB1 a2==0 skip; sub_42DD46 arg_8==0 abort; sub_40AF62 name relay)",
                 installed);
        return true;
    }
};

REGISTER_FEATURE(AnimalTypeGuardFeature)
}
