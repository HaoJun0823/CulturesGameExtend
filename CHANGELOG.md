# CHANGELOG — CulturesGameExtend

> DLL proxy 增强框架（游戏加载 `dinput8.dll` 代理 → `plugins\CulturesGameExtend.dll`）
> 目标游戏：Cultures（Saga 版）`Game.exe`（基址 0x400000）
> 部署：`build_deploy.sh`（cl 命令行编译 + 拷贝干净产物，避开 VS Release 中间产物）

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
