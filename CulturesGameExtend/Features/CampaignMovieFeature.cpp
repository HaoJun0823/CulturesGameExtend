// CampaignMovieFeature.cpp
// ===================================================================
// [CampaignMovie]
// 让文化II（campaign 1）进入首关（node 10）前播放开场电影 intro00.mpg。
//
// 背景（逆向结论）：
//   开场电影由 sub_40306E (0x40306E) 决定并就地播放（其唯一调用点
//   0x40235D）。该函数把"当前地图"的 campaignId / nodeId 与硬编码字面串
//   对表：
//     campaign 2 + node 10 -> "intro01" (北国风云)
//     campaign 3 + node 10 -> "intro02" (第八世界奇迹)
//     campaign 4 + node 10 -> "intro03" (saga)
//   **唯独缺 campaign 1（文化II）的分支** —— 原版 exe 里根本没有
//   "intro00" 字面串，文化II 从未被官方激活过开场电影。
//
//   函数体流程（已反汇编确认 0x40306E..0x403226）：
//     - 入口块（0x40309A..0x4030D1）：用全局 [0x569990]=当前地图对象，
//       调用 0x4e0e4b/0x4e0e38 查该地图是否定义了开场电影（intro 名在
//       地图容器 [mapobj+4] 里按 key 0x4fe49c 检索，命中则 buf[0]=1）。
//       **这是正常游玩时其它战役播 intro 的真实通道，且 +0x2c 恒为 0**，
//       所以 "+0x2c==1 才播" 的旧假设是错的——正常工作（3代等）也 mode=0。
//     - +0x2c==1 的硬编码分支（0x4030DE..0x403184）：仅当 +0x2c==1 时把
//       edi 装载为地图记录（0x41086f(g_pCampaignStaticDataMgr,
//       g_pGameStartRequest+0x30)）并匹配 campaign 2/3/4。正常游玩该分支
//       不进（mode 恒 0）。
//     - 收敛点 0x403184：cmp byte [esi],bl ; je 0x403226。注意：在 mode=0
//       路径下 edi = 0x4fe49c（一个字符串字面量），**不是地图指针**；
//       只有 +0x2c==1 路径才把 edi 设成地图记录。因此 stub 绝不能读收敛点
//       的 edi。
//       [0x569990] 实为**命令管理器**（通道1 与播放块 0x4031E6 的 this），
//       不是"当前地图对象"；真正含 campaign/node 的是地图记录
//       （0x41086f(g_pCampaignStaticDataMgr, &g_pGameStartRequest+0x30) 返回，
//       +0x124=campaign / +0x128=node）。此查询 __thiscall + retn 4（被调方清栈）。
//   方案：把 0x403184 这 8 字节改写为 E9->IntroC2Stub(+3 NOP)。stub 先复刻
//   原收敛（buf[0]!=0 直接播），buf[0]==0 时调 findmap 判 campaign==1 &&
//   node==10，命中则 strcpy(buf+1,"intro00") 并跳播放块 0x40318C。
//   所有全局解引用均有 NULL 防护（找不到记录 -> 不播，绝不崩）。
//   注意：_strcpy(0x4E5400) 是标准 cdecl，**先 push Source 后 push Destination**
//   （原版 0x403127/0x4030BB 两处调用点同构）；顺序写反会往只读内存写。
//
// 方案：把 0x403184 处这 8 字节（cmp/je）改写为 E9 -> 本 DLL 的
// IntroC2Stub（+3 NOP 补齐），在 stub 里补上 campaign 1 的分支：
//   - campaignId==1 && nodeId==10  -> strcpy(buf+1,"intro00"); buf[0]=1;
//     然后跳播放块 0x40318C；
//   - 否则完全复刻原 cmp/je：buf 空跳 0x403226，否则跳 0x40318C。
// 这样对所有其它战役行为零改动（它们的 intro 仍由原三个分支处理），
// 且文化II 即便地图的 bit1 没置位也会被补上开场（开放性强于原版）。
//
// "intro00" 字面串由本 DLL 提供（exe 无此串）。播放路径沿用引擎原有
// 逻辑：datax\FMV\%%s\%s.mpg -> datax\FMV\Eng\intro00.mpg（磁盘已存在
// intro00.mpg）。
//
// 结尾电影（seq_0000.mpg）：引擎在 sub_41FB7A 中用每战役记录字段
// (wsprintf(..,"seq_%4.4d",[esi+4])) 拼装，文化II 的电影 ID 已被改为 0，
// 故结尾无需代码补丁，数据层已解决。
//
// 配置（plugins/config/CulturesGameExtend_Game.ini）：
//   [CampaignMovie]
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
#include <vector>

namespace fe_movie {

const char* kName = "CampaignMovie";
const char* kCat  = "[CampaignMovie]";

// ---- hook 点（RVA = VA - 0x400000）----
constexpr uintptr_t R_IntroConv = 0x03184;   // 0x403184  cmp [esi],bl ; je 0x403226 (8 字节)
constexpr uintptr_t R_PlayBlk  = 0x0318C;   // 0x40318C  播放块起点
constexpr uintptr_t R_SkipBlk  = 0x03226;   // 0x403226  不播分支
constexpr uintptr_t R_StrCpy   = 0x0E5400;  // 0x4E5400  strcpy(dst,src)，原版用于拷 intro 串
constexpr uintptr_t R_GameStart = 0x10F804; // 0x50F804  g_pGameStartRequest（RVA=VA-0x400000=0x10F804）；+0x2c = 开场触发标志
constexpr uintptr_t R_Mgr       = 0x110BFC; // 0x510BFC  g_pCampaignStaticDataMgr 指针变量
constexpr uintptr_t R_FindMap  = 0x01086F; // 0x41086F  map 记录查询(__thiscall: ecx=mgr, arg=&req+0x30)
                                           // 注意：这里是 RVA！曾误写 VA 0x041086F → p_findmap=0x81086F
                                           // 跳 image 外堆区 → AV 崩溃（2026-08-11 已修）

// ---- 校验原 8 字节 ----
static const uint8_t kConvBytes[8] = {
    0x38, 0x1e,             // cmp byte ptr [esi], bl
    0x0f, 0x84, 0x9a, 0x00, 0x00, 0x00  // je 0x403226
};

// ---- DLL 提供的字面串（exe 无 "intro00"）----
static const char kIntro00[] = "intro00";

// ---- 运行期绑定的间接跳转目标（免手算 rel32）----
static void* p_play   = nullptr;   // 0x40318C
static void* p_skip   = nullptr;   // 0x403226
static void* p_strcpy = nullptr;   // 0x4E5400
static void* p_gstart = nullptr;   // 0x50F804  g_pGameStartRequest 指针
static void* p_mgr    = nullptr;   // 0x510BFC  g_pCampaignStaticDataMgr 指针变量
static void* p_findmap= nullptr;   // 0x41086F  map 记录查询（__thiscall）
static void* p_dbg    = nullptr;   // DebugIntro 诊断函数地址

// ===================================================================
// 诊断：把 stub 到达时的真实上下文写日志。
// 关键修正：[0x569990] 是**命令管理器**（非"当前地图对象"），仅作诊断记录。
// 真正含 campaign/node 的是地图记录，由引擎 0x41086f(g_pCampaignStaticDataMgr,
// g_pGameStartRequest+0x30) 返回（+0x124=campaign / +0x128=node）。
// 诊断打印：buf[0] / mode(+0x2c) / 命令管理器 / findmap 得到的 camp/node/flag。
// 诊断由 [CampaignMovie] Debug 控制（默认开）。
// ===================================================================
static unsigned char g_dbgOn    = 1;
static unsigned      g_dbgCount = 0;
static void*         g_maprec   = nullptr;   // 0x41086f 返回的地图记录
static DWORD         g_dbgMode  = 0;         // 诊断用：g_pGameStartRequest->+0x2c（NULL 防护后）

extern "C" void __cdecl DebugIntro(DWORD buf0, DWORD mode, DWORD rawMapobj,
                                   DWORD fmcamp, DWORD fmnode, DWORD fmflag) {
    if (!g_dbgOn) return;
    if (g_dbgCount++ > 40) return;            // 防日志爆炸
    LOG_INFO(kCat, "[dbg] buf[0]=%u mode(+0x2c)=%u raw_mapobj=0x%X | findmap: camp=%u node=%u flags=0x%X bit1=%u",
             (unsigned)buf0, (unsigned)mode, (unsigned)rawMapobj,
             (unsigned)fmcamp, (unsigned)fmnode, (unsigned)fmflag, (unsigned)((fmflag >> 1) & 1));
}

// ===================================================================
// IntroC2Stub —— 接管 0x403184 收敛点
//   进入时: esi = buf, ebx = 0（sub_40306E 入口 xor ebx,ebx 不变式）
//   2026-08-11 hy3 复核修正：
//     - mode=0 时 edi = 0x4fe49c（字符串字面量），不是地图指针；绝不读 edi。
//     - [0x569990] 是命令管理器，不是"当前地图对象"；真正含 campaign/node
//       的是地图记录，由 0x41086f(g_pCampaignStaticDataMgr, &req+0x30) 返回。
//     - 所有全局解引用先 NULL 防护（req/mgr 任一为 0 -> 记录=0 -> 不播）。
//     - strcpy 压栈：先 Source 后 Destination（_strcpy 读 [esp+4]=Destination）。
//   出口: 绝不直接返回 0x403184（避免 E9 死循环），跳 0x40318C/0x403226
// ===================================================================
__declspec(naked) void IntroC2Stub() {
    __asm {
        // ---- 0) 诊断 + 解析真实地图记录（保存全部寄存器/标志）----
        pushad
        pushfd
        movzx edx, byte ptr [esi]          ; edx = buf[0]
        ; 防护读 g_pGameStartRequest->+0x2c (mode)
        xor  eax, eax                      ; mode 默认 0
        mov  ebp, dword ptr [p_gstart]     ; ebp = &g_pGameStartRequest（指针变量地址）
        test ebp, ebp
        jz   dbg_mode_ok
        mov  ebp, dword ptr [ebp]          ; ebp = 结构指针 S
        test ebp, ebp
        jz   dbg_mode_ok
        mov  eax, dword ptr [ebp + 0x2c]   ; eax = mode (+0x2c)
    dbg_mode_ok:
        mov  dword ptr [g_dbgMode], eax
        mov  edi, dword ptr [0x569990]     ; edi = 命令管理器（仅诊断记录）
        ; 用引擎查询真实地图记录：0x41086f(mgr, &S+0x30)，双 NULL 防护
        xor  ebp, ebp                      ; 默认 maprec = 0
        mov  ecx, dword ptr [p_mgr]        ; ecx = &g_pCampaignStaticDataMgr
        test ecx, ecx
        jz   dbg_fm_done
        mov  ecx, dword ptr [ecx]          ; ecx = mgr 对象（__thiscall this）
        test ecx, ecx
        jz   dbg_fm_done
        mov  ebx, dword ptr [p_gstart]
        test ebx, ebx
        jz   dbg_fm_done
        mov  ebx, dword ptr [ebx]          ; ebx = S
        test ebx, ebx
        jz   dbg_fm_done
        lea  ebx, [ebx + 0x30]             ; &S+0x30（地图定位子结构）
        push ebx
        call dword ptr [p_findmap]         ; eax = 地图记录；__thiscall 被调方清栈
        mov  ebp, eax
    dbg_fm_done:
        mov  dword ptr [g_maprec], ebp
        ; 读 findmap 结果字段（NULL 则全 0）
        xor  ecx, ecx                      ; fmcamp
        xor  ebx, ebx                      ; fmnode
        xor  esi, esi                      ; fmflag
        test ebp, ebp
        jz   dbg_call
        mov  ecx, dword ptr [ebp + 0x124]  ; campaignId
        mov  ebx, dword ptr [ebp + 0x128]  ; nodeId
        mov  esi, dword ptr [ebp + 0x120]  ; flags
    dbg_call:
        push esi                           ; fmflag
        push ebx                           ; fmnode
        push ecx                           ; fmcamp
        push edi                           ; 命令管理器
        push dword ptr [g_dbgMode]         ; mode
        push edx                           ; buf[0]
        call dword ptr [p_dbg]
        add  esp, 24
        popfd
        popad

        // ---- 1) 先复刻原收敛逻辑（buf[0]!=0 直接播，不碰地图）----
        cmp  byte ptr [esi], bl            ; bl == 0（入口 xor ebx,ebx 不变式）
        jne  PLAY

        // ---- 2) buf[0]==0：用引擎地图记录判文化II ----
        mov  edi, dword ptr [g_maprec]     ; 真实地图记录
        test edi, edi
        jz   SKIP                          ; 查询失败 -> 不播
        cmp  dword ptr [edi + 0x124], 1    ; campaignId == 1 (文化II)
        jne  SKIP
        cmp  dword ptr [edi + 0x128], 0x0a ; nodeId == 10 (首关)
        jne  SKIP

        // ---- 3) 命中文化II 首关：写入 intro00 ----
        ; _strcpy 读 [esp+4]=Destination：标准 cdecl 先压 Source 后压 Destination
        mov  byte ptr [esi], 1             ; buf[0] = 1
        lea  eax, [esi + 1]                ; dst = buf+1
        push offset kIntro00               ; Source（先压）
        push eax                           ; Destination（后压）
        call dword ptr [p_strcpy]
        add  esp, 8
    PLAY:
        jmp  dword ptr [p_play]            ; -> 播放块 0x40318C
    SKIP:
        jmp  dword ptr [p_skip]            ; -> 不播分支 0x403226
    }
}

static bool VerifyBytes(DWORD base, DWORD off, const uint8_t* exp, size_t n) {
    std::vector<uint8_t> cur = Patch::ReadBytes(base + off, n);
    if (cur.size() == n && memcmp(cur.data(), exp, n) == 0) return true;
    LOG_ERROR(kCat, "verify FAILED @0x%X (bytes mismatch)", base + off);
    return false;
}

class CampaignMovieFeature : public Feature {
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

        if (!VerifyBytes(b, R_IntroConv, kConvBytes, sizeof(kConvBytes))) {
            LOG_ERROR(kCat, "patch point @0x%X already modified or version mismatch",
                      b + R_IntroConv);
            return false;
        }

        // 绑定间接跳转目标
        p_play   = (void*)(b + R_PlayBlk);
        p_skip   = (void*)(b + R_SkipBlk);
        p_strcpy = (void*)(b + R_StrCpy);
        p_gstart = (void*)(b + R_GameStart);
        p_mgr    = (void*)(b + R_Mgr);       // g_pCampaignStaticDataMgr 指针变量的地址
        p_findmap= (void*)(b + R_FindMap);
        p_dbg    = (void*)&DebugIntro;

        // 8 字节区域 -> E9 + 3 NOP
        if (!Patch::WriteJmp(b + R_IntroConv, (uintptr_t)&IntroC2Stub, 3)) {
            LOG_ERROR(kCat, "hook WriteJmp failed @0x%X", b + R_IntroConv);
            return false;
        }

        LOG_INFO(kCat, "installed: intro convergence @0x%X -> stub 0x%X",
                 (unsigned)(b + R_IntroConv), (unsigned)(uintptr_t)&IntroC2Stub);
        LOG_INFO(kCat, "  campaign 1 + node 10 -> 'intro00' (Eng\\intro00.mpg)");
        LOG_INFO(kCat, "  play blk=0x%X skip blk=0x%X strcpy=0x%X gstart=0x%X",
                 (unsigned)(b + R_PlayBlk), (unsigned)(b + R_SkipBlk),
                 (unsigned)(b + R_StrCpy), (unsigned)(b + R_GameStart));
        g_dbgOn = cfg.GetBool(kName, "Debug", true) ? 1 : 0;
        LOG_INFO(kCat, "  diag=%s (stub logs runtime buf[0]/mode/campaign/node)",
                 g_dbgOn ? "on" : "off");
        return true;
    }
};

REGISTER_FEATURE(CampaignMovieFeature)

} // namespace fe_movie
