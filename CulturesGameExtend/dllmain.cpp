// dllmain.cpp : 定义 DLL 应用程序的入口点。
#include "pch.h"
#include "Core/Logger.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Feature.h"
#include "Core/FeatureManager.h"
#include "Core/fs_compat.h"
#include "Core/Paths.h"

// 内置 Feature 的实现文件。
#include "Features/CulturesPatchesFeature.cpp"
#include "Features/UserCampaignsFeature.cpp"
#include "Features/AsgardCampaignFeature.cpp"
#include "Features/Cultures2CampaignFeature.cpp"
#include "Features/UnlockAllCampaignsFeature.cpp"
#include "Features/CampaignMovieFeature.cpp"
#include "Features/VersionStampFeature.cpp"
#include "Features/TitleOverrideFeature.cpp"
#include "Features/PerMapLogicFeature.cpp"
#include "Features/TextRendererFeature.cpp"

namespace {

// 日志级别字符串 -> 枚举
LogLevel ParseLogLevel(const std::string& s) {
    if (s == "Trace") return LogLevel::Trace;
    if (s == "Debug") return LogLevel::Debug;
    if (s == "Info")  return LogLevel::Info;
    if (s == "Warn")  return LogLevel::Warn;
    if (s == "Error") return LogLevel::Error;
    if (s == "None")  return LogLevel::None;
    return LogLevel::Info;
}

// cfg 通过指针传入（在 DllMain 主线程中创建，这里在工作线程中完成剩余初始化并在结束时释放）。
void RunExtend(IniConfig* pcfg) {
    IniConfig& cfg = *pcfg;

    // 1) 初始化日志（目录已在 DllMain 创建；以默认 Info 打开以记录启动过程）
    LogInit(ge_paths::kLogDir, ge_paths::kMainLog, LogLevel::Info);

    // 2) 用 INI 中的日志级别重新设定（可选）
    LogLevel lvl = ParseLogLevel(cfg.GetString("General", "LogLevel", "Info"));
    (void)lvl;
    LOG_INFO("[Boot]", "Log level = %s", cfg.GetString("General", "LogLevel", "Info").c_str());

    // 3) 识别游戏版本
    GameVersion ver;
    GameTarget t = ver.Detect(cfg);
    LOG_INFO("[Boot]", "Detected game: %s (exe=%s base=0x%X)",
             GameTargetName(t), ver.ExeName().c_str(), ver.GetBaseAddress());

    if (t == GameTarget::Unknown) {
        LOG_WARN("[Boot]", "Unknown game; features still attempted where target Any.");
    }

    // 4) 安装全部已启用且版本匹配的 Feature
    size_t n = FeatureManager::InstallAll(cfg, ver);
    LOG_INFO("[Boot]", "Extend framework initialized, %zu feature(s) active.", n);

    delete pcfg;
}

} // namespace

// 在工作线程中执行初始化，避免在 DllMain 持 Loader Lock 期间进行文件 IO / 内存写补丁
// 等可能引发死锁或崩溃的操作。
static DWORD WINAPI RunExtendThread(LPVOID lp) {
    IniConfig* pcfg = reinterpret_cast<IniConfig*>(lp);
    RunExtend(pcfg);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
    {
        // 不通知线程级 ATTACH，减少不必要的调用
        DisableThreadLibraryCalls(hModule);

        // 创建目录并加载双配置（只读，很快）。
        //   logs/               日志
        //   plugins/config/     CulturesGameExtend_Global.ini + CulturesGameExtend_Game.ini
        // 采用"Global 打底、Game 覆盖"：先加载 Global，再把 Game 合并覆盖其上。
        std::error_code ec;
        ge::fs::create_directories(ge_paths::kLogDir, ec);
        ge::fs::create_directories(ge_paths::kConfigDir, ec);
        IniConfig* pcfg = new IniConfig();
        pcfg->Load(ge_paths::GlobalIniPath());
        IniConfig gameIni;
        if (gameIni.Load(ge_paths::GameIniPath())) {
            pcfg->Merge(gameIni); // Game 覆盖 Global 同名键
        }
        // 非官方补丁配置（独立成文件），合并到主配置之上。
        // 该文件包含 [CulturesPatches] 与各 code cave 补丁开关。
        IniConfig patchesIni;
        if (patchesIni.Load(ge_paths::PatchesIniPath())) {
            pcfg->Merge(patchesIni);
        }
        // 无 INI 时继续（用默认值）；日志稍后在工作线程中初始化。

        // 再在工作线程中执行其余初始化（日志、版本识别、Feature 安装），
        // 让 Loader Lock 尽快释放。
        HANDLE hThread = CreateThread(nullptr, 0, RunExtendThread, pcfg, 0, nullptr);
        if (hThread) CloseHandle(hThread);
        else delete pcfg;
        break;
    }
    case DLL_PROCESS_DETACH:
        // 补丁在进程退出时无需显式还原（进程即将销毁）
        break;
    }
    return TRUE;
}
