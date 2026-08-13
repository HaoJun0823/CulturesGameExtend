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
// GameApi.h
// ===================================================================
// All game functions imported from game.exe are declared centrally here,
// for ease of maintenance and extension.
// No Feature may scatter its own game function pointer declarations; all must go through this file.
//
// Usage:
//   1. In any Feature's OnInstall, first call gameapi::Init(ver.GetBaseAddress());
//      (may be called repeatedly; idempotent)
//   2. Then simply call gameapi::Xxx(...) directly.
//
// Adding a new game function (follow this when you need to call more game functions later):
//   1. In the "Key Addresses" section below, add an addr:: constant (VA = address under the 0x400000 base);
//   2. In the "Function Pointers" section, add the typedef + extern pointer;
//   3. In GameApi.cpp's s_bindings table, add a line { RVA, (void**)&pointer }.
// ===================================================================
#pragma once
#include <cstdint>


namespace gameapi {

// 运行时镜像基址（由 Init 设置；游戏无 ASLR，通常恒为 0x400000）

// Runtime image base (set by Init; game has no ASLR, usually constant at 0x400000)
extern uintptr_t g_imageBase;

// 用实际基址绑定全部函数指针（可重复调用）

// Bind all function pointers using the actual base (may be called repeatedly)
void Init(uintptr_t imageBase);

// VA（0x400000 基址下的地址）-> 运行时地址

// VA (address under the 0x400000 base) -> runtime address
template <typename T = void*>
inline T Va(uintptr_t va) {
    return (T)(g_imageBase + (va - 0x400000));
}

// ---- 关键地址（0x400000 基址下的 VA）----

#pragma region Key addresses
// ---- Key addresses (VAs under the 0x400000 base) ----
namespace addr {
    // CampaignStaticDataManager（战役静态数据管理器，单例 g_pCampaignStaticDataMgr）
    // CampaignStaticDataManager (campaign static data manager, singleton g_pCampaignStaticDataMgr)
    constexpr uintptr_t L_IO_Load       = 0x410B58;
// 枚举加载全部地图/战役（data\maps + campaign00/01 c2m）
// Loads all maps/campaigns in the enumeration (data\maps + campaign00/01 c2m)
    constexpr uintptr_t LoadCampaignMap = 0x410E6D;
    constexpr uintptr_t IniFile_Open    = 0x424EF8;
// 解析单个地图（c2m 挂载包 / 文件夹 map.ini）并注册进列表
// Parses a single map (c2m mount package / folder map.ini) and registers it into the list
    constexpr uintptr_t IniFile_Close   = 0x425072;
    // IniFile（游戏二进制 INI 解析器）
    // IniFile (game's binary INI parser)
    constexpr uintptr_t g_pCampaignStaticDataMgr = 0x510BFC;
}
    // 全局单例
    // Global singletons
typedef void (__thiscall* IniFile_OpenFn)(void* self, const char* path, int a2, int a3, int a4, int a5);

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

    // ---- Reversed functions to be wired in on demand (fill in as needed) ----
    // MainMenuUI_Build          = 0x4D24BB; // Main menu UI builder (28-case state machine)
    // UI_CreateControl          = 0x41024E; // UI control creation primitive (x,y,w,h)
    // UI_InitControl            = 0x41026E;
    // UI_CreateButton           = 0x4B7C2E; // Button creation (this + text + control)
    // UI_AddButton              = 0x4768DA;
    // StringTable_GetText       = 0x4E15B3; // (table index, ID) -> text; returns "<i:id:NOT DEFINED!>" if not found
    // StringTable_GetMainMenuText = 0x4E15F6; // table 0 mainmenu.ini
    // StringTable_GetIngameGuiMainText = 0x4E1604; // table 1 ingameguimain.ini
    // StringTable_GetOdinText   = 0x4E1682; // table 11 odin001.ini
    // StringTable_GetSagaText   = 0x4E169E; // table 13 saga001.ini
typedef void (__thiscall* IniFile_CloseFn)(void* self);

// ---- 游戏函数指针（Init 后可用）----
// 命名约定：typedef 加 Fn 后缀；指针变量名与游戏函数同名。

// IniFile（__thiscall；self 为配置对象缓冲，须与游戏一致，如 int[1572]）
#pragma endregion

#pragma region Function pointers
// ---- Game function pointers (available after Init) ----
// Naming convention: typedefs get an Fn suffix; pointer variable names match the game function.

// IniFile (__thiscall; self is the config object buffer, must match the game, e.g. int[1572])
extern IniFile_OpenFn  IniFile_Open;
extern IniFile_CloseFn IniFile_Close;
typedef char (__thiscall* LoadCampaignMapFn)(void* mgr, void* cfg, const char* src, char a4, const char* a5, unsigned int a6);
extern LoadCampaignMapFn LoadCampaignMap;

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

// CampaignStaticDataManager::LoadCampaignMap (__thiscall)
//   this = CampaignStaticDataManager instance = *(DWORD*)Va(g_pCampaignStaticDataMgr)
//          (Note: not cfg! Assembly 0x410BFC mov ecx,[ebp+var_8] confirms this)
//   a2   = IniFile config object (must already be opened by IniFile_Open)
//   src  = map content root directory (Source mode; on map entry the game reads <src>\map.ini + map.dat.
//          Matches l_IO_Load's built-in map loop: Source = "data\maps\<dir>".
//          c2m mode passes nullptr)
//   a4   = 1 = c2m/currentusermap mode (src passes nullptr, a5 passes the c2m file name);
//          0 = Source directory mode (used for folder-form maps)
//   a5   = c2m file name (valid when a4=1; folder form passes nullptr)
//   a6   = campaign ID (campaign00=7, campaign01=8; must be < 9, see 0x411036 cmp eax,9)
}
#pragma endregion
