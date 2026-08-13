#pragma once
#include "IniConfig.h"
#include "GameVersion.h"

// 调度器：遍历全局 registry，根据 INI 开关与版本匹配安装各 Feature。

#pragma region Dispatcher
// Dispatcher: iterates the global registry and installs each Feature based on INI toggles and version matching.
class FeatureManager {
public:
    // 安装全部已启用且版本匹配的 Feature。
    // 单个 Feature 失败仅记日志，不影响其他 Feature。
    // 返回成功安装的 Feature 数量。
    // Install all enabled Features that match the version.
    // A single Feature's failure only logs; it does not affect other Features.
    // Returns the number of Features successfully installed.
    static size_t InstallAll(IniConfig& cfg, GameVersion& ver);

    // 卸载（可选）：当前框架暂不实现 OnUninstall，预留接口位。

    // Uninstall (optional): the current framework does not yet implement OnUninstall; reserved interface slot.
    static void UninstallAll();
};

#pragma endregion
