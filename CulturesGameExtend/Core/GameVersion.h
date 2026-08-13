#pragma once
#include <string>
#include "IniConfig.h"

// 目标游戏：只有 Game.exe 一个程序。

#pragma region Game target enum
// Target game: only Game.exe is a single program.
enum class GameTarget {
    Unknown,
// 无法识别 / 未匹配
// Unrecognized / not matched
    Game,
    Any,
// Game.exe
// Game.exe
};
const char* GameTargetName(GameTarget t);
// 通用（不区分版本）
// Generic (version-agnostic)
class GameVersion {
public:


    GameTarget Detect(const IniConfig& cfg);

// 版本识别：识别主程序 exe（默认 Game.exe，可配置）。
// GetBaseAddress 返回主程序模块基址（= GetModuleHandle(NULL)）。
#pragma endregion

#pragma region GameVersion class
// Version detection: recognizes the main program exe (defaults to Game.exe, configurable).
// GetBaseAddress returns the main program module base (= GetModuleHandle(NULL)).
    GameTarget Current() const { return m_target; }
    const std::string& ExeName() const { return m_exeName; }
    // 执行识别。返回识别到的版本。
    // Perform detection. Returns the detected version.
    DWORD GetBaseAddress() const { return m_base; }


    bool IsGame() const { return m_target == GameTarget::Game; }
private:
    GameTarget m_target = GameTarget::Unknown;


    std::string m_exeName;


    DWORD m_base = 0;
};
#pragma endregion
