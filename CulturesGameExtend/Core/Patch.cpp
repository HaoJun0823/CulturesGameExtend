#include "pch.h"
#include "Patch.h"
#include "Logger.h"


namespace {


#pragma region "Helpers (ProtectAndWrite)"
const char* kCategory = "[Patch]";


bool ProtectAndWrite(uintptr_t addr, const uint8_t* data, size_t size) {
    if (addr == 0 || size == 0) return false;
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(addr), size,
                        PAGE_EXECUTE_READWRITE, &old)) {
        LOG_ERROR(kCategory, "VirtualProtect failed at %p (err=%lu)", (void*)addr, GetLastError());
        return false;
    }
    memcpy(reinterpret_cast<void*>(addr), data, size);
    // 刷新指令缓存，避免 CPU 仍执行旧指令
    // Flush the instruction cache so the CPU doesn't keep executing old
    // instructions.
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), size);
    // 恢复原始页保护
    // Restore the original page protection.
    DWORD tmp = 0;
    VirtualProtect(reinterpret_cast<LPVOID>(addr), size, old, &tmp);
    return true;
}

#pragma endregion

}
// namespace
// namespace
namespace Patch {


bool WriteJmp(uintptr_t target, uintptr_t cave, int nopCount) {


#pragma region "WriteJmp / WriteBytes / scalar writers"
    if (target == 0 || cave == 0) return false;
    int32_t rel = static_cast<int32_t>(cave - (target + 5));

    // E9 + rel32，rel32 = cave - (target + 5)

    // E9 + rel32, rel32 = cave - (target + 5)
    uint8_t buf[5 + 16];
    buf[0] = 0xE9;
// 5 字节 jmp + 最多 16 字节 NOP 填充
// 5-byte jmp + up to 16 bytes of NOP padding
    memcpy(&buf[1], &rel, 4);
    int nops = nopCount > 0 ? nopCount : 0;
    if (nops > 16) nops = 16;


    for (int i = 0; i < nops; ++i) buf[5 + i] = 0x90;
    LOG_DEBUG(kCategory, "WriteJmp target=%p cave=%p nop=%d", (void*)target, (void*)cave, nops);
    return ProtectAndWrite(target, buf, static_cast<size_t>(5 + nops));


}
bool WriteBytes(uintptr_t addr, const uint8_t* data, size_t size) {
    return ProtectAndWrite(addr, data, size);


}
bool WriteU8(uintptr_t addr, uint8_t v) {
    return ProtectAndWrite(addr, &v, 1);


}
bool WriteU32(uintptr_t addr, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),


                     (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF) };
    return ProtectAndWrite(addr, b, 4);
}
bool WritePointer(uintptr_t addr, uintptr_t ptr) {
    return WriteU32(addr, static_cast<uint32_t>(ptr));


}
    // 32 位进程：指针本身是 4 字节
    // 32-bit process: the pointer itself is 4 bytes
std::vector<uint8_t> ReadBytes(uintptr_t addr, size_t size) {
    std::vector<uint8_t> out;

#pragma endregion

#pragma region "ReadBytes / ReadMemory / WriteMemory / EnsureWritable"
    if (addr == 0 || size == 0) return out;
    out.resize(size);
    memcpy(out.data(), reinterpret_cast<const void*>(addr), size);
    return out;
}
bool ReadMemory(uintptr_t addr, void* out, size_t size) {
    if (addr == 0 || !out || size == 0) return false;


    __try {
        memcpy(out, reinterpret_cast<const void*>(addr), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR(kCategory, "ReadMemory fault at %p", (void*)addr);
        return false;
    }
}
bool WriteMemory(uintptr_t addr, const void* data, size_t size) {
    return ProtectAndWrite(addr, static_cast<const uint8_t*>(data), size);


}
bool EnsureWritable(uintptr_t addr, size_t size) {
    if (addr == 0 || size == 0) return false;


    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(addr), size,
                        PAGE_EXECUTE_READWRITE, &old)) {
        LOG_ERROR(kCategory, "EnsureWritable VirtualProtect failed at %p", (void*)addr);
        return false;
    }
    return true;
}
}
#pragma endregion
