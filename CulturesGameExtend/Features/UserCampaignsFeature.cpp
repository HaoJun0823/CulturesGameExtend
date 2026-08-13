// UserCampaignsFeature.cpp
// ===================================================================
// [UserCampaigns]
// 允许 UserCampaign 目录以"文件夹形式"加载地图，与 *.c2m 打包文件等价：
//   datax\usercampaigns\campaign00\<地图文件夹>\map.ini        （A：直接布局）
//   datax\usercampaigns\campaign00\<地图文件夹>\currentusermap\map.ini （B：c2m 解包布局）
//
// 原理：游戏原生支持文件夹形式地图（data\maps\<dir>\map.ini 就是），但
// CampaignStaticDataManager::l_IO_Load (0x410B58) 对 campaign00/01 只枚举
// *.c2m 文件、不扫子目录。本 Feature 在战役管理器初始化完成后，复用游戏
// 自身的 IniFile_Open + LoadCampaignMap（见 Core/GameApi.h），把文件夹地图
// 注册进同一张战役列表 —— 不修改任何游戏二进制。
//
// 关键逆向事实（决定本实现的调用方式）：
//   * LoadCampaignMap (0x410E6D) 是 __thiscall，this = CampaignStaticDataManager
//     实例（= *(DWORD*)g_pCampaignStaticDataMgr），不是 IniFile 配置对象。
//     汇编证实：l_IO_Load 内置地图循环 0x410BF6 处 mov ecx,[ebp+var_8]。
//   * 文件夹形式地图 = Source 模式：src = 地图内容根目录（进图时游戏读
//     <src>\map.ini + map.dat），a4=0。与内置地图循环逐字节一致。
//   * c2m 形式 = a4=1 模式（src=nullptr, a5=c2m 文件名），依赖挂载包，
//     文件夹形式不要用。
//
// 配置（plugins/config/CulturesGameExtend_Game.ini 或 _Global.ini）：
//   [UserCampaigns]
//   Enabled = 1
//   ; allowcurrentusermapfolder=1 时，额外扫描 <地图文件夹>\currentusermap\map.ini
//   ; （c2m 解包布局，如 Campaign00\01_Ein_neuer_Anfang\currentusermap\map.ini）。
//   ; 默认(0)只扫目录根 map.ini（官方 data\maps\<dir> 布局）。
//   AllowCurrentUserMapFolder = 1
//   ; 可选：额外战役目录（相对 datax\usercampaigns\），name=战役ID;...
//   ; 注意：战役 ID 必须 < 9（游戏 LoadCampaignMap 的槽位上限，campaign00/01
//   ; 占 7/8；若要 9+ 需另行 patch 0x411036 的 cmp eax,9）。
//   ; CampaignDirs = campaign02=9;campaign03=10
// ===================================================================
// UserCampaignsFeature.cpp
// ===================================================================
// [UserCampaigns]
// Allow the UserCampaign directory to load maps in "folder form", equivalent to *.c2m package files:
//   datax\usercampaigns\campaign00\<map folder>\map.ini         (A: direct layout)
//   datax\usercampaigns\campaign00\<map folder>\currentusermap\map.ini (B: c2m-extracted layout)
//
// Principle: the game natively supports folder-form maps (data\maps\<dir>\map.ini is exactly that),
// but CampaignStaticDataManager::l_IO_Load (0x410B58) only enumerates *.c2m files for campaign00/01
// and does not scan sub-directories. After the campaign manager is initialized, this Feature reuses
// the game's own IniFile_Open + LoadCampaignMap (see Core/GameApi.h) to register folder maps into the
// same campaign list — without modifying any game binary.
//
// Key reverse-engineering facts (they decide this implementation's call convention):
//   * LoadCampaignMap (0x410E6D) is __thiscall, this = the CampaignStaticDataManager instance
//     (= *(DWORD*)g_pCampaignStaticDataMgr), NOT the IniFile config object.
//     Assembly confirms: l_IO_Load's built-in map loop at 0x410BF6 does "mov ecx,[ebp+var_8]".
//   * Folder-form map = Source mode: src = map content root dir (on map entry the game reads
//     <src>\map.ini + map.dat), a4=0. Byte-for-byte identical to the built-in map loop.
//   * c2m form = a4=1 mode (src=nullptr, a5=c2m filename), relies on a mounted package; do NOT use
//     it for folder form.
//
// Config (plugins/config/CulturesGameExtend_Game.ini or _Global.ini):
//   [UserCampaigns]
//   Enabled = 1
//   ; allowcurrentusermapfolder=1 also scans <map folder>\currentusermap\map.ini
//   ; (c2m-extracted layout, e.g. Campaign00\01_Ein_neuer_Anfang\currentusermap\map.ini).
//   ; default (0) only scans the directory-root map.ini (the official data\maps\<dir> layout).
//   AllowCurrentUserMapFolder = 1
//   ; optional: extra campaign dirs (relative to datax\usercampaigns\), name=campaignId;...
//   ; NOTE: campaign id must be < 9 (engine LoadCampaignMap slot limit; campaign00/01 occupy 7/8;
//   ; for 9+ you must separately patch the cmp eax,9 at 0x411036).
//   ; CampaignDirs = campaign02=9;campaign03=10
// ===================================================================
#include "pch.h"
#include "Core/Feature.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Logger.h"
#include "Core/Paths.h"
#include "Core/GameApi.h"
#include <windows.h>
#include <malloc.h>
#include <cstdlib>
#include <string>
#include <vector>


namespace fe_usercampaigns {


const char* kName = "UserCampaigns";
const char* kCat  = "[UserCampaigns]";

// 与 l_IO_Load 中 int v13[1572] 同尺寸的配置对象缓冲

// config-object buffer sized to match l_IO_Load's int v13[1572]
static const int kCfgSize = 1572 * 4;


#pragma region Helpers & Registration
static bool FileExists(const std::string& path) {
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 取 CampaignStaticDataManager 单例实例指针

// fetch the CampaignStaticDataManager singleton instance pointer
static void* GetCampaignMgr() {
    return *(void**)gameapi::Va<void**>(gameapi::addr::g_pCampaignStaticDataMgr);
}

// 注册单个文件夹形式地图：
//   <campaignDir>\<mapFolder>\map.ini            （布局 A：目录根，默认扫描）
//   <campaignDir>\<mapFolder>\currentusermap\map.ini （布局 B：c2m 解包保留虚拟根，
//                                                      仅 allowcurrentusermapfolder=1 时额外扫描）

// register a single folder-form map:
//   <campaignDir>\<mapFolder>\map.ini            (layout A: directory root, scanned by default)
//   <campaignDir>\<mapFolder>\currentusermap\map.ini (layout B: c2m extract keeps virtual root;
//                                                      scanned only when allowcurrentusermapfolder=1)
static void RegisterFolderMap(const std::string& campaignDir,
                              const std::string& mapFolder,
                              int campaignId,
                              bool allowCurrentUserMapFolder) {
    std::string base = campaignDir + "\\" + mapFolder;
    std::string iniPath, sourceDir;
// sourceDir = 进图时游戏读 <dir>\map.ini 的根
// sourceDir = dir from which the game reads <dir>\map.ini on map entry
    if (FileExists(base + "\\map.ini")) {
        iniPath   = base + "\\map.ini";
        sourceDir = base;
    } else if (allowCurrentUserMapFolder && FileExists(base + "\\currentusermap\\map.ini")) {
        iniPath   = base + "\\currentusermap\\map.ini";
        sourceDir = base + "\\currentusermap";
    } else {
        LOG_DEBUG(kCat, "no map.ini in %s (checked direct%s)",
                  base.c_str(), allowCurrentUserMapFolder ? " and currentusermap\\" : "");
        return;
    }
    void* mgr = GetCampaignMgr();


    if (!mgr) {
        LOG_WARN(kCat, "campaign manager is null, skip %s", iniPath.c_str());
        return;
    }
    char* cfg = (char*)_alloca(kCfgSize);


    memset(cfg, 0, kCfgSize);
    gameapi::IniFile_Open(cfg, iniPath.c_str(), 0, 0, 0, 0);
    if (cfg[0]) {
        char ok = gameapi::LoadCampaignMap(mgr, cfg, sourceDir.c_str(), 0, nullptr,
// 首字节非零 = IniFile_Open 解析成功（LoadCampaignMap 内部亦检查）
// first byte non-zero = IniFile_Open parsed successfully (LoadCampaignMap also checks)
                                           (unsigned int)campaignId);
        // 与 l_IO_Load 内置地图循环逐字节一致：
        //   this=mgr, a2=cfg, src=内容根目录, a4=0(Source模式), a5=nullptr, a6=战役ID
        // byte-for-byte identical to l_IO_Load's built-in map loop:
        //   this=mgr, a2=cfg, src=content root dir, a4=0 (Source mode), a5=nullptr, a6=campaign id
        gameapi::IniFile_Close(cfg);
        LOG_INFO(kCat, "folder map registered: %s (campaign %d, %s)",
                 iniPath.c_str(), campaignId, ok ? "ok" : "rejected");
    } else {
        LOG_DEBUG(kCat, "skip (no parseable ini): %s", iniPath.c_str());
    }
}
static void EnumerateFolderMaps(const std::string& campaignDir, int campaignId,
                                bool allowCurrentUserMapFolder) {

// 枚举战役目录下所有子文件夹（文件夹形式地图）

// enumerate all sub-folders under the campaign directory (folder-form maps)
    std::string pattern = campaignDir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        LOG_DEBUG(kCat, "dir not found: %s", campaignDir.c_str());
        return;
    }
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            fd.cFileName[0] != '.')
            RegisterFolderMap(campaignDir, fd.cFileName, campaignId,
                              allowCurrentUserMapFolder);
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
}
static void ParseExtraDirs(const std::string& spec,
                           std::vector<std::pair<std::string, int>>& out) {

// 解析 "name=id;name=id" 形式的额外目录列表（相对 datax\usercampaigns\）

// parse "name=id;name=id" extra-dir list (relative to datax\usercampaigns\)
    size_t pos = 0;
    while (pos <= spec.size()) {
        size_t sep = spec.find(';', pos);
        std::string item = spec.substr(
            pos, sep == std::string::npos ? std::string::npos : sep - pos);
        if (!item.empty()) {
            size_t eq = item.find('=');
            std::string name = eq == std::string::npos ? item : item.substr(0, eq);
            int id = eq == std::string::npos ? 7 : atoi(item.c_str() + eq + 1);
            if (!name.empty() && id > 0 && id < 32)
                out.push_back({ name, id });
        }
        if (sep == std::string::npos) break;
        pos = sep + 1;
    }
}
static void DoRegister(IniConfig& cfg) {
    std::vector<std::pair<std::string, int>> campaigns;

// 主注册逻辑

// main registration logic
    campaigns.push_back({ "datax\\usercampaigns\\campaign00", 7 });
    campaigns.push_back({ "datax\\usercampaigns\\campaign01", 8 });
    // 内置目录（战役 ID 与游戏 l_IO_Load 的 c2m 一致）
    // built-in dirs (campaign ids match the game's l_IO_Load c2m)
    ParseExtraDirs(cfg.GetString(kName, "CampaignDirs", ""), campaigns);
    bool allowCurrentUserMapFolder = cfg.GetBool(kName, "allowcurrentusermapfolder", false);
    // 额外目录（可选；ID<9 限制见文件头注释）
    // extra dirs (optional; id<9 limit see file header comment)
    LOG_INFO(kCat, "allowcurrentusermapfolder = %d", allowCurrentUserMapFolder ? 1 : 0);

    // allowcurrentusermapfolder=1 时额外扫描 <地图文件夹>\currentusermap\map.ini
    // （c2m 解包布局）；默认只扫目录根 map.ini。

    // allowcurrentusermapfolder=1 also scans <map folder>\currentusermap\map.ini (c2m-extracted layout);
    // default only scans the directory-root map.ini.
    for (auto& c : campaigns)
        EnumerateFolderMaps(c.first, c.second, allowCurrentUserMapFolder);


}
static DWORD WINAPI RegisterThread(LPVOID) {
    void* mgr = GetCampaignMgr();

// 轮询线程：等 g_pCampaignStaticDataMgr 非空 —— 即 l_IO_Load 执行完，
// 此时注册能与内置 c2m 进入同一张列表，且远早于主菜单渲染。

// polling thread: wait until g_pCampaignStaticDataMgr is non-null — i.e. l_IO_Load has finished,
// so registration joins the same list as built-in c2m, and far earlier than the main-menu render.
    for (int i = 0; i < 300 && !mgr; ++i) {
        Sleep(100);
        mgr = GetCampaignMgr();
// 最多等 30 秒
// wait up to 30 seconds
    }
    if (!mgr) {
        LOG_WARN(kCat, "campaign manager not ready, folder campaigns skipped");
        return 0;
    }
    IniConfig cfg;
    cfg.Load(ge_paths::GlobalIniPath());
    IniConfig gameIni;
    // 线程内重新加载配置（不依赖主线程 cfg 的生命周期）
    // reload config inside the thread (don't depend on the main thread cfg's lifetime)
    if (gameIni.Load(ge_paths::GameIniPath())) cfg.Merge(gameIni);
    IniConfig patchesIni;
    if (patchesIni.Load(ge_paths::PatchesIniPath())) cfg.Merge(patchesIni);
    DoRegister(cfg);
    LOG_INFO(kCat, "folder-style campaigns done");
    return 0;


}
class UserCampaignsFeature : public Feature {
public:
    const char* GetName() const override { return kName; }

#pragma endregion

#pragma region Feature Install
    GameTarget  GetTarget() const override { return GameTarget::Game; }
    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        if (!cfg.GetBool(kName, "Enabled", false)) {
            LOG_INFO(kCat, "Disabled (Enabled=0)");


            return true;
        }
        gameapi::Init(ver.GetBaseAddress());
        HANDLE h = CreateThread(nullptr, 0, RegisterThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
        // 集中初始化游戏 API（幂等；所有 Feature 都调用它）
        // central init of game API (idempotent; called by all Features)
        return true;
    }
};
REGISTER_FEATURE(UserCampaignsFeature)
}
#pragma endregion
