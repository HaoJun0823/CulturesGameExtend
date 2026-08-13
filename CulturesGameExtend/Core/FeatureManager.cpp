#include "pch.h"
#include "FeatureManager.h"
#include "Feature.h"
#include "Logger.h"


#pragma region Internal state
namespace {


const char* kCategory = "[FeatureManager]";


}
// namespace
// namespace
bool Feature::TargetMatch(GameVersion& ver) const {

#pragma endregion

#pragma region Feature / FeatureRegistry implementation
    GameTarget t = GetTarget();
    if (t == GameTarget::Any) return true;
    return (t == ver.Current());
}
FeatureRegistry& FeatureRegistry::Instance() {


    static FeatureRegistry inst;
    return inst;
}
void FeatureRegistry::Register(Feature* f) {


    if (f) m_features.push_back(f);
}
size_t FeatureManager::InstallAll(IniConfig& cfg, GameVersion& ver) {

#pragma endregion

#pragma region InstallAll / UninstallAll
    size_t ok = 0;
    size_t attempted = 0;
    for (Feature* f : FeatureRegistry::Instance().All()) {
// 分母：只算实际尝试安装的（排除 disabled / target 不匹配）
// Denominator: only counts Features actually attempted (excludes disabled / target-mismatch)
        const char* name = f->GetName();
        bool enabled = cfg.GetBool(name, "Enabled", false);
        if (!enabled) {
            LOG_INFO(kCategory, "Feature '%s' disabled (Enabled=0), skip.", name);


            continue;
        }
        if (!f->TargetMatch(ver)) {
            LOG_WARN(kCategory, "Feature '%s' skipped: target mismatch (feature=%s, game=%s).",
                     name, GameTargetName(f->GetTarget()), GameTargetName(ver.Current()));
            continue;
        }
        ++attempted;
        LOG_INFO(kCategory, "Installing Feature '%s' ...", name);
        if (f->OnInstall(cfg, ver)) {
            ++ok;
            LOG_INFO(kCategory, "Feature '%s' installed OK.", name);
        } else {
            LOG_ERROR(kCategory, "Feature '%s' install FAILED.", name);
        }
    }
    LOG_INFO(kCategory, "Installed %zu/%zu features (%zu registered, %zu disabled/target-mismatch).",
             ok, attempted, FeatureRegistry::Instance().All().size(),
             FeatureRegistry::Instance().All().size() - attempted);
    return ok;
}
void FeatureManager::UninstallAll() {
}
#pragma endregion
