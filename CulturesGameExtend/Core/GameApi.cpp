// GameApi.cpp
// 游戏函数指针的绑定表：所有从 game.exe 导入的函数在此统一解析地址。
// 新增函数：在 s_bindings 加一行 { RVA, (void**)&指针 } 即可。
// GameApi.cpp
// Binding table for game function pointers: all functions imported from game.exe
// are resolved to addresses here.
// To add a function: just add a line { RVA, (void**)&pointer } to s_bindings.
#include "pch.h"
#include "GameApi.h"


namespace gameapi {


uintptr_t g_imageBase = 0x400000;


IniFile_OpenFn  IniFile_Open  = nullptr;
IniFile_CloseFn IniFile_Close = nullptr;
LoadCampaignMapFn LoadCampaignMap = nullptr;


#pragma region Binding table
namespace {

// RVA（相对 0x400000）-> 函数指针槽位；Init 时按实际基址统一填充。

// RVA (relative to 0x400000) -> function pointer slot; filled uniformly by Init using the actual base.
struct Binding {
    uintptr_t rva;
    void** ppFn;
};


Binding s_bindings[] = {
    { 0x410E6D - 0x400000, (void**)&LoadCampaignMap },
    { 0x424EF8 - 0x400000, (void**)&IniFile_Open },
    { 0x425072 - 0x400000, (void**)&IniFile_Close },
};


}
// namespace
// namespace
void Init(uintptr_t imageBase) {

#pragma endregion

#pragma region Init
    g_imageBase = imageBase;
    for (auto& b : s_bindings)
        *b.ppFn = reinterpret_cast<void*>(imageBase + b.rva);
}
}

#pragma endregion
