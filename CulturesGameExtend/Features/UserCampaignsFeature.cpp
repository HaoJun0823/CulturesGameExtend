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
static const int kCfgSize = 1572 * 4;

static bool FileExists(const std::string& path) {
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 取 CampaignStaticDataManager 单例实例指针
static void* GetCampaignMgr() {
    return *(void**)gameapi::Va<void**>(gameapi::addr::g_pCampaignStaticDataMgr);
}

// 注册单个文件夹形式地图：
//   <campaignDir>\<mapFolder>\map.ini            （布局 A：目录根，默认扫描）
//   <campaignDir>\<mapFolder>\currentusermap\map.ini （布局 B：c2m 解包保留虚拟根，
//                                                      仅 allowcurrentusermapfolder=1 时额外扫描）
static void RegisterFolderMap(const std::string& campaignDir,
                              const std::string& mapFolder,
                              int campaignId,
                              bool allowCurrentUserMapFolder) {
    std::string base = campaignDir + "\\" + mapFolder;
    std::string iniPath, sourceDir; // sourceDir = 进图时游戏读 <dir>\map.ini 的根
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
    if (cfg[0]) { // 首字节非零 = IniFile_Open 解析成功（LoadCampaignMap 内部亦检查）
        // 与 l_IO_Load 内置地图循环逐字节一致：
        //   this=mgr, a2=cfg, src=内容根目录, a4=0(Source模式), a5=nullptr, a6=战役ID
        char ok = gameapi::LoadCampaignMap(mgr, cfg, sourceDir.c_str(), 0, nullptr,
                                           (unsigned int)campaignId);
        gameapi::IniFile_Close(cfg);
        LOG_INFO(kCat, "folder map registered: %s (campaign %d, %s)",
                 iniPath.c_str(), campaignId, ok ? "ok" : "rejected");
    } else {
        LOG_DEBUG(kCat, "skip (no parseable ini): %s", iniPath.c_str());
    }
}

// 枚举战役目录下所有子文件夹（文件夹形式地图）
static void EnumerateFolderMaps(const std::string& campaignDir, int campaignId,
                                bool allowCurrentUserMapFolder) {
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

// 解析 "name=id;name=id" 形式的额外目录列表（相对 datax\usercampaigns\）
static void ParseExtraDirs(const std::string& spec,
                           std::vector<std::pair<std::string, int>>& out) {
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

// 主注册逻辑
static void DoRegister(IniConfig& cfg) {
    std::vector<std::pair<std::string, int>> campaigns;
    // 内置目录（战役 ID 与游戏 l_IO_Load 的 c2m 一致）
    campaigns.push_back({ "datax\\usercampaigns\\campaign00", 7 });
    campaigns.push_back({ "datax\\usercampaigns\\campaign01", 8 });
    // 额外目录（可选；ID<9 限制见文件头注释）
    ParseExtraDirs(cfg.GetString(kName, "CampaignDirs", ""), campaigns);

    // allowcurrentusermapfolder=1 时额外扫描 <地图文件夹>\currentusermap\map.ini
    // （c2m 解包布局）；默认只扫目录根 map.ini。
    bool allowCurrentUserMapFolder = cfg.GetBool(kName, "allowcurrentusermapfolder", false);
    LOG_INFO(kCat, "allowcurrentusermapfolder = %d", allowCurrentUserMapFolder ? 1 : 0);

    for (auto& c : campaigns)
        EnumerateFolderMaps(c.first, c.second, allowCurrentUserMapFolder);
}

// 轮询线程：等 g_pCampaignStaticDataMgr 非空 —— 即 l_IO_Load 执行完，
// 此时注册能与内置 c2m 进入同一张列表，且远早于主菜单渲染。
static DWORD WINAPI RegisterThread(LPVOID) {
    void* mgr = GetCampaignMgr();
    for (int i = 0; i < 300 && !mgr; ++i) { // 最多等 30 秒
        Sleep(100);
        mgr = GetCampaignMgr();
    }
    if (!mgr) {
        LOG_WARN(kCat, "campaign manager not ready, folder campaigns skipped");
        return 0;
    }
    // 线程内重新加载配置（不依赖主线程 cfg 的生命周期）
    IniConfig cfg;
    cfg.Load(ge_paths::GlobalIniPath());
    IniConfig gameIni;
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
    GameTarget  GetTarget() const override { return GameTarget::Game; }

    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        if (!cfg.GetBool(kName, "Enabled", false)) {
            LOG_INFO(kCat, "Disabled (Enabled=0)");
            return true;
        }
        // 集中初始化游戏 API（幂等；所有 Feature 都调用它）
        gameapi::Init(ver.GetBaseAddress());
        HANDLE h = CreateThread(nullptr, 0, RegisterThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
        return true;
    }
};

REGISTER_FEATURE(UserCampaignsFeature)

} // namespace fe_usercampaigns
