# UserCampaigns 功能过程日志

> 项目：CulturesGameExtend（DLL proxy 增强，不修改 Game.exe 二进制）
> 目标游戏：Cultures（Saga 版），`G:\Projects\Cultures_Saga_CN\SAGA_GAME_HACK\Game.exe`
> 功能：允许 `datax\usercampaigns\<campaignXX>\` 以**文件夹形式**加载地图（等价 *.c2m 打包文件）
> 本文档记录：需求 → 逆向分析 → 首次实现（失败）→ 根因定位 → 修复 → 验证

---

## 1. 需求背景

源自 `G:\Projects\CulturesGameExtend\功能增强.md`，两条需求：

| # | 需求 | 状态 |
|---|---|---|
| ① | 支持 campaign00/01 以外的更多战役文件夹 | 未做（依赖 ②，后续） |
| ② | 支持用**文件夹形式**加载地图（`01_Ein_neuer_Anfang.c2m` ≡ 文件夹 `01_Ein_neuer_Anfang`） | ✅ 本次修复 |

用户实际摆放的地图（c2m 解包产物，campaign00 下 0 个 c2m、全为文件夹）：

```
DataX\UserCampaigns\Campaign00\01_Ein_neuer_Anfang\currentusermap\map.ini
DataX\UserCampaigns\Campaign00\01_Ein_neuer_Anfang\currentusermap\map.dat
DataX\UserCampaigns\Campaign00\01_Ein_neuer_Anfang\currentusermap\map.cif
DataX\UserCampaigns\Campaign00\01_Ein_neuer_Anfang\currentusermap\text\ger\briefings\*.hlt
DataX\UserCampaigns\Campaign00\01_Ein_neuer_Anfang\currentusermap\text\l10\...
...（Campaign00 共 22 关，Campaign01 共 6 关）
```

**注意布局**：map.ini 不在 `<地图文件夹>\map.ini`，而在 `<地图文件夹>\currentusermap\map.ini` —— c2m 解包时保留了虚拟根目录层。

---

## 2. 逆向分析（IDA 证据链）

### 2.1 加载入口：`CampaignStaticDataManager::l_IO_Load` @ `0x410B58`

战役管理器构造后立即执行，**4 个枚举循环**（xref 证实 LoadCampaignMap 的全部 4 个调用点都在此）：

| 循环 | 目录/文件 | 战役 ID | 模式 |
|---|---|---|---|
| 1 | `data\maps\<子目录>\map.ini`（官方内置） | 0 | **Source 目录模式** |
| 2 | `UserMaps\*.c2m`（用户自制地图） | 0 | c2m 挂载 |
| 3 | `datax\usercampaigns\campaign00\*.c2m` | 7 | c2m 挂载 |
| 4 | `datax\usercampaigns\campaign01\*.c2m` | 8 | c2m 挂载 |

**关键**：campaign00/01 循环只枚举 `*.c2m`、不扫子目录 —— 文件夹形式需由 DLL 补充注册。

### 2.2 单地图解析器：`LoadCampaignMap` @ `0x410E6D`

签名（IDA 反编译 + 汇编核对）：

```c
char __thiscall LoadCampaignMap(
        _DWORD *this,        // ★ CampaignStaticDataManager 实例（不是 cfg！）
        _BYTE *a2,           // IniFile 配置对象（已 IniFile_Open）
        char *Source,        // 地图内容根目录（Source 模式）
        char a4,             // 1 = c2m/currentusermap 模式；0 = Source 目录模式
        char *a5,            // c2m 文件名（a4=1 时）
        unsigned int a6)     // 战役 ID
```

内部路径构造逻辑：

```c
if (Source)                    // Source 模式（文件夹/官方地图）
    strncpy(Destination, Source, 0x103);   // Destination[0..] = 内容根目录
else if (a4)                   // c2m 模式
    strcpy(Destination, "currentusermap"); // 挂载包虚拟根
Destination[901] = a4;                       // 模式标志（进图时据此选加载路径）
```

解析字段：`version` / `mapsize` / `mapguid`(16B) / `misc_maptype`（"maci"/"mapt"/"mmon" 4 字节 tag）/ `misc_mapname`，结果存入 0x48C 字节地图信息块（GUID 查重 → 按文件名排序插入 mgr+9 的链表）。

槽位限制：`0x411036 cmp eax, 9`（战役 ID ≥ 9 拒绝注册）。

### 2.3 汇编确认调用序列（这是修复的关键）

**内置地图循环**（`0x410BF6`~`0x410C04`）：

```asm
mov ecx, [ebp+var_8]        ; ecx = CampaignStaticDataManager 实例 ← ★ this 不是 cfg！
push esi                    ; a6 = 0（战役 ID）
push esi                    ; a5 = 0
push esi                    ; a4 = 0
push ebx                    ; Source = 目录路径（DirEnum_NextName 返回值，如 "data\maps\Nordland"）
push eax                    ; a2 = cfg（IniFile 对象）
call CampaignStaticDataManager_LoadCampaignMap
```

**c2m 循环**（`0x410C9C`~`0x410CAD`）：

```asm
mov ecx, [ebp+var_8]        ; this = 管理器实例
push esi                    ; a6 = 0/7/8（战役 ID）
push [ebp+MaxCharCount]     ; a5 = c2m 文件名
push 1                      ; a4 = 1（currentusermap 模式）
push esi                    ; Source = null
push eax                    ; a2 = cfg
call CampaignStaticDataManager_LoadCampaignMap
```

### 2.4 进图加载（验证路径假设）

`MapLoader_LoadCurrentMap` @ `0x40A6F4`：根据地图信息块模式标志选源——
- `Destination[901]=0`（Source 模式）→ `$maproot$\map.ini` + `map.dat`，`$maproot$` 动态指向 Destination[0..] 存的目录
- `Destination[901]=1`（c2m）→ `currentusermap\map.ini`（挂载包 VFS）

**推论**：文件夹形式注册用 Source 模式，Source = 含 map.ini 的目录 → 进图时游戏读 `<Source>\map.ini`。

---

## 3. 首次实现（有 3 个错误）

`Features/UserCampaignsFeature.cpp` v1 逻辑：轮询 `g_pCampaignStaticDataMgr`（0x510BFC）非空 → 枚举 campaign00/01 子目录 → 有 `map.ini` 则 `IniFile_Open` + `LoadCampaignMap` 注册。

**症状**：日志显示 `folder-style campaigns done` 正常输出，但**一条 registered/skip 都没有** → 主菜单战役列表无任何条目。

---

## 4. 根因定位（加载不到地图）

与 `l_IO_Load` 内置地图循环逐指令核对后，发现 **3 个错误**：

| # | 错误 | 代码（v1） | 正确（对齐游戏） |
|---|---|---|---|
| 1 | **路径** | 找 `<mapFolder>\map.ini` | 实际布局是 `<mapFolder>\currentusermap\map.ini`（c2m 解包保留虚拟根）→ 全部静默跳过（连日志都没有，因为 GetFileAttributesA 失败即 return） |
| 2 | **this** | `LoadCampaignMap(cfg, ...)` 把 cfg 当 this | this 必须是 **CampaignStaticDataManager 实例** = `*(DWORD*)Va(0x510BFC)`（汇编 `mov ecx,[ebp+var_8]` 证实）；函数体里 `++this[v12]` 计数、`v19+9` 链表头都在 mgr 对象上 |
| 3 | **Source** | 传地图文件夹名 `mapFolder` | 传**完整内容根目录**（进图时游戏读 `<Source>\map.ini`）；对布局 B 应为 `<campaignDir>\<mapFolder>\currentusermap` |

附带修正：`GameApi.h` 中 `LoadCampaignMap` typedef 注释同样写错（"this = 配置对象"），已一并更正。

---

## 5. 修复方案（v2）

### 5.1 布局探测（兼容两种）

```cpp
if (FileExists(base + "\\map.ini"))                  // 布局 A：<folder>\map.ini
    { iniPath = base + "\\map.ini"; sourceDir = base; }
else if (FileExists(base + "\\currentusermap\\map.ini")) // 布局 B：c2m 解包
    { iniPath = base + "\\currentusermap\\map.ini"; sourceDir = base + "\\currentusermap"; }
```

### 5.2 调用序列（与内置地图循环逐字节一致）

```cpp
void* mgr = *(void**)gameapi::Va<void**>(gameapi::addr::g_pCampaignStaticDataMgr);
gameapi::IniFile_Open(cfg, iniPath.c_str(), 0, 0, 0, 0);
gameapi::LoadCampaignMap(mgr, cfg, sourceDir.c_str(), 0, nullptr, campaignId);
gameapi::IniFile_Close(cfg);
// 参数：this=mgr, a2=cfg, src=内容根目录, a4=0(Source模式), a5=nullptr, a6=战役ID(7/8)
```

### 5.3 修改文件

| 文件 | 变更 |
|---|---|
| `Core/GameApi.h` | 修正 `LoadCampaignMapFn` typedef 参数语义与注释（this=mgr 实例） |
| `Features/UserCampaignsFeature.cpp` | 布局探测 A/B + this 取 mgr + Source 传内容根目录 |

---

## 6. 验证

- **静态**：cl.exe（MSVC 14.16 / v141_xp）`/Zs /utf-8` 语法检查通过，零错误
- **运行时**（用户操作）：
  1. VS 打开 `CulturesGameExtend.sln`，Release|Win32 编译
  2. 拷贝 `Release\CulturesGameExtend.dll` → 游戏目录 `plugins\CulturesGameExtend.dll`
  3. 确认 `plugins\config\CulturesGameExtend_Game.ini` 中 `[UserCampaigns] Enabled=1`
  4. 启动游戏 → 主菜单 → 单人游戏 → 地图选择，应出现 campaign00/01 的文件夹地图
  5. 日志预期：
     ```
     [UserCampaigns] folder map registered: datax\usercampaigns\campaign00\01_Ein_neuer_Anfang\currentusermap\map.ini (campaign 7, ok)
     ...
     [UserCampaigns] folder-style campaigns done
     ```
- **进阶验证**：选中某地图进图，确认能读 `map.ini/map.dat/briefings`（`$maproot$` = `<folder>\currentusermap`）

---

## 7. 遗留问题 / 后续

1. **进图实测未做**：Source 模式进图路径（`$maproot$` → `<folder>\currentusermap`）基于逆向推论，需实测确认；若 briefing 文本走硬编码 `currentusermap\` 前缀则需额外处理
2. **需求 ①（campaign02+）**：patch `0x411036` 的 `cmp eax,9`（09→20 放宽到 32 槽）+ Feature 的 `CampaignDirs` 配置已预留
3. **UI 入口**：主菜单 case 3 硬编码 3 个官方战役按钮（Nordland/Weltwunder/Saga），用户战役无入口按钮（saga001.ini ID 16-21 文本存在但 UI 未引用）—— 需 hook case 3 追加按钮（`0x4D288C`~`0x4D2B92`）或 DLL C++ 接管 `MainMenuUI_Build`
4. **CampaignDirs 槽位**：9+ 的战役 ID 在放宽 patch 前不可用（v1 配置注释已说明）

---

## 8. 关键地址速查

| 符号 | 地址 | 说明 |
|---|---|---|
| `CampaignStaticDataManager_l_IO_Load` | `0x410B58` | 4 循环枚举加载 |
| `CampaignStaticDataManager_LoadCampaignMap` | `0x410E6D` | 单地图解析注册（__thiscall） |
| `g_pCampaignStaticDataMgr` | `0x510BFC` | 管理器单例指针（LoadCampaignMap 的 this 来源） |
| `IniFile_Open` / `IniFile_Close` | `0x424EF8` / `0x425072` | INI 解析 |
| 槽位上限 | `0x411036` | `cmp eax, 9`（09→20 放宽） |
| `MapLoader_LoadCurrentMap` | `0x40A6F4` | 进图加载（$maproot$ / currentusermap） |
