#pragma once
#include "IniConfig.h"
#include "GameVersion.h"

// 调度器：遍历全局 registry，根据 INI 开关与版本匹配安装各 Feature。
class FeatureManager {
public:
    // 安装全部已启用且版本匹配的 Feature。
    // 单个 Feature 失败仅记日志，不影响其他 Feature。
    // 返回成功安装的 Feature 数量。
    static size_t InstallAll(IniConfig& cfg, GameVersion& ver);

    // 卸载（可选）：当前框架暂不实现 OnUninstall，预留接口位。
    static void UninstallAll();
};
