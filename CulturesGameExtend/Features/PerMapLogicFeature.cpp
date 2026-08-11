// PerMapLogicFeature.cpp
// ===================================================================
// [PerMapLogic]
// 每张地图独立的游戏平衡：logic 与地图**同包分发**。
//
// 设计（按用户要求 v2，2026-08-11）：
//   logic 必须跟着地图走（随 .c2m / 地图文件夹一起分发、多人整包传输），
//   而不是外部目录。因此覆盖文件放在**地图包内部**的 logic\ 子目录：
//
//     官方文件夹图（data\maps\<dir>\）：  <dir>\logic\jobtypes.ini
//     用户文件夹图（currentusermap 布局）：<map文件夹>\currentusermap\logic\jobtypes.ini
//     c2m 包：本版本不支持包内探测（Source 为空时回退全局）
//
//   logic\ 内部布局与 data\logic 完全一致（jobtypes.ini、goodtypes.ini、
//   tribetypes\tribetypes.ini、atomicanimations\atomicanimations.ini ...）。
//   某文件缺失 -> 自动回退全局 data\logic\<同名>；整图无 logic\ -> 完全原版。
//
// 机制（2 个 hook，全部驻留 DLL）：
//   1. IniFile_Open(0x424EF8) 入口 trampoline：重载会话(g_redirect=1)期间，
//      把 12 个 data\logic\X.ini 改写为 <地图源目录>\logic\X.ini。
//      ★ 存在性用纯 Win32 GetFileAttributesA 判断（v2.2 起弃用引擎
//        sub_40667B 探测：它开/关 CRT fd + 归档搜索，在 IniFile_Open 入口
//        嵌套调用会扰动文件层 -> goodtypes 等表加载失败，症状=物品图标消失）。
//   2. MapLoader_LoadCurrentMap 收尾调用点 **0x40AA13**（call sub_407A21）hook：
//      改写 E8 -> PrepCallStub，在**地图 ini/世界/[StaticObjects] 全部处理完、
//      开局准备之前**执行 ReloadLogicForMap（依依赖顺序重跑 12 个 loader），
//      再 jmp 原 sub_407A21 正常开局。
//      ★ v3（0x40AA13）是调试器实机验证后的最终触发点：v2.2-2.5 的
//        0x40A81A/0x40A6F4 触发点会让紧随其后的 $maproot$\map.ini 打开失败
//        （句柄全零，节表缺失）-> [StaticObjects] 全部被跳过 -> 地图开局
//        物件/资源全空。收尾点重载只影响运行时读取，不再有这个问题。
//      地图源目录从加载器帧 [ebp+8]=Source 取得。
//   3. manager 指针直接读各 loader 存储的全局（无入口 hook，无启动竞态）。
//   ★ v2.4：重跑后把 10 张恒定大小表的新内容**拷回原表地址并恢复原指针**
//     （缓存了表指针的子系统不会因重载而失效；atomicanim/tribe/weapon 为
//     动态大小跳过）。v2.5 保留拷回，确保指针身份稳定。
//
// 多人：地图包（含 logic\）整包传输到对方 -> 双方同版本 DLL + 同图 =
//       同平衡，天然一致。
//
// 配置（plugins/config/CulturesGameExtend_Game.ini）：
//   [PerMapLogic]
//   Enabled = 1
//   Debug = 1
//   ; ★ 每表独立开关（二分定位 bug 用）：1=进图重载该表，0=跳过
//   ReloadGoodtypes = 1      ReloadJobtypes = 1      ...
// ===================================================================
#include "pch.h"
#include "Core/Feature.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Patch.h"
#include "Core/Logger.h"
#include "Core/GameApi.h"
#include <cstring>
#include <cstdio>
#include <vector>

namespace fe_pml {

const char* kName = "PerMapLogic";
const char* kCat  = "[PerMapLogic]";

// ---- 关键地址（VA，0x400000 基址）----
constexpr uintptr_t VA_IniFileOpen  = 0x424EF8; // IniFile_Open 入口（thiscall，路径在 [esp+4]）
constexpr uintptr_t VA_MapLoaderEntry = 0x40A6F4; // MapLoader_LoadCurrentMap 入口（v3.5：goodtypes 在此重载，
                                                  //   在 [StaticObjects] 解析之前 -> 地图开局资源按新表解析）
constexpr uintptr_t VA_PrepCall     = 0x40AA13; // MapLoader 末尾 call sub_407A21（其余 11 表重载点）
constexpr uintptr_t VA_GameplayPrep = 0x407A21; // sub_407A21 本体（开局准备）

// ---- 12 个平衡 loader（顺序 = 依赖顺序，重跑必须保持）----
struct LogicFile {
    const char* logical;   // 相对 data\logic\ 的路径
    uintptr_t   loader;    // loader 函数 VA
    uintptr_t   mgrGlob;   // loader 存储 manager 指针的全局 VA
};
static const LogicFile kLogicFiles[] = {
    { "landscapetypes.ini",                     0x4162E5, 0x511534 },
    { "trianglepatterntypes.ini",               0x415EBB, 0x5114C4 },
    { "goodtypes.ini",                          0x41592E, 0x511424 },
    { "housetypes.ini",                         0x41531D, 0x5113A4 },
    { "vehicletypes.ini",                       0x414DE0, 0x511314 },
    { "atomicanimations\\atomicanimations.ini", 0x414B17, 0x511300 },
    { "tribetypes\\tribetypes.ini",             0x413E7B, 0x511204 },
    { "jobtypes.ini",                           0x4139DB, 0x511184 },
    { "weapontypes.ini",                        0x4133F6, 0x5110D0 },
    { "armortypes.ini",                         0x41312C, 0x510FDC },
    { "animaltypes.ini",                        0x412C52, 0x510F30 },
    { "humanjobexperiencetypes.ini",            0x412855, 0x510C20 },
};

// ---- 运行期状态（stub 引用，需稳定地址）----
static volatile unsigned char g_redirect = 0;   // IniFile_Open 重定向会话开关
static bool   g_installed = false;
static bool   g_dbg       = true;
static char   g_canonical[12][64] = { {0} };    // data\logic\X.ini 规范路径
static char   g_candidate[MAX_PATH] = { 0 };    // PathRewrite 输出缓冲
static void*  p_tramp_inopen    = nullptr;   // IniFile_Open 原入口 trampoline
static void*  p_tramp_maploader = nullptr;   // MapLoader 入口 trampoline（v3.5）
static void*  p_orig_407A21     = nullptr;   // 原 sub_407A21（v3 触发点 jmp 目标）
static void*  p_pathrewrite  = nullptr;         // -> PathRewrite
static void*  p_reload       = nullptr;         // -> ReloadLogicForMap
static void*  p_reload_entry = nullptr;         // -> ReloadGoodTypesAtEntry
static void*  g_mapSrc  = nullptr;              // MapLoader arg1 Source（官方/用户文件夹图=地图源目录串）
static void*  g_mapPath = nullptr;              // MapLoader arg2（c2m/缓冲区模式=文件名串）
static char   g_mapSrcStr[MAX_PATH] = { 0 };    // 当前地图真实源目录
static bool   g_mapHasLogic = false;            // 该图是否有 <src>\logic 目录

// ---- 每表独立开关（二分定位 bug 用）：配置 [PerMapLogic] Reload<Name> = 1/0 ----
static const char* kReloadKeys[12] = {
    "ReloadLandscapetypes",         "ReloadTrianglepatterntypes",
    "ReloadGoodtypes",              "ReloadHousetypes",
    "ReloadVehicletypes",           "ReloadAtomicanimations",
    "ReloadTribetypes",             "ReloadJobtypes",
    "ReloadWeapontypes",            "ReloadArmortypes",
    "ReloadAnimaltypes",            "ReloadHumanjobexperiencetypes",
};
static bool g_reload[12] = { true, true, true, true, true, true,
                             true, true, true, true, true, true };

// ---- goodtypes 影子表（多线程安全重载，v3.3）----
// 症状：只有 ReloadGoodtypes=1 时游戏坏（物品/图标消失），其余 11 表全安全。
// 根因（用户二分 + 静态确认）：goodtypes loader(sub_41592E) 执行时先把
//   dword_511420 清零 -> operator new -> 逐槽填充，期间表指针指向"全 0/半空
//   新表"几十毫秒（含磁盘 IO）。渲染/物品线程每帧读表 -> 撞上半空表 ->
//   图标/物品查找失败并缓存坏结果。其它表安全 = 图标渲染只查 goodtypes。
// 修复：重载会话内把 loader 内部 6 处 `dword_511420` 引用的 imm32 改为
//   DLL 影子全局 g_shadowGood -> loader 解析期间 dword_511420 保持原表
//   （渲染线程永远读到完整表）-> 解析完 memcpy 到原表 -> 恢复 imm32。
// 另：覆盖文件与全局逐字节相同时跳过重载（重载本无意义且带窗口风险）。
static void* g_shadowGood = nullptr;   // loader 解析用的影子表指针槽（DLL 内）
// ★ v3.6 真正的根因：goodtypes 记录 +36 是**运行期回填的派生句柄**，不来自 ini。
//   loader 解析时把 +36 写成 -1（未解析）；启动流程 sub_401981 里有一次性守卫
//   `if (!this[40]) { sub_406C3E(); sub_47D3E6(); sub_407665(); this[40]=1; }`，
//   其中 sub_407665 -> sub_415E8A 遍历 56 条记录：
//       h = sub_47FEF0(rec[+32] /*landscapetype*/, 1);  rec[+36] = h;
//   sub_47FEF0 = 在 landscape 图形数组（dword_568C24，stride 140，计数 dword_568C20）
//   里按 landscapetype 找条目并返回**索引**（纯查找、无副作用、可随时重算）。
//   +36 = "good -> landscape 图形索引"缓存，**全程只解析一次**。
//   => 任何形式的 goodtypes 重载（v2 换表 / v3.x memcpy）都会把 +36 变回 -1，
//      所有 good 失去图形句柄 -> 无图标/无法放入世界/无法采集（用户实测症状）。
//      这也解释了"覆盖文件与全局 md5 相同也坏"——+36 根本不在 ini 里。
//   修复：重载后调 sub_415E8A() 重新解析 +36（引擎自己的入口，语义 100% 对齐）。
static unsigned char g_goodSnap[0x35A0];   // 重载前的表快照（自校验 diff 用）
struct ImmPatch { uintptr_t va; int immOff; };
static const ImmPatch kGoodPatches[] = {
    { 0x415954, 1 },  // 68 imm32        push offset dword_511420（memset 清指针）
    { 0x41596C, 1 },  // A3 imm32        mov  dword_511420, eax（写新表指针）
    { 0x41597B, 2 },  // FF 35 imm32     push dword_511420（strcpy "none"）
    { 0x4159F8, 2 },  // 8B 0D imm32     mov  ecx, dword_511420（解析读表）
    { 0x415C5A, 1 },  // A1 imm32        mov  eax, dword_511420（字段写）
    { 0x415D87, 1 },  // A1 imm32        mov  eax, dword_511420（字段写）
};
static void PatchGoodGlobals(DWORD b, bool useShadow) {
    uint32_t dst = useShadow ? (uint32_t)(uintptr_t)&g_shadowGood : 0x511420u;
    for (const auto& p : kGoodPatches)
        Patch::WriteU32(b + (p.va - 0x400000) + p.immOff, dst);
}
// 覆盖文件是否与全局不同（无覆盖/相同 -> false，跳过重载）
static bool GoodTypesOverrideDiffers() {
    if (!g_mapSrcStr[0]) return false;
    char p[MAX_PATH], q[MAX_PATH];
    sprintf(p, "%s\\logic\\goodtypes.ini", g_mapSrcStr);
    sprintf(q, "data\\logic\\goodtypes.ini");
    HANDLE hp = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hp == INVALID_HANDLE_VALUE) return false; // 无覆盖 -> 全局
    HANDLE hq = CreateFileA(q, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hq == INVALID_HANDLE_VALUE) { CloseHandle(hp); return false; }
    DWORD sp = GetFileSize(hp, nullptr), sq = GetFileSize(hq, nullptr);
    if (sp != sq) { CloseHandle(hp); CloseHandle(hq); return true; }
    bool diff = false;
    char b1[8192], b2[8192];
    DWORD total = 0;
    while (total < sp) {
        DWORD r1 = 0, r2 = 0;
        ReadFile(hp, b1, sizeof(b1), &r1, nullptr);
        ReadFile(hq, b2, sizeof(b2), &r2, nullptr);
        if (r1 != r2 || memcmp(b1, b2, r1) != 0) { diff = true; break; }
        if (r1 == 0) break;
        total += r1;
    }
    CloseHandle(hp); CloseHandle(hq);
    return diff;
}
// 影子表安全重载：快照 -> loader 写影子 -> memcpy 到原表 -> 恢复 imm32
//   -> 重解析 +36 图形句柄（v3.6 关键）-> 重建 land 索引 -> 自校验 diff
static void ReloadGoodTypesSafe(DWORD b) {
    void* orig = *(void**)(b + (0x511420 - 0x400000));
    bool haveSnap = false;
    if (orig) { memcpy(g_goodSnap, orig, 0x35A0); haveSnap = true; }  // 重载前快照

    PatchGoodGlobals(b, true);   // loader 解析期间 dword_511420 保持原表
    void* mgr = *(void**)(b + (0x511424 - 0x400000));
    void* fn  = (void*)(b + (0x41592E - 0x400000));
    if (mgr && fn)
        ((void* (__fastcall*)(void*, void*))fn)(mgr, mgr); // ecx=this, edx 忽略
    void* shadow = *(void**)&g_shadowGood;                 // 新表（完整）
    orig = *(void**)(b + (0x511420 - 0x400000));           // 原表（未变）
    if (orig && shadow && shadow != orig)
        memcpy(orig, shadow, 0x35A0);                       // 微秒级换入
    PatchGoodGlobals(b, false);  // 恢复 loader 正常引用

    // ★★ v3.6 根因修复：重新解析记录 +36 的 landscape 图形句柄。
    //   loader 把 +36 全写成 -1，而它本来由启动一次性流程 sub_415E8A 填好。
    //   memcpy 后必须重跑，否则所有 good 无图形 -> 无图标/放不下/采不到。
    typedef int (__cdecl* ResolveGfxFn)(void);
    ResolveGfxFn resolveGfx = (ResolveGfxFn)(b + (0x415E8A - 0x400000));
    int lsCount = *(int*)(b + (0x568C20 - 0x400000));   // landscape 图形表条目数
    if (lsCount > 0) {
        resolveGfx();
        LOG_INFO(kCat, "  [gfx] good->landscape handles re-resolved (sub_415E8A, lsTable=%d)",
                 lsCount);
    } else {
        LOG_WARN(kCat, "  [gfx] SKIPPED: landscape gfx table empty (dword_568C20=0)"
                       " -> +36 handles stay -1 (icons/harvest will break)");
    }

    // v3.4：重建 land->goodid 索引（dword_510C60）。索引由 sub_412B62 在启动时
    //   基于全局表构建，采集/生成系统查它找"地貌 X 上的物品 id"。
    typedef void (__thiscall* RebuildIndexFn)(void*);
    RebuildIndexFn rebuild = (RebuildIndexFn)(b + (0x412B62 - 0x400000));
    void* idxMgr = *(void**)(b + (0x510F20 - 0x400000));
    if (rebuild && idxMgr) {
        rebuild(idxMgr);
        LOG_INFO(kCat, "  [idx] land->goodid index rebuilt (sub_412B62)");
    }

    // 自校验：对比重载前后整表。理想结果 = 只有 ini 里真正改过的字段有差异。
    //   若出现别的偏移变成 0/-1，说明还有运行期回填字段被 memcpy 清掉（继续修）。
    if (haveSnap && orig && g_dbg) {
        const unsigned char* A = g_goodSnap;
        const unsigned char* B = (const unsigned char*)orig;
        int nd = 0; char det[220]; det[0] = 0; int used = 0;
        for (int off = 0; off < 0x35A0; off += 4) {
            unsigned int x = *(const unsigned int*)(A + off);
            unsigned int y = *(const unsigned int*)(B + off);
            if (x == y) continue;
            ++nd;
            if (used < 5) {
                char one[48];
                sprintf(one, " g%d+%d:%d->%d", off / 208, off % 208, (int)x, (int)y);
                if (strlen(det) + strlen(one) < sizeof(det) - 1) { strcat(det, one); ++used; }
            }
        }
        LOG_INFO(kCat, "  [diff] dwords changed vs pre-reload = %d;%s", nd, det[0] ? det : " (none)");
    }
}

// ---- v3.5：地图加载器入口的 goodtypes 重载 ----
// 必须在 [StaticObjects] 解析（重载前用全局表）之前完成，否则地图开局资源
// 按旧表解析 -> 重载后表与地图不一致 -> 物品对象创建失败（无图标/无法放置/
// 采集空）。v2.5 入口触发失败的根因是 12 表全重载的文件层扰动；这里只重载
// goodtypes（1 次 IniFile_Open + 影子表），风险已消除。
static bool g_entryHook = false;   // 入口 hook 是否装成功（收尾点据此跳过 goodtypes）
static void ReloadGoodTypesAtEntry() {
    if (!g_installed || !g_entryHook) return;
    DWORD b = (DWORD)gameapi::g_imageBase;
    g_mapSrcStr[0] = 0;
    g_mapHasLogic = false;
    if (g_mapSrc && *(char*)g_mapSrc) {
        strncpy(g_mapSrcStr, (const char*)g_mapSrc, sizeof(g_mapSrcStr) - 1);
        g_mapSrcStr[sizeof(g_mapSrcStr) - 1] = 0;
        char dir[MAX_PATH];
        sprintf(dir, "%s\\logic", g_mapSrcStr);
        g_mapHasLogic = (GetFileAttributesA(dir) != INVALID_FILE_ATTRIBUTES);
    }
    if (!g_mapHasLogic || !g_mapSrcStr[0]) return;
    if (!GoodTypesOverrideDiffers()) {
        LOG_INFO(kCat, "  goodtypes at entry: override absent/identical, skip");
        return;
    }
    g_redirect = 1;          // IniFile_Open 改写：loader 读地图包 logic\goodtypes.ini
    ReloadGoodTypesSafe(b);
    g_redirect = 0;
    LOG_INFO(kCat, "  goodtypes reloaded at map entry (pre-StaticObjects)");
}

// ---- 拷回表（指针身份保持）：重载后把新内容拷回原表地址、恢复原指针 ----
// 表全局 VA + 表大小（0 = 跳过，动态大小表如 atomicanim/tribe/weapon）
struct TableEntry { uintptr_t globVa; int size; };
static const TableEntry kTables[] = {
    { 0x511528, 0x35A0  }, // landscapetypes
    { 0x5114C0, 0x210   }, // trianglepatterntypes
    { 0x511420, 0x35A0  }, // goodtypes
    { 0x5113A0, 0x151BC }, // housetypes
    { 0x511310, 0xD04   }, // vehicletypes
    { 0x511180, 0x1DC0  }, // jobtypes
    { 0x510FD8, 0x104   }, // armortypes
    { 0x510F2C, 0x880   }, // animaltypes
    { 0x510C18, 0xFD8   }, // humanjobexperiencetypes 主表
    { 0x510C1C, 0x9A0   }, // humanjobexperiencetypes 附表
};
static void* g_origTables[10] = { nullptr };    // 启动时原表指针
static bool  g_snapshotted = false;

// 全表完整性诊断：打印各表槽1名字（大小恒定表）
static void LogTablesIntegrity(DWORD b, const char* tag) {
    if (!g_dbg) return;
    const char* names[] = { "landscape", "triangle", "good", "house", "vehicle",
                            "job", "armor", "animal", "humanjobexp" };
    const int offs[] = { 156, 48, 208, 1572, 476, 136, 52, 64, 52 };
    const uintptr_t glbs[] = { 0x511528, 0x5114C0, 0x511420, 0x5113A0, 0x511310,
                               0x511180, 0x510FD8, 0x510F2C, 0x510C18 };
    for (int i = 0; i < 9; ++i) {
        void* p = *(void**)(b + (glbs[i] - 0x400000));
        if (!p) { LOG_INFO(kCat, "  [%s] %-11s table=NULL", tag, names[i]); continue; }
        const char* s = (const char*)p + offs[i];
        LOG_INFO(kCat, "  [%s] %-11s slot1='%.14s'", tag, names[i],
                 *(char*)s ? s : "(empty)");
    }
}

// 重载后：把新表内容拷回原表地址 + 恢复原指针（指针身份永不变）
static void CopyBackTables(DWORD b) {
    for (int i = 0; i < 10; ++i) {
        if (!g_origTables[i] || !kTables[i].size) continue;
        void* cur = *(void**)(b + (kTables[i].globVa - 0x400000));
        if (cur && cur != g_origTables[i]) {
            memcpy(g_origTables[i], cur, (size_t)kTables[i].size);
            *(void**)(b + (kTables[i].globVa - 0x400000)) = g_origTables[i];
        }
    }
}

// ---- 运行时 trampoline：拷贝原 copyLen 字节 + E9 跳回 entry+copyLen ----
static uintptr_t MakeTrampoline(uintptr_t entry, int copyLen) {
    uintptr_t t = (uintptr_t)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                                          PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    std::vector<uint8_t> orig = Patch::ReadBytes(entry, copyLen);
    if (orig.size() != (size_t)copyLen) {
        VirtualFree((void*)t, 0, MEM_RELEASE);
        return 0;
    }
    memcpy((void*)t, orig.data(), copyLen);
    uintptr_t back = entry + copyLen;
    *(uint8_t*)(t + copyLen) = 0xE9;
    *(int32_t*)(t + copyLen + 1) = (int32_t)(back - (t + copyLen + 5));
    return t;
}

// 引擎路径可用性检查已弃用：不调 sub_40667B（会开/关 CRT fd、做归档搜索，
// 在 IniFile_Open 入口里嵌套调用会扰动引擎文件层 -> goodtypes 等表加载失败）。
// 改用纯 Win32 GetFileAttributesA 探测真实路径（<地图源目录>\logic\X.ini）。

// ---- 路径重定向（仅重载会话内生效；低开销直通）----
static const char* __cdecl PathRewrite(const char* path) {
    if (!g_redirect || !path) return path;
    if (!g_mapHasLogic || !g_mapSrcStr[0]) return path;
    for (int i = 0; i < 12; ++i) {
        if (_stricmp(path, g_canonical[i]) != 0) continue;
        sprintf(g_candidate, "%s\\logic\\%s", g_mapSrcStr, kLogicFiles[i].logical);
        if (GetFileAttributesA(g_candidate) != INVALID_FILE_ATTRIBUTES) {
            if (g_dbg) LOG_INFO(kCat, "  [rewrite] %s -> MAP %s", path, g_candidate);
            return g_candidate;
        }
        if (g_dbg) LOG_INFO(kCat, "  [rewrite] %s -> GLOBAL (no map override)", path);
        return path; // 该表缺失 -> 全局
    }
    return path;
}

// ---- 重载 12 张平衡表（仅当地图带 logic\ 目录时才重跑；否则完全不碰表）----
static void ReloadLogicForMap() {
    if (!g_installed) return;
    DWORD b = (DWORD)gameapi::g_imageBase;
    // 地图源目录：arg1(Source) 非空 = 官方/用户文件夹图的真实源目录；c2m 不支持包内探测
    g_mapSrcStr[0] = 0;
    g_mapHasLogic = false;
    if (g_mapSrc && *(char*)g_mapSrc) {
        strncpy(g_mapSrcStr, (const char*)g_mapSrc, sizeof(g_mapSrcStr) - 1);
        g_mapSrcStr[sizeof(g_mapSrcStr) - 1] = 0;
        char dir[MAX_PATH];
        sprintf(dir, "%s\\logic", g_mapSrcStr);
        g_mapHasLogic = (GetFileAttributesA(dir) != INVALID_FILE_ATTRIBUTES);
    }
    const char* ident = g_mapSrcStr[0] ? g_mapSrcStr
                      : (g_mapPath && *(char*)g_mapPath) ? (const char*)g_mapPath : "?";

    // 首次触发：快照原表指针（此时必为启动时分配，之后指针身份永不变）
    if (!g_snapshotted) {
        for (int i = 0; i < 10; ++i)
            g_origTables[i] = *(void**)(b + (kTables[i].globVa - 0x400000));
        g_snapshotted = true;
        LOG_INFO(kCat, "[snapshot] original table pointers captured (%d tables)", 10);
    }

    // ---- 诊断（v2.4）：CWD + 直接打开测试 + 全表完整性 ----
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(sizeof(cwd), cwd);
    LOG_INFO(kCat, "[diag] map=%s cwd=%s hasLogic=%d", ident, cwd, (int)g_mapHasLogic);
    LogTablesIntegrity(b, "before");
    if (g_mapHasLogic) {
        char iniBuf[0x1890];
        gameapi::IniFile_Open((void*)iniBuf, "data\\logic\\goodtypes.ini", 0, 0, 0, 0);
        LOG_INFO(kCat, "[diag] direct open data\\logic\\goodtypes.ini flag=%u",
                 (unsigned)((unsigned char)iniBuf[0]));
        gameapi::IniFile_Close((void*)iniBuf);
    }

    // 关键：无 logic 目录 -> 绝不重跑（主菜单 demo 图/无覆盖图完全不碰表）
    if (!g_mapHasLogic) {
        LOG_INFO(kCat, "map has no logic folder, tables untouched (map=%s)", ident);
        return;
    }

    int over = 0, ok = 0;
    g_redirect = 1;
    for (int i = 0; i < 12; ++i) {
        if (!g_reload[i]) {
            LOG_INFO(kCat, "  %-42s -> SKIPPED (switch off)", kLogicFiles[i].logical);
            continue;
        }
        if (i == 2) { // goodtypes：已在入口（0x40A6F4）重载（v3.5），收尾点跳过；入口失败时兜底 shadow-safe
            if (g_entryHook) {
                LOG_INFO(kCat, "  %-42s -> HANDLED at map entry (pre-StaticObjects)", kLogicFiles[i].logical);
                continue;
            }
            if (!GoodTypesOverrideDiffers()) {
                LOG_INFO(kCat, "  %-42s -> GLOBAL (override absent/identical, reload skipped)",
                         kLogicFiles[i].logical);
                continue;
            }
            g_redirect = 1;
            ReloadGoodTypesSafe(b);
            g_redirect = 0;
            ++ok; ++over;
            LOG_INFO(kCat, "  %-42s -> MAP <src>\\logic (shadow-safe reload, fallback)", kLogicFiles[i].logical);
            continue;
        }
        int r = 0;
        if (g_mapSrcStr[0]) {
            char p[MAX_PATH];
            sprintf(p, "%s\\logic\\%s", g_mapSrcStr, kLogicFiles[i].logical);
            r = (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
            if (r) ++over;
        }
        void* mgr = *(void**)(b + (kLogicFiles[i].mgrGlob - 0x400000));
        void* fn  = (void*)(b + (kLogicFiles[i].loader - 0x400000));
        if (mgr && fn) {
            ((void* (__fastcall*)(void*, void*))fn)(mgr, mgr); // ecx=this, edx 忽略
            ++ok;
        } else {
            LOG_WARN(kCat, "loader[%d] %s mgr=%p fn=%p skipped",
                     i, kLogicFiles[i].logical, mgr, fn);
        }
        LOG_INFO(kCat, "  %-42s -> %s", kLogicFiles[i].logical,
                 r ? "MAP <src>\\logic" : "GLOBAL data\\logic");
    }
    g_redirect = 0;

    // 拷回：新内容写回原表地址、恢复原指针（缓存指针的子系统不会失效）
    CopyBackTables(b);
    LogTablesIntegrity(b, "after");
    LOG_INFO(kCat, "logic reloaded: map=%s loaders=%d/12 overridden=%d/12 (tables copied back to original addresses)",
             ident, ok, over);

    // ---- 自动取证（Debug=1）：重载后现场 dump ----
    if (g_dbg) {
        // 12 表指针（确认拷回后 = 原地址）
        for (int i = 0; i < 10; ++i) {
            void* p = *(void**)(b + (kTables[i].globVa - 0x400000));
            LOG_INFO(kCat, "  [ptrs] tbl[%d] @0x%X = 0x%X (orig 0x%X)",
                     i, (unsigned)kTables[i].globVa, (unsigned)p, (unsigned)g_origTables[i]);
        }
        // goodtypes 全 65 槽完整性
        void* gt = *(void**)(b + (0x511420 - 0x400000));
        int empty = 0, zeroLand = 0;
        if (gt) {
            for (int i = 1; i < 66; ++i) {
                const char* nm = (const char*)gt + i * 208;
                int lt = *(int*)((char*)gt + i * 208 + 32);
                if (!*nm) ++empty;
                if (!lt) ++zeroLand;
            }
            LOG_INFO(kCat, "  [dump] goodtypes=0x%X emptyName=%d zeroLandscape=%d",
                     (unsigned)gt, empty, zeroLand);
            for (int i = 1; i <= 8; ++i)
                LOG_INFO(kCat, "    g[%d]='%.20s' land=%d",
                         i, (const char*)gt + i * 208, *(int*)((char*)gt + i * 208 + 32));
        }
        // 魔法表占用
        int used = 0;
        for (int i = 0; i < 64; ++i) {
            if (*(unsigned char*)(b + (0x50F930 - 0x400000) + 528 * i)) ++used;
        }
        LOG_INFO(kCat, "  [dump] magic table used=%d/64", used);
    }
}

// ===================================================================
// IniFile_Open 入口 stub —— 重定向会话内改写 [esp+0x28]（Source）
//   thiscall：ecx=目标缓冲，[esp+4]=Source；pushad(32)+pushfd(4) 后 Source=[esp+0x28]
// ===================================================================
__declspec(naked) void IniOpenStub() {
    __asm {
        pushad
        pushfd
        cmp  byte ptr [g_redirect], 0
        jz   pass
        mov  eax, [esp + 0x28]          ; Source（路径）
        push eax
        call dword ptr [p_pathrewrite]  ; PathRewrite(path) -> eax
        add  esp, 4
        mov  [esp + 0x28], eax          ; 写回栈上 Source
    pass:
        popfd
        popad
        jmp  dword ptr [p_tramp_inopen] ; 原入口 + 原 9 字节
    }
}

// ===================================================================
// 地图加载器入口 stub（v3.5：hook MapLoader_LoadCurrentMap 0x40A6F4 入口）
//   进入时（cdecl）：[esp]=返回地址，[esp+4]=Source（地图源目录串）
//   pushad(32)+pushfd(4) 后 Source=[esp+0x28]
//   只做 goodtypes 重载（在 [StaticObjects] 解析之前）-> 地图开局资源按新表解析
// ===================================================================
__declspec(naked) void PrepMapEntryStub() {
    __asm {
        pushad
        pushfd
        mov  eax, [esp + 0x28]          ; Source（地图源目录）
        mov  [g_mapSrc], eax
        call dword ptr [p_reload_entry] ; ReloadGoodTypesAtEntry()
        popfd
        popad
        jmp  dword ptr [p_tramp_maploader] ; 原入口 9 字节 trampoline
    }
}

// ===================================================================
// 地图加载收尾触发 stub（hook 0x40AA13 的 call sub_407A21）
//   进入时：[esp]=返回地址(0x40AA18)，[esp+4]=1（调用点 push 1）
//   此时地图 ini/世界/[StaticObjects] 已全部处理完，重载只影响运行时读取
//   （v3：触发点从 0x40A6F4 入口移回收尾点 —— 调试器实证：入口重载会让
//   紧随其后的 $maproot$\map.ini 打开失败（句柄全零），导致 [StaticObjects]
//   全被跳过 → 空图；收尾点重载没有这个问题）
//   地图身份从加载器帧取：arg1=Source=[ebp+8]，arg2=[ebp+0xC]
// ===================================================================
__declspec(naked) void PrepCallStub() {
    __asm {
        pushad
        pushfd
        mov  eax, [ebp + 8]             ; Source（地图源目录串）
        mov  ecx, [ebp + 0xC]           ; MaxCharCount
        mov  [g_mapSrc], eax
        mov  [g_mapPath], ecx
        call dword ptr [p_reload]       ; ReloadLogicForMap()
        popfd
        popad
        jmp  dword ptr [p_orig_407A21]  ; 原 sub_407A21(1)；它 ret 回 0x40AA18 -> pop ecx 清栈
    }
}

// ---- 字节校验 ----
static bool VerifyBytes(DWORD base, uintptr_t va, const uint8_t* exp, size_t n) {
    std::vector<uint8_t> cur = Patch::ReadBytes(base + (va - 0x400000), n);
    if (cur.size() == n && memcmp(cur.data(), exp, n) == 0) return true;
    LOG_ERROR(kCat, "verify FAILED @0x%X", (unsigned)va);
    return false;
}

class PerMapLogicFeature : public Feature {
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }

    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        if (!cfg.GetBool(kName, "Enabled", false)) {
            LOG_INFO(kCat, "Disabled (Enabled=0)");
            return true;
        }
        gameapi::Init(ver.GetBaseAddress());
        DWORD b = ver.GetBaseAddress();
        g_dbg = cfg.GetBool(kName, "Debug", true);

        // 0.5) 读每表独立开关（二分定位：默认全开）
        int on = 0;
        for (int i = 0; i < 12; ++i) {
            g_reload[i] = cfg.GetBool(kName, kReloadKeys[i], true);
            if (g_reload[i]) ++on;
        }
        LOG_INFO(kCat, "per-table reload switches: %d/12 on", on);
        if (g_dbg)
            for (int i = 0; i < 12; ++i)
                LOG_INFO(kCat, "  %-28s = %s", kReloadKeys[i],
                         g_reload[i] ? "ON" : "off");

        // 0) 校验：0x40AA13 应为 call（E8）；IniFile_Open 开头应为标准序言
        const uint8_t kCallOp = 0xE8;
        if (!VerifyBytes(b, VA_PrepCall, &kCallOp, 1)) return false;
        const uint8_t kIniPro[9] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x08, 0x03, 0x00, 0x00 };
        if (!VerifyBytes(b, VA_IniFileOpen, kIniPro, sizeof(kIniPro))) return false;

        // 1) 预生成 12 条规范路径
        for (int i = 0; i < 12; ++i)
            sprintf(g_canonical[i], "data\\logic\\%s", kLogicFiles[i].logical);

        // 2) IniFile_Open trampoline + hook
        uintptr_t iniEntry = b + (VA_IniFileOpen - 0x400000);
        uintptr_t tramp = MakeTrampoline(iniEntry, 9);
        if (!tramp) { LOG_ERROR(kCat, "IniFile_Open trampoline alloc failed"); return false; }
        p_tramp_inopen = (void*)tramp;
        p_pathrewrite  = (void*)&PathRewrite;
        if (!Patch::WriteJmp(iniEntry, (uintptr_t)&IniOpenStub, 0)) {
            LOG_ERROR(kCat, "IniFile_Open hook WriteJmp failed @0x%X", (unsigned)VA_IniFileOpen);
            return false;
        }

        // 2.5) 地图加载器入口 hook（v3.5：goodtypes 在此重载，[StaticObjects] 解析之前）
        //      只对 goodtypes；其余 11 表仍在收尾点重载。入口失败 -> 收尾点兜底 shadow-safe。
        const uint8_t kMapPro[9] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x6C, 0x02, 0x00, 0x00 };
        g_entryHook = false;
        if (VerifyBytes(b, VA_MapLoaderEntry, kMapPro, sizeof(kMapPro))) {
            uintptr_t mapEntry = b + (VA_MapLoaderEntry - 0x400000);
            uintptr_t mTramp = MakeTrampoline(mapEntry, 9);
            if (mTramp) {
                p_tramp_maploader = (void*)mTramp;
                p_reload_entry    = (void*)&ReloadGoodTypesAtEntry;
                if (Patch::WriteJmp(mapEntry, (uintptr_t)&PrepMapEntryStub, 0)) {
                    g_entryHook = true;
                    LOG_INFO(kCat, "  entry hook OK: 0x%X -> PrepMapEntryStub (goodtypes pre-StaticObjects)",
                             (unsigned)VA_MapLoaderEntry);
                } else {
                    LOG_WARN(kCat, "entry hook WriteJmp failed @0x%X", (unsigned)VA_MapLoaderEntry);
                }
            }
        } else {
            LOG_WARN(kCat, "entry 0x%X 序言不符，跳过入口 hook（收尾点兜底）", (unsigned)VA_MapLoaderEntry);
        }

        // 3) 地图加载收尾点 hook（v3：0x40AA13，其余 11 表重载点）
        p_reload      = (void*)&ReloadLogicForMap;
        p_orig_407A21 = (void*)(b + (VA_GameplayPrep - 0x400000));
        uintptr_t callSite = b + (VA_PrepCall - 0x400000);
        int32_t rel = (int32_t)((uintptr_t)&PrepCallStub - (callSite + 5));
        uint8_t bytes[5] = {
            0xE8,
            (uint8_t)(rel & 0xFF), (uint8_t)((rel >> 8) & 0xFF),
            (uint8_t)((rel >> 16) & 0xFF), (uint8_t)((rel >> 24) & 0xFF)
        };
        if (!Patch::WriteBytes(callSite, bytes, 5)) {
            LOG_ERROR(kCat, "prep-call patch failed @0x%X", (unsigned)VA_PrepCall);
            return false;
        }

        g_installed = true;
        LOG_INFO(kCat, "installed: entry 0x%X(gt=%d) + call 0x%X -> PrepCallStub; IniFile_Open hooked",
                 (unsigned)VA_MapLoaderEntry, (int)g_entryHook, (unsigned)VA_PrepCall);
        if (g_dbg)
            LOG_INFO(kCat, "  map logic folder: <map source dir>\\logic\\*  (same layout as data\\logic; c2m maps not supported)");
        return true;
    }
};

REGISTER_FEATURE(PerMapLogicFeature)

} // namespace fe_pml
