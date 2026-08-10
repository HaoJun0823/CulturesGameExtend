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
namespace ge_paths {

    // 目录
    constexpr const char* kLogDir     = "logs";
    constexpr const char* kPluginsDir = "plugins";
    constexpr const char* kConfigDir  = "plugins/config";

    // CulturesGameExtend 双配置文件（框架/官方配置）
    constexpr const char* kGlobalIni  = "CulturesGameExtend_Global.ini";
    constexpr const char* kGameIni    = "CulturesGameExtend_Game.ini";
    // 非官方补丁配置（独立成文件，便于与框架配置分离）
    constexpr const char* kPatchesIni = "CulturesGameExtend_Patches.ini";

    // 主日志文件名
    constexpr const char* kMainLog    = "CulturesGameExtend.log";

    // 便捷：拼出完整相对路径（用 '/' 分隔，兼容 fs_compat）
    inline std::string Join(const char* a, const char* b) {
        return std::string(a) + "/" + b;
    }
    inline std::string GlobalIniPath()  { return Join(kConfigDir, kGlobalIni); }
    inline std::string GameIniPath()    { return Join(kConfigDir, kGameIni); }
    inline std::string PatchesIniPath() { return Join(kConfigDir, kPatchesIni); }

    // 返回主程序（exe）所在目录，带尾部反斜杠（如 "C:\\Game\\"）。
    // 解析失败时返回空字符串，调用方需自行兜底。
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
    inline std::string Resolve(const char* rel) {
        std::string dir = ExeDir();
        if (dir.empty())
            return std::string(rel); // 解析失败时退回相对路径
        return dir + rel;
    }

} // namespace ge_paths
