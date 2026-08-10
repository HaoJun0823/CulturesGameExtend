#pragma once
#include <string>
#include <vector>
#include "IniConfig.h"
#include "GameVersion.h"

class FeatureManager; // 前向声明

// Feature 抽象基类。
// 每个独立功能继承该类，实现 GetName / GetTarget / OnInstall，
// 并通过 REGISTER_FEATURE 宏在全局 registry 自注册（无需改动任何现有文件）。
class Feature {
public:
    virtual ~Feature() = default;

    // Feature 唯一名称，同时作为 INI 中的 [Section] 名与日志分类前缀。
    virtual const char* GetName() const = 0;

    // 该 Feature 适用的游戏版本：
    //   GameTarget::Any  -> 通用功能（不区分版本）
    //   GameTarget::Game -> 仅 Game.exe
    virtual GameTarget GetTarget() const { return GameTarget::Any; }

    // 安装逻辑。返回 true 表示成功启用。
    // cfg : 已加载的 INI 配置
    // ver : 已识别的游戏版本
    virtual bool OnInstall(IniConfig& cfg, GameVersion& ver) = 0;

    // 版本是否匹配：Any 永远匹配；否则需版本一致。
    bool TargetMatch(GameVersion& ver) const;
};

// 全局 Feature 注册表（Meyers 单例）。
class FeatureRegistry {
public:
    static FeatureRegistry& Instance();

    void Register(Feature* f);
    const std::vector<Feature*>& All() const { return m_features; }

private:
    FeatureRegistry() = default;
    std::vector<Feature*> m_features;
};

// 自注册辅助：定义一个静态对象，其构造时把 pInstance 注册进 registry。
// 用法（在 feature 的 cpp 底部）：
//   REGISTER_FEATURE(MyFeature);
// 其中 MyFeature 为默认可构造的 Feature 子类。
#define REGISTER_FEATURE(Cls) \
    namespace { \
        struct Cls##_Registrar { \
            Cls##_Registrar() { FeatureRegistry::Instance().Register(new Cls()); } \
        } Cls##_registrar_inst; \
    }
