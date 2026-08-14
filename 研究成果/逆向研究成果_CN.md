# CulturesGameExtend 逆向研究成果（中文版）

> 本文档汇总本解决方案（`CulturesGameExtend`）中**所有已根据 cpp 源码实地验证**的逆向结论。
> 所有 VA（虚拟地址）均基于 `Game.exe` 固定基址 `0x400000`（无 ASLR / `RELOCS_STRIPPED`）。
> 凡标注「已实证」「cpp 验证」的，均可在对应 `*.cpp` / `*.h` 中按行号追溯。
> 编写日期：2026-08-13。

---

## 0. 目录

1. [项目定位与总体架构](#1-项目定位与总体架构)
2. [注入与加载机制（dinput8 代理）](#2-注入与加载机制dinput8-代理)
3. [核心基础设施（Patch / GameApi / IniConfig / Logger）](#3-核心基础设施)
4. [文本渲染系统（TextRenderer + GdiFont）](#4-文本渲染系统)
5. [战役与电影（Cultures2 / Asgard / CampaignMovie）](#5-战役与电影)
6. [每图平衡表重载（PerMapLogic）](#6-每图平衡表重载permaplogic)
7. [社区静态补丁系统（CulturesPatches / 代码洞穴）](#7-社区静态补丁系统)
8. [其余 Feature（TitleOverride / UnlockAll / WarningLog / VersionStamp / UserCampaigns / MapLoaderExtra）](#8-其余-feature)
9. [关键地址速查表](#9-关键地址速查表)
10. [铁律与已趟过的坑](#10-铁律与已趟过的坑)

---

## 1. 项目定位与总体架构

`CulturesGameExtend` 是一个**社区扩展 DLL**，通过 `dinput8.dll` 代理注入 `Game.exe` 进程，
在不修改 `Game.exe` 本体的前提下，叠加一组可配置功能（汉化文本引擎、战役扩展、平衡表重载、补丁系统等）。

### 1.1 解决方案结构

```
CulturesGameExtend/
├── CulturesGameExtend/        ; 主工程，产出 CulturesGameExtend.dll
│   ├── Core/                  ; 与游戏无关的基础设施（Patch / GameApi / IniConfig / GdiFont / ...）
│   ├── Features/              ; 每个独立功能一个 .cpp，REGISTER_FEATURE 自注册
│   ├── dllmain.cpp            ; DllMain → 创线程跑 RunExtend（规避 Loader Lock）
│   └── framework.h / pch.h
├── CulturesProxyDLL/          ; dinput8.dll 代理，负责把 CulturesGameExtend.dll 注入进程
├── cultures-saga-patches/     ; 静态补丁数据权威源（与 Features/CulturesPatchesFeature 对应）
├── Resource/                  ; 游戏根镜像（fonts / Data 资源），build_deploy.sh 根对根拷贝
└── build_deploy.sh            ; 一键编译 + 部署脚本（v141 cl.exe）
```

### 1.2 Feature 插件架构（已实证：`Core/Feature.h`、`Core/FeatureManager.cpp`）

- **基类 `Feature`**：三虚函数 `GetName()`（= INI 的 `[Section]` 名 + 日志分类前缀）、
  `GetTarget()`（返回 `GameTarget::Game` / `Any`）、`OnInstall(IniConfig&, GameVersion&)`（安装入口）。
- **自注册宏 `REGISTER_FEATURE(Cls)`**（`Feature.h:50`）：在匿名命名空间内定义一个静态对象，
  构造时把 `new Cls()` 注册进全局 `FeatureRegistry`（Meyers 单例）。
  **后果**：新增 Feature 只需在 `.cpp` 底部加一行 `REGISTER_FEATURE(XxxFeature)`，
  并让 `dllmain.cpp` `#include` 该 `.cpp`；**不可在 cl 源列表里重复列**（否则 `LNK2005` 重定义）。
- **`FeatureManager::InstallAll`**（`FeatureManager.cpp:27`）：遍历 registry，对每个 Feature
  读 `[Section] Enabled`（默认 0）→ 关则跳过；`TargetMatch` 不匹配则跳过；
  否则调用 `OnInstall`。日志统计 `ok/attempted/registered/disabled`。

### 1.3 三层 INI 合并（已实证：`Core/IniConfig.cpp`）

加载顺序：`GlobalIniPath()` 打底 → `GameIniPath()` 覆盖 → `PatchesIniPath()` 再覆盖。
`IniConfig::Merge`（`IniConfig.cpp:67`）逐 key 覆盖；section/key 名统一 `tolower` 归一化；
支持 `0x` 前缀十六进制（`GetInt`）；注释行以 `;` / `#` 截断（`StripComment`）。
配置修改**重启游戏生效**（加载期一次性读取，无热重载）。

### 1.4 版本识别（已实证：`Core/GameVersion.cpp`）

`GameVersion::Detect`：以「主程序 exe 所在目录」为基准（`ge_paths::Resolve`，不依赖 CWD），
默认 `Game.exe`，可被 `[Version] ExeName` 覆盖；基址 `m_base = GetModuleHandle(NULL)`（= 0x400000）。

---

## 2. 注入与加载机制（dinput8 代理）

> 已实证：`CulturesProxyDLL/dllmain.cpp`（265 行，含详尽英文注释）。

- **代理本质**：重命名后的 `dinput8.dll` 放在游戏目录，导出 `DirectInput8Create`（用
  `#pragma comment(linker,"/EXPORT:DirectInput8Create=_SHADOW_DirectInput8Create")`），
  桩体 `jmp [g_pOrigDIM8Create]` 转发到系统 `dinput8.dll`（`LoadLibraryW` + `GetProcAddress`）。
  缺这层转发 → 游戏加载报 `0xc000007b`。
- **注入清单**：`plugins/dll_proxy.ini`，每行一个 DLL 路径（相对路径相对**代理自身目录**解析，
  不依赖 CWD）；注释 `#`/`;`；`LoadLibraryA` 直接在本进程注入（无需 `CreateRemoteThread`，
  因为代理已身处游戏进程内）。
- **Loader Lock 规避**：`DllMain` 仅同步解析系统 `dinput8.dll` 转发指针（快、不持锁死锁），
  其余重活（解析 ini、注入 CulturesGameExtend.dll）全部丢给 `CreateThread` 起的 **worker 线程**
  `ProxyWorker`。否则 `LoadLibrary` 进 `CulturesGameExtend.dll` 的 `DllMain` 也要 Loader Lock
  → 经典死锁 / 静默失败。
- **外 DLL 警告**：`WarnIfForeignDll` 对非 `CulturesGameExtend.dll` 的注入 DLL 写稳定性告警。

---

## 3. 核心基础设施

### 3.1 Patch（已实证：`Core/Patch.cpp` / `Patch.h`）

- `WriteJmp(target, cave, nopCount)`：写 5 字节 `E9 rel32`（`rel = cave-(target+5)`），
  多余 `nopCount` 字节补 `90`。
- `WriteBytes/WriteU8/WriteU32/WritePointer/ReadBytes/ReadMemory/WriteMemory/EnsureWritable`。
- `ProtectAndWrite` 模式：先 `VirtualProtect(PAGE_EXECUTE_READWRITE)` → 改字节 →
  `FlushInstructionCache` → 恢复原页保护。所有写游戏代码处皆走此路径。

### 3.2 GameApi（已实证：`Core/GameApi.h` / `GameApi.cpp`）

- `g_imageBase` 默认 `0x400000`，`Init(base)` 时设定。
- 模板 `Va<T>(va) = g_imageBase + (va - 0x400000)`。
- 统一绑定表 `s_bindings`（`GameApi.cpp:23`）：`{rva, (void**)&funcPtr}` 形式，`Init` 时按基址填充。
  已绑定：`LoadCampaignMap=0x410E6D`、`IniFile_Open=0x424EF8`、`IniFile_Close=0x425072`。
- **已知游戏符号（cpp 注释确证）**：
  - `LoadCampaignMap`（0x410E6D）：`__thiscall`，`this = *(DWORD*)Va(g_pCampaignStaticDataMgr)`，
    `g_pCampaignStaticDataMgr = 0x510BFC`。参数 `a4=1`=c2m 模式、`a4=0`=Source 目录模式；
    `a6`=战役 ID（须 `<9`，见 `0x411036 cmp eax,9`，越界崩溃）。
  - `IniFile_Open`（0x424EF8）：`__thiscall`，路径在 `[esp+4]`（见 PerMapLogic 重定向改写 `[esp+0x28]`）。
  - 字符串表：`StringTable_GetMainMenuText=0x4E15F6`（表 0）、
    `StringTable_GetOdinText=0x4E1682`（表 11）、`StringTable_GetSagaText=0x4E169E`（表 13）。

### 3.3 GdiFont（已实证：`Core/GdiFont.h` / `.cpp`）

GDI 字形光栅化引擎，**游戏无关、可独立测试**：
- 输入 UTF-8 字节流 → `DecodeUtf8` 解成 Unicode 码点（RFC 3629，越界字节跳过）。
- 每个码点用 Windows GDI 从已安装/已加载 TTF 动态光栅化为内存灰度位图。
- **光栅化走 DIB + `TextOutW`**（非 `GetGlyphOutline`）：无显示会话里 `GetGlyphOutline` 灰度格式易崩，
  `TextOutW` 最稳。
- **必须用 `SetTextAlign(TA_BASELINE)`**：默认 `TA_TOP` 会把 `(x,y)` 当字形顶边，整字落画布下方被裁。
- 字形带 `unordered_map` 缓存（码点→灰度图）。
- `BlitGlyph`：把灰度字形按文本色 alpha 混合进 **16bpp(RGB565)** 或 **32bpp(RGBA)** 目标缓冲；
  支持 `fbW/fbH` 边界裁剪（防越界写崩）、`clipX/Y/W/H` 引擎 UI 裁剪矩形、幂等重绘（`idempotent`，
  避免抗锯齿跨帧累积变浓）。
- `CreateFromFile`：用 `AddFontResourceEx(FR_PRIVATE)` 把 `.ttf` 注册进本进程（自包含，不依赖系统装字体）。

---

## 4. 文本渲染系统

> 已实证：`Features/TextRendererFeature.cpp`（约 1440 行）、`Core/GdiFont.*`。
> 详见 `SAGA_GAME_HACK/.workbuddy/memory/MEMORY.md` 的「TextRenderer 最终状态」。

### 4.1 设计定案（用户 2026-08-12 拍板）

- **全字符 GDI 接管**：ASCII 也走 GDI（非仅 CJK），统一渲染路径。
- **原子汇合点 hook**：`sub_439610`（唯一文本绘制汇合点，3 调用者：`sub_40ECF9`×2、`sub_4E1E3F`×1）
  + `sub_439633`（字宽）。`sub_4658C7` 是唯一调用者 `sub_439610`。
- **`sub_439610` 调用约定（最终定案）**：`__thiscall`，`ecx`=字体对象；
  栈 5 参 `[ebp+8]`=颜色{aBGR}、 `[ebp+0xC]`=**DrawContext**、`[ebp+0x10]`=ch、
  `[ebp+0x14]`=x、`[ebp+0x18]`=y。跳板抄 6 字节 → 0x439616。
- **DrawContext 结构**（`+0x2C`=像素基址、`+0x30`=pitch(px)、`+0x08..0x14`=clip{x,y,w,h}、
  `+0x18`=宽-1(非高)、`+0x38`=pitch(字节) → bpp=`+0x38`/`+0x30`、**主界面 16bpp**、32bpp 双路径；
  `+0x50`=等宽推进）。真实高度靠 `ProbeFbHeight` 探测。

### 4.2 关键修复与铁律

- **富文本紧凑**：`WordSplitPatch=1`（补丁① 0xD053B 单字节化 → 富文本词=单字节 → 不超宽）。
  `OnGlyphWidth` NUL 修复（ch<=0→0）修词尾 `\0` 白加 9px。
- **超链接整句变亮**：hook `sub_4C9FC4`（token 重绘）入口 → `HoverRepaintStub`，
  `a4=1` 且 token+32(链接标志)≠0 才遍历；`sub_4C9FC4` 是 `__thiscall(this=排版对象, a2=surface,
  a3=token, a4=1)`，`a4` 必须=1 才变亮、surface 须同对象；排版 `this` 来自 ECX 的 `[esp+4]`（非 `[esp+24]` ESI 垃圾，2884 崩溃根因）。
- **BlitGlyph 必须上界裁剪**（fbW/fbH），越界写即崩。
- **UTF-8 源**：所有喂入文本必须 UTF-8（GBK/1252 会乱码）；`SelfTest` 可不启动游戏落盘 `logs/TextRenderer_selftest.bmp` 验证。

---

## 5. 战役与电影

### 5.1 Cultures2 战役（已实证：`Features/Cultures2CampaignFeature.cpp`）

- **复用 screen 5**（战役上下文交换），`Game.exe` 仅改 **7 处立即数** + 1 跳表指针：
  `R_SwitchScreen=0xD1E45`、`R_CmdJmpTable=0xD62BD`（slot[6]=`kAsgardSlot`）、
  `R_Cons1Tail=0x0223B`、`R_CmdMgrSlot=0x169990`、`R_ScreenSlot=0x16967C`、
  `R_CmdCheck=0x0E0E4B`、`R_CmdClear=0x0E0EDB`、`R_CmdName1=0x108740`。
- 原版表：`R_NodesNordland=0x1089F8`、`R_RoutesNordland=0x0F7EE8`；
  DLL 内数据表 `kNodesC2[]`(node 10..110)、`kRoutesC2[]`(frame 26..34)。
- **白屏修复**：`AddUnlock(progress,1,10,1)`（`R_AddUnlock=0x016D62`）解锁首关节点 10。
  `R_ProgressSlot=0x111690`。
- **粘性模式**：`g_pendingC2` 在 `OnSwitchScreen` 里保持；`ApplyMode` 写 7 处立即数。

### 5.2 Asgard 战役按钮（已实证：`Features/AsgardCampaignFeature.cpp`）

- hook `HOOK=0xD295E`（VA 0x4D295E，`call operator new` for Nordland 按钮）→ `AsgardButtonStub`；
  `RET=0xD2963`；控件 ID `kAsgardCtlId=0x138E`(5006)；引用
  `F_New=0xE55BD`、`F_GetSagaTxt=0xE169E`、`F_CreateBtn=0xB7C2E`、`F_Sub439B9C=0x39B9C`、
  `F_AddBtn=0x768DA`、`D_UiRoot=0x154F20`；saga 表 ID 25/26。

### 5.3 战役电影（已实证：`Features/CampaignMovieFeature.cpp`）

- hook `R_IntroConv=0x03184`（VA 0x403184，8 字节 `cmp [esi],bl; je 0x403226`）→ `IntroC2Stub`。
- `R_StrCpy=0x0E5400`：**strcpy 为 cdecl，压栈先 Source 后 Dest**（写反 stub 会 AV）。
- `R_GameStart=0x10F804`（`+0x2c`=开场 mode 标志）、`R_Mgr=0x110BFC`、
  `R_FindMap=0x01086F`（`__thiscall` 查地图记录，`+0x124`=campaign、`+0x128`=node）。
- **已修 bug（2026-08-11）**：曾误写 VA `0x041086F` → `p_findmap=0x81086F` 跳映像外 AV，已修正为 `0x01086F`。
- campaign 1 + node 10 → strcpy `"intro00"`。结尾 `sub_41FB7A` 巨型 switch，`seq_%4.4d`。

---

## 6. 每图平衡表重载（PerMapLogic）

> 已实证：`Features/PerMapLogicFeature.cpp`（601 行，含详尽注释与版本演进 v2→v3.6）。
> **这是最危险的 Feature，直接印证记忆中的「logic 表重载铁律」。**

### 6.1 目标

logic 与地图**同包分发**（随 `.c2m`/地图文件夹一起分发、多人整包传输）。覆盖文件放在地图包
**内部** `logic\` 子目录，布局与 `data\logic` 完全一致
（`jobtypes.ini`、`goodtypes.ini`、`tribetypes\tribetypes.ini`、`atomicanimations\...` 等）。
某文件缺失 → 回退全局 `data\logic\<同名>`；整图无 `logic\` → 完全原版。

### 6.2 两个 hook（全部驻留 DLL）

1. **`IniFile_Open`（0x424EF8）入口 trampoline**：重定向会话（`g_redirect=1`）内把
   12 个 `data\logic\X.ini` 改写为 `<地图源目录>\logic\X.ini`（`PathRewrite`，`__cdecl`）。
   存在性用纯 Win32 `GetFileAttributesA` 判断（**弃用引擎 `sub_40667B` 探测**——会开/关 CRT fd
   扰动文件层 → goodtypes 等表加载失败、图标消失）。
2. **`MapLoader_LoadCurrentMap` 收尾点 `0x40AA13`（`call sub_407A21`）hook**：改动 `E8`→`PrepCallStub`，
   在地图 ini/世界/`[StaticObjects]` 全部处理完、开局准备之前执行 `ReloadLogicForMap`，再 `jmp` 原 `sub_407A21`。
   - **v3 最终触发点**：v2.2–2.5 的 `0x40A81A`/`0x40A6F4` 入口触发点会让紧随其后的
     `$maproot$\map.ini` 打开失败（句柄全零、节表缺失）→ `[StaticObjects]` 全跳过 → 空图。
   - v3.5 额外在 `MapLoader` 入口 `0x40A6F4` 仅对 **goodtypes** 重载（在 `[StaticObjects]` 解析之前），
     收尾点据此跳过 goodtypes；入口失败则收尾点兜底 shadow-safe。

### 6.3 12 个平衡 loader（顺序 = 依赖顺序，重跑必须保持）

| # | 文件 | loader VA | manager 全局 VA |
|---|------|-----------|-----------------|
| 0 | landscapetypes.ini | 0x4162E5 | 0x511534 |
| 1 | trianglepatterntypes.ini | 0x415EBB | 0x5114C4 |
| 2 | goodtypes.ini | 0x41592E | 0x511420 |
| 3 | housetypes.ini | 0x41531D | 0x5113A4 |
| 4 | vehicletypes.ini | 0x414DE0 | 0x511314 |
| 5 | atomicanimations\atomicanimations.ini | 0x414B17 | 0x511300 |
| 6 | tribetypes\tribetypes.ini | 0x413E7B | 0x511204 |
| 7 | jobtypes.ini | 0x4139DB | 0x511184 |
| 8 | weapontypes.ini | 0x4133F6 | 0x5110D0 |
| 9 | armortypes.ini | 0x41312C | 0x510FDC |
| 10 | animaltypes.ini | 0x412C52 | 0x510F30 |
| 11 | humanjobexperiencetypes.ini | 0x412855 | 0x510C20 |

### 6.4 ★★★ 根因铁律（v3.6，cpp 实证的「无图标/物品放不进/采集空」成因）

- goodtypes 记录 **+36** 是**运行期回填的派生句柄**，不来自 ini。
  loader 解析时把 +36 写成 `-1`（未解析）；启动一次性守卫（位于 `sub_401981`）：
  `if (!this[40]) { sub_406C3E(); sub_47D3E6(); sub_407665(); this[40]=1; }`，
  其中 `sub_407665 → sub_415E8A` 遍历 56 条记录：`rec[+36] = sub_47FEF0(rec[+32])`
  （在 landscape 图形数组 `dword_568C24`，stride 140，计数 `dword_568C20` 里按 landscapetype 找索引）。
  **+36 = "good → landscape 图形索引"缓存，全程只解析一次。**
- ⇒ 任何形式的 goodtypes 重载都会把 +36 变回 -1 → 所有 good 失去图形句柄 →
  **无图标 / 无法放入世界 / 无法采集**（用户实测症状）。这也解释了「覆盖文件与全局 md5 相同也坏」——+36 根本不在 ini 里。
- **修复**：重载后调 `sub_415E8A()` 重解析 +36（引擎自己的入口，语义 100% 对齐）。
- **附加**：`dword_510C60` = land→goodid 索引，由 `sub_412B62` 基于全局表构建，
  采集/生成系统查它找「地貌 X 上的物品 id」。重载后必须重跑 `sub_412B62`（参数 `idxMgr=*(0x510F20)`）。
- **影子表机制（v3.3）**：goodtypes 重载期间把 loader 内部 6 处 `dword_511420` 引用的 imm32
  改为 DLL 影子全局 `g_shadowGood`，让渲染线程永远读完整原表；解析完 `memcpy` 到原表再恢复 imm32。
- **拷回表（指针身份保持）**：重载后把新内容拷回原表地址、恢复原指针（10 张恒定大小表，
  见 `kTables[]`，跳过 atomicanim/tribe/weapon 动态大小表）→ 缓存了表指针的子系统不失效。
- **自校验**：重载前后整表 `memcmp`（0x35A0），理想结果 = 只有 ini 真改字段有差异；
  若别的偏移变 0/-1 说明还有运行期回填字段被清（继续修）。
- **每表独立开关**：`[PerMapLogic] Reload<Name> = 1/0`，二分定位 bug 用。

### 6.5 多人一致性

地图包（含 `logic\`）整包传输到对方 → 双方同版本 DLL + 同图 = 同平衡，天然一致。

---

## 7. 社区静态补丁系统（CulturesPatches / 代码洞穴）

> 已实证：`Features/CulturesPatchesFeature.cpp`、`cave_trampolines.h`、`caves_blob.h`。
> 详见 `SAGA_GAME_HACK/.workbuddy/memory/MEMORY.md` 的「CulturesPatches 里程碑」。

- **`[CulturesPatches]` 总开关 `Enabled`**；数据权威源 = `cultures-saga-patches/`（Python 静态补丁工具）。
- **`ApplyPatchDir`**：解析 `plugins/config/patches/*.ini`，格式
  `<addr> <hex...> [ | <verify hex> ]`，行内 `;`/`#` 注释；写入前可校验原字节。
- **`ApplyCodeCaves`**：`VirtualAlloc(NULL)` 申请独立可执行页（因映像 0xF21CB 页加载时保留未提交，
  原址 `MEM_COMMIT` 会 `ERROR_ACCESS_DENIED(5)`）；blob 拷贝后做两类重定位——①绝对自引用 +delta、
  ②相对外跳 `rel32 −= delta`；否则必崩零页/err=5。
- **blob**：`kCaveBlob[]` 668 字节，`kCaveOffset=0xF21CB`（`caves_blob.h`）。
- **8 个跳板**（`cave_trampolines.h`，`CaveTrampoline{site,caveOff,verifyLen,verify[8]}`）：
  `0xA2F42`(Shortcuts)、`0xBB894`(Shortcuts)、`0xC0C04`(AssistantCtrlClick)、
  `0xC0E4B`(AssistantCtrlClick)、`0xD4731`(MultiplayerVersionCheck)、
  `0xDB625`(MultiplayerVersionCheck)、`0xDCD97`(MultiplayerVersionCheck)、
  `0xDF939`(MultiplayerStability)。

---

## 8. 其余 Feature

### 8.1 TitleOverride（已实证：`Features/TitleOverrideFeature.cpp`）
双 **IAT hook**（改导入表槽，不改导出函数入口，更安全可逆）：
`R_IatCreateWindowExA=0xF31D4`、`R_IatSendMessageA=0xF31B8`；`WM_SETTEXT=0xC`；
UTF-16 标题（`MultiByteToWideChar(CP_UTF8)`）；轮询兜底线程每 0.5s 修正。

### 8.2 UnlockAllCampaigns（已实证：`Features/UnlockAllCampaignsFeature.cpp`）
hook `R_IsUnlocked=0x16E20`（`__thiscall`，8 字节 `{campaignId,nodeId}` 表线性查找，`ret 8`）；
stub `mov eax,1; ret 8`；默认 `Enabled=0`（不碰存档进度）；全程序 6 调用点全在 0x4Dxxxx 战役屏。

### 8.3 WarningLog（已实证：`Features/WarningLogFeature.cpp`）
- 背景：`Warning!`（IDA 定案 `sub_47A14D`）是开发者 assert 对话框（`MessageBoxA(..,0x31=惊叹+OKCANCEL)`）；
  `result = MessageBoxA - 2`；Cancel(=2)→0→`__debugbreak`(int 3)→无调试器时进程终止。
- `ForbidExit`：1 字节 NOP 掉 `0x47A1C3` 的 `int3`(CC→90)（安全网）。
- `LogEnabled`：hook `0x47A1B8` 的 `call ds:MessageBoxA`（6 字节），stub 读已格式化 `lpText([ebp+8])`
  + 调用者返回地址 `([ebp+4])` 写日志，假装按 OK(`eax=1`) 跳回 `0x47A1BE` 续跑（跳过弹框且不触发 int3）。
- 所有 `Warning!` 调用者（7 处 + 虚表）经此唯一 `MessageBoxA` 调用，一次 hook 全覆盖。

### 8.4 VersionStamp（已实证：`Features/VersionStampFeature.cpp`）
不改游戏代码结构，仅把三处 `push imm32` 字符串地址改写成本 DLL 字面串
（版本号来自 `CGE_VERSION_STR` 编译宏，构建日期 `__DATE__`/`__TIME__`）：
`R_MenuPush=0xD189C`（主菜单模板 0x5088B4）、`R_NetBuildPush=0xDB322`（netlog Build 0x509860）、
`R_NetHeadPush=0xDB2E7`（netlog 头 0x509870）。
注：`0x4FF2EC` 附近另有一处版本模板经文件级指针扫描确认**零引用**（死模板），无需处理。

### 8.5 UserCampaigns / MapLoaderExtra（已实证：`Features/UserCampaignsFeature.cpp`、`MapLoaderExtraFeature.cpp`）
- 文件夹地图注册：`campaign00=7`、`campaign01=8`；`AllowCurrentUserMapFolder` 额外扫 `currentusermap\map.ini`；
  `kCfgSize=1572*4`。
- 复用 `IniFile_Open + LoadCampaignMap(a4=0 Source)` 注册文件夹地图；轮询线程等
  `g_pCampaignStaticDataMgr` 非空（最多 30s）。
- **2026-08-13 重构**：不再 hook `l_IO_Load`，改用原生函数范式（与 `l_IO_Load` 内置循环逐字节一致）。

---

## 9. 关键地址速查表

> 基址恒 `0x400000`；下表 VA 即文件内 `constexpr uintptr_t` 值（已 cpp 验证）。

| 用途 | VA | 来源文件 |
|------|----|----------|
| 映像基址 | 0x400000 | GameApi.cpp |
| `g_pCampaignStaticDataMgr` | 0x510BFC | GameApi.h |
| `LoadCampaignMap` (thiscall) | 0x410E6D | GameApi.cpp |
| `IniFile_Open` (thiscall) | 0x424EF8 | GameApi.cpp |
| `IniFile_Close` | 0x425072 | GameApi.cpp |
| `StringTable_GetMainMenuText` (表0) | 0x4E15F6 | GameApi.h |
| `StringTable_GetOdinText` (表11) | 0x4E1682 | GameApi.h |
| `StringTable_GetSagaText` (表13) | 0x4E169E | GameApi.h |
| `sub_439610` 原子 blit 汇合点 | 0x439610 | TextRendererFeature.cpp |
| `sub_439633` 字宽 | 0x439633 | TextRendererFeature.cpp |
| `sub_4C9FC4` token 重绘（超链接 hover） | 0x4C9FC4 | TextRendererFeature.cpp |
| `MapLoader_LoadCurrentMap` 入口 | 0x40A6F4 | PerMapLogicFeature.cpp |
| `MapLoader` 收尾 `call sub_407A21` | 0x40AA13 | PerMapLogicFeature.cpp |
| `sub_407A21` 开局准备 | 0x407A21 | PerMapLogicFeature.cpp |
| `sub_415E8A` 重解析 good +36 句柄 | 0x415E8A | PerMapLogicFeature.cpp |
| `sub_412B62` 重建 land→goodid 索引 | 0x412B62 | PerMapLogicFeature.cpp |
| `dword_568C20` landscape 图形表计数 | 0x568C20 | PerMapLogicFeature.cpp |
| `dword_510C60` land→goodid 索引 | 0x510C60 | PerMapLogicFeature.cpp |
| `goodtypes` 表指针 `dword_511420` | 0x511420 | PerMapLogicFeature.cpp |
| `sub_47A14D` Warning! assert 框 | 0x47A14D | WarningLogFeature.cpp |
| `0x47A1B8` call ds:MessageBoxA | 0x47A1B8 | WarningLogFeature.cpp |
| `0x47A1C3` int3（ForbidExit NOP） | 0x47A1C3 | WarningLogFeature.cpp |
| `0x403184` 开场电影收敛点 | 0x403184 | CampaignMovieFeature.cpp |
| `strcpy` (cdecl, Source→Dest) | 0x4E5400 | CampaignMovieFeature.cpp |
| `R_FindMap` (__thiscall) | 0x01086F | CampaignMovieFeature.cpp |
| `g_pGameStartRequest` (+0x2c mode) | 0x10F804 | CampaignMovieFeature.cpp |
| `0x4D295E` Asgard 按钮 hook 点 | 0x4D295E | AsgardCampaignFeature.cpp |
| `R_CmdJmpTable` (slot[6]) | 0xD62BD | Cultures2CampaignFeature.cpp |
| `R_SwitchScreen` | 0xD1E45 | Cultures2CampaignFeature.cpp |
| `R_AddUnlock` | 0x016D62 | Cultures2CampaignFeature.cpp |
| `R_ProgressSlot` | 0x111690 | Cultures2CampaignFeature.cpp |
| `R_IsUnlocked` (__thiscall) | 0x16E20 | UnlockAllCampaignsFeature.cpp |
| IAT `CreateWindowExA` | 0xF31D4 | TitleOverrideFeature.cpp |
| IAT `SendMessageA` | 0xF31B8 | TitleOverrideFeature.cpp |
| `0xD189C` 主菜单版本 push | 0xD189C | VersionStampFeature.cpp |
| `0xDB322` netlog build push | 0xDB322 | VersionStampFeature.cpp |
| `0xDB2E7` netlog 头 push | 0xDB2E7 | VersionStampFeature.cpp |
| 代码洞穴 blob 偏移 | 0xF21CB | caves_blob.h |
| 8 跳板 site（见 §7） | 0xA2F42..0xDF939 | cave_trampolines.h |

---

## 10. 铁律与已趟过的坑

1. **logic 表重载须重跑派生 resolver**：`sub_415E8A`（good +36 图形句柄）+ `sub_412B62`
   （`dword_510C60` 索引）；只 memcpy 表不重跑 → 无图标/放不进/采集空。影子表→memcpy→resolver→整表 diff 自校验。
2. **naked stub 调 C 函数后 jmp __thiscall 原函必须还原 `ecx`(this)**（ebx 暂存），否则 AV。
3. **BlitGlyph 必须上界裁剪**（fbW/fbH），越界写即崩。
4. **hook 外部对象指针前必须 VirtualQuery 守卫**（猜错=不渲染+日志，不踩内存）。
5. **strcpy(0x4E5400) 压栈先 Source 后 Dest**；`R_FindMap` 曾误写 `0x041086F` 跳映像外 AV。
6. **代码洞穴重定位必须同时处理绝对自引用与相对外跳两类**，只做其一必崩零页/err=5。
7. **Loader Lock**：`DllMain` 内不得同步 `LoadLibrary` 依赖 DLL，必须创线程。
8. **部署纪律**：`build_deploy.sh` 可能静默编译失败（grep 吞错）→ 每次部署后必须字节级验证 DLL（搜关键字符串）。
9. **c2m 联机传图整树递归打包无白名单**（含 `logic\`，dotfile 除外）→ 平衡表天然随图同步。
10. **富文本词=单字节**（WordSplitPatch=1）才不超宽；`OnGlyphWidth` NUL→0 修词尾 `\0` 白加 9px。
