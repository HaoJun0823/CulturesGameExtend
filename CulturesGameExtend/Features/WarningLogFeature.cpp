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
// WarningLogFeature.cpp
// ===================================================================
// [WarningLog] Log the in-game Warning! developer-assert box, and prevent it from
//   closing the game.
//
// Background (IDA confirmed sub_47A14D):
//   Warning! is essentially a developer assert dialog (MessageBoxA(..,0x31=exclaim
//   +OKCANCEL)). Return value = MessageBoxA(...) - 2; pressing Cancel(=2)->result=0
//   ->__debugbreak(int 3) -> with no debugger attached the process terminates
//   (game closes). Pressing OK(=1)->result=-1, no trigger.
//   User ask: 1) log these warning texts; 2) prevent the game from exiting
//   (Cancel no longer fatal).
//
// Approach:
//   1) ForbidExit: 1-byte NOP the int 3 at 0x47A1C3 (CC->90). On Cancel jnz no
//      longer jumps -> falls through NOP -> continues, process doesn't crash;
//      return value unchanged -> zero risk to all callers (safety net).
//   2) LogEnabled: hook the `call ds:MessageBoxA` inside sub_47A14D (0x47A1B8,
//      6 bytes); the stub reads the already-formatted lpText([ebp+8]) + caller
//      return address([ebp+4]) and logs them, then simulates an stdcall return of
//      IDOK(=1) and jumps back to 0x47A1BE to continue -- completely skipping the
//      dialog, not triggering int3, and preserving the original function's cursor
//      restoration side effects.
//   All Warning! callers (7 sites + vtable) go through this single MessageBoxA call
//   -> one hook covers everything.
//
// Config (plugins/config/CulturesGameExtend_Game.ini):
//   [WarningLog]
//   Enabled = 1     ; master switch (default on)
//   ForbidExit = 1  ; prevent exit (default on)
//   LogEnabled = 1  ; log to file (default on)
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

// Log text + caller to a dedicated file
void LogWarning(const char* text, void* caller) {
    if (!text) text = "(null)";
    WarnLogWrite("caller=0x%p text=%s", caller, text);
}

// 接管 0x47A1B8 的 `call ds:MessageBoxA`：记录 + 跳过弹框 + 假装按了 OK

// Take over the `call ds:MessageBoxA` at 0x47A1B8: log + skip dialog + pretend OK
extern "C" void __declspec(naked) WarnSuppressStub() {
    __asm {
        pushad
        mov  eax, [ebp+8]
// lpText（已是格式化串，如 "Can't set House near Position 12 34!!!"）
// lpText (already formatted, e.g. "Can't set House near Position 12 34!!!")
        mov  ecx, [ebp+4]
        push ecx
// sub_47A14D 的返回地址 = 哪个 Warning! 调用者
// sub_47A14D return address = which Warning! caller
        push eax
        call LogWarning
// 参数2: caller
// arg2: caller
        add  esp, 8
        popad
// 参数1: text
// arg1: text
        add  esp, 16
        mov  eax, 1
        mov  edx, 47A1BEh
        jmp  edx
    }
// 模拟 MessageBoxA(stdcall) 清 4 参(16B)
// simulate MessageBoxA(stdcall) clearing 4 args (16B)
}
class WarningLogFeature : public Feature {
// 假装玩家按了 OK(IDOK=1) -> dec eax;dec eax = -1 -> 不触发 int3
// pretend player pressed OK(IDOK=1) -> dec eax;dec eax = -1 -> no int3
public:
    const char* GetName() const override { return kName; }
// 续跑到 dec eax ...（跳过弹框）
// continue to dec eax ... (skip dialog)
    GameTarget  GetTarget() const override { return GameTarget::Game; }
    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        if (!cfg.GetBool(kName, "Enabled", true)) {
            LOG_INFO(kCat, "Disabled (Enabled=0)");


            return true;
        }
        bool forbid  = cfg.GetBool(kName, "ForbidExit", true);
        bool logging = cfg.GetBool(kName, "LogEnabled", true);


        DWORD b = ver.GetBaseAddress();
        if (logging) {
            WarnLogInit(ge_paths::kLogDir, "GameWarnings.log");
            uintptr_t callSite = b + (0x47A1B8 - 0x400000);
            auto cur = Patch::ReadBytes(callSite, 6);
            if (cur.size() == 6 && cur[0] == 0xFF && cur[1] == 0x15) {
                if (Patch::WriteJmp(callSite, (uintptr_t)&WarnSuppressStub, 1))
                    LOG_INFO(kCat, "installed: suppress+log Warning! box @0x%X (-> logs/GameWarnings.log)", (unsigned)callSite);
// 0x400000
// 0x400000
                else


                    LOG_ERROR(kCat, "WarningSuppress hook failed @0x%X", (unsigned)callSite);
            } else {
                LOG_WARN(kCat, "Warning! call-site bytes mismatch @0x%X (skip hook)", (unsigned)callSite);
            }
        }
                // 5 字节 E9 + 1 字节 NOP = 6 字节，完整覆盖 `call ds:MessageBoxA`
                // 5-byte E9 + 1-byte NOP = 6 bytes, fully covers `call ds:MessageBoxA`
        if (forbid) {
            if (Patch::WriteU8(b + (0x47A1C3 - 0x400000), 0x90))
                LOG_INFO(kCat, "forbid-exit: NOP'd int3 @0x47A1C3 (Cancel no longer kills)");
            else
                LOG_ERROR(kCat, "forbid-exit patch failed @0x%X", (unsigned)(b + 0x47A1C3 - 0x400000));
        }
        return true;
    }


};
            // 安全网：即便 hook 未装，Cancel 也不再致命
            // Safety net: even if the hook isn't installed, Cancel is no longer fatal
REGISTER_FEATURE(WarningLogFeature)
}