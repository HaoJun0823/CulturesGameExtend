// VersionStampFeature.cpp
// ===================================================================
// [VersionStamp]
// DLL 挂载后，把游戏内所有可见的 "Build 日期/版本" 信息替换为
// CulturesGameExtend 自身的版本 + 构建日期：
//   1. 主菜单版本行（sub_4D1672）：
//      原模板 "%s - %s %d.%2.2d - %s Jul 18 2005 15:27:49"（0x5088B4）
//      -> "%s - %s %d.%2.2d - %s CulturesGameExtend vX.Y.Z (Build ...)"
//   2. 网络日志头（sub_4DB265 / netlog.cpp）：
//      "Build: %s\n" 的日期参数 "Jul 18 2005"（0x509860）
//      -> "CulturesGameExtend vX.Y.Z (Build ...)"
//      "Network log - Build: Thu Feb 28 16:22:57 2002 (c) Funatics ..."（0x509870）
//      -> "Network log - CulturesGameExtend vX.Y.Z (Build ...)"
//
// 实现方式：不改游戏代码结构，仅把三处 `push imm32` 的立即数
// （字符串地址）改写成本 DLL 提供的字面串地址（OnInstall 时运行时计算）。
// 版本号来自编译宏 CGE_VERSION_STR（由 build_deploy.sh 从 git tag 注入，
// 每次构建自动决定）；构建日期用 __DATE__/__TIME__（编译器注入）。
//
// 注：游戏里另有一处 "Version %d.%2.2d - Build date Jul 18 2005 15:25:18"
// 模板（0x4FF2EC 附近）经文件级指针扫描确认**零引用**（死模板，不显示），
// 无需处理。
//
// 配置（plugins/config/CulturesGameExtend_Game.ini）：
//   [VersionStamp]
//   Enabled = 1
// ===================================================================
#include "pch.h"
#include "Core/Feature.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Patch.h"
#include "Core/Logger.h"
#include "Core/GameApi.h"
#include <cstring>
#include <vector>

#ifndef CGE_VERSION_STR
#define CGE_VERSION_STR "0.0.0"     // 构建脚本未注入时的兜底
#endif

namespace fe_vstamp {

const char* kName = "VersionStamp";
const char* kCat  = "[VersionStamp]";

// ---- 三处 push imm32 落点（RVA = VA - 0x400000）----
// 004D189C  68 B4 88 50 00  push offset aSSD22dSJul1820   (主菜单版本模板)
// 004DB322  68 60 98 50 00  push offset aJul182005        (netlog Build 日期参数)
// 004DB2E7  68 70 98 50 00  push offset aNetworkLogBuil   (netlog 头)
constexpr uintptr_t R_MenuPush    = 0xD189C;
constexpr uintptr_t R_NetBuildPush= 0xDB322;
constexpr uintptr_t R_NetHeadPush = 0xDB2E7;

// ---- 校验原始字节（push offset 完整 5 字节）----
static const uint8_t kMenuPush[5]    = { 0x68, 0xB4, 0x88, 0x50, 0x00 };
static const uint8_t kNetBuildPush[5]= { 0x68, 0x60, 0x98, 0x50, 0x00 };
static const uint8_t kNetHeadPush[5] = { 0x68, 0x70, 0x98, 0x50, 0x00 };

// ---- DLL 提供的替换串（.rdata）----
// 主菜单版本模板：保留原 4 个占位符（sprintf 参数顺序不变），仅替换尾部字面量
static const char kMenuTemplate[] =
    "%s - %s %d.%2.2d - %s CulturesGameExtend v" CGE_VERSION_STR
    " (Build " __DATE__ " " __TIME__ ")";
// netlog "Build: %s\n" 的日期参数
static const char kBuildArg[] =
    "CulturesGameExtend v" CGE_VERSION_STR " (Build " __DATE__ " " __TIME__ ")";
// netlog 头
static const char kNetHeader[] =
    "Network log - CulturesGameExtend v" CGE_VERSION_STR
    " (Build " __DATE__ " " __TIME__ ")\n";

static bool VerifyBytes(DWORD base, uintptr_t rva, const uint8_t* exp, size_t n,
                        const char* what) {
    std::vector<uint8_t> cur = Patch::ReadBytes(base + rva, n);
    if (cur.size() == n && memcmp(cur.data(), exp, n) == 0) return true;
    LOG_ERROR(kCat, "verify FAILED: %s @0x%X (bytes mismatch)", what, base + rva);
    return false;
}

class VersionStampFeature : public Feature {
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }

    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        if (!cfg.GetBool(kName, "Enabled", false)) {
            LOG_INFO(kCat, "Disabled (Enabled=0)");
            return true;
        }
        gameapi::Init(ver.GetBaseAddress());
        const DWORD b = ver.GetBaseAddress();

        if (!VerifyBytes(b, R_MenuPush,     kMenuPush,     sizeof(kMenuPush),     "menu template push"))     return false;
        if (!VerifyBytes(b, R_NetBuildPush, kNetBuildPush, sizeof(kNetBuildPush), "netlog build push"))     return false;
        if (!VerifyBytes(b, R_NetHeadPush,  kNetHeadPush,  sizeof(kNetHeadPush),  "netlog header push"))    return false;

        // 把 push 的目标地址改写为 DLL 内字面串（立即数 @ +1，共 4 字节）
        if (!Patch::WriteU32(b + R_MenuPush + 1, (uint32_t)(uintptr_t)kMenuTemplate)) {
            LOG_ERROR(kCat, "patch menu template ptr failed");
            return false;
        }
        if (!Patch::WriteU32(b + R_NetBuildPush + 1, (uint32_t)(uintptr_t)kBuildArg)) {
            LOG_ERROR(kCat, "patch netlog build ptr failed");
            return false;
        }
        if (!Patch::WriteU32(b + R_NetHeadPush + 1, (uint32_t)(uintptr_t)kNetHeader)) {
            LOG_ERROR(kCat, "patch netlog header ptr failed");
            return false;
        }

        LOG_INFO(kCat, "installed: v%s (Build %s %s)",
                 CGE_VERSION_STR, __DATE__, __TIME__);
        LOG_INFO(kCat, "  menu@0x%X -> 0x%X | netlog@0x%X/0x%X -> 0x%X/0x%X",
                 (unsigned)(b + R_MenuPush), (unsigned)(uintptr_t)kMenuTemplate,
                 (unsigned)(b + R_NetBuildPush), (unsigned)(b + R_NetHeadPush),
                 (unsigned)(uintptr_t)kBuildArg, (unsigned)(uintptr_t)kNetHeader);
        return true;
    }
};

REGISTER_FEATURE(VersionStampFeature)

} // namespace fe_vstamp
