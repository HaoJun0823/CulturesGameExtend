// GameApi.h
// ===================================================================
// 所有从 game.exe 导入的游戏函数统一集中声明于此，便于维护与扩展。
// 任何 Feature 不得自行散落声明游戏函数指针，一律走本文件。
//
// 使用方式：
//   1. 进程内任意 Feature 的 OnInstall 里先调用 gameapi::Init(ver.GetBaseAddress());
//      （可重复调用，幂等）
//   2. 之后直接调用 gameapi::Xxx(...) 即可。
//
// 新增一个游戏函数（以后要 call 更多游戏函数时照此办理）：
//   1. 在下方 "关键地址" 区补 addr::常量（VA = 0x400000 基址下的地址）；
//   2. 在 "函数指针" 区补 typedef + extern 指针；
//   3. 在 GameApi.cpp 的 s_bindings 绑定表加一行 { RVA, (void**)&指针 }。
// ===================================================================
#pragma once
#include <cstdint>

namespace gameapi {

// 运行时镜像基址（由 Init 设置；游戏无 ASLR，通常恒为 0x400000）
extern uintptr_t g_imageBase;

// 用实际基址绑定全部函数指针（可重复调用）
void Init(uintptr_t imageBase);

// VA（0x400000 基址下的地址）-> 运行时地址
template <typename T = void*>
inline T Va(uintptr_t va) {
    return (T)(g_imageBase + (va - 0x400000));
}

// ---- 关键地址（0x400000 基址下的 VA）----
namespace addr {
    // CampaignStaticDataManager（战役静态数据管理器，单例 g_pCampaignStaticDataMgr）
    constexpr uintptr_t L_IO_Load       = 0x410B58; // 枚举加载全部地图/战役（data\maps + campaign00/01 c2m）
    constexpr uintptr_t LoadCampaignMap = 0x410E6D; // 解析单个地图（c2m 挂载包 / 文件夹 map.ini）并注册进列表
    // IniFile（游戏二进制 INI 解析器）
    constexpr uintptr_t IniFile_Open    = 0x424EF8;
    constexpr uintptr_t IniFile_Close   = 0x425072;
    // 全局单例
    constexpr uintptr_t g_pCampaignStaticDataMgr = 0x510BFC;

    // ---- 已逆向、后续按需接入的常用函数（随用随补）----
    // MainMenuUI_Build          = 0x4D24BB; // 主菜单 UI 构建器（28 case 状态机）
    // UI_CreateControl          = 0x41024E; // UI 控件创建基元 (x,y,w,h)
    // UI_InitControl            = 0x41026E;
    // UI_CreateButton           = 0x4B7C2E; // 按钮创建（this + 文本 + 控件）
    // UI_AddButton              = 0x4768DA;
    // StringTable_GetText       = 0x4E15B3; // (表索引, ID) -> 文本；找不到返回 "<i:id:NOT DEFINED!>"
    // StringTable_GetMainMenuText = 0x4E15F6; // 表0 mainmenu.ini
    // StringTable_GetIngameGuiMainText = 0x4E1604; // 表1 ingameguimain.ini
    // StringTable_GetOdinText   = 0x4E1682; // 表11 odin001.ini
    // StringTable_GetSagaText   = 0x4E169E; // 表13 saga001.ini
}

// ---- 游戏函数指针（Init 后可用）----
// 命名约定：typedef 加 Fn 后缀；指针变量名与游戏函数同名。

// IniFile（__thiscall；self 为配置对象缓冲，须与游戏一致，如 int[1572]）
typedef void (__thiscall* IniFile_OpenFn)(void* self, const char* path, int a2, int a3, int a4, int a5);
typedef void (__thiscall* IniFile_CloseFn)(void* self);
extern IniFile_OpenFn  IniFile_Open;
extern IniFile_CloseFn IniFile_Close;

// CampaignStaticDataManager::LoadCampaignMap（__thiscall）
//   this = CampaignStaticDataManager 实例 = *(DWORD*)Va(g_pCampaignStaticDataMgr)
//          （注意：不是 cfg！汇编 0x410BFC mov ecx,[ebp+var_8] 证实）
//   a2   = IniFile 配置对象（须已由 IniFile_Open 打开）
//   src  = 地图内容根目录（Source 模式；进图时游戏读 <src>\map.ini + map.dat。
//          与 l_IO_Load 内置地图循环一致：Source = "data\maps\<dir>"。
//          c2m 模式传 nullptr）
//   a4   = 1 = c2m/currentusermap 模式（src 传 nullptr，a5 传 c2m 文件名）；
//          0 = Source 目录模式（文件夹形式地图用这个）
//   a5   = c2m 文件名（a4=1 时有效；文件夹形式传 nullptr）
//   a6   = 战役 ID（campaign00=7, campaign01=8；必须 < 9，见 0x411036 cmp eax,9）
typedef char (__thiscall* LoadCampaignMapFn)(void* mgr, void* cfg, const char* src, char a4, const char* a5, unsigned int a6);
extern LoadCampaignMapFn LoadCampaignMap;

} // namespace gameapi
