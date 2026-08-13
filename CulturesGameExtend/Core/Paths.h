#pragma once
#include <string>
#include <cstddef>
#include <windows.h>

// 精简后的根目录布局：
//   logs/              日志文件夹
//   plugins/           插件文件夹（存放 dll）
//   plugins/config/    插件配置（含 CulturesGameExtend 的双 ini 与补丁数据）
//
// 说明：所有路径都以"主程序 exe 所在目录"为根（通过 GetModuleFileName 解析），
// 不再依赖进程当前工作目录（CWD）。因为游戏 exe 与 dinput8.dll / plugins 都
// 位于同一目录，用 exe 目录做基准最可靠（与 CulturesProxyDLL 的解析方式一致）。

// Streamlined root directory layout:
//   logs/              log folder
//   plugins/           plugin folder (holds DLLs)
//   plugins/config/    plugin config (includes CulturesGameExtend's twin INIs and patch data)
//
// Note: all paths are rooted at the "main program exe directory" (resolved via
// GetModuleFileName), instead of relying on the process current working directory
// (CWD). Because the game exe, dinput8.dll and plugins all reside in the same
// directory, using the exe directory as the base is the most reliable (consistent
// with CulturesProxyDLL's resolution method).
namespace ge_paths {

    // 目录

    // Directories
    constexpr const char* kLogDir     = "logs";
    constexpr const char* kPluginsDir = "plugins";
    constexpr const char* kConfigDir  = "plugins/config";

    // CulturesGameExtend 双配置文件（框架/官方配置）

    // CulturesGameExtend twin config files (framework / official config)
    constexpr const char* kGlobalIni  = "CulturesGameExtend_Global.ini";
    constexpr const char* kGameIni    = "CulturesGameExtend_Game.ini";
    // 非官方补丁配置（独立成文件，便于与框架配置分离）
    // Unofficial patch config (kept in a separate file to decouple from the framework config)
    constexpr const char* kPatchesIni = "CulturesGameExtend_Patches.ini";

    // 主日志文件名

    // Main log file name
    constexpr const char* kMainLog    = "CulturesGameExtend.log";

    // 便捷：拼出完整相对路径（用 '/' 分隔，兼容 fs_compat）

    // Convenience: build a complete relative path (separated by '/', compatible with fs_compat)
    inline std::string Join(const char* a, const char* b) {
        return std::string(a) + "/" + b;
    }
    inline std::string GlobalIniPath()  { return Join(kConfigDir, kGlobalIni); }
    inline std::string GameIniPath()    { return Join(kConfigDir, kGameIni); }
    inline std::string PatchesIniPath() { return Join(kConfigDir, kPatchesIni); }

    // 返回主程序（exe）所在目录，带尾部反斜杠（如 "C:\\Game\\"）。
    // 解析失败时返回空字符串，调用方需自行兜底。

    // Returns the main program (exe) directory with a trailing backslash (e.g. "C:\\Game\\").
    // Returns an empty string on resolution failure; the caller must handle the fallback.
    inline std::string ExeDir() {
        char buf[MAX_PATH] = {};
        DWORD n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return std::string();
        std::string path(buf, n);
        size_t slash = path.find_last_of("\\/");
        if (slash == std::string::npos)
            return std::string();
        return path.substr(0, slash + 1);
    }

    // 把相对路径解析为"exe 目录 + 相对路径"的绝对路径（以 '\\' 分隔拼接）。

    // Resolve a relative path into an absolute path of "exe directory + relative path" (joined with '\\').
    inline std::string Resolve(const char* rel) {
        std::string dir = ExeDir();
        if (dir.empty())
            return std::string(rel);
// 解析失败时退回相对路径
// fall back to the relative path on resolution failure
        return dir + rel;
    }
}

