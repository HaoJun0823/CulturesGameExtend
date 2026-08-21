# CHANGELOG — CulturesGameExtend（中英双语 / Bilingual）

> DLL proxy 增强框架（游戏加载 `dinput8.dll` 代理 → `plugins\CulturesGameExtend.dll`）
> 目标游戏：Cultures（Saga 版）`Game.exe`（基址 0x400000）
> 部署：`build_deploy.sh`（cl 命令行编译 + 拷贝干净产物，避开 VS Release 中间产物）

---

## v0.6.0 — PerMapLogic 重载稳定化 + 4 个崩溃守卫 Feature（里程碑 / milestone，2026-08-21，commit `156bf89`）

> 里程碑 / Milestone：PerMapLogic 重载功能从"多轮对话不稳定、重载即崩 #DE"变为**实机验证稳定**——`campaign_01_01` 重载 12/12 表且运行时自校验 `dwords changed vs pre-reload = 0`（per-map 表与 global 逐字节一致），全程零崩溃、零新 dump。版本锚 / Version anchor：`v2026-08-21-permaplogic-stable`。
> Milestone: the PerMapLogic reload feature went from "unstable across many dialogues, crashing on reload (#DE)" to **verified stable** — `campaign_01_01` reloads 12/12 tables with a runtime self-check `dwords changed vs pre-reload = 0` (per-map tables byte-identical to global), zero crash, zero new dump.

### 背景 / Background
- **中文**：重载崩溃根因是 **per-map 数据损坏**（2026-08-14 批量生成时动物段整体平移一位：global `chicken` 落槽 30、per-map 落槽 29），叠加派生 resolver 未在重载后重建 → 所有按索引缓存的槽位错位 → `#DE` / `atomicanimations` 越界 AV。此前应急的 DivZeroGuard（SKIP 思路）被用户明确不认可其根因处理；真正根治须走"数据保真 + 重载后重建全部派生 resolver"，而非"去缓存"或"SKIP 兜底"。
- **EN**: The reload crash root cause was **corrupt per-map data** (a 2026-08-14 batch-generation bug shifted the animal segment by one: global `chicken` at slot 30, per-map at 29), compounded by derived resolvers not being rebuilt after reload → every index-cached slot misaligned → `#DE` / `atomicanimations` OOB AV. The earlier emergency DivZeroGuard (SKIP approach) was explicitly judged by the user as not addressing the root cause; the real fix is "faithful data + rebuild all derived resolvers after reload", not "remove cache" or "SKIP".

### 新增 / Added
- **中文**：`PerMapLogicFeature::ReloadLogicForMap()` 末尾统一重建全部按索引缓存的派生 resolver：
  - `sub_415E8A()` 重解 good→landscape +36 句柄（仅当景观表非空）；
  - `sub_412B62()` 重建 land→goodid 索引（`dword_510C60`）；
  - 调用点加合理性守卫：loader 函数指针须在 `[0x400000,0x600000)` 否则降级为日志 SKIP，杜绝 `call 1` 类越界执行。
- **中文**：4 个守卫 Feature（REGISTER_FEATURE 自注册，dllmain.cpp include）：
  - `AnimalTypeGuardFeature` — **根因修复**：实体创建入口 `sub_40ADB1` 校验 movespeed 索引（= `sub_40AF62(name)` 解析结果）==0 则 `return -1`，游戏自有链路干净跳过坏 animaltype（不创建、不留残废对象），根治 `EXCEPTION_INT_DIVIDE_BY_ZERO (0xC0000094)` 家族（Game.exe.42700/24464/22816）。
  - `AtomicAnimGuardFeature` — `atomicanimations` 表索引越界守卫：`sub_42E38A` 内层循环读垃圾子元素计数越界 AV（Game.exe.32664.dmp）；洞穴校验 `this->0x78 < dword_5112F8`，越界跳游戏自有早退点 `0x42E646`。
  - `CustomSafetyPatchesFeature` — `sub_493EF2` 两处 `div` 字节防护（独立于上游 CulturesPatches，配置放 `CulturesGameExtend_CustomSafety.ini`，便于未来同步上游）。
  - `DivZeroGuardFeature` — `0x42DE1F` / `0x446EAF` 两处 `idiv` 除零兜底（运行期代码洞穴做 `divisor==0 跳过`）。**防御纵深**，dllmain 注释已明确标注其 SKIP 性质（停除法但留 movespeed=0 残废实体），非根因方案；真正的根因修复是 `AnimalTypeGuard`。
- **中文**：`tools/scan_comment_splice.py` — 扫描 `// 注释\` 行尾反斜杠吞行隐患（C/C++ 阶段 2 行拼接会把下一行代码并入注释 → 数组/结构体静默少编译一项 → 运行期错位崩溃）。
- **中文**：`Paths.h` 新增 `kCustomSafetyIni`；`dllmain.cpp` 加载 `CulturesGameExtend_CustomSafety.ini` 并 merge 入主配置。

### 修复 / Fixed
- **中文**：编译开关 `/we4010` 把"注释行尾反斜杠"从警告升级为错误，从编译期卡死该隐患；`build_deploy.sh` 新增 [1.5/3] `grep landscapetypes.ini` 安全网——若 `kLogicFiles` 少编译 1 项则立即 `exit 1`，而非等游戏崩（对应 5688 dump 根因）。
- **中文**：`build_deploy.sh` [4/4] 本地化拷贝包进 `set +e` + `|| echo`，单个 `cp` 失败（如文件被锁、`_build/DataX` 缺失）不再中止整个部署——崩溃修复的 DLL 已在 [2/3] 落地。
- **中文**：字体资源 `Resource/plugins/fonts/l10.ini` → `l11.ttf`（字体切换）。

### 验证 / Verification
- **中文**：用户实跑 `campaign_01_01` 等图：**无崩溃**；`logs/CulturesGameExtend.log` 仅 INFO 级，`[resolver]` 每次重载后都正确重建、`[diff] dwords changed vs pre-reload = 0`、0 条 `Dropped animaltype entity`、AnimalTypeGuard 全程未触发；per-map `tribetypes.ini` 与 global 逐字节一致（chicken 槽 30/30 对齐）。
- **中文**：下游 `Data/` 侧数据修复（301 个 per-map 文件重生成 global 忠实副本、回滚 type 42 adult_animal 非白名单区块）属运行期数据层，本仓库不含游戏 `Data/`，此处仅记录源码层根治；详见项目记忆 `MEMORY.md` 的 "PerMapLogic 崩溃根因"。

### 配置 / Config
```ini
[AnimalTypeGuard]
Enabled = 1     ; animaltype 除零崩溃根因修复（C 兜底）

[CustomSafetyPatches]
Enabled = 1
```
（DivZeroGuard / AtomicAnimGuard 默认随 Feature 注册，开关见各自 `[...]` 段；CustomSafety 补丁明细见 `Resource/plugins/config/custompatches/`）

### 关联产物 / Artifacts
- **中文**：`tools/scan_comment_splice.py`（注释吞行扫描器）；`Resource/plugins/config/CulturesGameExtend_CustomSafety.ini` + `custompatches/`（自定义安全补丁）；`Resource/plugins/fonts/l11.ttf`（字体资源）。

---

## v0.5.0 — WarningLog：记录 Warning! 开发者断言框 + 禁止其关闭游戏（里程碑 / milestone，2026-08-13，commit `d73dac7`）

> 里程碑 / Milestone：根治 `Warning!` 弹框（实为开发者 assert 残留）在玩家按 Cancel/X/Esc 时 `__debugbreak` → 无调试器 → 进程自杀的问题。版本锚 / Version anchor：`v2026-08-13-warninglog`。
> Milestone: fixes the `Warning!` box (a leftover developer assert) killing the process via `__debugbreak` when the player presses Cancel/X/Esc with no debugger attached.

### 背景 / Background
- **中文**：`sub_47A14D`（所有 `Warning!` 框共用封装）尾部 `int 3` 是"Cancel→`__debugbreak`→无调试器→进程自杀"的根因。一个放置错误（`Can't set House near Position %d %d!!!`，来自战役结尾 switch `sub_41FB7A` 某 case）本应无害，却因这个共享 helper 被当成开发者断言而关掉整个游戏。
- **EN**: The `int 3` at the tail of `sub_47A14D` (the shared wrapper for all `Warning!` boxes) is the root cause — Cancel triggers `__debugbreak`, which kills the process when no debugger is attached. A harmless placement error (`Can't set House near Position %d %d!!!`, from a case in the campaign-ending switch `sub_41FB7A`) was shutting down the whole game because this shared helper treats it as a developer assert.

### 新增 / Added
- **中文**：`Features/WarningLogFeature.cpp`（REGISTER_FEATURE 自注册 + `dllmain.cpp` include）。对 `sub_47A14D` 内唯一的 `call ds:MessageBoxA`（`0x47A1B8`，6 字节）做 trampoline hook：
  - stub 读出已格式化的 `lpText`（`[ebp+8]`，如 `"Can't set House near Position 12 34!!!"`）与调用者返回地址（`[ebp+4]`）写入独立日志；
  - 随后 `add esp,16` + `mov eax,1`（模拟按 OK / `IDOK=1`，使 `dec;dec` 得 -1 不触发 `int 3`） + `jmp 0x47A1BE` 续跑 —— **完全跳过弹框、不触发 `int 3`、且保留原函数光标恢复等副作用**。
  - 所有 `Warning!` 调用者（7 处代码 + 虚表 `sub_478822`）均经此唯一 `MessageBoxA` 调用，一次 hook 全覆盖。
- **中文**：`Core/Logger.{h,cpp}` 新增独立 warning 日志：`WarnLogInit(dir,"GameWarnings.log")` + `WarnLogWrite(fmt,...)`（并行 `g_warnFile`/mutex，写 `logs/GameWarnings.log`，沿用既有 flush + `OutputDebugStringA` 模式）。日志每行形如 `[时间] caller=0x4xxxxxxx text=...`。

### 修复 / Fixed
- **中文**：`ForbidExit` 安全网 —— 1 字节补丁 `Patch::WriteU8(0x47A1C3, 0x90)` 把 `int 3`(CC) NOP 成 `nop`(90)。即便 hook 万一未装上，Cancel 也不再致命（返回值完全不变 → 对所有调用者零风险）。
- **中文**：顺带发现 `sub_47A14D` 的死代码 bug —— `strcpy(Destination[26], lpText)` + `strcat(...aPressCancelToD)` 拼出"按 Cancel 调试"提示，但 `MessageBoxA` 实际传的是 `lpText` 而非 `Destination`（提示从不在框中显示，且对长文本会栈溢出）；因 `Destination` 未被使用，目前无害（属潜伏 bug，仅记录未修）。

### 验证 / Verification
- **中文**：编译通过（v141 cl.exe，EXIT=0，DLL 501760B）；完整部署（`build_deploy.sh`）成功，DLL 已落 `SAGA_GAME_HACK/plugins/CulturesGameExtend.dll`，功能默认全开（无需改 ini）。运行游戏触发任一 `Warning!` 后，`logs/GameWarnings.log` 应出现 `caller=0x... text=...` 行，且游戏不再因该警告关闭、弹框被静默跳过。

### 配置 / Config
```ini
[WarningLog]
Enabled = 1     ; 总开关
ForbidExit = 1  ; 禁止退出（NOP int3 安全网）
LogEnabled = 1  ; 记录日志（hook 接管 MessageBoxA）
```

### 关联产物 / Artifacts
- **中文**：`logs/GameWarnings.log`（运行时生成，位于游戏目录 `logs/`）。

---

## v0.5.1 — MapLoaderExtra：弃 hook 复刻链，改用原生函数注册（里程碑 / milestone，2026-08-13，commit `8522781`）

> 重构 / Refactor：把 `MapLoaderExtraFeature` 从「hook `l_IO_Load` + 复刻引擎内部脆弱注册链」改为「与 `UserCampaignsFeature` 同款的原生函数调用」。`LoadCampaignMap`(0x410E6D) 自己解析 `map.ini`/`map.dat`，路径按相对 CWD 走，从根本上消除旧版的崩溃（绝对路径构造坏指针 → `sub_425072` 虚调用崩）。
> Refactor: `MapLoaderExtraFeature` now calls the native `IniFile_Open` + `LoadCampaignMap` pair (same pattern as `UserCampaignsFeature`) instead of hooking `l_IO_Load` and replaying the engine's fragile internal chain. Removes the crash root cause entirely.

### 背景 / Background
- **中文**：旧版 `MapLoaderExtraFeature` hook `sub_410B58`(l_IO_Load) 入口并复刻 `sub_4064E4→sub_424EF8→sub_410E6D→sub_425072` 脆弱链，对绝对路径构造垃圾指针 → `sub_425072` 虚调用崩溃（dump `Game.exe.15708.dmp` @0x42508D 实锤）。应急 SEH 兜底后地图注册失败（日志 `registration threw exception; skipped`）。软连接方案同样脆弱（需 `SeCreateSymbolicLinkPrivilege`、FAT32/exFAT 不支持、跨卷重解析点不保证跟随）。
- **EN**: The old `MapLoaderExtraFeature` hooked `sub_410B58` and replayed the fragile `sub_4064E4→sub_424EF8→sub_410E6D→sub_425072` chain, building a bad pointer on absolute paths → `sub_425072` virtual call crash (dump `Game.exe.15708.dmp` @0x42508D). SEH fallback only made map registration fail (`registration threw exception; skipped`). Symlinks are equally fragile (need `SeCreateSymbolicLinkPrivilege`, unsupported on FAT32/exFAT, reparse-point I/O not guaranteed).

### 修复 / Fixed
- **中文**：彻底删除 hook/naked stub/MakeTrampoline/SEH 兜底，改为战后（战役管理器就绪后轮询）对每个 `ExtraMapPaths` 根目录枚举子目录，逐张 `IniFile_Open(map.ini) → LoadCampaignMap(src, a4=0) → IniFile_Close` —— 与 `UserCampaignsFeature::RegisterFolderMap` 逐字节一致。
- **中文**：新增 `[MapLoaderExtra] CampaignId`（默认 0）：0=单图/遭遇战列表（等同 `data\maps`）；7/8=用户战役列表（第三方独立文件夹最稳）。选 0 时 `src` 传完整相对路径 `.\CustomMaps\<子目录>`，Source 模式直接定位，不依赖 `data\maps` 前缀。

### 验证 / Verification
- **中文**：`build_deploy.sh` EXIT=0，DLL 507904B 已部署；字节级确认新版串在、旧 hook 串全 MISS。**用户实跑验证通过**：`.\CustomMaps\<合法地图>\` 被成功注册（日志 `registered: ... (campaign 0, ok)`）且能正常进图，旧版崩溃/`registration threw exception; skipped` 彻底消失。campaignId=0（单图/遭遇战列表）路径可行；若日后个别第三方文件夹仍注册失败，可改 ini `CampaignId=7/8`（usercampaign 已验证稳，无需重编译）。

### 配置 / Config
```ini
[MapLoaderExtra]
Enabled = 1
ExtraMapPaths = .\CustomMaps   ; 分号分隔；每个根目录下的子目录 = 一张地图
CampaignId = 0                 ; 0=单图/遭遇战；7/8=用户战役（第三方文件夹最稳）
```

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
| v0.4.0 | `b426b73` | 2026-08-13 | CulturesPatches 全量实现 + 运行期 code-cave 注入稳定（里程碑） |
| v0.3.0 | - | 2026-08-11 | 文化II 战役屏加载 + 全解锁 Feature（里程碑） |
| v0.2.0 | `ac68242` | 2026-08-10 | AsgardCampaign 战役入口按钮 |
| v0.1.0 | `68d976e` | 2026-08-10 | 架构重构 + UserCampaigns + verify 机制 + build_deploy.sh |
| - | `b8a9379` | - | 添加项目文件 |
| - | `76a5f58` | - | 添加 .gitattributes / .gitignore / LICENSE |

---

## 附录 A：项目铁律与部署纪律 / Appendix A: Project Iron Rules & Deployment Discipline

> 本节汇总跨特性、反复踩坑后固化的约定。完整细节见项目记忆 `MEMORY.md` 与各日日志。
> This section distills cross-cutting conventions hardened after repeated pitfalls. Full detail lives in project memory `MEMORY.md` and the daily logs.

### A.1 部署纪律 / Deployment Discipline
- **中文**：`build_deploy.sh` 可能**静默编译失败**（grep 吞错）→ 每次部署后必须用 Python **字节级验证 DLL**（搜关键字符串）。配置/补丁（`*.ini`）必须与 DLL **同步部署**——旧 DLL + 新 ini 会导致 `bad byte '|'` 全跳过。
- **EN**: `build_deploy.sh` can **silently fail to compile** (grep swallows errors) → after every deploy, **byte-verify the DLL** with Python (search key strings). Config/patches (`*.ini`) **MUST deploy together with the DLL** — an old DLL + new ini makes `bad byte '|'` skip everything.

### A.2 code-cave 重定位铁律（CulturesPatches）/ Code-Cave Relocation Iron Rule
- **中文**：把 blob 经 `VirtualAlloc(NULL)` 搬进独立页时，重定位必须**同时**处理两类引用：
  1. **绝对自引用**（4 字节值落在 blob 窗口内）→ `+delta`；
  2. **相对外跳**（`E9/E8/Jcc` 的 `rel32` 目标在 Game.exe 映像内）→ `rel32 -= delta`。
  只做①漏② → 跳进 cave 零页执行 → 解引用 NULL 崩溃。**严禁** `VirtualAlloc(MEM_COMMIT, …)` 落在映像保留间隙 → `err=5 ACCESS_DENIED`。
- **EN**: When relocating a blob into an independent page via `VirtualAlloc(NULL)`, relocation MUST handle **BOTH** kinds of references: (1) **absolute self-references** (4-byte values inside the blob window) → `+delta`; (2) **relative external jumps** (`E9/E8/Jcc` `rel32` whose target is inside the Game.exe image) → `rel32 -= delta`. Doing only (1) → jumps into the cave zero-page → NULL deref crash. **Never** `VirtualAlloc(MEM_COMMIT, …)` inside an image-reserved gap → `err=5 ACCESS_DENIED`.

### A.3 地址约定 / Address Convention
- **中文**：`gameapi::Va()` 收 **VA**（0x4xxxxx）；`Patch::WriteBytes(base+off)` 收 **RVA**。二者不可混用（曾因把 IDA VA 当 RVA 加 base 越界）。
- **EN**: `gameapi::Va()` takes **VA** (0x4xxxxx); `Patch::WriteBytes(base+off)` takes **RVA**. Never mix them (once caused out-of-bounds by adding base to an IDA VA).

### A.4 引擎层铁律 / Engine-Layer Iron Rules
- **中文**：
  - **logic 表重载**须重跑派生 resolver（`sub_415E8A` + `sub_412B62` 重建 `dword_510C60`），否则"无图标 / 物品放不进 / 采集空"；流程：影子表 → memcpy → resolver → 整表 diff 自校验。
  - **naked stub** 调 C 函数后 `jmp __thiscall` 原函**必须还原 ecx(this)**（ebx 暂存）。
  - **BlitGlyph 必须上界裁剪**（fbW/fbH），越界写即崩。
  - **hook 外部对象指针前必须 VirtualQuery 守卫**（猜错=不渲染+日志，不踩内存）。
  - c2m 联机传图整树递归打包无白名单（含 `logic\`，dotfile 除外）。
- **EN**:
  - **logic table reload** must re-run the derived resolver (`sub_415E8A` + `sub_412B62` rebuild `dword_510C60`), else "no icon / items can't be placed / gathering empty"; flow: shadow table → memcpy → resolver → whole-table diff self-check.
  - A **naked stub** calling a C function then `jmp __thiscall` original **MUST restore ecx(this)** (stash in ebx).
  - **BlitGlyph MUST clamp to upper bound** (fbW/fbH); out-of-bounds write crashes.
  - **Before hooking an external object pointer, VirtualQuery-guard it** (wrong guess = no render + log, never stomp memory).
  - c2m multiplayer map transfer packs the whole tree recursively with no allowlist (includes `logic\`, except dotfiles).

### A.5 ini / 补丁配置约定 / ini & Patch Config Convention
- **中文**：双语注释 = 同组先中文块后英文块，禁用 `|` 分隔、不逐行交叉；`patches.ini` 单一总开关 `[CulturesPatches] Enabled`，`patches/*.ini` 社区可直接增删。
- **EN**: Bilingual comments = same group: Chinese block first then English block; never use `|` as separator, don't interleave line-by-line; `patches.ini` single master switch `[CulturesPatches] Enabled`; `patches/*.ini` can be added/removed by the community directly.

---

## 附录 B：TextRenderer 最终状态（摘要）/ Appendix B: TextRenderer Final State (Summary)

> 富文本渲染 Feature，用户于 2026-08-12 确认 `v2026-08-12-richtext-compact` 正常。里程碑链：engine-color → tooltip-fixed → autoposition → **richtext-compact** (`762e9d5`)。完整细节见 `MEMORY.md` 的 "★★ TextRenderer 最终状态"。
> Rich-text rendering Feature, user-confirmed `v2026-08-12-richtext-compact` normal on 2026-08-12. Milestone chain: engine-color → tooltip-fixed → autoposition → **richtext-compact** (`762e9d5`). Full detail in `MEMORY.md` "★★ TextRenderer 最终状态".

### B.1 架构 / Architecture
- **中文**：字形层 chokepoint hook `sub_439610`（原子 blit 汇合点，唯一文本汇合）+ `sub_439633`（字宽）——**全字符 GDI 接管**（ASCII 也接管，用户拍板）。
- **EN**: Glyph-layer chokepoint hook `sub_439610` (atomic blit convergence, the only text convergence point) + `sub_439633` (glyph width) — **full GDI takeover of all characters** (ASCII included, per user decision).

### B.2 `sub_439610` 调用约定【最终定案】/ Calling Convention [FINAL]
- **中文**：`__thiscall`，`ecx`=字体对象；栈 5 参 `[ebp+8]`=颜色{aBGR}、`，` `[ebp+0xC]`=**DrawContext**、`[ebp+0x10]`=ch、`[ebp+0x14]`=x、`[ebp+0x18]`=y。跳板抄 6 字节 → `0x439616`。
- **EN**: `__thiscall`, `ecx`=font object; 5 stack args `[ebp+8]`=color{aBGR}, `[ebp+0xC]`=**DrawContext**, `[ebp+0x10]`=ch, `[ebp+0x14]`=x, `[ebp+0x18]`=y. Trampoline copies 6 bytes → `0x439616`.

### B.3 DrawContext 结构 / DrawContext Struct
- **中文**：`+0x2C`=像素基址、`+0x30`=pitch(px)、`+0x08..0x14`=clip{x,y,w,h}、`+0x18`=**宽-1 非高度**、`+0x38`=pitch(字节)→bpp=`+0x38`/`+0x30`（**主界面 16bpp**，32bpp 双路径）、`+0x50`=等宽推进。真实高度靠 `ProbeFbHeight` 探测。
- **EN**: `+0x2C`=pixel base, `+0x30`=pitch(px), `+0x08..0x14`=clip{x,y,w,h}, `+0x18`=**width-1 NOT height**, `+0x38`=pitch(bytes)→bpp=`+0x38`/`+0x30` (**main UI 16bpp**, dual path for 32bpp), `+0x50`=monospace advance. True height via `ProbeFbHeight`.

### B.4 位置模型 / Position Model
- **中文**：ASCII=半格(cellW/2)、CJK=全格(cellW)；slotW 渲染与 `OnGlyphWidth` 同步；垂直=`FontLineHeight(font[8]+2)` 自适应；水平 `kBearingX=3` 内置；持久表面(Tooltip, `ShouldSkipRepeat` 命中)用墨迹居中零偏移。
- **EN**: ASCII=half cell (cellW/2), CJK=full cell (cellW); slotW render synced with `OnGlyphWidth`; vertical=`FontLineHeight(font[8]+2)` auto; horizontal `kBearingX=3` built-in; persistent surfaces (Tooltip, `ShouldSkipRepeat` hit) use centered ink zero-offset.

### B.5 关键补丁与开关 / Key Patches & Toggles
- **中文**：
  - 词后空格修复 = **NOP `0x4CA612`**（`add [esi+38h],eax` → `90 90 90`），取消每词后空格宽 → 中文紧凑。★ `0x4CA612` 的 E9 hook 会让**全部文字消失**（连纯跳转 stub 也消失，机制未明）→ NOP 方案**禁用**。
  - `WordSplitPatch=1` 启用（补丁① `0xD053B` 单字节化 → 富文本词=单字节 → 不超宽 → 长文本完整）；`SpaceHex` 实测词缓冲=单字节 GBK 首字节（E8 00），`=0` 时词=整段 → 超宽溢出/消失。
  - `WordColStub`（`sub_4E21B7` 词收集 `0x4E2242`，CJK 单码点词）+ `OverflowStub`（`0x4E2267` 超宽词拆字）保留；补丁②（`0xE2251`）NOP 必死循环，**不可用**。
  - `OnGlyphWidth` NUL 修复（`ch<=0→0`）：修词尾 `\0` 白加 9px（"分散"另一成因）；`HalfCellExtra=2` 定稿；`AntiAlias` 开关（无影响暂不动）。
- **EN**:
  - Word post-space fix = **NOP `0x4CA612`** (`add [esi+38h],eax` → `90 90 90`), cancels per-word trailing space width → compact CJK. ★ An E9 hook at `0x4CA612` makes **ALL text vanish** (even a pure-jump stub) — mechanism unknown → NOP approach **DISABLED**.
  - `WordSplitPatch=1` active (patch① `0xD053B` single-byte → rich-text word=single byte → no overflow → full long text); `SpaceHex` measured word buffer = single-byte GBK lead (E8 00); `=0` → word=whole segment → overflow/vanish.
  - `WordColStub` (`sub_4E21B7` word collect `0x4E2242`, CJK single-codepoint word) + `OverflowStub` (`0x4E2267` overflow word split) retained; patch② (`0xE2251`) NOP causes infinite loop — **unusable**.
  - `OnGlyphWidth` NUL fix (`ch<=0→0`): fixes spurious +9px at word-end `\0` ("分散" other cause); `HalfCellExtra=2` finalized; `AntiAlias` toggle (no effect, untouched).

### B.6 超链接 hover 整句变亮（08-13 定案）/ Hyperlink Hover Whole-Sentence Brighten (08-13 FINAL)
- **中文**：hook `sub_4C9FC4`（token 重绘）入口 → `HoverRepaintStub`，`a4=1` **且** `token+32`（链接标志）≠0 才遍历。
  - **`sub_4C9FC4` 调用约定【易错】**：`__thiscall(this=排版对象, a2=绘制surface, a3=token, a4=1 变亮)`。`a4` 必须=1 才变亮；`surface(a2)` 必须与原始调用同一对象。`HoverLinkRepaint` 必须 4 参 `repaint(self,surface,tok,1)`；少传 `a4` → 栈垃圾 → 既崩又静默失效。`token+36`=linkId（同链接词分组）。
  - 排版结构：行列表头=排版对象+0x24、词列表头=行+0x08；迭代器 `sub_416CA0`(first)/`sub_4D0BE0`(next) `__thiscall`(ecx=state)，行/词各自独立 12 字节 state buffer。
  - 排版对象 `this` 来自 `sub_4C9FC4` 的 ECX（pushad 后 `[esp+4]`），**不是 `[esp+24]`（ESI 垃圾）**——2884 崩溃根因。
- **EN**: Hook `sub_4C9FC4` (token repaint) entry → `HoverRepaintStub`, `a4=1` **AND** `token+32` (link flag)≠0 to traverse.
  - **`sub_4C9FC4` calling convention [error-prone]**: `__thiscall(this=layout obj, a2=draw surface, a3=token, a4=1 brighten)`. `a4` MUST be 1 to brighten; `surface(a2)` MUST be the same object as the original call. `HoverLinkRepaint` MUST be 4-arg `repaint(self,surface,tok,1)`; missing `a4` → stack garbage → both crash and silent no-op. `token+36`=linkId (group same-link words).
  - Layout struct: line list head=layout obj+0x24, word list head=line+0x08; iterators `sub_416CA0`(first)/`sub_4D0BE0`(next) `__thiscall`(ecx=state), line/word each use independent 12-byte state buffer.
  - The layout obj `this` comes from `sub_4C9FC4`'s ECX (after pushad `[esp+4]`), **NOT `[esp+24]` (ESI garbage)** — root cause of the 2884 crash.
