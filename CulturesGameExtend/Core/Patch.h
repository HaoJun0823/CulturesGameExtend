#pragma once
#include <cstdint>
#include <vector>

// 内存补丁工具集（零依赖，仅 Windows API）。
// 所有写操作均通过 VirtualProtect 临时改为 PAGE_EXECUTE_READWRITE，写后恢复。

namespace Patch {

// 把目标地址处的指令改写为 5 字节近跳转：
//   E9 <rel32>  -> 跳转到 cave 函数
// 被覆盖但未被指令本身用到的额外字节用 0x90 (NOP) 填充（nopCount >= 0）。
// 注：调用方必须确保 cave 函数（__declspec(naked)）内部已手抄被覆盖的原指令
//     并在末尾 jmp 回正确的返回地址。
// 返回 true 表示成功。
bool WriteJmp(uintptr_t target, uintptr_t cave, int nopCount = 0);

// 直接写入任意字节序列（用于数值/数据修改）。
bool WriteBytes(uintptr_t addr, const uint8_t* data, size_t size);

// 写入单字节 / 4 字节（小端）。
bool WriteU8(uintptr_t addr, uint8_t v);
bool WriteU32(uintptr_t addr, uint32_t v);

// 把指针（32 位地址）写入目标地址（用于字符串指针替换等）。
bool WritePointer(uintptr_t addr, uintptr_t ptr);

// 读取目标地址当前若干字节（用于调试/校验，可选）。
std::vector<uint8_t> ReadBytes(uintptr_t addr, size_t size);

// 读取目标地址处的指针值。
bool ReadMemory(uintptr_t addr, void* out, size_t size);

// 写入目标地址处的内存（任意大小，自动改页属性）。
bool WriteMemory(uintptr_t addr, const void* data, size_t size);

// 确保目标地址处 size 字节可写（临时改页属性，写后恢复）。
bool EnsureWritable(uintptr_t addr, size_t size);

} // namespace Patch
