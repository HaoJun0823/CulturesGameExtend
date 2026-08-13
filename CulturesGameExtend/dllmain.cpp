// dllmain.cpp : Defines the entry point for the DLL application.
// dllmain.cpp : 定义 DLL 应用程序的入口点。
#include "pch.h"
#include "Core/Logger.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Feature.h"
#include "Core/FeatureManager.h"
#include "Core/fs_compat.h"

#pragma region Built-in Feature implementations
// Built-in Feature implementation files.
#include "Core/Paths.h"
#include "Features/CulturesPatchesFeature.cpp"

// 内置 Feature 的实现文件。
#include "Features/UserCampaignsFeature.cpp"
#include "Features/AsgardCampaignFeature.cpp"
#include "Features/Cultures2CampaignFeature.cpp"
#include "Features/UnlockAllCampaignsFeature.cpp"
#include "Features/CampaignMovieFeature.cpp"
#include "Features/VersionStampFeature.cpp"
#include "Features/TitleOverrideFeature.cpp"
#include "Features/PerMapLogicFeature.cpp"
#include "Features/TextRendererFeature.cpp"
#include "Features/WarningLogFeature.cpp"
#pragma endregion

#include "Features/MapLoaderExtraFeature.cpp"

#pragma region Log level parsing
// Map log level string -> enum
namespace {

LogLevel ParseLogLevel(const std::string& s) {

// 日志级别字符串 -> 枚举
    if (s == "Trace") return LogLevel::Trace;
    if (s == "Debug") return LogLevel::Debug;
    if (s == "Info")  return LogLevel::Info;
    if (s == "Warn")  return LogLevel::Warn;
    if (s == "Error") return LogLevel::Error;
    if (s == "None")  return LogLevel::None;
    return LogLevel::Info;
#pragma endregion

#pragma region Extend worker
// cfg is passed by pointer (created on the DllMain main thread; the remaining
// initialization is finished here on the worker thread and freed at the end).
}
void RunExtend(IniConfig* pcfg) {

// cfg 通过指针传入（在 DllMain 主线程中创建，这里在工作线程中完成剩余初始化并在结束时释放）。

    // 1) Initialize logging (directories already created in DllMain; opened at default Info to record startup)
    IniConfig& cfg = *pcfg;

    // 2) Re-apply the log level from the INI (optional)
    LogInit(ge_paths::kLogDir, ge_paths::kMainLog, LogLevel::Info);

    // 1) 初始化日志（目录已在 DllMain 创建；以默认 Info 打开以记录启动过程）
    LogLevel lvl = ParseLogLevel(cfg.GetString("General", "LogLevel", "Info"));

    // 2) 用 INI 中的日志级别重新设定（可选）
    (void)lvl;

    // 3) Identify the game version
    LOG_INFO("[Boot]", "Log level = %s", cfg.GetString("General", "LogLevel", "Info").c_str());
    GameVersion ver;

    // 3) 识别游戏版本
    GameTarget t = ver.Detect(cfg);
    LOG_INFO("[Boot]", "Detected game: %s (exe=%s base=0x%X)",

             GameTargetName(t), ver.ExeName().c_str(), ver.GetBaseAddress());
    if (t == GameTarget::Unknown) {

        LOG_WARN("[Boot]", "Unknown game; features still attempted where target Any.");

    // 4) Install all enabled Features that match the version
    }
    size_t n = FeatureManager::InstallAll(cfg, ver);

    // 4) 安装全部已启用且版本匹配的 Feature

    LOG_INFO("[Boot]", "Extend framework initialized, %zu feature(s) active.", n);
    delete pcfg;

#pragma endregion

}
// namespace
}


#pragma region DllMain entry
// Perform initialization on a worker thread to avoid file IO / memory-write
// patching while holding the Loader Lock inside DllMain, which could deadlock
// or crash.
static DWORD WINAPI RunExtendThread(LPVOID lp) {
// namespace
    IniConfig* pcfg = reinterpret_cast<IniConfig*>(lp);

// 在工作线程中执行初始化，避免在 DllMain 持 Loader Lock 期间进行文件 IO / 内存写补丁
// 等可能引发死锁或崩溃的操作。
    RunExtend(pcfg);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {

    case DLL_PROCESS_ATTACH:
    {
        // Suppress thread-level ATTACH notifications to reduce unnecessary calls
        DisableThreadLibraryCalls(hModule);

        // Create directories and load the twin config (read-only, fast).
        //   logs/                logs
        //   plugins/config/     CulturesGameExtend_Global.ini + CulturesGameExtend_Game.ini
        // Use "Global as base, Game overrides": load Global first, then merge Game over it.
        std::error_code ec;
        // 不通知线程级 ATTACH，减少不必要的调用
        ge::fs::create_directories(ge_paths::kLogDir, ec);

        // 创建目录并加载双配置（只读，很快）。
        //   logs/               日志
        //   plugins/config/     CulturesGameExtend_Global.ini + CulturesGameExtend_Game.ini
        // 采用"Global 打底、Game 覆盖"：先加载 Global，再把 Game 合并覆盖其上。
        ge::fs::create_directories(ge_paths::kConfigDir, ec);
        IniConfig* pcfg = new IniConfig();
        pcfg->Load(ge_paths::GlobalIniPath());
        IniConfig gameIni;
        if (gameIni.Load(ge_paths::GameIniPath())) {
            pcfg->Merge(gameIni);
// Game overrides Global's same-named keys
        }
        IniConfig patchesIni;
// Game 覆盖 Global 同名键
        // Unofficial patch config (kept in a separate file), merged on top of the main config.
        // Contains [CulturesPatches] and the per code-cave patch toggles.
        if (patchesIni.Load(ge_paths::PatchesIniPath())) {
            pcfg->Merge(patchesIni);
        // 非官方补丁配置（独立成文件），合并到主配置之上。
        // 该文件包含 [CulturesPatches] 与各 code cave 补丁开关。
        }
        HANDLE hThread = CreateThread(nullptr, 0, RunExtendThread, pcfg, 0, nullptr);
        // Continue without INI (use defaults); logging is initialized later on the worker thread.

        // Then finish the rest of init on the worker thread (logging, version detection,
        // Feature install) so the Loader Lock is released as soon as possible.
        if (hThread) CloseHandle(hThread);
        else delete pcfg;
        // 无 INI 时继续（用默认值）；日志稍后在工作线程中初始化。

        // 再在工作线程中执行其余初始化（日志、版本识别、Feature 安装），
        // 让 Loader Lock 尽快释放。
        break;
    }
    case DLL_PROCESS_DETACH:
        break;
        // Patches need no explicit restore on process exit (the process is about to be destroyed)
    }
    return TRUE;
        // 补丁在进程退出时无需显式还原（进程即将销毁）
}
#pragma endregion
