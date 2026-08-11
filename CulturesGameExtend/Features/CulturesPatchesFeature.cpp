// CulturesPatchesFeature.cpp
// 对应 [CulturesPatches]。
// 核心：将 cultures-saga-patches 的补丁转为运行时内存注入。
// 纯字节 / 可确定性编码的简单汇编补丁 -> 由 plugins/config/patches/*.ini
//   数据文件驱动（格式：每行 `<addr> <hex> <hex> ...`，addr 相对 image base，
//   直接 WriteBytes(base + addr)）。
// 所有 patches/*.ini 由 [CulturesPatches] 总开关统一控制（Enabled=0 即全部停用），
// 单个补丁无需独立开关——社区可直接增删 patches/ 下的 ini 文件。
#include "pch.h"
#include "Core/Feature.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Patch.h"
#include "Core/Logger.h"
#include "Core/fs_compat.h"
#include "Core/Paths.h"
#include <sstream>
#include <fstream>

namespace fe_culturespatches {

const char* kName = "CulturesPatches";
const char* kCat  = "[CulturesPatches]";

// ---- 校验原字节（写入前比对，防止错误数据写坏代码） ----
bool VerifyBytes(DWORD base, DWORD off, const uint8_t* expected, size_t n) {
    std::vector<uint8_t> cur = Patch::ReadBytes(base + off, n);
    if (cur.size() != n) return false;
    return memcmp(cur.data(), expected, n) == 0;
}

// ---- 补丁数据文件应用（纯字节；可选 verify 列） ----
// ini 行格式（对齐 Python 版 verify_and_replace 语义）：
//   <addr> <new bytes> [ | <verify bytes> ]
//   - new bytes     ：要写入的字节（必填）
//   - verify bytes  ：写入前先校验的原字节（可选；不带 `|` 的行保持旧行为直接写入）。
//                     校验不符则跳过该行并告警，防止错误数据破坏游戏代码。
bool ApplyPatchDir(const std::string& dir, DWORD base) {
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
        size_t fileApplied = 0, lineNo = 0;
        std::string line;
        while (std::getline(in, line)) {
            ++lineNo;
            ge::StripUtf8Bom(line);
            // 剥离行内注释（';' 或 '#' 之后的全部丢弃），支持 `addr hex ; 说明`
            size_t cmt = line.find_first_of(";#");
            if (cmt != std::string::npos) line = line.substr(0, cmt);
            size_t b = line.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) continue; // 空行 / 纯注释行
            line = line.substr(b);
            if (line.empty()) continue;

            // 按 `|` 切出 new / verify 两段（verify 可选）
            std::string newPart = line, verifyPart;
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
                    LOG_WARN(kCat, "%s:%zu bad byte '%s'", f.c_str(), lineNo, tok.c_str());
                    bytes.clear(); break;
                }
                bytes.push_back((uint8_t)bv);
            }
            if (bytes.empty()) { ++skipped; continue; }

            // 可选 verify：写入前校验原字节，不符则跳过（防止错误数据写坏代码）
            if (!verifyPart.empty()) {
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
            ++applied; ++fileApplied;
            LOG_DEBUG(kCat, "@0x%X <- %zu byte(s)", base + off, bytes.size());
        }
        LOG_INFO(kCat, "%s: %zu patch(es) applied", f.c_str(), fileApplied);
    }
    LOG_INFO(kCat, "Patch dir done: %zu applied, %zu skipped", applied, skipped);
    return true;
}

class CulturesPatchesFeature : public Feature {
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }

    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        // 总开关：Enabled = 0 时禁用全部补丁（patches/*.ini 不会加载）。
        if (!cfg.GetBool(kName, "Enabled", true)) {
            LOG_INFO(kCat, "Disabled by master switch (Enabled=0); skipping all patches.");
            return true;
        }

        DWORD base = ver.GetBaseAddress();

        // 纯字节补丁（patches/*.ini 全部加载，社区可直接增删文件，无需单独开关）
        std::string dir = cfg.GetString(kName, "PatchDir",
            ge_paths::Join(ge_paths::kConfigDir, "patches"));
        ApplyPatchDir(dir, base);

        return true;
    }
};

REGISTER_FEATURE(CulturesPatchesFeature)

} // namespace fe_culturespatches
