// CulturesPatchesFeature.cpp
// 对应 [CulturesPatches]。
// 核心：将 cultures-saga-patches 的补丁转为运行时内存注入。
// 分两类：
//  1) 纯字节 / 可确定性编码的简单汇编补丁 -> 由 plugins/config/patches/*.ini
//     数据文件驱动（格式：每行 `<addr> <hex> <hex> ...`，addr 相对 image base，
//     直接 WriteBytes(base + addr)）。
//  2) 复杂 code cave 补丁（内部含相对跳转/数据表/多 label）-> 动态构建机器码
//     写入 code cave 区域（默认 base + 0xf21cb 递增），再布置 hook。
//     所有相对跳转目标均为绝对游戏地址（0x400000+RVA）或 cave 内地址，
//     在运行时根据 cave 实际偏移动态计算 rel，保证与游戏基址无关。
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

// ---- code cave 分配器 ----
struct CaveAllocator {
    DWORD base;
    DWORD next;
    CaveAllocator(DWORD baseAddr, DWORD startOff) : base(baseAddr), next(startOff) {}
    DWORD Alloc(size_t size) { DWORD off = next; next += (DWORD)size; return off; }
};

// ---- 基础写入 / 校验 / 相对跳转 ----
bool WriteBytesAt(DWORD base, DWORD off, const void* data, size_t n) {
    return Patch::WriteBytes(base + off, (const uint8_t*)data, n);
}
bool VerifyBytes(DWORD base, DWORD off, const uint8_t* expected, size_t n) {
    std::vector<uint8_t> cur = Patch::ReadBytes(base + off, n);
    if (cur.size() != n) return false;
    return memcmp(cur.data(), expected, n) == 0;
}
// 写入 E9 rel32（从 fromOff 跳转到 toOff，均为相对 image base 的偏移）
bool WriteJmpRel(DWORD base, DWORD fromOff, DWORD toOff) {
    uint8_t b[5] = { 0xE9, 0, 0, 0, 0 };
    int32_t rel = (int32_t)((base + toOff) - (base + fromOff + 5));
    memcpy(&b[1], &rel, 4);
    return WriteBytesAt(base, fromOff, b, 5);
}
// 写入 E8 rel32（call 绝对目标 toOff）
bool WriteCallRel(DWORD base, DWORD fromOff, DWORD toOff) {
    uint8_t b[5] = { 0xE8, 0, 0, 0, 0 };
    int32_t rel = (int32_t)((base + toOff) - (base + fromOff + 5));
    memcpy(&b[1], &rel, 4);
    return WriteBytesAt(base, fromOff, b, 5);
}
// 用数据表初始化一段 cave（可含 0xEE 占位，随后由 WriteXxxRel 填充）
bool WriteRaw(DWORD base, DWORD off, const uint8_t* data, size_t n) {
    return WriteBytesAt(base, off, data, n);
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

// ===================================================================
// 复杂 code cave 补丁
// ===================================================================

// assistantCtrlClick：Shift+点击增/减助理请求量改为 ±10。
// cave 内容（相对 base+caveOff）：
//   ADD ESP,0xC ; MOV [EBP-0xC],EBX ; PUSH EAX ; MOV EAX,[0x554F40] ;
//   MOV EAX,[EAX] ; SHR EAX,1 ; TEST AL,1 ; JZ exit ;
//   IMUL EAX,[EBP-0xC],0xA ; MOV [EBP-0xC],EAX ; exit: POP EAX ; JMP 0xc0e51
bool ApplyAssistantCtrlClick(DWORD base, DWORD caveOff) {
    // 固定前缀 28 字节（到 POP EAX 为止）
    static const uint8_t pre[] = {
        0x83, 0xC4, 0x0C,                 // ADD ESP, 0xC
        0x89, 0x5D, 0xF4,                 // MOV [EBP-0xC], EBX
        0x50,                             // PUSH EAX
        0xA1, 0x40, 0x4F, 0x55, 0x00,     // MOV EAX, [0x554F40]
        0x8B, 0x00,                       // MOV EAX, [EAX]
        0xD1, 0xE8,                       // SHR EAX, 1
        0xA8, 0x01,                       // TEST AL, 1
        0x74, 0x07,                       // JZ +7 (-> POP EAX @ +27)
        0x6B, 0x45, 0xF4, 0x0A,           // IMUL EAX, [EBP-0xC], 0xA
        0x89, 0x45, 0xF4,                 // MOV [EBP-0xC], EAX
        0x58                              // POP EAX
    };                                    // 28 bytes, next = caveOff+28
    if (!WriteRaw(base, caveOff, pre, sizeof(pre))) { LOG_ERROR(kCat, "assistantCtrlClick: cave write failed"); return false; }
    // JMP 0xc0e51（动态 rel）
    if (!WriteJmpRel(base, caveOff + 28, 0xc0e51)) { LOG_ERROR(kCat, "assistantCtrlClick: cave jmp write failed"); return false; }
    DWORD multiplierAddr = caveOff + 6; // AssistentMultiplier

    // hook1 @0xc0e4b：JMP cave + NOP
    static const uint8_t incVerify[] = { 0x83, 0xC4, 0x0C, 0x89, 0x5D, 0xF4 };
    if (!VerifyBytes(base, 0xc0e4b, incVerify, sizeof(incVerify))) {
        LOG_WARN(kCat, "assistantCtrlClick: increment hook verify mismatch @0xc0e4b");
        return false;
    }
    if (!WriteJmpRel(base, 0xc0e4b, caveOff)) { LOG_ERROR(kCat, "assistantCtrlClick: hook1 write failed"); return false; }
    if (!WriteBytesAt(base, 0xc0e4b + 5, "\x90", 1)) { LOG_ERROR(kCat, "assistantCtrlClick: hook1 nop failed"); return false; }

    // hook2 @0xc0c04：JMP multiplier
    static const uint8_t decVerify[] = { 0xE9, 0x48, 0x02, 0x00, 0x00 };
    if (!VerifyBytes(base, 0xc0c04, decVerify, sizeof(decVerify))) {
        LOG_WARN(kCat, "assistantCtrlClick: decrement hook verify mismatch @0xc0c04");
        return false;
    }
    if (!WriteJmpRel(base, 0xc0c04, multiplierAddr)) { LOG_ERROR(kCat, "assistantCtrlClick: hook2 write failed"); return false; }

    LOG_INFO(kCat, "assistantCtrlClick applied (cave@0x%X)", caveOff);
    return true;
}

// multiplayerStability 线程休眠 cave：
//   PUSH 0xF ; CALL [0x4f305c] ; CALL 0xdf90d ; JMP 0xdf93e
bool ApplyMpThreadSleep(DWORD base, DWORD caveOff) {
    static const uint8_t pre[] = {
        0x6A, 0x0F,                       // PUSH 0xF
        0xFF, 0x15, 0x5C, 0x30, 0x4F, 0x00 // CALL [0x4f305c] (Sleep)
    };                                    // 8 bytes, next = caveOff+8
    if (!WriteRaw(base, caveOff, pre, sizeof(pre))) { LOG_ERROR(kCat, "mpThreadSleep: cave write failed"); return false; }
    if (!WriteCallRel(base, caveOff + 8, 0xdf90d)) { LOG_ERROR(kCat, "mpThreadSleep: call write failed"); return false; }
    if (!WriteJmpRel(base, caveOff + 13, 0xdf93e)) { LOG_ERROR(kCat, "mpThreadSleep: jmp write failed"); return false; }

    static const uint8_t hookVerify[] = { 0xE8, 0xCF, 0xFF, 0xFF, 0xFF };
    if (!VerifyBytes(base, 0xdf939, hookVerify, sizeof(hookVerify))) {
        LOG_WARN(kCat, "mpThreadSleep: hook verify mismatch @0xdf939");
        return false;
    }
    if (!WriteJmpRel(base, 0xdf939, caveOff)) { LOG_ERROR(kCat, "mpThreadSleep: hook write failed"); return false; }
    LOG_INFO(kCat, "multiplayerStability thread sleep applied (cave@0x%X)", caveOff);
    return true;
}

// multiplayerVersionCheck：
//   cave1: 版本警告（PUSH minor; PUSH major; PUSH 0x1e; JMP 0xd4736）
//   cave2/cave3（Host 版本检查、客户端发送版本）结构复杂且含多处交叉 label，
//   本版本仅实现版本警告部分，其余留待后续扩展。
bool ApplyMultiplayerVersionCheck(DWORD base, CaveAllocator& alloc) {
    DWORD off1 = alloc.Alloc(11);
    // PUSH minor(2); PUSH major(99); PUSH 0x1E(30); JMP rel32
    uint8_t v1[11] = { 0x6A, 0x02, 0x6A, 0x63, 0x6A, 0x1E, 0xE9, 0, 0, 0, 0 };
    if (!WriteRaw(base, off1, v1, 6)) { LOG_ERROR(kCat, "mpVersionCheck: cave1 write failed"); return false; }
    if (!WriteJmpRel(base, off1 + 6, 0xd4736)) { LOG_ERROR(kCat, "mpVersionCheck: cave1 jmp failed"); return false; }

    // hook1 @0xd4731：JMP cave1
    static const uint8_t v1hook[] = { 0x53, 0x6A, 0x01, 0x6A, 0x1E };
    if (!VerifyBytes(base, 0xd4731, v1hook, sizeof(v1hook))) {
        LOG_WARN(kCat, "mpVersionCheck: hook1 verify mismatch");
        return false;
    }
    if (!WriteJmpRel(base, 0xd4731, off1)) { LOG_ERROR(kCat, "mpVersionCheck: hook1 write failed"); return false; }

    LOG_INFO(kCat, "multiplayerVersionCheck applied (version warning cave@0x%X)", off1);
    return true;
}

class CulturesPatchesFeature : public Feature {
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }

    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        // 主开关：Enabled = 0 时全部禁止（纯字节补丁 + 所有 code cave 补丁）
        if (!cfg.GetBool(kName, "Enabled", true)) {
            LOG_INFO(kCat, "Disabled by master switch (Enabled=0); skipping all patches.");
            return true;
        }

        DWORD base = ver.GetBaseAddress();

        // 1) 纯字节补丁
        std::string dir = cfg.GetString(kName, "PatchDir",
            ge_paths::Join(ge_paths::kConfigDir, "patches"));
        ApplyPatchDir(dir, base);

        // 2) 复杂 code cave 补丁（各补丁独立开关）
        DWORD caveStart = (DWORD)cfg.GetInt(kName, "CodeCaveStart", 0xf21cb);
        CaveAllocator alloc(base, caveStart);

        if (cfg.GetBool("AssistantCtrlClick", "Enabled", false)) {
            DWORD caveOff = alloc.Alloc(33);
            ApplyAssistantCtrlClick(base, caveOff);
        }
        if (cfg.GetBool("MultiplayerStability", "Enabled", false) &&
            cfg.GetBool("MultiplayerStability", "ThreadSleep", false)) {
            DWORD caveOff = alloc.Alloc(18);
            ApplyMpThreadSleep(base, caveOff);
        }
        if (cfg.GetBool("MultiplayerVersionCheck", "Enabled", false)) {
            ApplyMultiplayerVersionCheck(base, alloc);
        }

        return true;
    }
};

REGISTER_FEATURE(CulturesPatchesFeature)

} // namespace fe_culturespatches
