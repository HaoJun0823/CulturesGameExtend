#!/bin/bash
# ============================================================
# build_deploy.sh — CulturesGameExtend 一键编译 + 部署
# 用法: bash build_deploy.sh   (Git Bash)
# 说明:
#   - 用 cl.exe 命令行编译（MSBuild 在自动化环境可能被拦）
#   - 部署只拷"干净产物"到游戏目录，避开 VS Release 的中间产物
#     (.iobj/.ipdb/.pdb/.exp/.lib 不拷)
#   - 配置/补丁以项目源码 Resource/ 为准（Resource 镜像游戏根目录）
# 游戏目录: G:\Projects\Cultures_Saga_CN\SAGA_GAME_HACK
# ============================================================
set -e

PROJ_DIR="G:/Projects/CulturesGameExtend"
SRC_DIR="$PROJ_DIR/CulturesGameExtend"
GAME_DIR="G:/Projects/Cultures_Saga_CN/SAGA_GAME_HACK"

CL="/c/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/MSVC/14.16.27023/bin/Hostx86/x86/cl.exe"
MSVC_INC="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.16.27023\include"
SDK_INC_UCRT="C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\ucrt"
SDK_INC_UM="C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um"
SDK_INC_SHARED="C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\shared"
MSVC_LIB="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.16.27023\lib\x86"
SDK_LIB_UCRT="C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\ucrt\x86"
SDK_LIB_UM="C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x86"

echo "==> [1/3] 编译 (cl, Release|Win32 等价配置)"
# 版本号：从最近 git tag（vX.Y.Z-*）自动提取主版本，注入 VersionStamp 编译宏。
# 例：v0.4.0-milestone -> "0.4.0"；无 tag 时回退 0.0.0。
CGE_VER=$(git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//; s/-.*//')
[ -z "$CGE_VER" ] && CGE_VER="0.0.0"
echo "==> 注入版本: CGE_VERSION_STR=\"$CGE_VER\""
cd "$SRC_DIR"
"$CL" /nologo /LD /EHsc /Y- /utf-8 /std:c++17 /O2 \
  /D WIN32 /D NDEBUG /D CULTURESGAMEEXTEND_EXPORTS /D _WINDOWS /D _USRDLL \
  /D _SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING \
  /D "CGE_VERSION_STR=\"$CGE_VER\"" \
  /I. "/I$MSVC_INC" "/I$SDK_INC_UCRT" "/I$SDK_INC_UM" "/I$SDK_INC_SHARED" \
  dllmain.cpp Core/Logger.cpp Core/IniConfig.cpp Core/GameVersion.cpp \
  Core/GameApi.cpp Core/Patch.cpp Core/FeatureManager.cpp \
  Core/GdiFont.cpp \
  /link /OUT:CulturesGameExtend.dll /SUBSYSTEM:WINDOWS \
  "/LIBPATH:$MSVC_LIB" "/LIBPATH:$SDK_LIB_UCRT" "/LIBPATH:$SDK_LIB_UM" \
  kernel32.lib user32.lib gdi32.lib winmm.lib advapi32.lib shell32.lib ole32.lib

echo "==> [2/3] 部署 DLL -> 游戏 plugins/"
cp -f "$SRC_DIR/CulturesGameExtend.dll" "$GAME_DIR/plugins/CulturesGameExtend.dll"

echo "==> [3/3] 部署资源 (以源码 Resource/ 为准；Resource 镜像游戏根目录)"
# Resource 即游戏根目录的镜像：直接把整棵树拷进游戏根目录与 Release。
cp -rf "$PROJ_DIR/Resource/." "$GAME_DIR/"
cp -rf "$PROJ_DIR/Resource/." "$PROJ_DIR/Release/"

echo "==> 完成。验证:"
ls -la "$GAME_DIR/plugins/CulturesGameExtend.dll"
echo "    Game.ini 配置:"
grep -E "Enabled|AllowCurrentUserMapFolder" "$GAME_DIR/plugins/config/CulturesGameExtend_Game.ini" || true
echo "    Release 资源镜像:"
ls "$PROJ_DIR/Release/plugins/config/" 2>/dev/null || echo "    (Release 资源缺失)"
echo
echo "注意: dinput8.dll (代理载体) 无改动时无需更新；若其变更请单独拷贝 Release/dinput8.dll -> 游戏目录"
