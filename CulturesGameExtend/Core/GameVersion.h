#pragma once
#include <string>
#include "IniConfig.h"

// 目标游戏：只有 Game.exe 一个程序。
enum class GameTarget {
    Unknown, // 无法识别 / 未匹配
    Game,    // Game.exe
    Any,     // 通用（不区分版本）
};

const char* GameTargetName(GameTarget t);

// 版本识别：识别主程序 exe（默认 Game.exe，可配置）。
// GetBaseAddress 返回主程序模块基址（= GetModuleHandle(NULL)）。
class GameVersion {
public:
    // 执行识别。返回识别到的版本。
    GameTarget Detect(const IniConfig& cfg);

    GameTarget Current() const { return m_target; }
    const std::string& ExeName() const { return m_exeName; }
    DWORD GetBaseAddress() const { return m_base; }

    bool IsGame() const { return m_target == GameTarget::Game; }

private:
    GameTarget m_target = GameTarget::Unknown;
    std::string m_exeName;
    DWORD m_base = 0;
};
