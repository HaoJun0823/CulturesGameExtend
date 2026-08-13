// WarningLogFeature.cpp
// ===================================================================
// [WarningLog] 记录游戏内 Warning! 开发者断言框，并禁止其关闭游戏。
//
// 背景（IDA 定案 sub_47A14D）：
//   Warning! 封装本质是开发者 assert 对话框（MessageBoxA(..,0x31=惊叹+OKCANCEL)）。
//   返回 result = MessageBoxA(...) - 2；按 Cancel(=2)→result=0→__debugbreak(int 3)
//   → 无调试器挂接时进程直接终止（游戏关闭）。按 OK(=1)→result=-1 不触发。
//   用户诉求：① 在日志里记录这些 warning 文本；② 禁止游戏退出（Cancel 不再致命）。
//
// 方案：
//   1) ForbidExit：1 字节 NOP 掉 0x47A1C3 的 int 3(CC→90)。Cancel 时 jnz 不跳 →
//      落到 nop → 继续，进程不崩；返回值不变 → 对所有调用者零风险（安全网）。
//   2) LogEnabled：hook sub_47A14D 内的 `call ds:MessageBoxA`（0x47A1B8，6 字节），
//      接管 stub 读取已格式化的 lpText([ebp+8]) + 调用者返回地址([ebp+4]) 写日志，
//      然后模拟 stdcall 返回 IDOK(=1) 并跳回 0x47A1BE 续跑 —— 完全跳过弹框，
//      且不触发 int3，且保留原函数的光标恢复等副作用。
//   所有 Warning! 调用者（7 处 + 虚表）都经此唯一 MessageBoxA 调用，一次 hook 全覆盖。
//
// 配置（plugins/config/CulturesGameExtend_Game.ini）：
//   [WarningLog]
//   Enabled = 1     ; 总开关（默认开）
//   ForbidExit = 1  ; 禁止退出（默认开）
//   LogEnabled = 1  ; 记录日志（默认开）
// ===================================================================
#include "pch.h"
#include "Core/Feature.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Patch.h"
#include "Core/Logger.h"
#include "Core/Paths.h"
#include <cstring>

namespace fe_warn {

const char* kName = "WarningLog";
const char* kCat  = "[WarningLog]";

// 文本 + 调用者写独立日志
void LogWarning(const char* text, void* caller) {
    if (!text) text = "(null)";
    WarnLogWrite("caller=0x%p text=%s", caller, text);
}

// 接管 0x47A1B8 的 `call ds:MessageBoxA`：记录 + 跳过弹框 + 假装按了 OK
extern "C" void __declspec(naked) WarnSuppressStub() {
    __asm {
        pushad
        mov  eax, [ebp+8]      // lpText（已是格式化串，如 "Can't set House near Position 12 34!!!"）
        mov  ecx, [ebp+4]      // sub_47A14D 的返回地址 = 哪个 Warning! 调用者
        push ecx               // 参数2: caller
        push eax               // 参数1: text
        call LogWarning
        add  esp, 8
        popad
        add  esp, 16           // 模拟 MessageBoxA(stdcall) 清 4 参(16B)
        mov  eax, 1            // 假装玩家按了 OK(IDOK=1) -> dec eax;dec eax = -1 -> 不触发 int3
        mov  edx, 47A1BEh      // 续跑到 dec eax ...（跳过弹框）
        jmp  edx
    }
}

class WarningLogFeature : public Feature {
public:
    const char* GetName() const override { return kName; }
    GameTarget  GetTarget() const override { return GameTarget::Game; }

    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        if (!cfg.GetBool(kName, "Enabled", true)) {
            LOG_INFO(kCat, "Disabled (Enabled=0)");
            return true;
        }
        bool forbid  = cfg.GetBool(kName, "ForbidExit", true);
        bool logging = cfg.GetBool(kName, "LogEnabled", true);
        DWORD b = ver.GetBaseAddress();   // 0x400000

        if (logging) {
            WarnLogInit(ge_paths::kLogDir, "GameWarnings.log");
            uintptr_t callSite = b + (0x47A1B8 - 0x400000);
            auto cur = Patch::ReadBytes(callSite, 6);
            if (cur.size() == 6 && cur[0] == 0xFF && cur[1] == 0x15) {
                // 5 字节 E9 + 1 字节 NOP = 6 字节，完整覆盖 `call ds:MessageBoxA`
                if (Patch::WriteJmp(callSite, (uintptr_t)&WarnSuppressStub, 1))
                    LOG_INFO(kCat, "installed: suppress+log Warning! box @0x%X (-> logs/GameWarnings.log)", (unsigned)callSite);
                else
                    LOG_ERROR(kCat, "WarningSuppress hook failed @0x%X", (unsigned)callSite);
            } else {
                LOG_WARN(kCat, "Warning! call-site bytes mismatch @0x%X (skip hook)", (unsigned)callSite);
            }
        }

        if (forbid) {
            // 安全网：即便 hook 未装，Cancel 也不再致命
            if (Patch::WriteU8(b + (0x47A1C3 - 0x400000), 0x90))
                LOG_INFO(kCat, "forbid-exit: NOP'd int3 @0x47A1C3 (Cancel no longer kills)");
            else
                LOG_ERROR(kCat, "forbid-exit patch failed @0x%X", (unsigned)(b + 0x47A1C3 - 0x400000));
        }
        return true;
    }
};

REGISTER_FEATURE(WarningLogFeature)

} // namespace fe_warn
