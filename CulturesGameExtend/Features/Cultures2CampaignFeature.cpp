// Cultures2CampaignFeature.cpp
// ===================================================================
// [Cultures2Campaign]
// 让「文化II：阿斯加德之门」按钮真正加载文化II 战役：
//   点击 -> 进入大地图/选关界面（带红色路线）-> 选关进图。
//
// -------------------------------------------------------------------
// 一、为什么是「复用 screen 5」而不是「新建 screen 24」
// -------------------------------------------------------------------
// 主菜单 MainMenuUI_Build(0x4D24BB) 是 28-case 状态机，跳表在 0x4D5CFB，
// case = screen-1。曾设想占用 screen 24（跳表项指向 default），但实地逆向
// EnterScreen(0x4D1F44)/LeaveScreen(0x4D1E6E) 后确认：screen 24/25 是多人
// 游戏 auto-join 屏，26-28 也各有真实 handler —— **没有空闲屏幕号**。
//
// 而 screen 5（北国风云III / campaign 2）的 handler sub_4D310A 与绘制函数
// sub_4D98BD 的 screen5 分支，**整段逻辑都是通用的**，战役相关的只有 7 处
// 立即数/绝对地址：
//   handler：cmp ecx,2（战役号） + 4 处节点坐标表基址引用
//   painter：路线表基址 + sub_416E20 的战役号
// 背景帧 push 2（0-based frame 2 = 1-based 帧3）文化II/III 共用同一张大地图
// 屏幕（已验证两版 bmd 该帧字节完全相同），所以不用改。
//
// 于是方案变为「**战役上下文交换**」：进入 screen 5 前，按目标战役把这 7 处
// 改成文化II 的值（节点/路线表放在本 DLL 里），离开时改回北国风云的值。
// game.exe 里不新增任何代码，只改 7 个立即数。
//
// -------------------------------------------------------------------
// 二、模式仲裁点：SwitchScreen 入口（关键设计）
// -------------------------------------------------------------------
// 进入 screen 5 的路径不止一条（Asgard 按钮 / Nordland 按钮 /
// start_campaign_2_screen 命令消费）。若在每个按钮上分别设、清标志，
// 漏掉任何一条就会用错数据表。
// 因此改为：只在 MainMenu_SwitchScreen(0x4D1E45) 入口挂一个钩子 ——
//   if (screen == 5) ApplyMode(g_pendingC2), g_pendingC2 = 0;
// 只有「文化II 入口」会在调用前把 g_pendingC2 置 1，其余任何路径进入
// screen 5 时 pending 都是 0，自动还原成北国风云。**一个点覆盖全部入口。**
//
// -------------------------------------------------------------------
// 三、入口按钮：ctlId 5006（原版空闲）
// -------------------------------------------------------------------
// MainMenu_OnCommand(0x4D5E71) 用跳表 0x4D62BD 分发 ctlId 5000..5050。
// 实测 slot[6]（ctl 5006 / 0x138E）指向 default 出口 0x4D62B4，**原版未使用**。
// 于是把 Asgard 按钮的 ctlId 从 0x1397(5015，原版=UserCampaign00 屏，这正是
// 「点了跳到别处」的根因) 改为 0x138E，并把 slot[6] 指向本 DLL 的 stub。
// 游戏 .text 一个字节都不用动，只改一个跳表指针。
//
// -------------------------------------------------------------------
// 四、补上悬空的 start_campaign_1_screen
// -------------------------------------------------------------------
// 注册者 sub_4C7F00 会为 campaignId==1 注册命令 "start_campaign_1_screen"
// (0x508740)，但消费者 sub_402122 只处理 2/3/4（-> SwitchScreen 5/6/7），
// **campaign 1 没有任何消费者** —— 通关一关后回不到文化II 大地图。
// 本 Feature 在该函数尾部 0x40223B（`5F B0 01 5E C3`，正好 5 字节）挂钩，
// 按 2/3/4 完全相同的模式补上 campaign 1 的分支。
//
// -------------------------------------------------------------------
// 五、资源前置
// -------------------------------------------------------------------
// Data\gui\lang\ger\bobs\ls_menu_logos.bmd 必须是 35 帧版本
// （原 26 帧 + 追加原版文化2 的 9 条路线精灵 = 0-based 帧 26..34）。
// 由 tools/build_c2_campaign_bmd.py 生成。若漏部署，Bob_DrawFrame(0x464B77)
// 开头有帧号边界检查，越界会静默返回 —— 只是路线不显示，不会崩。
//
// 配置（plugins/config/CulturesGameExtend_Game.ini）：
//   [Cultures2Campaign]
//   Enabled = 1
// ===================================================================
#include "pch.h"
#include "Core/Feature.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Patch.h"
#include "Core/Logger.h"
#include "Core/GameApi.h"
#include <cstring>

namespace fe_c2 {

const char* kName = "Cultures2Campaign";
const char* kCat  = "[Cultures2Campaign]";

// ===================================================================
// RVA 常量（RVA = VA - 0x400000）
// ===================================================================

// ---- screen5 handler sub_4D310A 内的 5 处战役相关立即数 ----
// 004D3167  83 f9 02              cmp ecx, 2                   <- imm8  @ +2
// 004D3185  8b 0d f8 89 50 00     mov ecx, [0x5089F8]          <- disp32 @ +2
// 004D31A0  bf f8 89 50 00        mov edi, 0x5089F8            <- imm32  @ +1
// 004D31BE  8b b8 fc 89 50 00     mov edi, [eax + 0x5089FC]    <- disp32 @ +2
// 004D31C7  8b 88 00 8a 50 00     mov ecx, [eax + 0x508A00]    <- disp32 @ +2
constexpr uintptr_t R_HdlCampaign = 0xD3167;
constexpr uintptr_t R_HdlNodeA    = 0xD3185;
constexpr uintptr_t R_HdlNodeB    = 0xD31A0;
constexpr uintptr_t R_HdlNodeC    = 0xD31BE;
constexpr uintptr_t R_HdlNodeD    = 0xD31C7;

// ---- painter sub_4D98BD 的 screen5 分支 2 处 ----
// 004D9952  b8 e8 7e 4f 00        mov eax, 0x4F7EE8            <- imm32 @ +1
// 004D9962  6a 02                 push 2                       <- imm8  @ +1
constexpr uintptr_t R_PntRoute    = 0xD9952;
constexpr uintptr_t R_PntCampaign = 0xD9962;

// ---- 原版（北国风云 / campaign 2）数据表 ----
constexpr uintptr_t R_NodesNordland  = 0x1089F8; // VA 0x5089F8
constexpr uintptr_t R_RoutesNordland = 0x0F7EE8; // VA 0x4F7EE8

// ---- 入口：OnCommand 跳表 ----
constexpr uintptr_t R_CmdJmpTable = 0xD62BD;  // VA 0x4D62BD，索引 = ctlId - 5000
constexpr uintptr_t R_CmdDefault  = 0xD62B4;  // VA 0x4D62B4，slot[6] 当前值（default 出口）
constexpr uintptr_t R_SwitchThunk = 0xD6238;  // VA 0x4D6238：mov ecx,[0x56967C]; call SwitchScreen
constexpr int       kAsgardSlot   = 6;        // ctlId 5006 (0x138E)

// ---- hook：SwitchScreen 入口（6 字节：56 57 8B 7C 24 0C）----
constexpr uintptr_t R_SwitchScreen    = 0xD1E45; // VA 0x4D1E45
constexpr uintptr_t R_SwitchScreenRet = 0xD1E4B; // VA 0x4D1E4B（mov esi,ecx）

// ---- hook：start_campaign_N_screen 消费者尾部（5 字节：5F B0 01 5E C3）----
constexpr uintptr_t R_Cons1Tail = 0x0223B;   // VA 0x40223B

// ---- 全局槽位与函数 ----
constexpr uintptr_t R_CmdMgrSlot  = 0x169990; // VA 0x569990 命令管理器指针槽
constexpr uintptr_t R_ScreenSlot  = 0x16967C; // VA 0x56967C 主菜单对象指针槽
constexpr uintptr_t R_CmdCheck    = 0x0E0E4B; // VA 0x4E0E4B __thiscall char (mgr, const char*)
constexpr uintptr_t R_CmdClear    = 0x0E0EDB; // VA 0x4E0EDB __thiscall void (mgr, const char*)
constexpr uintptr_t R_SwitchCall  = 0xD1E45;  // VA 0x4D1E45 __thiscall void (mm, int screen, int arg)
constexpr uintptr_t R_CmdName1    = 0x108740; // VA 0x508740 "start_campaign_1_screen"
constexpr uintptr_t R_ProgressSlot = 0x111690; // VA 0x511690 战役进度对象指针槽
constexpr uintptr_t R_AddUnlock    = 0x016D62; // VA 0x416D62 AddUnlock(progress, camp, node, flag)

// ===================================================================
// 文化II 数据表（驻留本 DLL）
//   来源：原版 Gates of Asgard / Cultures2.exe @ file offset 0x0FA6E0
//   路线帧号对应 35 帧版 ls_menu_logos.bmd 的 0-based 26..34
//   注：campaign_01_09 官方未做地图，故无 nodeId 90；起点 10 不需要入线。
// ===================================================================
#pragma pack(push, 4)
struct MapNode  { int id;    int x;        int y; };   // 12 字节，id==0 终止
struct RouteEnt { int mapId; int frameIdx;        };   //  8 字节，mapId==0 终止
#pragma pack(pop)

static const MapNode kNodesC2[] = {
    {  10, 290, 197 },
    {  20, 372, 276 },
    {  30, 335, 270 },
    {  40, 418, 356 },
    {  50, 479, 342 },
    {  60, 501, 382 },
    {  70, 497, 355 },
    {  80, 530, 421 },
    { 100, 570, 386 },
    { 110, 438, 406 },
    {   0,   0,   0 },   // terminator
};

static const RouteEnt kRoutesC2[] = {
    {  20, 26 }, {  30, 27 }, {  40, 28 }, {  50, 29 }, {  60, 30 },
    {  70, 31 }, {  80, 32 }, { 100, 33 }, { 110, 34 },
    {   0,  0 },         // terminator
};

// ===================================================================
// 运行时状态
// ===================================================================
static DWORD         g_base      = 0;
static unsigned char g_pendingC2 = 0;   // 下一次进入 screen 5 是否用文化II
static unsigned char g_activeC2  = 0;   // 当前 screen 5 生效的模式（仅日志/调试）

// stub 引用的间接跳转目标（免手算 rel32）
static void* p_onSwitch     = nullptr;  // &OnSwitchScreen
static void* p_switchRet    = nullptr;  // 0x4D1E4B
static void* p_switchThunk  = nullptr;  // 0x4D6238
static void* p_switchScreen = nullptr;  // 0x4D1E45
static void* p_cmdMgrSlot   = nullptr;  // &(0x569990)
static void* p_screenSlot   = nullptr;  // &(0x56967C)
static void* p_cmdCheck     = nullptr;  // 0x4E0E4B
static void* p_cmdClear     = nullptr;  // 0x4E0EDB
static void* p_cmdName1     = nullptr;  // 0x508740（值 = 字符串地址）
static void* p_progressSlot = nullptr;  // &(0x511690) 战役进度对象（解锁表持有者）
static void* p_addUnlock    = nullptr;  // 0x416D62 AddUnlock(this=progress, camp, node, flag)

// 文化II 全部关卡节点（一次性解锁，首次进入大地图时填充解锁表）
static const int kUnlockAll[] = { 10, 20, 30, 40, 50, 60, 70, 80, 100, 110 };

static unsigned char g_unlockedOnce = 0;   // 避免每次进屏重复写档

// ===================================================================
// ApplyMode —— 战役上下文交换：把 7 处立即数切到目标战役
// ===================================================================
static void ApplyMode(bool c2) {
    const DWORD b = g_base;

    const DWORD nodes  = c2 ? (DWORD)(uintptr_t)kNodesC2
                            : (DWORD)(b + R_NodesNordland);
    const DWORD routes = c2 ? (DWORD)(uintptr_t)kRoutesC2
                            : (DWORD)(b + R_RoutesNordland);
    const uint8_t cid  = c2 ? 1 : 2;

    // handler
    Patch::WriteU8 (b + R_HdlCampaign + 2, cid);
    Patch::WriteU32(b + R_HdlNodeA    + 2, nodes);
    Patch::WriteU32(b + R_HdlNodeB    + 1, nodes);
    Patch::WriteU32(b + R_HdlNodeC    + 2, nodes + 4);   // ->x
    Patch::WriteU32(b + R_HdlNodeD    + 2, nodes + 8);   // ->y
    // painter
    Patch::WriteU32(b + R_PntRoute    + 1, routes);
    Patch::WriteU8 (b + R_PntCampaign + 1, cid);
}

// 由 SwitchScreenStub 调用：进入 screen 5 前决定用哪套战役数据
//
// 模式必须是「粘性」的：g_pendingC2 一旦由入口（Asgard 按钮 / C2 通关消费者）
// 置 1，就保持到「离开 screen 5」为止。绝不能在第一次进入后清空 ——
// 否则后续任意一次 SwitchScreen(5)（关卡返回、UI 重建等）都会以 g_pendingC2=0
// 重新套用北国风云的 7 处立即数，把刚切好的文化II 数据覆盖回去，
// 表现就是「点了文化II、看到的却还是北国风云3代」。
static void __cdecl OnSwitchScreen(int screen) {
    if (screen == 5) {
        bool c2 = (g_pendingC2 != 0);
        ApplyMode(c2);
        g_activeC2 = c2 ? 1 : 0;

        // ---- 文化II 解锁表初始化（关键！）----
        // 引擎整张程序没有任何路径解锁 campaign 1（文化II 从未被官方激活过）。
        // 而 screen 5 handler 用 IsUnlocked(1, node) 决定按钮是否创建，
        // 不初始化就会整个大地图白屏。
        // 这里只解锁第一关 node 10，后续关卡交由正常通关流程逐步解锁
        // （与原生战役一致），避免「一进就全开」。
        if (c2 && !g_unlockedOnce) {
            void* prog = p_progressSlot ? *(void**)p_progressSlot : nullptr;
            if (prog) {
                typedef void (__thiscall* AddUnlock_t)(void* th, int camp, int node, int flag);
                AddUnlock_t fn = (AddUnlock_t)p_addUnlock;
                fn(prog, 1, 10, 1);   // 仅首关
                g_unlockedOnce = 1;
                LOG_INFO(kCat, "seeded campaign 1 first node (10)");
            } else {
                LOG_WARN(kCat, "progress object not ready, skip C2 unlock seed");
            }
        }
    } else {
        // 离开 screen 5：复位为北国风云默认模式，
        // 这样点「北国风云III」按钮（ctl 5011）再次进入 screen 5 时不会残留文化II。
        g_pendingC2 = 0;
    }
    // 注意：screen==5 时不再清除 g_pendingC2（粘性），保证每次重建都套用正确数据。
}

// ===================================================================
// SwitchScreenStub —— hook MainMenu_SwitchScreen(0x4D1E45) 入口
//   进入时栈：[esp]=返回地址 [esp+4]=screen [esp+8]=arg，ecx=主菜单对象
//   必须完整保留所有寄存器与标志位，再重放被覆盖的 6 字节。
// ===================================================================
__declspec(naked) void SwitchScreenStub() {
    __asm {
        pushad                              // -32
        pushfd                              // -4  => 原 [esp+4] 现在在 [esp+0x28]
        mov  eax, dword ptr [esp + 0x28]    // screen
        push eax
        call dword ptr [p_onSwitch]
        add  esp, 4
        popfd
        popad

        // ---- 重放被 E9 覆盖的原指令（56 57 8B 7C 24 0C）----
        push esi
        push edi
        mov  edi, dword ptr [esp + 0x0C]
        jmp  dword ptr [p_switchRet]        // 0x4D1E4B
    }
}

// ===================================================================
// CmdAsgardStub —— OnCommand 跳表 slot[6]（ctlId 5006）的目标
//   等价于原版战役按钮的 `push 0; push <screen>; jmp 0x4D6238`，
//   只是先立起「下一次进 screen 5 用文化II」的标志。
// ===================================================================
__declspec(naked) void CmdAsgardStub() {
    __asm {
        mov  byte ptr [g_pendingC2], 1
        push 0                              // arg
        push 5                              // screen
        jmp  dword ptr [p_switchThunk]      // 0x4D6238
    }
}

// ===================================================================
// Campaign1TailStub —— hook sub_402122 尾部 0x40223B
//   补上 start_campaign_1_screen 的消费分支（原版只有 2/3/4）。
//   到达此处时 esi/edi 仍是原函数入口压栈的，必须原样 pop 并返回 al=1。
// ===================================================================
__declspec(naked) void Campaign1TailStub() {
    __asm {
        mov  ecx, dword ptr [p_cmdMgrSlot]
        mov  ecx, dword ptr [ecx]           // ecx = 命令管理器
        push dword ptr [p_cmdName1]
        call dword ptr [p_cmdCheck]         // char CommandCheck(mgr, name)  (callee-clean)
        test al, al
        je   L_done

        mov  byte ptr [g_pendingC2], 1
        mov  ecx, dword ptr [p_screenSlot]
        mov  ecx, dword ptr [ecx]           // ecx = 主菜单对象
        push 0                              // arg
        push 5                              // screen
        call dword ptr [p_switchScreen]     // SwitchScreen(mm, 5, 0)

        mov  ecx, dword ptr [p_cmdMgrSlot]
        mov  ecx, dword ptr [ecx]
        push dword ptr [p_cmdName1]
        call dword ptr [p_cmdClear]         // CommandClear(mgr, name)

    L_done:
        // ---- 重放被覆盖的原尾部（5F B0 01 5E C3）----
        pop  edi
        mov  al, 1
        pop  esi
        ret
    }
}

// ===================================================================
// 安装
// ===================================================================
static bool Verify(DWORD base, uintptr_t rva, const uint8_t* exp, size_t n, const char* what) {
    std::vector<uint8_t> cur = Patch::ReadBytes(base + rva, n);
    if (cur.size() == n && memcmp(cur.data(), exp, n) == 0) return true;
    LOG_ERROR(kCat, "verify FAILED: %s @0x%X", what, (unsigned)(base + rva));
    return false;
}

class Cultures2CampaignFeature : public Feature {
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }

    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        if (!cfg.GetBool(kName, "Enabled", false)) {
            LOG_INFO(kCat, "Disabled (Enabled=0)");
            return true;
        }
        gameapi::Init(ver.GetBaseAddress());
        const DWORD b = ver.GetBaseAddress();
        g_base = b;

        // ---------------- 1) 校验全部落点的原始字节 ----------------
        static const uint8_t vHdlCampaign[] = { 0x83, 0xF9, 0x02 };
        static const uint8_t vHdlNodeA[]    = { 0x8B, 0x0D, 0xF8, 0x89, 0x50, 0x00 };
        static const uint8_t vHdlNodeB[]    = { 0xBF, 0xF8, 0x89, 0x50, 0x00 };
        static const uint8_t vHdlNodeC[]    = { 0x8B, 0xB8, 0xFC, 0x89, 0x50, 0x00 };
        static const uint8_t vHdlNodeD[]    = { 0x8B, 0x88, 0x00, 0x8A, 0x50, 0x00 };
        static const uint8_t vPntRoute[]    = { 0xB8, 0xE8, 0x7E, 0x4F, 0x00 };
        static const uint8_t vPntCampaign[] = { 0x6A, 0x02 };
        static const uint8_t vSwitch[]      = { 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C };
        static const uint8_t vCons1Tail[]   = { 0x5F, 0xB0, 0x01, 0x5E, 0xC3 };

        if (!Verify(b, R_HdlCampaign, vHdlCampaign, sizeof(vHdlCampaign), "handler cmp campaignId")) return false;
        if (!Verify(b, R_HdlNodeA,    vHdlNodeA,    sizeof(vHdlNodeA),    "handler node ref A"))     return false;
        if (!Verify(b, R_HdlNodeB,    vHdlNodeB,    sizeof(vHdlNodeB),    "handler node ref B"))     return false;
        if (!Verify(b, R_HdlNodeC,    vHdlNodeC,    sizeof(vHdlNodeC),    "handler node ref C"))     return false;
        if (!Verify(b, R_HdlNodeD,    vHdlNodeD,    sizeof(vHdlNodeD),    "handler node ref D"))     return false;
        if (!Verify(b, R_PntRoute,    vPntRoute,    sizeof(vPntRoute),    "painter route base"))     return false;
        if (!Verify(b, R_PntCampaign, vPntCampaign, sizeof(vPntCampaign), "painter campaignId"))     return false;
        if (!Verify(b, R_SwitchScreen, vSwitch,     sizeof(vSwitch),      "SwitchScreen prologue"))  return false;
        if (!Verify(b, R_Cons1Tail,   vCons1Tail,   sizeof(vCons1Tail),   "campaign consumer tail")) return false;

        // 跳表 slot[6] 必须仍是 default 出口（确认 ctl 5006 未被占用）
        DWORD slot6 = 0;
        if (!Patch::ReadMemory(b + R_CmdJmpTable + kAsgardSlot * 4, &slot6, 4)) {
            LOG_ERROR(kCat, "read cmd jumptable slot[6] failed");
            return false;
        }
        if (slot6 != (DWORD)(b + R_CmdDefault)) {
            LOG_ERROR(kCat, "cmd jumptable slot[6] not free: 0x%X (expect 0x%X)",
                      slot6, (unsigned)(b + R_CmdDefault));
            return false;
        }

        // ---------------- 2) 绑定 stub 的间接跳转目标 ----------------
        p_onSwitch     = (void*)&OnSwitchScreen;
        p_switchRet    = (void*)(b + R_SwitchScreenRet);
        p_switchThunk  = (void*)(b + R_SwitchThunk);
        p_switchScreen = (void*)(b + R_SwitchCall);
        p_cmdMgrSlot   = (void*)(b + R_CmdMgrSlot);
        p_screenSlot   = (void*)(b + R_ScreenSlot);
        p_cmdCheck     = (void*)(b + R_CmdCheck);
        p_cmdClear     = (void*)(b + R_CmdClear);
        p_cmdName1     = (void*)(b + R_CmdName1);
        p_progressSlot = (void*)(b + R_ProgressSlot);
        p_addUnlock    = (void*)(b + R_AddUnlock);

        // ---------------- 3) 装 hook ----------------
        // 3a) SwitchScreen 入口：6 字节 -> E9 rel32 + 1 NOP
        if (!Patch::WriteJmp(b + R_SwitchScreen, (uintptr_t)&SwitchScreenStub, 1)) {
            LOG_ERROR(kCat, "hook SwitchScreen failed");
            return false;
        }
        // 3b) OnCommand 跳表 slot[6] -> 文化II 入口 stub
        if (!Patch::WriteU32(b + R_CmdJmpTable + kAsgardSlot * 4,
                             (uint32_t)(uintptr_t)&CmdAsgardStub)) {
            LOG_ERROR(kCat, "patch cmd jumptable slot[6] failed");
            return false;
        }
        // 3c) start_campaign_1_screen 消费分支
        if (!Patch::WriteJmp(b + R_Cons1Tail, (uintptr_t)&Campaign1TailStub, 0)) {
            LOG_ERROR(kCat, "hook campaign consumer tail failed");
            return false;
        }

        LOG_INFO(kCat, "installed: SwitchScreen hook@0x%X stub=0x%X",
                 (unsigned)(b + R_SwitchScreen), (unsigned)(uintptr_t)&SwitchScreenStub);
        LOG_INFO(kCat, "  cmd slot[6] (ctl 5006) -> 0x%X", (unsigned)(uintptr_t)&CmdAsgardStub);
        LOG_INFO(kCat, "  campaign1 consumer @0x%X -> 0x%X",
                 (unsigned)(b + R_Cons1Tail), (unsigned)(uintptr_t)&Campaign1TailStub);
        LOG_INFO(kCat, "  C2 nodes=0x%X (%d) routes=0x%X (%d), frames 26..34",
                 (unsigned)(uintptr_t)kNodesC2,  (int)(sizeof(kNodesC2)  / sizeof(kNodesC2[0])  - 1),
                 (unsigned)(uintptr_t)kRoutesC2, (int)(sizeof(kRoutesC2) / sizeof(kRoutesC2[0]) - 1));
        return true;
    }
};

REGISTER_FEATURE(Cultures2CampaignFeature)

} // namespace fe_c2
