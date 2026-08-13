// CulturesPatchesFeature.cpp
// 对应 [CulturesPatches]。
// 核心：将 cultures-saga-patches 的补丁转为运行时内存注入。
// 纯字节 / 可确定性编码的简单汇编补丁 -> 由 plugins/config/patches/*.ini
//   数据文件驱动（格式：每行 `<addr> <hex> <hex> ...`，addr 相对 image base，
//   直接 WriteBytes(base + addr)）。
// 所有 patches/*.ini 由 [CulturesPatches] 总开关统一控制（Enabled=0 即全部停用），
// 单个补丁无需独立开关——社区可直接增删 patches/ 下的 ini 文件。
// CulturesPatchesFeature.cpp
// Corresponds to [CulturesPatches].
// Core: convert cultures-saga-patches patches into runtime in-memory injection.
// Pure-byte / deterministically-encoded simple assembly patches -> driven by data files
//   plugins/config/patches/*.ini (format: each line `<addr> <hex> <hex> ...`, addr is relative to
//   the image base, written directly via WriteBytes(base + addr)).
// All patches/*.ini are governed by the single [CulturesPatches] master switch (Enabled=0 disables
// everything); individual patches need no separate switch — the community can add/remove ini files
// under patches/ directly.
#include "pch.h"
#include "Core/Feature.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Patch.h"
#include "Core/Logger.h"
#include "Core/fs_compat.h"
#include "Core/Paths.h"
#include "caves_blob.h"
#include "cave_trampolines.h"
#include <windows.h>
#include <sstream>
#include <fstream>


namespace fe_culturespatches {


const char* kName = "CulturesPatches";
const char* kCat  = "[CulturesPatches]";

// ---- 校验原字节（写入前比对，防止错误数据写坏代码） ----

#pragma region Byte Verify & Patch-File Application
// ---- verify original bytes (compare before writing, to avoid corrupting code with bad data) ----
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

// ---- apply patch data files (pure bytes; optional verify column) ----
// ini line format (aligned with the Python verify_and_replace semantics):
//   <addr> <new bytes> [ | <verify bytes> ]
//   - new bytes     : bytes to write (required)
//   - verify bytes  : original bytes to verify before writing (optional; a line without `|` keeps
//       the old behavior of writing directly). On mismatch the line is skipped with a warning,
//       preventing bad data from corrupting game code.
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
            // strip inline comments (everything after ';' or '#'), supports `addr hex ; note`
            size_t cmt = line.find_first_of(";#");
            if (cmt != std::string::npos) line = line.substr(0, cmt);
            size_t b = line.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) continue;
// 空行 / 纯注释行
// blank / comment-only line
            line = line.substr(b);
            if (line.empty()) continue;
            std::string newPart = line, verifyPart;

            // 按 `|` 切出 new / verify 两段（verify 可选）

            // split new / verify parts by `|` (verify optional)
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
            if (!verifyPart.empty()) {

            // 可选 verify：写入前校验原字节，不符则跳过（防止错误数据写坏代码）

            // optional verify: check original bytes before writing; on mismatch skip (avoid corruption)
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
bool ApplyCodeCaves(DWORD base) {

// ---- 运行时代码洞穴（与 Python add_code_cave 布局完全一致） ----
// 洞穴区位于文件偏移 0xF21CB（VA = base + 0xF21CB）。该页在 PE 加载时
// 被加载器「保留但未提交」，对它做 VirtualAlloc(MEM_COMMIT) 会返回
// ERROR_ACCESS_DENIED(5)，因此不能在映像地址上提交。
// 正确做法：运行时 VirtualAlloc(NULL) 申请一块独立可执行页放置洞穴，
// 由于 Game.exe 非 LAA（Characteristics 无 0x0020），该地址必 < 0x80000000，
// 8 个跳板的 E9 rel32（±2GB）完全够用。再把 blob 内所有「落在洞穴窗口
// [caveVA, caveVA+blob) 的绝对地址」整体加上 (X - caveVA) 完成重定位，
// 最后把 patches 的跳板(E9)在运行时改写为指向 X + caveOff。
// blob 由 utils/compiler(Keystone) 编译、regen.py 提取并烘焙进 caves_blob.h；
// 跳板表在 cave_trampolines.h（regen.py 生成）。
#pragma endregion

#pragma region Runtime Code Cave (relocation)
// ---- runtime code cave (layout identical to the Python add_code_cave) ----
// The cave region sits at file offset 0xF21CB (VA = base + 0xF21CB). That page is "reserved but not
// committed" by the loader at PE load time; doing VirtualAlloc(MEM_COMMIT) on it returns
// ERROR_ACCESS_DENIED(5), so it cannot be committed at the image address.
// Correct approach: at runtime VirtualAlloc(NULL) an independent executable page to hold the cave.
// Since Game.exe is not LAA (Characteristics lacks 0x0020), that address is necessarily < 0x80000000,
// and the 8 trampolines' E9 rel32 (±2GB) are more than enough. Then add (X - caveVA) to every
// "absolute address inside the cave window [caveVA, caveVA+blob)" inside the blob to complete
// relocation, and finally rewrite the patches' trampolines (E9) at runtime to point at X + caveOff.
// The blob is compiled by utils/compiler (Keystone), extracted and baked into caves_blob.h by regen.py;
// the trampoline table is in cave_trampolines.h (generated by regen.py).
    const DWORD caveVA = base + kCaveOffset;
    void* p = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
// 0x4F21CB
// 0x4F21CB
    if (!p) {

    // 1) 申请独立可执行页（不占用映像地址，规避 ACCESS_DENIED）

    // 1) allocate an independent executable page (does not occupy the image address, avoids ACCESS_DENIED)
        LOG_ERROR(kCat, "Code cave alloc failed (err=%u)", (unsigned)GetLastError());
        return false;
    }
    DWORD_PTR X    = (DWORD_PTR)p;
    DWORD_PTR delta = X - (DWORD_PTR)caveVA;
    memcpy((void*)X, kCaveBlob, kCaveBlobSize);
    DWORD imageSize = 0;

    // 2) 复制 blob 并对洞穴内引用重定位

    // 2) copy the blob and relocate references inside the cave
    BYTE* pBase = (BYTE*)base;

    // Game.exe 映像上界（用于判定“外部引用”）：cave 页里的相对跳转若目标落在
    // 映像范围内（即 Game.exe 自身代码/数据），属于指向 Game.exe 的外部引用，需重定位。

    // Game.exe image upper bound (used to decide "external reference"): a relative jump inside the
    // cave page whose target falls within the image range (i.e. Game.exe's own code/data) is an
    // external reference to Game.exe and must be relocated.
    if (pBase[0] == 'M' && pBase[1] == 'Z') {
        DWORD e_lfanew = *(DWORD*)(pBase + 0x3C);
        imageSize = *(DWORD*)(pBase + e_lfanew + 0x50);
    }
    if (imageSize == 0) imageSize = 0x200000;
// PE32 OptionalHeader.SizeOfImage
// PE32 OptionalHeader.SizeOfImage
    for (size_t i = 0; i + 4 <= kCaveBlobSize; ++i) {
        DWORD v;
        memcpy(&v, (const void*)(X + i), 4);
// 兜底：足够覆盖本游戏映像
// fallback: large enough to cover this game's image
        if (v >= caveVA && v < caveVA + (DWORD)kCaveBlobSize) {

    // 2a) 绝对引用重定位：blob 内 4 字节绝对地址落在 [caveVA, caveVA+blob) 的视为
    //     “洞穴内部引用”，整体随洞穴搬移 +delta。外部绝对地址（Game.exe VA）保持不变
    //     （Game.exe 基址固定，原值即有效）。

    // 2a) absolute-reference relocation: a 4-byte absolute address inside the blob that falls in
    //     [caveVA, caveVA+blob) is treated as an "intra-cave reference" and moves with the cave (+delta).
    //     External absolute addresses (Game.exe VA) stay unchanged (Game.exe base is fixed, original
    //     value is already valid).
            v += (DWORD)delta;
            memcpy((void*)(X + i), &v, 4);
        }
    }
    for (size_t i = 0; i + 5 < kCaveBlobSize; ++i) {
        BYTE op;
        memcpy(&op, (const void*)(X + i), 1);
        int   ilen = 0;

    // 2b) 相对分支重定位：E9/E8/Jcc(0F 80..8F) 的 rel32 若原始目标落在 Game.exe 映像
    //     （外部引用）而非洞穴内部，则 rel32 -= delta，使运行时仍跳回真实 Game.exe 地址。
    //     目标在洞穴内部者保持不动（相对位移与基址无关，搬移后自洽）。
    //     既非内部也非映像内者当作数据，绝不修改（防误把数据中的 0xE9 当跳转）。

    // 2b) relative-branch relocation: the rel32 of E9/E8/Jcc(0F 80..8F) whose original target falls in
    //     the Game.exe image (external reference) rather than inside the cave gets rel32 -= delta, so at
    //     runtime it still jumps back to the real Game.exe address.
    //     Targets inside the cave are left untouched (relative offset is base-independent, self-consistent
    //     after relocation). Anything neither intra-cave nor within the image is treated as data and never
    //     modified (avoids misreading a 0xE9 inside data as a jump).
        size_t relOff = 0;
        if (op == 0xE9 || op == 0xE8) {
            ilen = 5; relOff = i + 1;
        } else if (op == 0x0F) {
            BYTE op2;
            if (i + 6 > kCaveBlobSize) continue;
// JMP/CALL rel32
// JMP/CALL rel32
            memcpy(&op2, (const void*)(X + i + 1), 1);
            if ((op2 & 0xF0) == 0x80) {
                ilen = 6; relOff = i + 2;
            } else {
                continue;
            }
        } else {
// Jcc rel32 (0F 80..8F)
// Jcc rel32 (0F 80..8F)
            continue;
        }
        DWORD rel;
        memcpy(&rel, (const void*)(X + relOff), 4);
        DWORD_PTR origTarget = (DWORD_PTR)caveVA + i + ilen + (int)rel;
        bool intra  = (origTarget >= caveVA && origTarget < caveVA + (DWORD)kCaveBlobSize);
        bool extImg = (origTarget >= base && origTarget < base + imageSize);
        if (intra) continue;
        if (!extImg) continue;
        rel -= (DWORD)delta;
        // 原始目标（以 caveVA 为基址，不受上面绝对重定位影响）
        // original target (based at caveVA, not affected by the absolute relocation above)
        memcpy((void*)(X + relOff), &rel, 4);
    }
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)X, kCaveBlobSize);
    LOG_INFO(kCat, "Code cave committed @0x%p (blob %zu bytes, src VA 0x%X)",
// 洞穴内部相对跳：保持不动
// intra-cave relative jump: leave untouched
             (void*)X, kCaveBlobSize, caveVA);
    for (size_t t = 0; t < kCaveTrampolineCount; ++t) {
// 非内部也非映像内：当作数据，不碰
// neither intra-cave nor in-image: treat as data, don't touch
        const CaveTrampoline& tr = kCaveTrampolines[t];
        DWORD siteVA = base + tr.site;
// 外部引用：补偿基址差，跳回真实 Game.exe 地址
// external reference: compensate base difference, jump back to real Game.exe address
        DWORD target = (DWORD)(X + tr.caveOff);
        DWORD rel = target - (siteVA + 5);
        BYTE buf[5];


        buf[0] = 0xE9;
        memcpy(&buf[1], &rel, 4);
        if (tr.verifyLen > 0) {

    // 3) 运行时改写跳板：E9 rel32 = (X + caveOff) - (siteVA + 5)

    // 3) rewrite trampolines at runtime: E9 rel32 = (X + caveOff) - (siteVA + 5)
            std::vector<uint8_t> cur = Patch::ReadBytes(siteVA, tr.verifyLen);
            bool ok = (cur.size() == (size_t)tr.verifyLen);
            for (int k = 0; ok && k < tr.verifyLen; ++k)
                if (cur[k] != tr.verify[k]) ok = false;
            if (!ok) {


                LOG_WARN(kCat, "Trampoline verify mismatch @0x%X (skip)", siteVA);
                continue;
            }

        // 校验原字节（可选，防误写）

        // verify original bytes (optional, guard against accidental writes)
        }
        DWORD old;
        if (!VirtualProtect((LPVOID)siteVA, 5, PAGE_EXECUTE_READWRITE, &old)) {
            LOG_ERROR(kCat, "Trampoline VP failed @0x%X (err=%u)", siteVA, (unsigned)GetLastError());
            return false;
        }
        memcpy((LPVOID)siteVA, buf, 5);
        VirtualProtect((LPVOID)siteVA, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)siteVA, 5);
    }


    LOG_INFO(kCat, "Applied %zu trampolines", kCaveTrampolineCount);
    return true;
}
class CulturesPatchesFeature : public Feature {
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }
    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        if (!cfg.GetBool(kName, "Enabled", true)) {
            LOG_INFO(kCat, "Disabled by master switch (Enabled=0); skipping all patches.");
            return true;
        }

#pragma endregion

#pragma region Feature Install
        DWORD base = ver.GetBaseAddress();
        if (!ApplyCodeCaves(base)) {
            LOG_ERROR(kCat, "Code cave setup failed; aborting all patch application.");
            return false;


        }
        // 总开关：Enabled = 0 时禁用全部补丁（patches/*.ini 不会加载）。
        // master switch: Enabled = 0 disables all patches (patches/*.ini won't load).
        std::string dir = cfg.GetString(kName, "PatchDir",
            ge_paths::Join(ge_paths::kConfigDir, "patches"));
        ApplyPatchDir(dir, base);
        return true;


    }

        // 运行时代码洞穴：必须先于字节补丁提交，否则 patches/*.ini 中的
        // 跳板(E9)会跳入未提交页而崩溃。洞穴失败时中止全部补丁以免运行期崩溃。

        // runtime code cave: must be committed before the byte patches, otherwise the trampolines
        // (E9) in patches/*.ini would jump into an uncommitted page and crash. On cave failure, abort
        // all patches to avoid a runtime crash.
};
REGISTER_FEATURE(CulturesPatchesFeature)
}
#pragma endregion
