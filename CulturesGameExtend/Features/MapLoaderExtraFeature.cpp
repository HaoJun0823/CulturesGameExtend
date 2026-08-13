// MapLoaderExtraFeature —— 通过游戏原生函数，把 ini 指定"地图根目录"下的
// 每个子目录（含 map.ini + map.dat）注册成一张游戏地图。
//
// 设计（2026-08-13 重构）：
//   旧版 hook 了 sub_410B58（l_IO_Load）并复刻引擎内部脆弱链
//   （sub_4064E4→sub_424EF8→sub_410E6D→sub_425072），对绝对路径会构造坏指针
//   虚调用崩溃，靠 SEH 吞咽又导致地图注册失败。
//
//   新版完全不 hook、不碰文件系统（无软连接），而是复用 UserCampaignsFeature
//   已验证的"原生函数"范式：战役管理器就绪后，对每个地图文件夹直接调
//     IniFile_Open(map.ini) -> LoadCampaignMap(src, a4=0) -> IniFile_Close
//   这条链本身安全，与 l_IO_Load 内置 data\maps 循环逐字节一致。
//
// 调用约定（与 l_IO_Load / UserCampaignsFeature 一致）：
//   this = CampaignStaticDataManager 单例 = *(DWORD*)g_pCampaignStaticDataMgr
//   IniFile_Open(cfg, "<src>\map.ini", 0,0,0,0)
//   LoadCampaignMap(mgr, cfg, "<src>", 0, nullptr, campaignId)  // a4=0 Source 目录模式
//   IniFile_Close(cfg)
//
// campaignId（[MapLoaderExtra].CampaignId，默认 0）：
//   0 = 单图/遭遇战列表（等同 data\maps 的归属）；
//   7/8 = 用户战役列表（usercampaign 同款，第三方文件夹最稳）。
//   ★ 若选 0 时引擎把内容根锁死到 data\maps\ 下导致进图找不到 map.dat，
//     改 ini 把 CampaignId 设为 7 即可，无需重编译。

// MapLoaderExtraFeature —— using the game's native functions, register every sub-directory under the
// ini-specified "map root" (containing map.ini + map.dat) as a game map.
//
// Design (2026-08-13 refactor):
//   The old version hooked sub_410B58 (l_IO_Load) and replicated the engine's fragile internal chain
//   (sub_4064E4 -> sub_424EF8 -> sub_410E6D -> sub_425072); for absolute paths it built a bad pointer
//   that crashed on a virtual call, and masking it with SEH in turn caused map registration to fail.
//
//   The new version does NOT hook and does NOT touch the file system (no symlinks); instead it reuses
//   the "native function" pattern already validated by UserCampaignsFeature: once the campaign manager
//   is ready, for each map folder call directly
//     IniFile_Open(map.ini) -> LoadCampaignMap(src, a4=0) -> IniFile_Close
//   This chain itself is safe and byte-for-byte identical to the data\maps loop built into l_IO_Load.
//
// Calling convention (same as l_IO_Load / UserCampaignsFeature):
//   this = CampaignStaticDataManager singleton = *(DWORD*)g_pCampaignStaticDataMgr
//   IniFile_Open(cfg, "<src>\map.ini", 0,0,0,0)
//   LoadCampaignMap(mgr, cfg, "<src>", 0, nullptr, campaignId)  // a4=0 Source-directory mode
//   IniFile_Close(cfg)
//
// campaignId ([MapLoaderExtra].CampaignId, default 0):
//   0 = single-map / skirmish list (same ownership as data\maps);
//   7/8 = user-campaign list (same as usercampaign, the most stable for third-party folders).
//   ★ If choosing 0 causes the engine to lock the content root under data\maps\ so map.dat isn't found
//     on map entry, set CampaignId to 7 in the ini — no recompile needed.

#include "pch.h"
#include "Core/Logger.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Feature.h"
#include "Core/Paths.h"
#include "Core/GameApi.h"
#include <windows.h>
#include <string>
#include <vector>


namespace {


const char* kSection = "MapLoaderExtra";
// ini 段名（无括号）
// ini section name (no brackets)
const char* kCat     = "[MapLoaderExtra]";
const int kCfgSize = 1572 * 4;
// 日志前缀
// log prefix
static bool FileExists(const std::string& p) {

// 与 IniFile_Open 内部 memset(this, 0, 0x1890) 同尺寸的配置对象缓冲

// config-object buffer sized to match IniFile_Open's internal memset(this, 0, 0x1890)
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;


#pragma region Helpers & Registration
}
static void* GetCampaignMgr() {
    return *(void**)gameapi::Va<void**>(gameapi::addr::g_pCampaignStaticDataMgr);

// 取 CampaignStaticDataManager 单例实例指针

// fetch the CampaignStaticDataManager singleton instance pointer
}
static std::string NormalizeMapPath(const std::string& s) {
    std::string p = s;

// 路径规范化：
//   - 剥掉前导 ".\\" / "./"（与 UserCampaignsFeature 的相对风格一致）
//   - 绝对/盘符路径才走 ge_paths::Resolve 归一化；相对路径原样保留
//     （LoadCampaignMap 的 Source 模式按相对 CWD 解析）

// path normalization:
//   - strip leading ".\\" / "./" (relative style consistent with UserCampaignsFeature)
//   - only absolute / drive-letter paths go through ge_paths::Resolve for normalization; relative
//     paths are kept as-is (LoadCampaignMap's Source mode resolves relative to CWD)
    bool isAbs = (p.size() >= 2 && p[1] == ':') ||
                 (p.size() >= 1 && (p[0] == '\\' || p[0] == '/'));
    if (isAbs) p = ge_paths::Resolve(p.c_str());
    if (p.size() >= 2 && p[0] == '.' && (p[1] == '/' || p[1] == '\\'))
        p = p.substr(2);
    return p;
}
static void RegisterOne(const std::string& root, const std::string& subdir, int campaignId) {
    std::string src    = root + "\\" + subdir;

// 注册单个文件夹地图：<root>\<subdir>\map.ini

// register a single folder map: <root>\<subdir>\map.ini
    std::string iniPath = src + "\\map.ini";
    if (!FileExists(iniPath)) {
// 内容根目录（相对 CWD）
// content root (relative to CWD)
        LOG_DEBUG(kCat, "skip (no map.ini): %s", src.c_str());
        return;
    }
    void* mgr = GetCampaignMgr();
    if (!mgr) {
        LOG_WARN(kCat, "campaign manager null, skip %s", src.c_str());


        return;
    }
    std::vector<char> cfg(kCfgSize, 0);
    gameapi::IniFile_Open(cfg.data(), iniPath.c_str(), 0, 0, 0, 0);
    if (cfg[0]) {


        char ok = gameapi::LoadCampaignMap(mgr, cfg.data(), src.c_str(), 0, nullptr,
// IniFile_Open 内部会 memset(this,0,0x1890)
// IniFile_Open internally does memset(this,0,0x1890)
                                           (unsigned int)campaignId);
        gameapi::IniFile_Close(cfg.data());
        LOG_INFO(kCat, "registered: %s (campaign %d, %s)",
// 首字节非零 = IniFile_Open 解析成功（LoadCampaignMap 内部亦检查）
// first byte non-zero = IniFile_Open parsed successfully (LoadCampaignMap also checks)
                 src.c_str(), campaignId, ok ? "ok" : "rejected");
        // a4=0 -> Source 目录模式：引擎按 src 路径直接定位 map.ini / map.dat
        // a4=0 -> Source directory mode: engine locates map.ini / map.dat directly by src path
    } else {
        LOG_DEBUG(kCat, "skip (ini unparseable): %s", iniPath.c_str());
    }
}
static void Enumerate(const std::string& root, int campaignId) {
    if (!FileExists(root)) {
        LOG_DEBUG(kCat, "root not found: %s", root.c_str());
        return;
    }

// 枚举 root 下所有子目录，每个当作一张地图尝试注册

// enumerate all sub-directories under root, each attempted as a map registration
    std::string pattern = root + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.')
            RegisterOne(root, fd.cFileName, campaignId);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
static void ParseRoots(const std::string& spec, std::vector<std::string>& out) {
    std::string cur;
    for (char c : spec) {
        if (c == ';') {
            if (!cur.empty()) { out.push_back(NormalizeMapPath(cur)); cur.clear(); }

// 解析分号分隔的"地图根目录"列表

// parse the semicolon-separated "map root" list
        } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(NormalizeMapPath(cur));
}
static void DoRegister(IniConfig& cfg) {
    std::vector<std::string> roots;
    ParseRoots(cfg.GetString(kSection, "ExtraMapPaths", ""), roots);
    if (roots.empty()) {
        std::string def = NormalizeMapPath(".\\CustomMaps");

// 主注册逻辑（在线程内执行，配置在线程里重新加载）

// main registration logic (runs inside a thread; config reloaded inside the thread)
        if (!def.empty()) {
            roots.push_back(def);
            LOG_INFO(kCat, "no ExtraMapPaths configured; defaulting to .\\CustomMaps -> %s", def.c_str());

    // 默认路径：游戏目录下的 .\CustomMaps（ini 未显式配置时生效）

    // default path: .\CustomMaps under the game directory (used when ini doesn't configure explicitly)
            if (!FileExists(def))
                CreateDirectoryA(def.c_str(), nullptr);
        }
    }
    int campaignId = cfg.GetInt(kSection, "CampaignId", 0);
            // 帮用户把默认目录建出来，方便直接往里丢地图
            // create the default directory for the user, so they can drop maps in directly
    LOG_INFO(kCat, "campaignId = %d ; %zu root(s):", campaignId, roots.size());
    for (const auto& r : roots)
        LOG_INFO(kCat, "  + %s", r.c_str());
    for (const auto& r : roots)


        Enumerate(r, campaignId);
}
static DWORD WINAPI RegisterThread(LPVOID) {
    void* mgr = GetCampaignMgr();


    for (int i = 0; i < 300 && !mgr; ++i) {
        Sleep(100);
        mgr = GetCampaignMgr();

// 轮询线程：等 g_pCampaignStaticDataMgr 非空 —— 即 l_IO_Load 执行完，
// 此时注册能与内置地图进入同一张列表，且远早于主菜单渲染。

// polling thread: wait until g_pCampaignStaticDataMgr is non-null — i.e. l_IO_Load has finished,
// so registration joins the same list as built-in maps, and far earlier than the main-menu render.
    }
    if (!mgr) {
        LOG_WARN(kCat, "campaign manager not ready, extra maps skipped");
// 最多等 30 秒
// wait up to 30 seconds
        return 0;
    }
    IniConfig cfg;
    cfg.Load(ge_paths::GlobalIniPath());
    IniConfig gameIni;
    if (gameIni.Load(ge_paths::GameIniPath())) cfg.Merge(gameIni);
    IniConfig patchesIni;
    if (patchesIni.Load(ge_paths::PatchesIniPath())) cfg.Merge(patchesIni);
    // 线程内重新加载配置（不依赖主线程 cfg 的生命周期）
    // reload config inside the thread (don't depend on the main thread cfg's lifetime)
    DoRegister(cfg);
    LOG_INFO(kCat, "extra maps done");
    return 0;
}
class MapLoaderExtraFeature : public Feature {
public:


    const char* GetName() const override { return kSection; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }
    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        if (!cfg.GetBool(kSection, "Enabled", false)) {

#pragma endregion

#pragma region Feature Install
            LOG_INFO(kCat, "Disabled (Enabled=0)");
            return true;
        }
        gameapi::Init(ver.GetBaseAddress());


        HANDLE h = CreateThread(nullptr, 0, RegisterThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
        return true;
    }
};
        // 集中初始化游戏 API（幂等；UserCampaignsFeature 等也会调用）
        // central init of game API (idempotent; also called by UserCampaignsFeature etc.)
REGISTER_FEATURE(MapLoaderExtraFeature);
}
#pragma endregion
