#!/bin/bash
# ============================================================
# build_deploy.sh — CulturesGameExtend 一键编译 + 部署
# 用法: bash build_deploy.sh   (Git Bash)
# 说明:
#   - 用 cl.exe 命令行编译（MSBuild 在自动化环境可能被拦）
#   - 部署只拷"干净产物"到游戏目录，避开 VS Release 的中间产物
#     (.iobj/.ipdb/.pdb/.exp/.lib 不拷)
#   - 配置/补丁以项目源码 Resource/ 为准（Resource 镜像游戏根目录）
#   - 若同级存在 CulturesGameLocalization（子模块或兄弟目录），
#     其 _build/Data、_build/DataX 也会一并部署（UTF-8 本地化文件）
# 游戏目录: G:\Projects\Cultures_Saga_CN\SAGA_GAME_HACK
# ============================================================
set -e

PROJ_DIR="G:/Projects/CulturesGameExtend"
SRC_DIR="$PROJ_DIR/CulturesGameExtend"
GAME_DIR="G:/Projects/Cultures_Saga_CN/SAGA_GAME_HACK"
# 本地化仓库（子模块 checkout 在项目根 CulturesGameLocalization/，
# 兄弟目录部署时改这里；找不到则跳过本地化部署）
LOCALIZATION_DIR="$PROJ_DIR/CulturesGameLocalization"
[ -d "$LOCALIZATION_DIR/_build/Data" ] || LOCALIZATION_DIR="G:/Projects/CulturesGameLocalization"

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
"$CL" /nologo /LD /EHsc /Y- /utf-8 /std:c++17 /O2 /we4010 \
  /D WIN32 /D NDEBUG /D CULTURESGAMEEXTEND_EXPORTS /D _WINDOWS /D _USRDLL \
  /D _SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING \
  /D _CRT_SECURE_NO_WARNINGS \
  /D "CGE_VERSION_STR=\"$CGE_VER\"" \
  /I. "/I$MSVC_INC" "/I$SDK_INC_UCRT" "/I$SDK_INC_UM" "/I$SDK_INC_SHARED" \
  dllmain.cpp Core/Logger.cpp Core/IniConfig.cpp Core/GameVersion.cpp \
  Core/GameApi.cpp Core/Patch.cpp Core/FeatureManager.cpp \
  Core/GdiFont.cpp \
  /link /OUT:CulturesGameExtend.dll /SUBSYSTEM:WINDOWS \
  "/LIBPATH:$MSVC_LIB" "/LIBPATH:$SDK_LIB_UCRT" "/LIBPATH:$SDK_LIB_UM" \
  kernel32.lib user32.lib gdi32.lib winmm.lib advapi32.lib shell32.lib ole32.lib

echo "==> [1.5/3] 校验 kLogicFiles 含全部 12 项（含 landscapetypes.ini）"
# 安全网：直接 grep 编译产物里的字符串。若注释反斜杠吞行导致少编译 1 项，
# landscapetypes.ini 不会进数组，这里立即失败，而不是等游戏崩溃（见 5688 dump）。
if ! grep -a -q "landscapetypes.ini" "$SRC_DIR/CulturesGameExtend.dll"; then
  echo "  !! 编译产物缺少 landscapetypes.ini —— 极可能是注释行尾反斜杠把下一行吞进注释，" >&2
  echo "     导致 kLogicFiles 少编译 1 项而循环仍按 12 跑。请检查 PerMapLogicFeature.cpp。" >&2
  exit 1
fi
echo "    OK (12 项 logic 表齐全)"

echo "==> [2/3] 部署 DLL -> 游戏 plugins/"
cp -f "$SRC_DIR/CulturesGameExtend.dll" "$GAME_DIR/plugins/CulturesGameExtend.dll"

echo "==> [3/3] 部署资源 (以源码 Resource/ 为准；Resource 镜像游戏根目录)"
# Resource 即游戏根目录的镜像：直接把整棵树拷进游戏根目录与 Release。
cp -rf "$PROJ_DIR/Resource/." "$GAME_DIR/"
cp -rf "$PROJ_DIR/Resource/." "$PROJ_DIR/Release/"

set +e   # 本地化文本为可选项：游戏运行时文件可能被锁，任何失败都不应中止部署
echo "==> [4/4] 部署本地化文件 (UTF-8 文本，来自 CulturesGameLocalization)"
if [ -d "$LOCALIZATION_DIR/_build/Data" ]; then
  echo "    从 $LOCALIZATION_DIR/_build 部署..."
  # 本地化文本为可选项；游戏运行时部分文件可能被锁、或 _build/DataX 缺失，
  # 任一拷贝失败都不应中止整个部署（崩溃修复的 DLL 已在 [2/3] 落地）。
  cp -rf "$LOCALIZATION_DIR/_build/Data/."    "$GAME_DIR/Data/"    || echo "    (警告: Data 拷贝跳过/部分失败)"
  cp -rf "$LOCALIZATION_DIR/_build/DataX/."   "$GAME_DIR/DataX/"   || echo "    (无 DataX，跳过)"
  cp -rf "$LOCALIZATION_DIR/_build/."         "$PROJ_DIR/Release/" || echo "    (警告: Release 拷贝跳过/部分失败)"
  echo "    OK"
  # 验证
  echo "    验证:"
  ls "$GAME_DIR/Data/maps/" | head -3
  echo "    ..."
  ls "$GAME_DIR/DataX/FMV/" 2>/dev/null || echo "    (无 DataX/FMV)"
else
  echo "    跳过: $_build/Data 不存在（$LOCALIZATION_DIR）"
  echo "    提示: 先运行 CulturesGameLocalization 的 build_text.py"
fi
set -e   # 恢复严格模式

echo "==> 完成。验证:"
ls -la "$GAME_DIR/plugins/CulturesGameExtend.dll"
echo "    Game.ini 配置:"
grep -E "Enabled|AllowCurrentUserMapFolder" "$GAME_DIR/plugins/config/CulturesGameExtend_Game.ini" || true
echo "    Release 资源镜像:"
ls "$PROJ_DIR/Release/plugins/config/" 2>/dev/null || echo "    (Release 资源缺失)"
echo
echo "注意: dinput8.dll (代理载体) 无改动时无需更新；若其变更请单独拷贝 Release/dinput8.dll -> 游戏目录"
