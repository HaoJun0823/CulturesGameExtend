#pragma once
#include <cstdint>
#include <vector>

// 内存补丁工具集（零依赖，仅 Windows API）。
// 所有写操作均通过 VirtualProtect 临时改为 PAGE_EXECUTE_READWRITE，写后恢复。


// Memory-patching toolkit (zero-dependency, Windows API only).
// All writes temporarily switch the page to PAGE_EXECUTE_READWRITE via
// VirtualProtect, then restore it after writing.

namespace Patch {

// 把目标地址处的指令改写为 5 字节近跳转：
//   E9 <rel32>  -> 跳转到 cave 函数
// 被覆盖但未被指令本身用到的额外字节用 0x90 (NOP) 填充（nopCount >= 0）。
// 注：调用方必须确保 cave 函数（__declspec(naked)）内部已手抄被覆盖的原指令
//     并在末尾 jmp 回正确的返回地址。
// 返回 true 表示成功。

// Rewrite the instruction at the target address into a 5-byte near jump:
//   E9 <rel32>  -> jump to the cave function
// Extra bytes that are overwritten but not used by the instruction itself are
// filled with 0x90 (NOP) (nopCount >= 0).
// Note: the caller must ensure the cave function (__declspec(naked)) has manually
// transcribed the overwritten original instructions and ends with a jmp back to
// the correct return address.
// Returns true on success.
bool WriteJmp(uintptr_t target, uintptr_t cave, int nopCount = 0);

// 直接写入任意字节序列（用于数值/数据修改）。

// Directly write an arbitrary byte sequence (for numeric / data modifications).
bool WriteBytes(uintptr_t addr, const uint8_t* data, size_t size);

// 写入单字节 / 4 字节（小端）。

// Write a single byte / 4-byte value (little-endian).
bool WriteU8(uintptr_t addr, uint8_t v);
bool WriteU32(uintptr_t addr, uint32_t v);

// 把指针（32 位地址）写入目标地址（用于字符串指针替换等）。

// Write a pointer (32-bit address) into the target address (for string-pointer
// substitution, etc.).
bool WritePointer(uintptr_t addr, uintptr_t ptr);

// 读取目标地址当前若干字节（用于调试/校验，可选）。

// Read the current bytes at the target address (for debugging / verification,
// optional).
std::vector<uint8_t> ReadBytes(uintptr_t addr, size_t size);

// 读取目标地址处的指针值。

// Read the pointer value at the target address.
bool ReadMemory(uintptr_t addr, void* out, size_t size);

// 写入目标地址处的内存（任意大小，自动改页属性）。

// Write memory at the target address (any size, auto-adjusts page attributes).
bool WriteMemory(uintptr_t addr, const void* data, size_t size);

// 确保目标地址处 size 字节可写（临时改页属性，写后恢复）。

// Ensure the size bytes at the target address are writable (temporarily changes
// page attributes, restores after writing).
bool EnsureWritable(uintptr_t addr, size_t size);


}
// namespace Patch
// namespace Patch