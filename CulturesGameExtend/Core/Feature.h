#pragma once
#include <string>
#include <vector>
#include "IniConfig.h"
#include "GameVersion.h"


class FeatureManager;
// 前向声明
// Forward declaration
class Feature {

// Feature 抽象基类。
// 每个独立功能继承该类，实现 GetName / GetTarget / OnInstall，
// 并通过 REGISTER_FEATURE 宏在全局 registry 自注册（无需改动任何现有文件）。

#pragma region Feature base class
// Feature abstract base class.
// Each individual feature inherits from this class and implements GetName / GetTarget / OnInstall,
// then self-registers into the global registry via the REGISTER_FEATURE macro (no existing file needs editing).
public:
    virtual ~Feature() = default;
    virtual const char* GetName() const = 0;

    // Feature 唯一名称，同时作为 INI 中的 [Section] 名与日志分类前缀。

    // Feature's unique name; also serves as the INI [Section] name and the log category prefix.
    virtual GameTarget GetTarget() const { return GameTarget::Any; }

    // 该 Feature 适用的游戏版本：
    //   GameTarget::Any  -> 通用功能（不区分版本）
    //   GameTarget::Game -> 仅 Game.exe

    // The game version this Feature applies to:
    //   GameTarget::Any  -> generic feature (version-agnostic)
    //   GameTarget::Game -> Game.exe only
    virtual bool OnInstall(IniConfig& cfg, GameVersion& ver) = 0;

    // 安装逻辑。返回 true 表示成功启用。
    // cfg : 已加载的 INI 配置
    // ver : 已识别的游戏版本

    // Install logic. Returns true on successful enable.
    // cfg : loaded INI configuration
    // ver : recognized game version
    bool TargetMatch(GameVersion& ver) const;

    // 版本是否匹配：Any 永远匹配；否则需版本一致。

    // Whether the version matches: Any always matches; otherwise the version must be identical.
};
class FeatureRegistry {

// 全局 Feature 注册表（Meyers 单例）。
#pragma endregion

#pragma region Feature registry
// Global Feature registry (Meyers singleton).
public:
    static FeatureRegistry& Instance();
    void Register(Feature* f);


    const std::vector<Feature*>& All() const { return m_features; }
private:


    FeatureRegistry() = default;
    std::vector<Feature*> m_features;
};
#pragma endregion

#pragma region Self-registration macro
// 自注册辅助：定义一个静态对象，其构造时把 pInstance 注册进 registry。
// 用法（在 feature 的 cpp 底部）：
//   REGISTER_FEATURE(MyFeature);
// 其中 MyFeature 为默认可构造的 Feature 子类。
// Self-registration helper: defines a static object whose constructor registers pInstance into the registry.
// Usage (at the bottom of a feature's cpp):
//   REGISTER_FEATURE(MyFeature);
// where MyFeature is a default-constructible Feature subclass.
#define REGISTER_FEATURE(Cls) \
    namespace { \
        struct Cls##_Registrar { \
            Cls##_Registrar() { FeatureRegistry::Instance().Register(new Cls()); } \
        } Cls##_registrar_inst; \
    }
#pragma endregion
