# CHANGELOG — CulturesGameExtend（中英双语 / Bilingual）

> DLL proxy 增强框架（游戏加载 `dinput8.dll` 代理 → `plugins\CulturesGameExtend.dll`）
> 目标游戏：Cultures（Saga 版）`Game.exe`（基址 0x400000）
> 部署：`build_deploy.sh`（cl 命令行编译 + 拷贝干净产物，避开 VS Release 中间产物）

---

## v0.4.0 — CulturesPatches 全量实现 + 运行期 code-cave 注入稳定（里程碑 / milestone，2026-08-13，commit `b426b73`）

> 里程碑 / Milestone：从 `cultures-saga-patches`（Python 静态补丁权威源）完整落地到 C++ 运行期实现，并经用户实跑验证**不再崩溃**。版本锚 / Version anchor：`v2026-08-13-cultures-patches-runtime`。
> Milestone: the full `cultures-saga-patches` (Python static-patch authoritative source) is now realized as a C++ runtime implementation, verified crash-free by the user.

### 背景 / Background
- **中文**：`cultures-saga-patches` 是 22 个针对 `Game.exe`（基址 0x400000）的静态补丁（增强/修复）。其 C++ 重实现此前仅覆盖纯字节 `.ini`（16 个），**6 个 code-cave（运行时代码注入）补丁完全缺失**，且 `houseLimits.ini` 存在一个字节 BUG。
- **EN**: `cultures-saga-patches` is 22 static patches for `Game.exe` (base 0x400000). Its C++ reimplementation only covered pure byte-patch `.ini` (16 of them); **6 code-cave (runtime code-injection) patches were entirely missing**, and `houseLimits.ini` had a byte BUG.

### 新增 / Added
- **中文**：补全 6 个缺失的 code-cave 补丁为干净 `.ini`（仅保留纯字节补丁，E9 跳板已搬至运行期机制）：`AssistantCtrlClick`、`FixParticleOutsideMapCrash`、`HumanListFilters`、`MultiplayerStability`、`MultiplayerVersionCheck`、`Shortcuts`。
- **EN**: Added the 6 missing code-cave patches as clean `.ini` (byte-only; their E9 trampolines were moved into the runtime mechanism): `AssistantCtrlClick`, `FixParticleOutsideMapCrash`, `HumanListFilters`, `MultiplayerStability`, `MultiplayerVersionCheck`, `Shortcuts`.
- **中文**：`CulturesPatchesFeature.cpp` 新增 `ApplyCodeCaves()` —— 运行期把 668 字节 blob（`caves_blob.h`，由 Python `add_code_cave` 布局经 Keystone 提取，VA = 0x400000+0xF21CB）注入独立内存页，并按 `cave_trampolines.h` 的 8 条跳板表把原站点改写为 `E9 rel32`。
- **EN**: `CulturesPatchesFeature.cpp` gained `ApplyCodeCaves()` — at runtime it injects a 668-byte blob (`caves_blob.h`, extracted via Keystone from the Python `add_code_cave` layout, VA = 0x400000+0xF21CB) into an independent memory page and rewrites the original sites into `E9 rel32` per the 8-entry `cave_trampolines.h` table.
- **中文**：`houseLimits.ini` 字节 BUG 修正：`0xAA73` 处 `0x3D8400` → `0x3D8600`(=2000×0x7E0)，自相矛盾的注释已同步修正（3 目录同步）。
- **EN**: Fixed the `houseLimits.ini` byte BUG: at `0xAA73`, `0x3D8400` → `0x3D8600` (=2000×0x7E0); the self-contradictory comment was corrected (synced across 3 dirs).

### 修复 / Fixed
1. **err=5（ACCESS_DENIED）**
   - **中文**：初始 `ApplyCodeCaves` 用 `VirtualAlloc(MEM_COMMIT, 0x4F2000)` 落在**映像保留但未提交的间隙页**（PE `.text` 段提交止于 0x4F2000），Windows 禁止对映像保留区间做 MEM_COMMIT → `err=5`，`Feature install FAILED`。
   - **EN**: Initial `ApplyCodeCaves` used `VirtualAlloc(MEM_COMMIT, 0x4F2000)` landing in an **image-reserved-but-uncommitted gap** (PE `.text` commit ends at 0x4F2000); Windows forbids MEM_COMMIT on image-reserved ranges → `err=5`, `Feature install FAILED`.
   - **修复 / Fix**：改用 `VirtualAlloc(NULL, …)` 取**独立页面 X**（<0x80000000，Game.exe 非 LAA，E9 rel32 可达），memcpy blob 后运行时重定位。/ Switched to `VirtualAlloc(NULL, …)` for an **independent page X** (<0x80000000; Game.exe is non-LAA so E9 rel32 reaches it), then runtime-relocated the blob.
2. **运行期零页崩溃（dump `Game.exe.42448.dmp`，0xC0000005 ACCESS_VIOLATION）**
   - **中文**：重定位循环**只处理 4 字节绝对自引用**，漏掉 `E9/E8/Jcc` 相对外跳的 `rel32`；搬移后相对跳转目标 = 原地址 + delta，飞入 cave 页零填充区执行 → 解引用 NULL 崩。`EIP=0x3340E3B`（cave 页内偏移 0xE3B，远超 668B blob）。
   - **EN**: The relocation loop only handled **4-byte absolute self-references**, missing `E9/E8/Jcc` relative external jumps' `rel32`; after relocation their targets became original+delta, jumping into the cave page's zero-fill region → NULL deref. `EIP=0x3340E3B` (offset 0xE3B inside the cave page, far beyond the 668B blob).
   - **修复 / Fix**：新增**相对分支重定位**——目标在 Game.exe 映像内（外部引用）→ `rel32 -= delta`；目标在 blob 内部 → 保持不动（相对位移与基址无关）；两者皆非 → 当数据不碰（防误把 `MOV reg,imm` 的 `0xE8` 当 CALL）。
   - *Offline simulation / 离线仿真*: 31 条分支 → 24 外部全部落回合法 Game.exe VA；7 内部落 `[X, X+blob)`；2 误报正确跳过。/ 31 branches → 24 external all resolve to legal Game.exe VAs; 7 internal land in `[X, X+blob)`; 2 false-positives correctly skipped.

### 验证 / Verification
- **中文**：用户实跑确认**不再崩溃** → 标记里程碑。DLL 497152B 内嵌 668B blob（offset 0x59DA0）；8 跳板命中 blob；部署版 `.ini` 零残留 E9 跳板；`[CulturesPatches] install` 日志无 FAILED。
- **EN**: User confirmed **no crash** on a real run → milestone marked. DLL 497152B embeds the 668B blob (offset 0x59DA0); all 8 trampolines hit the blob; deployed `.ini` has zero stale E9 trampolines; `[CulturesPatches] install` log shows no FAILED.

### 配置 / Config
- **中文**：全部 22 个补丁由 `[CulturesPatches]` 总开关 + `patches/*.ini` 控制；纯字节补丁走 `ApplyPatchDir()`，code-cave 走 `ApplyCodeCaves()`（先于 `ApplyPatchDir` 调用）。
- **EN**: All 22 patches are governed by the `[CulturesPatches]` master switch + `patches/*.ini`; byte patches go through `ApplyPatchDir()`, code-caves through `ApplyCodeCaves()` (called before `ApplyPatchDir`).

### 关联产物 / Artifacts
- **中文**：`caves_blob.h`（668B blob）、`cave_trampolines.h`（8 跳板表）、`/tmp/regen.py`（抽取 22 补丁、拆分跳板、生成两 header、探测 8 处绝对自引用）。
- **EN**: `caves_blob.h` (668B blob), `cave_trampolines.h` (8-entry trampoline table), `/tmp/regen.py` (extracts 22 patches, splits trampolines, generates both headers, detects 8 absolute self-refs).

---

## v0.3.0 — 文化II 战役屏可加载 + 全解锁 Feature（里程碑，2026-08-11）

> 里程碑：主菜单"文化II：阿斯加德之门"按钮可用、点击进入文化II 大地图、成功进入第一关、不崩溃。

### 新增功能
- **Cultures2Campaign Feature —— 文化II 战役屏加载（里程碑落地）**
  - 复用主菜单 **screen 5（北国风云模板）**，进入前把 **7 处战役相关立即数**切换到文化II 数据表：
    - handler 5 处：`cmp ecx,2→1`（战役号）、节点坐标表基址 `0x5089F8 → DLL 内 kNodesC2`；
    - painter 2 处：路线表 `0x4F7EE8 → DLL 内 kRoutesC2`、`push 2 → push 1`（战役号）。
  - 三个 hook（全部 DLL 驻留，零 code cave）：
    1. `SwitchScreen` 入口仲裁 @ `0x4D1E45`：进 screen 5 前按 `g_pendingC2` 决定用哪套数据；**粘性模式**（离开 screen 5 才复位，修复"翻回北国风云"白屏 bug）。
    2. `MainMenu_OnCommand` 跳表 slot[6] @ `0x4D62BD`：ctl `5006` 原版空闲 → 指向 `CmdAsgardStub`（`g_pendingC2=1` + `SwitchScreen(5)`）。
    3. `start_campaign_1_screen` 消费尾部 @ `0x40223B`：通关后回文化II 大地图而非北国风云。
  - **白屏 bug 修复**：全程序 44 个 `AddUnlock` 调用点无一解锁 campaign 1（文化II 从未被引擎初始化），故首次进入时调用引擎 `AddUnlock(0x416D62)` 播种首关 `node 10`（只首关，后续由通关流程逐步解锁）。
  - DLL 内数据表：`kNodesC2[]`(10 节点，源原版 Gates of Asgard)、`kRoutesC2[]`(9 路线 → bmd 帧 26..34)。
  - **bmd 前置**：`Data\gui\lang\ger\bobs\ls_menu_logos.bmd` 须为 35 帧版（原 26 + 文化II 路线帧 26-34，已部署）。
- **UnlockAllCampaigns Feature —— 无视进度全解锁**
  - hook `sub_416E20`(IsUnlocked) 恒返回 1（DLL 内 `IsUnlockedStub`：`mov eax,1 / ret 8`）。
  - IsUnlocked 仅决定"关卡可否游玩"，完成判定走另一张表 → 全解锁不影响通关记录完整性。
  - 调用面：全程序仅 6 处（均在战役屏 handler/painter），主菜单战役是否列出由地图数据决定、与此无关 → 一处 hook 即覆盖所有战役所有关卡。
  - 默认关闭（`Enabled = 0`），改 `1` 即生效，无需重编。
- **Asgard 按钮 ctlId 由 5015 → 5006**：5015 原版已占（= UserCampaign00 屏，是"点了跳错"根因）；5006 跳表项现由 Cultures2Campaign 接管。

### 配置
```ini
[Cultures2Campaign]
Enabled = 1
[UnlockAllCampaigns]
Enabled = 0        ; 1 = 无视 campaign.ini 解锁全部战役/关卡
```

---

## v0.2.0 — AsgardCampaign 战役入口（2026-08-10, commit `ac68242`）

### 新增功能
- **主菜单"文化II：阿斯加德之门"战役入口按钮**（AsgardCampaign Feature）
  - 位于"单人游戏"菜单的"北国风云"按钮**之上**（case 3 按钮序列：教程 → 自由游戏 → **文化II** → 北国风云 → 第八世界奇迹 → 萨迦 → 自定义战役 → 返回）
  - hook `MainMenuUI_Build` case 3 @ `0x4D295E`（Nordland 按钮对象创建的 `call operator new`，5 字节 E9 跳转）
  - code cave @ `0xF22CB`（116 字节）复刻原生按钮创建序列：`new(0x60) → GetSagaText(文本对) → UI_CreateButton → sub_439B9C → UI_AddButton → 坐标累加`
  - 按钮文本：saga 表（表 13）ID 25/26，写入 `Data\text\l10\strings\saga\saga001.ini`（"《文化II：阿斯加德之门》战役"）
  - 控件 ID：`0x1397`（5015，case 3 空闲槽）
- **配置**（`plugins\config\CulturesGameExtend_Game.ini`）：
  ```ini
  [AsgardCampaign]
  Enabled = 1
  ; AddButton = 1
  ; CodeCaveStart = 0xF22CB
  ```

### 修复（开发过程中的三次崩溃）
1. **VA/RVA 混淆**：IDA VA（0x4D295E）被当 RVA 加 base → 越界 0x8D295E → verify mismatch。全部改 RVA。
2. **FillRel 覆盖 E8/E9 操作码**：`memcpy(buf+off)` 应为 `buf+off+1` → cave 首字节被写成 rel → 乱码执行。
3. **kCave 实际字节位置与 FillRel 参数错位**：改布局后 call/jmp 实际位置（76/92/111）与参数（79/98/116）脱节 → rel 填错位 → 跳 0x7D4F231C 崩。修复并加入 Python 字节级验证。

### 关键约定（新增代码必读）
- 项目两套地址：`gameapi::Va()` 传 **VA**（0x4xxxxx）；`Patch::WriteBytes(base+off)` 传 **RVA**
- 手写机器码数组：offset 必须以**实际字节位置**为准，改动布局后用脚本验证所有 E8/E9 位置

---

## v0.1.0 — 架构重构 + 用户战役增强（2026-08-10, commit `68d976e`）

### 架构
- Core 核心模块：`FeatureManager`（Feature 自注册）、`GameApi`（游戏函数集中绑定）、`Patch`（内存写入）、`IniConfig`、`Logger`、`GameVersion`
- Features 功能模块：`CulturesPatches`、`UserCampaigns`
- `build_deploy.sh`：一键 cl 编译 + 部署到游戏目录（绕开 VS Release 脏中间产物）
- `cultures-saga-patches` 子模块（Python 静态补丁工具，补丁数据权威来源）

### 功能
- **UserCampaigns**：`datax\usercampaigns\campaign00/01` 支持**文件夹形式**地图加载
  - 轮询 `g_pCampaignStaticDataMgr` 就绪后，枚举子目录 + `IniFile_Open` + `LoadCampaignMap` 注册
  - 兼容两种布局：`<folder>\map.ini`（目录根）与 `<folder>\currentusermap\map.ini`（c2m 解包）
  - 配置 `AllowCurrentUserMapFolder`（=1 额外扫 currentusermap 布局）
- **CulturesPatches 补丁 verify 机制**：ini 行格式 `<addr> <new bytes> | <verify bytes>`
  - 写入前校验原字节，不符跳过（对齐 Python `verify_and_replace` 语义）
  - 全部 17 个补丁 ini 已注入 verify 列（从 Python expected 批量生成，3 份目录同步）

### 修复
- **humanLimits.ini**：`0xb2ae` 数值错误（0x18C380 → 0xF116C0，5000×0xC58），曾致堆损坏崩溃（Game.exe.49516.dmp）
- **multiplayerTransferSize.ini**：`0xDFD8D` 偏移错误（0xFFFFAE50 → 0xFFFFAEB0）
- 部署版本错配问题（旧 DLL + 新 ini 导致 `bad byte '|'` 全跳过）——建立"配置/补丁与 DLL 必须同步部署"约定

---

## 版本记录（全部提交）

| 版本 | Commit | 日期 | 内容 |
|---|---|---|---|
| v0.2.0 | `ac68242` | 2026-08-10 | AsgardCampaign 战役入口按钮 |
| v0.1.0 | `68d976e` | 2026-08-10 | 架构重构 + UserCampaigns + verify 机制 + build_deploy.sh |
| - | `b8a9379` | - | 添加项目文件 |
| - | `76a5f58` | - | 添加 .gitattributes / .gitignore / LICENSE |
