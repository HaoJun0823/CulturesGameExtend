// pch.h: This is the precompiled header file.
// The files listed below are compiled only once, improving build performance for subsequent builds.
// This also affects IntelliSense performance, including code completion and many code-browsing features.
// However, if any of the files listed here is updated between builds, they will all be recompiled.
// Do not add files that change frequently here, as that negates the performance benefit.

// pch.h: 这是预编译标头文件。
#ifndef PCH_H
// 下方列出的文件仅编译一次，提高了将来生成的生成性能。
// 这还将影响 IntelliSense 性能，包括代码完成和许多代码浏览功能。
// 但是，如果此处列出的文件中的任何一个在生成之间有更新，它们全部都将被重新编译。
// 请勿在此处添加要频繁更新的文件，这将使得性能优势无效。


#pragma region Precompiled headers
// Add headers to precompile here
#define PCH_H

// Standard library (precompiled for speed)
#include "framework.h"

// 添加要在此处预编译的标头
#include <string>

// 标准库（预编译以提速）
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <iterator>

// Filesystem compat layer (auto-falls back to experimental under v141_xp)
#include <mutex>
#pragma endregion

#include "Core/fs_compat.h"

// 文件系统兼容层（v141_xp 下自动回退到 experimental）
//PCH_H
#endif

