// CustomSafetyPatchesFeature.cpp
// ===================================================================
// [CustomSafety] — 自定义安全补丁（独立于上游 CulturesPatches，便于未来同步上游）
// Custom safety patches (independent of the upstream CulturesPatches, so upstream
// re-sync stays clean).
//
// 背景（崩溃复盘，Game.exe.46600.dmp）：
//   Game.exe 的 sub_493EF2 在计算建筑/单位动画缩放比例时有两条 div 分支：
//     v33==0 : edi = a8 * *v65 / a5   -> div [ebp+0x28]   @ 0x494484
//     v33==1 : 某比例 = ... % *v65     -> div [esi]        @ 0x494474
//   除数为 0 时触发 EXCEPTION_INT_DIVIDE_BY_ZERO (0xC0000094)。
//   根因：sub_492A93 的 type==2 建筑绘制分支把第 5 参(a5)硬编码为 0 传入 sub_493EF2；
//   当 [PerMapLogic] 启用（logic 全量加载）让某个建筑动画走进 v33==0 分支时即崩。
//   dump 实证：faulting Eip=0x494484，[ebp+0x28]=0，Eax=Edx=0。
//
// 修复策略（最小、通用）：
//   在 [PerMapLogic] 启用时，对两处 div 做纯字节防护——除数为 0 时把商/余数置 0，
//   直接跳过除法，不再触发 #DE。等价于“安全网”，不影响正常（除数非 0）路径。
//   本功能不改动上游 CulturesPatches 代码与 patches/ 数据，独立置于 custompatches/。
//
// 与触发开关的绑定（用户决策）：
//   触发源 = [PerMapLogic]（logic 全量加载）；防护 = 本功能。二者共用 [PerMapLogic]
//   开关，确保“触发(逻辑全开)”与“防护”严格对齐，消除一致性隐患。
//   另设 [CustomSafety] Enabled 作为本功能自身开关：默认 1；若要复现崩溃做测试，
//   可单独置 0（保留 [PerMapLogic]=1）让保护失效。
//
// 配置：
//   plugins/config/CulturesGameExtend_CustomSafety.ini  -> [CustomSafety] 段
//   plugins/config/custompatches/*.ini                   -> 纯字节补丁数据
//   补丁数据格式（对齐 CulturesPatches 的 ApplyPatchDir）：
//     <addr(相对基址偏移)> <新字节...> [ | <写入前校验的原字节...> ]
// ===================================================================
// CustomSafetyPatchesFeature.cpp
// ===================================================================
// [CustomSafety] -- custom safety patches (independent of upstream CulturesPatches)
//
// Crash recap (Game.exe.46600.dmp): sub_493EF2 divides by zero in two scaling
//   branches (div [ebp+0x28] @ 0x494484, div [esi] @ 0x494474); the building draw
//   path (sub_492A93 type==2) hard-codes the 5th arg a5=0, and [PerMapLogic] loading
//   logic makes a building animation enter the v33==0 branch -> #DE.
//
// Fix: when [PerMapLogic] is enabled, byte-guard both div sites so a zero divisor
//   yields quotient/remainder 0 instead of raising #DE. Pure byte patch, no code cave.
//
// Switch binding (user decision B): gate on [PerMapLogic]; plus a self [CustomSafety]
//   Enabled (default 1) so the guard can be toggled off independently (to reproduce
//   the crash for testing).
//
// Config: plugins/config/CulturesGameExtend_CustomSafety.ini ([CustomSafety]);
//   patch data in plugins/config/custompatches/*.ini.
// ===================================================================
#include "pch.h"
#include "Core/Feature.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Patch.h"
#include "Core/Logger.h"
#include "Core/fs_compat.h"
#include "Core/Paths.h"
#include <windows.h>
#include <sstream>
#include <fstream>
#include <vector>
#include <algorithm>

namespace fe_customsafety {

const char* kName = "CustomSafety";
const char* kCat  = "[CustomSafety]";

// ---- 写入前校验原字节（防止错误数据写坏代码） ----
// verify original bytes before writing (guard against corrupting code with bad data)
bool VerifyBytes(DWORD base, DWORD off, const uint8_t* expected, size_t n) {
    std::vector<uint8_t> cur = Patch::ReadBytes(base + off, n);
    if (cur.size() != n) return false;
    return memcmp(cur.data(), expected, n) == 0;
}

// ---- 纯字节补丁数据文件夹应用（格式：`<addr> <new> [ | <verify> ]`） ----
// apply patch data directory (format: `<addr> <new> [ | <verify> ]`)
bool ApplyCustomPatchDir(const std::string& dir, DWORD base) {
    std::error_code ec;
    if (!ge::fs::exists(dir, ec)) {
        LOG_WARN(kCat, "Patch dir not found: %s", dir.c_str());
        return true;
    }

    std::vector<std::string> files;
    try {
        for (auto& e : ge::fs::directory_iterator(dir, ec)) {
            if (!ge::fs::is_regular_file(e.path(), ec)) continue;
            std::string ext = e.path().extension().string();
            for (auto& c : ext) c = (char)tolower((unsigned char)c);
            if (ext == ".ini")
                files.push_back(e.path().string());
        }
    } catch (...) {
        LOG_ERROR(kCat, "Failed to enumerate dir: %s", dir.c_str());
        return false;
    }
    std::sort(files.begin(), files.end());

    size_t applied = 0, skipped = 0;
    for (auto& f : files) {
        std::ifstream in(f);
        if (!in.is_open()) { LOG_ERROR(kCat, "Cannot open %s", f.c_str()); continue; }
        LOG_INFO(kCat, "Parsing patch file %s", f.c_str());
        size_t lineNo = 0;
        std::string line;
        while (std::getline(in, line)) {
            ++lineNo;
            ge::StripUtf8Bom(line);
            // 剥离行内注释（';' 或 '#' 之后全部丢弃），支持 `addr hex ; 说明`
            // strip inline comments (after ';' or '#'), supports `addr hex ; note`
            size_t cmt = line.find_first_of(";#");
            if (cmt != std::string::npos) line = line.substr(0, cmt);
            size_t b = line.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) continue;
            line = line.substr(b);
            if (line.empty()) continue;

            std::string newPart = line, verifyPart;
            // 按 `|` 切出 new / verify 两段（verify 可选）
            // split new / verify by `|` (verify optional)
            size_t bar = line.find('|');
            if (bar != std::string::npos) {
                newPart = line.substr(0, bar);
                verifyPart = line.substr(bar + 1);
            }
            std::istringstream iss(newPart);
            std::string tok;
            if (!(iss >> tok)) continue;
            std::string at = tok;
            if (at.size() >= 2 && at[0] == '0' && (at[1] == 'x' || at[1] == 'X')) at = at.substr(2);
            char* end = nullptr;
            unsigned long long v = _strtoui64(at.c_str(), &end, 16);
            if (end == at.c_str() || v > 0xFFFFFFFFULL) {
                LOG_WARN(kCat, "%s:%zu bad address '%s'", f.c_str(), lineNo, tok.c_str());
                ++skipped; continue;
            }
            DWORD off = (DWORD)v;
            std::vector<uint8_t> bytes;
            while (iss >> tok) {
                std::string bt = tok;
                if (bt.size() >= 2 && bt[0] == '0' && (bt[1] == 'x' || bt[1] == 'X')) bt = bt.substr(2);
                char* be = nullptr;
                long bv = strtol(bt.c_str(), &be, 16);
                if (be == bt.c_str() || bv < 0 || bv > 255) {
                    bytes.clear(); break;
                }
                bytes.push_back((uint8_t)bv);
            }
            if (bytes.empty()) { ++skipped; continue; }

            if (!verifyPart.empty()) {
                // 可选 verify：写入前校验原字节，不符则跳过（防错误数据写坏代码）
                // optional verify: check original bytes before writing; skip on mismatch
                std::vector<uint8_t> verify;
                std::istringstream vis(verifyPart);
                while (vis >> tok) {
                    std::string bt = tok;
                    if (bt.size() >= 2 && bt[0] == '0' && (bt[1] == 'x' || bt[1] == 'X')) bt = bt.substr(2);
                    char* be = nullptr;
                    long bv = strtol(bt.c_str(), &be, 16);
                    if (be == bt.c_str() || bv < 0 || bv > 255) { verify.clear(); break; }
                    verify.push_back((uint8_t)bv);
                }
                if (!verify.empty() && !VerifyBytes(base, off, verify.data(), verify.size())) {
                    LOG_WARN(kCat, "%s:%zu verify mismatch @0x%X, skipped", f.c_str(), lineNo, base + off);
                    ++skipped; continue;
                }
            }
            if (!Patch::WriteBytes(base + off, bytes.data(), bytes.size())) {
                LOG_ERROR(kCat, "%s:%zu write failed @0x%X (%zu bytes)", f.c_str(), lineNo, base + off, bytes.size());
                ++skipped; continue;
            }
            ++applied;
            LOG_INFO(kCat, "@0x%X <- %zu byte(s)", base + off, bytes.size());
        }
    }
    LOG_INFO(kCat, "Patch dir done: %zu applied, %zu skipped", applied, skipped);
    return true;
}

class CustomSafetyFeature : public Feature {
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }
    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        // 触发开关绑定 [PerMapLogic]：仅当 logic 全量加载（崩溃路径）时启用防护。
        // master switch bound to [PerMapLogic]: guard only when the crash path is live.
        if (!cfg.GetBool("PerMapLogic", "Enabled", false)) {
            LOG_INFO(kCat, "PerMapLogic disabled -> safety guard inactive (no crash path).");
            return true;
        }
        // 本功能自身开关（默认开；置 0 可在保留 [PerMapLogic]=1 时复现崩溃用于测试）。
        // self switch (default on; set 0 to reproduce the crash while keeping [PerMapLogic]=1).
        if (!cfg.GetBool(kName, "Enabled", true)) {
            LOG_INFO(kCat, "Disabled by [CustomSafety] Enabled=0.");
            return true;
        }

        DWORD base = ver.GetBaseAddress();
        std::string rel = cfg.GetString(kName, "PatchDir",
            ge_paths::Join(ge_paths::kConfigDir, "custompatches"));
        std::string dir = ge_paths::Resolve(rel.c_str());
        LOG_INFO(kCat, "Active (bound to [PerMapLogic] Enabled=1). PatchDir=%s -> %s",
                 rel.c_str(), dir.c_str());
        ApplyCustomPatchDir(dir, base);
        return true;
    }
};
REGISTER_FEATURE(CustomSafetyFeature)
}
