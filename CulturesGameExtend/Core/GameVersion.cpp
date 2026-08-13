#include "pch.h"
#include "GameVersion.h"
#include "fs_compat.h"
#include "Paths.h"
#include "Logger.h"


#pragma region Internal state
namespace {


const char* kCategory = "[Version]";


}
// namespace
// namespace
const char* GameTargetName(GameTarget t) {

#pragma endregion

#pragma region GameTargetName
    switch (t) {
        case GameTarget::Game:    return "Game.exe";
        case GameTarget::Any:     return "Any";
        default:                  return "Unknown";
    }
}
GameTarget GameVersion::Detect(const IniConfig& cfg) {

#pragma endregion

#pragma region GameVersion::Detect
    m_target  = GameTarget::Unknown;
    m_exeName.clear();
    m_base    = (DWORD)(uintptr_t)GetModuleHandle(NULL);
    std::string exeName = cfg.GetString("Version", "ExeName", "Game.exe");

    // 版本识别以"主程序 exe 所在目录"为基准（而非进程当前工作目录 CWD）。
    // 默认 Game.exe，可在 [Version] ExeName 配置。

    // Version detection is based on the "main program exe's directory" (not the process CWD).
    // Defaults to Game.exe, configurable via [Version] ExeName.
    std::string exePath = ge_paths::Resolve(exeName.c_str());
    std::error_code ec;


    if (!ge::fs::exists(exePath, ec)) {
        LOG_WARN(kCategory, "Game.exe not found (exe dir): %s, version = Unknown", exePath.c_str());
        return m_target;
    }
    m_target  = GameTarget::Game;


    m_exeName = exePath;
    LOG_INFO(kCategory, "Detected game: %s (base=0x%X)", exePath.c_str(), m_base);
    return m_target;
}
#pragma endregion
