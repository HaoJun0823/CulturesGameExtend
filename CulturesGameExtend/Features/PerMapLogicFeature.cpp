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
// PerMapLogicFeature.cpp
// ===================================================================
// [PerMapLogic]
// Per-map game balance: logic travels WITH the map package.
//
// Design (user decision v2, 2026-08-11):
//   Logic must follow the map (distributed with .c2m / map folder, transferred
//   wholesale in multiplayer) instead of living in an external directory.
//   Therefore override files live INSIDE the map package, in its logic\ subfolder:
//
//     official folder map (data\maps\<dir>\):  <dir>\logic\jobtypes.ini
//     user folder map (currentusermap layout):<map folder>\currentusermap\logic\jobtypes.ini
//     c2m package: in-package detection NOT supported this version
//                  (falls back to global when Source is empty)
//
//   logic\ internal layout is identical to data\logic (jobtypes.ini, goodtypes.ini,
//   tribetypes\tribetypes.ini, atomicanimations\atomicanimations.ini ...).
//   A missing file -> auto fall back to global data\logic\<same>; a map with no
//   logic\ at all -> completely vanilla.
//
// Mechanism (2 hooks, both resident in the DLL):
//   1. IniFile_Open(0x424EF8) entry trampoline: during a redirect session
//      (g_redirect=1), rewrite the 12 data\logic\X.ini paths to
//      <map source dir>\logic\X.ini.
//      * Existence is checked with plain Win32 GetFileAttributesA (v2.2 stopped
//        using the engine's sub_40667B probe: it opens/closes CRT fds and does
//        archive searching; calling it nested inside IniFile_Open's entry perturbs
//        the file layer -> goodtypes etc. fail to load, symptom = icons vanish).
//   2. MapLoader_LoadCurrentMap tail call site **0x40AA13** (call sub_407A21) hook:
//      rewrite E8 -> PrepCallStub, which runs ReloadLogicForMap AFTER the map
//      ini/world/[StaticObjects] are fully processed and BEFORE game-prep, then
//      jmp to the original sub_407A21 to start normally.
//      * v3 (0x40AA13) is the final trigger point validated on-device with a
//        debugger: the v2.2-2.5 trigger points 0x40A81A/0x40A6F4 made the
//        immediately-following $maproot$\map.ini open fail (zero handle, missing
//        section table) -> [StaticObjects] all skipped -> map starts empty.
//        Tail reload only affects runtime reads, no such problem.
//      The map source dir is taken from the loader frame [ebp+8]=Source.
//   3. manager pointers are read directly from the globals each loader stores
//      (no entry hook, no startup race).
//   * v2.4: after re-running, copy the 10 fixed-size tables' new content BACK to
//      the original table address and restore the original pointer (subsystems
//      that cached the table pointer don't break; atomicanim/tribe/weapon are
//      dynamic-size and skipped). v2.5 keeps the copy-back for pointer stability.
//
// Multiplayer: the map package (including logic\) is transferred wholesale to the
//   peer -> both sides on same DLL version + same map = same balance, consistent
//   by construction.
//
// Config (plugins/config/CulturesGameExtend_Game.ini):
//   [PerMapLogic]
//   Enabled = 1
//   Debug = 1
//   ; * per-table switch (for binary-search debugging): 1=reload this table on map
//     entry, 0=skip
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

#pragma region Key addresses (VA, base 0x400000)
constexpr uintptr_t VA_IniFileOpen  = 0x424EF8;
// IniFile_Open 入口（thiscall，路径在 [esp+4]）
// IniFile_Open entry (thiscall, path at [esp+4])
constexpr uintptr_t VA_MapLoaderEntry = 0x40A6F4;
constexpr uintptr_t VA_PrepCall     = 0x40AA13;
// MapLoader_LoadCurrentMap 入口（v3.5：goodtypes 在此重载，
// MapLoader_LoadCurrentMap entry (v3.5: reload goodtypes here,
constexpr uintptr_t VA_GameplayPrep = 0x407A21;
                                                  //   在 [StaticObjects] 解析之前 -> 地图开局资源按新表解析）
                                                  //   before [StaticObjects] parsing -> map starts with new table)
struct LogicFile {
// MapLoader 末尾 call sub_407A21（其余 11 表重载点）
// MapLoader tail call sub_407A21 (reload point for other 11 tables)
    const char* logical;
    uintptr_t   loader;
// sub_407A21 本体（开局准备）
// sub_407A21 itself (game prep)
    uintptr_t   mgrGlob;

// ---- 12 个平衡 loader（顺序 = 依赖顺序，重跑必须保持）----
#pragma endregion

#pragma region 12 balance loaders (order = dependency order, must preserve on re-run)
};
static const LogicFile kLogicFiles[] = {
// 相对 data\logic\ 的路径
// path relative to data\logic\
    { "landscapetypes.ini",                     0x4162E5, 0x511534 },
    { "trianglepatterntypes.ini",               0x415EBB, 0x5114C4 },
// loader 函数 VA
// loader function VA
    { "goodtypes.ini",                          0x41592E, 0x511424 },
    { "housetypes.ini",                         0x41531D, 0x5113A4 },
// loader 存储 manager 指针的全局 VA
// global VA where the loader stores its manager pointer
    { "vehicletypes.ini",                       0x414DE0, 0x511314 },
    { "atomicanimations\\atomicanimations.ini", 0x414B17, 0x511300 },
    { "tribetypes\\tribetypes.ini",             0x413E7B, 0x511204 },
    { "jobtypes.ini",                           0x4139DB, 0x511184 },
    { "weapontypes.ini",                        0x4133F6, 0x5110D0 },
    { "armortypes.ini",                         0x41312C, 0x510FDC },
    { "animaltypes.ini",                        0x412C52, 0x510F30 },
    { "humanjobexperiencetypes.ini",            0x412855, 0x510C20 },
};
static volatile unsigned char g_redirect = 0;
static bool   g_installed = false;
static bool   g_dbg       = true;
static char   g_canonical[12][64] = { {0} };
static char   g_candidate[MAX_PATH] = { 0 };
static void*  p_tramp_inopen    = nullptr;
static void*  p_tramp_maploader = nullptr;

// ---- 运行期状态（stub 引用，需稳定地址）----
#pragma endregion

#pragma region Runtime state (referenced by stubs, need stable addresses)
static void*  p_orig_407A21     = nullptr;
// IniFile_Open 重定向会话开关
// IniFile_Open redirect-session switch
static void*  p_pathrewrite  = nullptr;
static void*  p_reload       = nullptr;
static void*  p_reload_entry = nullptr;
static void*  g_mapSrc  = nullptr;
// data\logic\X.ini 规范路径
// canonical data\logic\X.ini paths
static void*  g_mapPath = nullptr;
static char   g_mapSrcStr[MAX_PATH] = { 0 };
// PathRewrite 输出缓冲
// PathRewrite output buffer
static bool   g_mapHasLogic = false;
static const char* kReloadKeys[12] = {
// IniFile_Open 原入口 trampoline
// IniFile_Open original-entry trampoline
    "ReloadLandscapetypes",         "ReloadTrianglepatterntypes",
    "ReloadGoodtypes",              "ReloadHousetypes",
// MapLoader 入口 trampoline（v3.5）
// MapLoader entry trampoline (v3.5)
    "ReloadVehicletypes",           "ReloadAtomicanimations",
    "ReloadTribetypes",             "ReloadJobtypes",
// 原 sub_407A21（v3 触发点 jmp 目标）
// original sub_407A21 (v3 trigger jmp target)
    "ReloadWeapontypes",            "ReloadArmortypes",
    "ReloadAnimaltypes",            "ReloadHumanjobexperiencetypes",
// -> PathRewrite
// -> PathRewrite
};
static bool g_reload[12] = { true, true, true, true, true, true,
// -> ReloadLogicForMap
// -> ReloadLogicForMap
                             true, true, true, true, true, true };
static void* g_shadowGood = nullptr;
// -> ReloadGoodTypesAtEntry
// -> ReloadGoodTypesAtEntry
static unsigned char g_goodSnap[0x35A0];
struct ImmPatch { uintptr_t va; int immOff; };
// MapLoader arg1 Source（官方/用户文件夹图=地图源目录串）
// MapLoader arg1 Source (official/user folder map = map source dir string)
static const ImmPatch kGoodPatches[] = {
    { 0x415954, 1 },
// MapLoader arg2（c2m/缓冲区模式=文件名串）
// MapLoader arg2 (c2m/buffer mode = file name string)
    { 0x41596C, 1 },
    { 0x41597B, 2 },
// 当前地图真实源目录
// current map's real source dir
    { 0x4159F8, 2 },
    { 0x415C5A, 1 },
// 该图是否有 <src>\logic 目录
// does this map have a <src>\logic dir
    { 0x415D87, 1 },

// ---- 每表独立开关（二分定位 bug 用）：配置 [PerMapLogic] Reload<Name> = 1/0 ----
#pragma endregion

#pragma region Per-table switches (binary-search debugging): [PerMapLogic] Reload<Name> = 1/0
};
static void PatchGoodGlobals(DWORD b, bool useShadow) {
    uint32_t dst = useShadow ? (uint32_t)(uintptr_t)&g_shadowGood : 0x511420u;
    for (const auto& p : kGoodPatches)
        Patch::WriteU32(b + (p.va - 0x400000) + p.immOff, dst);
}
static bool GoodTypesOverrideDiffers() {
    if (!g_mapSrcStr[0]) return false;
    char p[MAX_PATH], q[MAX_PATH];
    sprintf_s(p, "%s\\logic\\goodtypes.ini", g_mapSrcStr);

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
#pragma endregion

#pragma region goodtypes shadow table (thread-safe reload, v3.3)
// Symptom: the game breaks ONLY when ReloadGoodtypes=1 (icons/items vanish); the
//   other 11 tables are all safe.
// Root cause (user binary search + static confirmation): while goodtypes loader
//   (sub_41592E) runs, it first zeroes dword_511420 -> operator new -> fills slots;
//   during that window the table pointer points at an all-zero/half-empty new table
//   for tens of milliseconds (including disk IO). The render/item thread reads the
//   table every frame -> hits the half-empty table -> icon/item lookup fails and
//   caches a bad result. Other tables are safe = icon rendering only queries goodtypes.
// Fix: during the reload session, redirect the loader's 6 references to dword_511420
//   to a DLL shadow global g_shadowGood -> the loader's parse keeps dword_511420
//   pointing at the original (complete) table (render thread always sees a complete
//   table) -> after parse, memcpy into the original table -> restore the imm32.
// Also: if the override file is byte-identical to the global, skip the reload
//   (reload is meaningless and carries a window risk).
    sprintf_s(q, "data\\logic\\goodtypes.ini");
// loader 解析用的影子表指针槽（DLL 内）
// shadow table pointer slot used by the loader (inside DLL)
    HANDLE hp = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
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
// * v3.6 real root cause: a goodtypes record's +36 is a RUNTIME-FILLED derived
//   handle, not from the ini. The loader writes +36 as -1 (unresolved); the start-up
//   flow has a one-time guard sub_401981:
//   `if (!this[40]) { sub_406C3E(); sub_47D3E6(); sub_407665(); this[40]=1; }`,
//   where sub_407665 -> sub_415E8A walks 56 records:
//       h = sub_47FEF0(rec[+32] /*landscapetype*/, 1);  rec[+36] = h;
//   sub_47FEF0 = finds an entry in the landscape graphics array (dword_568C24,
//   stride 140, count dword_568C20) by landscapetype and returns the **index**
//   (pure lookup, no side effects, recomputable anytime).
//   +36 = "good -> landscape graphics index" cache, resolved exactly ONCE.
//   => Any goodtypes reload (v2 table swap / v3.x memcpy) sets +36 back to -1,
//      all goods lose their graphics handle -> no icons / can't place in world /
//      can't harvest (user-confirmed symptom). This also explains "override
//      byte-identical to global still breaks" -- +36 simply isn't in the ini.
//   Fix: after reload, call sub_415E8A() to re-resolve +36 (the engine's own entry,
//     100% semantic match).
    if (hp == INVALID_HANDLE_VALUE) return false;
// 重载前的表快照（自校验 diff 用）
// table snapshot before reload (for self-check diff)
    HANDLE hq = CreateFileA(q, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hq == INVALID_HANDLE_VALUE) { CloseHandle(hp); return false; }
    DWORD sp = GetFileSize(hp, nullptr), sq = GetFileSize(hq, nullptr);
    if (sp != sq) { CloseHandle(hp); CloseHandle(hq); return true; }
// 68 imm32        push offset dword_511420（memset 清指针）
// 68 imm32        push offset dword_511420 (memset clears pointer)
    bool diff = false;
    char b1[8192], b2[8192];
// A3 imm32        mov  dword_511420, eax（写新表指针）
// A3 imm32        mov  dword_511420, eax (write new table pointer)
    DWORD total = 0;
    while (total < sp) {
// FF 35 imm32     push dword_511420（strcpy "none"）
// FF 35 imm32     push dword_511420 (strcpy "none")
        DWORD r1 = 0, r2 = 0;
        ReadFile(hp, b1, sizeof(b1), &r1, nullptr);
// 8B 0D imm32     mov  ecx, dword_511420（解析读表）
// 8B 0D imm32     mov  ecx, dword_511420 (parse reads table)
        ReadFile(hq, b2, sizeof(b2), &r2, nullptr);
        if (r1 != r2 || memcmp(b1, b2, r1) != 0) { diff = true; break; }
// A1 imm32        mov  eax, dword_511420（字段写）
// A1 imm32        mov  eax, dword_511420 (field write)
        if (r1 == 0) break;
        total += r1;
// A1 imm32        mov  eax, dword_511420（字段写）
// A1 imm32        mov  eax, dword_511420 (field write)
    }
    CloseHandle(hp); CloseHandle(hq);
    return diff;
}
static void ReloadGoodTypesSafe(DWORD b) {
    void* orig = *(void**)(b + (0x511420 - 0x400000));
    bool haveSnap = false;
// 覆盖文件是否与全局不同（无覆盖/相同 -> false，跳过重载）
// Does the override differ from the global? (no override / identical -> false, skip reload)
    if (orig) { memcpy(g_goodSnap, orig, 0x35A0); haveSnap = true; }
    PatchGoodGlobals(b, true);
    void* mgr = *(void**)(b + (0x511424 - 0x400000));
    void* fn  = (void*)(b + (0x41592E - 0x400000));
    if (mgr && fn)
        ((void* (__fastcall*)(void*, void*))fn)(mgr, mgr);
    void* shadow = *(void**)&g_shadowGood;
// 无覆盖 -> 全局
// no override -> use global
    orig = *(void**)(b + (0x511420 - 0x400000));
    if (orig && shadow && shadow != orig)
        memcpy(orig, shadow, 0x35A0);
    PatchGoodGlobals(b, false);
    typedef int (__cdecl* ResolveGfxFn)(void);
    ResolveGfxFn resolveGfx = (ResolveGfxFn)(b + (0x415E8A - 0x400000));
    int lsCount = *(int*)(b + (0x568C20 - 0x400000));
    if (lsCount > 0) {
        resolveGfx();
        LOG_INFO(kCat, "  [gfx] good->landscape handles re-resolved (sub_415E8A, lsTable=%d)",
                 lsCount);
    } else {
        LOG_WARN(kCat, "  [gfx] SKIPPED: landscape gfx table empty (dword_568C20=0)"
                       " -> +36 handles stay -1 (icons/harvest will break)");
    }
    typedef void (__thiscall* RebuildIndexFn)(void*);
    RebuildIndexFn rebuild = (RebuildIndexFn)(b + (0x412B62 - 0x400000));
    void* idxMgr = *(void**)(b + (0x510F20 - 0x400000));
    if (rebuild && idxMgr) {
// 影子表安全重载：快照 -> loader 写影子 -> memcpy 到原表 -> 恢复 imm32
//   -> 重解析 +36 图形句柄（v3.6 关键）-> 重建 land 索引 -> 自校验 diff
// Shadow-table safe reload: snapshot -> loader writes shadow -> memcpy into original
//   -> restore imm32 -> re-resolve +36 graphics handle (v3.6 key) -> rebuild land
//   index -> self-check diff
        rebuild(idxMgr);
        LOG_INFO(kCat, "  [idx] land->goodid index rebuilt (sub_412B62)");
    }
    if (haveSnap && orig) {
// 重载前快照
// snapshot before reload
        const unsigned char* A = g_goodSnap;


        const unsigned char* B = (const unsigned char*)orig;
// loader 解析期间 dword_511420 保持原表
// during parse, dword_511420 keeps pointing at original table
        int nd = 0; char det[220]; det[0] = 0; int used = 0;
        for (int off = 0; off < 0x35A0; off += 4) {
            unsigned int x = *(const unsigned int*)(A + off);
            unsigned int y = *(const unsigned int*)(B + off);
            if (x == y) continue;
// ecx=this, edx 忽略
// ecx=this, edx ignored
            ++nd;
            if (g_dbg && used < 5) {
// 新表（完整）
// new (complete) table
                char one[48];
                sprintf_s(one, " g%d+%d:%d->%d", off / 208, off % 208, (int)x, (int)y);
// 原表（未变）
// original (unchanged)
                if (strlen(det) + strlen(one) < sizeof(det) - 1) { strcat_s(det, one); ++used; }
            }
        }
// 微秒级换入
// microsecond swap-in
        LOG_INFO(kCat, "  [diff] dwords changed vs pre-reload = %d%s", nd,
                 (g_dbg && det[0]) ? det : " (should equal count of edited fields)");
// 恢复 loader 正常引用
// restore loader's normal references
    }

    // ★★ v3.6 根因修复：重新解析记录 +36 的 landscape 图形句柄。
    //   loader 把 +36 全写成 -1，而它本来由启动一次性流程 sub_415E8A 填好。
    //   memcpy 后必须重跑，否则所有 good 无图形 -> 无图标/放不下/采不到。

    // ** v3.6 root-cause fix: re-resolve the record +36 landscape graphics handle.
    //    the loader writes +36 all to -1, but it was filled once at start-up by
    //    sub_415E8A. After memcpy we MUST re-run it, or every good loses its
    //    graphics -> no icons / can't place / can't harvest.
}
static bool g_entryHook = false;
static bool g_goodAtEntryLoaded = false;
// landscape 图形表条目数
// landscape graphics table entry count
static bool g_goodAtEntryOverridden = false;
static void ReloadGoodTypesAtEntry() {
    if (!g_installed || !g_entryHook) return;
    DWORD b = (DWORD)gameapi::g_imageBase;
    g_mapSrcStr[0] = 0;
    g_mapHasLogic = false;
    if (g_mapSrc && *(char*)g_mapSrc) {
        strncpy_s(g_mapSrcStr, (const char*)g_mapSrc, sizeof(g_mapSrcStr) - 1);
        g_mapSrcStr[sizeof(g_mapSrcStr) - 1] = 0;

    // v3.4：重建 land->goodid 索引（dword_510C60）。索引由 sub_412B62 在启动时
    //   基于全局表构建，采集/生成系统查它找"地貌 X 上的物品 id"。

    // v3.4: rebuild the land->goodid index (dword_510C60). The index is built by
    //   sub_412B62 at start-up from the global table; the harvest/spawn system
    //   queries it for "item id on landscape X".
        char dir[MAX_PATH];
        sprintf_s(dir, "%s\\logic", g_mapSrcStr);
        g_mapHasLogic = (GetFileAttributesA(dir) != INVALID_FILE_ATTRIBUTES);
    }
    g_goodAtEntryLoaded = true;
    g_goodAtEntryOverridden = false;
    if (g_mapHasLogic && GoodTypesOverrideDiffers()) {

    // 自校验：对比重载前后整表。理想结果 = 只有 ini 里真正改过的字段有差异。
    //   若出现别的偏移变成 0/-1，说明还有运行期回填字段被 memcpy 清掉（继续修）。

    // Self-check: compare the whole table before/after reload. Ideal result = only
    //   the fields actually edited in the ini differ. If other offsets become 0/-1,
    //   another runtime-filled field was cleared by the memcpy (keep fixing).
        g_redirect = 1;
        ReloadGoodTypesSafe(b);
        g_redirect = 0;
        g_goodAtEntryOverridden = true;
        LOG_INFO(kCat, "  goodtypes.ini -> MAP <src>\\logic (reloaded at map entry, pre-StaticObjects)");
    } else {
        ReloadGoodTypesSafe(b);
        LOG_INFO(kCat, "  goodtypes.ini -> GLOBAL data\\logic (reloaded at map entry, pre-StaticObjects)");
    }
}
struct TableEntry { uintptr_t globVa; int size; };
static const TableEntry kTables[] = {
    { 0x511528, 0x35A0  },
    { 0x5114C0, 0x210   },
    { 0x511420, 0x35A0  },
    { 0x5113A0, 0x151BC },
    { 0x511310, 0xD04   },
    { 0x511180, 0x1DC0  },
    { 0x510FD8, 0x104   },

// ---- v3.5：地图加载器入口的 goodtypes 重载 ----
// 必须在 [StaticObjects] 解析（重载前用全局表）之前完成，否则地图开局资源
// 按旧表解析 -> 重载后表与地图不一致 -> 物品对象创建失败（无图标/无法放置/
// 采集空）。v2.5 入口触发失败的根因是 12 表全重载的文件层扰动；这里只重载
// goodtypes（1 次 IniFile_Open + 影子表），风险已消除。
#pragma endregion

#pragma region v3.5 goodtypes reload at MapLoader entry
// Must complete BEFORE [StaticObjects] parsing (which uses the global table);
//   otherwise the map starts with the old table -> after reload the table and map
//   disagree -> item objects fail to create (no icons / can't place / empty harvest).
// The v2.5 entry-trigger failure root cause was file-layer perturbation from
//   reloading all 12 tables; here we reload ONLY goodtypes (one IniFile_Open +
//   shadow table), risk eliminated.
    { 0x510F2C, 0x880   },
// 入口 hook 是否装成功（收尾点据此跳过 goodtypes）
// entry hook installed? (tail hook skips goodtypes if so)
    { 0x510C18, 0xFD8   },
    { 0x510C1C, 0x9A0   },
// goodtypes 在入口是否已处理（计入 loaders）
// goodtypes processed at entry? (counted in loaders)
};
static void* g_origTables[10] = { nullptr };
// 入口是否真用了地图覆盖（计入 overridden）
// entry actually used the map override? (counted in overridden)
static bool  g_snapshotted = false;
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
static uintptr_t MakeTrampoline(uintptr_t entry, int copyLen) {
    uintptr_t t = (uintptr_t)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
    // 无论本图有无 logic 目录，goodtypes 都必须在入口处理一次：
    //   - 有覆盖且不同于全局 -> 读 MAP <src>\logic（重载）
    //   - 有覆盖但等于全局 / 无覆盖 -> 读 GLOBAL data\logic（重载，清掉上一图的脏数据）
    // 否则上一图改过的 goodtypes 会残留在表里、污染下一图（用户实测隐患）。
    // goodtypes must be processed once at entry regardless of whether this map has a
    //   logic dir:
    //   - override present and differs from global -> read MAP <src>\logic (reload)
    //   - override absent or identical to global -> read GLOBAL data\logic (reload,
    //     clears the previous map's dirty data)
    // otherwise the previous map's modified goodtypes lingers in the table and
    //   pollutes the next map (user-confirmed hazard).
                                          PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
// 防上一图串味
// prevent cross-map flavor carryover
    std::vector<uint8_t> orig = Patch::ReadBytes(entry, copyLen);
    if (orig.size() != (size_t)copyLen) {
        VirtualFree((void*)t, 0, MEM_RELEASE);
// IniFile_Open 改写：loader 读地图包 logic\goodtypes.ini
// IniFile_Open rewrite: loader reads map package logic\goodtypes.ini
        return 0;
    }
    memcpy((void*)t, orig.data(), copyLen);
    uintptr_t back = entry + copyLen;
    *(uint8_t*)(t + copyLen) = 0xE9;
    *(int32_t*)(t + copyLen + 1) = (int32_t)(back - (t + copyLen + 5));
        // 无覆盖或覆盖与全局相同：用全局 ini 重载，确保表回到本图应有的状态
        // no override or identical to global: reload with global ini to restore the
        // table to this map's intended state
    return t;
}
static const char* __cdecl PathRewrite(const char* path) {
    if (!g_redirect || !path) return path;

// ---- 拷回表（指针身份保持）：重载后把新内容拷回原表地址、恢复原指针 ----
// 表全局 VA + 表大小（0 = 跳过，动态大小表如 atomicanim/tribe/weapon）
#pragma endregion

#pragma region Copy-back tables (pointer identity preserved)
// After reload, copy new content back to the original table address and restore the
//   original pointer (pointer identity never changes).
// Table global VA + size (0 = skip, dynamic-size tables like atomicanim/tribe/weapon)
    if (!g_mapHasLogic || !g_mapSrcStr[0]) return path;
    for (int i = 0; i < 12; ++i) {
        if (_stricmp(path, g_canonical[i]) != 0) continue;
// landscapetypes
// landscapetypes
        sprintf_s(g_candidate, "%s\\logic\\%s", g_mapSrcStr, kLogicFiles[i].logical);
        if (GetFileAttributesA(g_candidate) != INVALID_FILE_ATTRIBUTES)
// trianglepatterntypes
// trianglepatterntypes
            return g_candidate;
        return path;
// goodtypes
// goodtypes
    }
    return path;
// housetypes
// housetypes
}
static void ReloadLogicForMap() {
// vehicletypes
// vehicletypes
    if (!g_installed) return;
    DWORD b = (DWORD)gameapi::g_imageBase;
// jobtypes
// jobtypes
    g_mapSrcStr[0] = 0;
    g_mapHasLogic = false;
// armortypes
// armortypes
    if (g_mapSrc && *(char*)g_mapSrc) {
        strncpy_s(g_mapSrcStr, (const char*)g_mapSrc, sizeof(g_mapSrcStr) - 1);
// animaltypes
// animaltypes
        g_mapSrcStr[sizeof(g_mapSrcStr) - 1] = 0;
        char dir[MAX_PATH];
// humanjobexperiencetypes 主表
// humanjobexperiencetypes main table
        sprintf_s(dir, "%s\\logic", g_mapSrcStr);
        g_mapHasLogic = (GetFileAttributesA(dir) != INVALID_FILE_ATTRIBUTES);
// humanjobexperiencetypes 附表
// humanjobexperiencetypes sub table
    }
    const char* ident = g_mapSrcStr[0] ? g_mapSrcStr
                      : (g_mapPath && *(char*)g_mapPath) ? (const char*)g_mapPath : "?";
// 启动时原表指针
// original table pointers at start-up
    if (!g_snapshotted) {
        for (int i = 0; i < 10; ++i)

// 重载后：把新表内容拷回原表地址 + 恢复原指针（指针身份永不变）

// After reload: copy new content back to the original table address + restore pointer
            g_origTables[i] = *(void**)(b + (kTables[i].globVa - 0x400000));
        g_snapshotted = true;
        LOG_INFO(kCat, "[snapshot] original table pointers captured (%d tables)", 10);
    }
    if (!g_mapHasLogic) {
        LOG_INFO(kCat, "map has no logic folder, tables untouched (map=%s)", ident);
        return;
    }
    int over = 0, ok = 0;
    g_redirect = 1;

// ---- 运行时 trampoline：拷贝原 copyLen 字节 + E9 跳回 entry+copyLen ----
#pragma endregion

#pragma region Runtime trampoline (copy copyLen bytes + E9 back to entry+copyLen)
    for (int i = 0; i < 12; ++i) {
        if (!g_reload[i]) {
            LOG_INFO(kCat, "  %-42s -> SKIPPED (switch off)", kLogicFiles[i].logical);
            continue;
        }
        if (i == 2) {
            if (g_entryHook) {
                ++ok;
                if (g_goodAtEntryOverridden) ++over;
                continue;
            }
            if (!GoodTypesOverrideDiffers()) {
                LOG_INFO(kCat, "  %-42s -> GLOBAL (override absent/identical, reload skipped)",
                         kLogicFiles[i].logical);
                continue;

// 引擎路径可用性检查已弃用：不调 sub_40667B（会开/关 CRT fd、做归档搜索，
// 在 IniFile_Open 入口里嵌套调用会扰动引擎文件层 -> goodtypes 等表加载失败）。
// 改用纯 Win32 GetFileAttributesA 探测真实路径（<地图源目录>\logic\X.ini）。

// ---- 路径重定向（仅重载会话内生效；低开销直通）----
#pragma endregion

// Engine path availability check abandoned: do not call sub_40667B (it opens/closes
//   CRT fds, does archive searching; calling it nested inside IniFile_Open's entry
//   perturbs the engine file layer -> goodtypes etc. fail to load). Use plain Win32
//   GetFileAttributesA to probe the real path (<map source dir>\logic\X.ini).

#pragma region Path rewrite (only active during reload session; low-overhead pass-through)
            }
            g_redirect = 1;
            ReloadGoodTypesSafe(b);
            g_redirect = 0;
            ++ok; ++over;
            LOG_INFO(kCat, "  %-42s -> MAP <src>\\logic (shadow-safe reload, fallback)", kLogicFiles[i].logical);
            continue;
        }
        int r = 0;
// 该表缺失 -> 全局
// table missing -> global
        if (g_mapSrcStr[0]) {
            char p[MAX_PATH];
            sprintf_s(p, "%s\\logic\\%s", g_mapSrcStr, kLogicFiles[i].logical);
            r = (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) ? 1 : 0;

// ---- 重载 12 张平衡表（仅当地图带 logic\ 目录时才重跑；否则完全不碰表）----
#pragma endregion

#pragma region Reload 12 balance tables (only when the map carries a logic dir)
            if (r) ++over;
        }
        void* mgr = *(void**)(b + (kLogicFiles[i].mgrGlob - 0x400000));
    // 地图源目录：arg1(Source) 非空 = 官方/用户文件夹图的真实源目录；c2m 不支持包内探测
    // Map source dir: arg1(Source) non-null = real source dir of official/user folder
    //   map; c2m in-package detection not supported.
        void* fn  = (void*)(b + (kLogicFiles[i].loader - 0x400000));
        if (mgr && fn) {
            ((void* (__fastcall*)(void*, void*))fn)(mgr, mgr);
            ++ok;
        } else {
            LOG_WARN(kCat, "loader[%d] %s mgr=%p fn=%p skipped",
                     i, kLogicFiles[i].logical, mgr, fn);
        }
        LOG_INFO(kCat, "  %-42s -> %s", kLogicFiles[i].logical,
                 r ? "MAP <src>\\logic" : "GLOBAL data\\logic");
    }

    // 首次触发：快照原表指针（此时必为启动时分配，之后指针身份永不变）

    // First trigger: snapshot original table pointers (must be the start-up-allocated
    //   ones; pointer identity never changes afterwards)
    g_redirect = 0;
    CopyBackTables(b);
    LOG_INFO(kCat, "logic reloaded: map=%s loaders=%d/12 overridden=%d/12 (tables copied back to original addresses)",
             ident, ok, over);
}
__declspec(naked) void IniOpenStub() {

    // ---- 诊断：无 logic 目录 -> 绝不重跑（主菜单 demo 图/无覆盖图完全不碰表）

    // Diagnostic: no logic dir -> NEVER re-run (main-menu demo map / un-overridden map
    //   completely untouched)
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
// goodtypes：已在入口（0x40A6F4）重载，收尾点跳过；入口失败时兜底 shadow-safe
// goodtypes: already reloaded at entry (0x40A6F4); tail skips;
        popad
            //   entry failure -> shadow-safe fallback here
        jmp  dword ptr [p_tramp_inopen] ; 原入口 + 原 9 字节
    }
// 入口已处理 -> 计入 loaders
// entry handled -> counted in loaders
}
__declspec(naked) void PrepMapEntryStub() {
// 入口真用了覆盖 -> 计入 overridden
// entry really used override -> counted in overridden
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
static bool VerifyBytes(DWORD base, uintptr_t va, const uint8_t* exp, size_t n) {
// ecx=this, edx 忽略
// ecx=this, edx ignored
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

    // 拷回：新内容写回原表地址、恢复原指针（缓存指针的子系统不会失效）

    // Copy back: write new content to original table addresses, restore pointers
    //   (subsystems caching the table pointer don't break)
        if (!cfg.GetBool(kName, "Enabled", false)) {
            LOG_INFO(kCat, "Disabled (Enabled=0)");
            return true;
        }

// ===================================================================
// IniFile_Open 入口 stub —— 重定向会话内改写 [esp+0x28]（Source）
//   thiscall：ecx=目标缓冲，[esp+4]=Source；pushad(32)+pushfd(4) 后 Source=[esp+0x28]
// ===================================================================
#pragma endregion

#pragma region Hook stubs
// ===================================================================
// IniFile_Open entry stub -- rewrite [esp+0x28] (Source) during redirect session
//   thiscall: ecx=target buffer, [esp+4]=Source; after pushad(32)+pushfd(4) Source=[esp+0x28]
// ===================================================================
        gameapi::Init(ver.GetBaseAddress());
        DWORD b = ver.GetBaseAddress();
        g_dbg = cfg.GetBool(kName, "Debug", true);
        int on = 0;
        for (int i = 0; i < 12; ++i) {
            g_reload[i] = cfg.GetBool(kName, kReloadKeys[i], true);
            if (g_reload[i]) ++on;
        }
        LOG_INFO(kCat, "per-table reload switches: %d/12 on", on);
        const uint8_t kCallOp = 0xE8;
        if (!VerifyBytes(b, VA_PrepCall, &kCallOp, 1)) return false;
        const uint8_t kIniPro[9] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x08, 0x03, 0x00, 0x00 };
        if (!VerifyBytes(b, VA_IniFileOpen, kIniPro, sizeof(kIniPro))) return false;
        for (int i = 0; i < 12; ++i)
            sprintf_s(g_canonical[i], "data\\logic\\%s", kLogicFiles[i].logical);
        uintptr_t iniEntry = b + (VA_IniFileOpen - 0x400000);
        uintptr_t tramp = MakeTrampoline(iniEntry, 9);

// ===================================================================
// 地图加载器入口 stub（v3.5：hook MapLoader_LoadCurrentMap 0x40A6F4 入口）
//   进入时（cdecl）：[esp]=返回地址，[esp+4]=Source（地图源目录串）
//   pushad(32)+pushfd(4) 后 Source=[esp+0x28]
//   只做 goodtypes 重载（在 [StaticObjects] 解析之前）-> 地图开局资源按新表解析
// ===================================================================

// ===================================================================
// MapLoader entry stub (v3.5: hook MapLoader_LoadCurrentMap 0x40A6F4 entry)
//   on entry (cdecl): [esp]=return addr, [esp+4]=Source (map source dir string)
//   after pushad(32)+pushfd(4) Source=[esp+0x28]
//   only reload goodtypes (before [StaticObjects] parsing) -> map starts with new table
// ===================================================================
        if (!tramp) { LOG_ERROR(kCat, "IniFile_Open trampoline alloc failed"); return false; }
        p_tramp_inopen = (void*)tramp;
        p_pathrewrite  = (void*)&PathRewrite;
        if (!Patch::WriteJmp(iniEntry, (uintptr_t)&IniOpenStub, 0)) {
            LOG_ERROR(kCat, "IniFile_Open hook WriteJmp failed @0x%X", (unsigned)VA_IniFileOpen);
            return false;
        }
        const uint8_t kMapPro[9] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x6C, 0x02, 0x00, 0x00 };
        g_entryHook = false;
        if (VerifyBytes(b, VA_MapLoaderEntry, kMapPro, sizeof(kMapPro))) {
            uintptr_t mapEntry = b + (VA_MapLoaderEntry - 0x400000);
            uintptr_t mTramp = MakeTrampoline(mapEntry, 9);

// ===================================================================
// 地图加载收尾触发 stub（hook 0x40AA13 的 call sub_407A21）
//   进入时：[esp]=返回地址(0x40AA18)，[esp+4]=1（调用点 push 1）
//   此时地图 ini/世界/[StaticObjects] 已全部处理完，重载只影响运行时读取
//   （v3：触发点从 0x40A6F4 入口移回收尾点 —— 调试器实证：入口重载会让
//   紧随其后的 $maproot$\map.ini 打开失败（句柄全零），导致 [StaticObjects]
//   全被跳过 → 空图；收尾点重载没有这个问题）
//   地图身份从加载器帧取：arg1=Source=[ebp+8]，arg2=[ebp+0xC]
// ===================================================================

// ===================================================================
// MapLoader tail trigger stub (hook 0x40AA13's call sub_407A21)
//   on entry: [esp]=return addr(0x40AA18), [esp+4]=1 (call site push 1)
//   at this point the map ini/world/[StaticObjects] are all processed; reload only
//   affects runtime reads (v3: trigger moved from entry 0x40A6F4 to tail -- debugger
//   confirmed entry reload made the following $maproot$\map.ini open fail (zero
//   handle) -> [StaticObjects] all skipped -> empty map; tail reload has no such issue)
//   map identity from loader frame: arg1=Source=[ebp+8], arg2=[ebp+0xC]
// ===================================================================
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
            LOG_WARN(kCat, "entry 0x%X prologue mismatch, skip entry hook (epilogue fallback)", (unsigned)VA_MapLoaderEntry);
        }

// ---- 字节校验 ----
#pragma endregion

#pragma region Byte verification + Feature class
// Byte verification
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

        // 0.5) 读每表独立开关（二分定位：默认全开）

        // 0.5) read per-table switches (binary search: default all on)
        return true;
    }
};
REGISTER_FEATURE(PerMapLogicFeature)
}
#pragma endregion
