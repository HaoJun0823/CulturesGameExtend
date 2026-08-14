# CulturesGameExtend Reverse-Engineering Findings (English)

> This document consolidates **every reverse-engineering conclusion in this solution (`CulturesGameExtend`)
> that has been verified against the actual cpp source.**
> All VAs (virtual addresses) assume the fixed `Game.exe` image base `0x400000`
> (no ASLR / `RELOCS_STRIPPED`). Items marked "verified" can be traced line-by-line in the
> corresponding `*.cpp` / `*.h`. Compiled 2026-08-13.

---

## 0. Table of Contents

1. [Project Scope & Overall Architecture](#1-project-scope--overall-architecture)
2. [Injection & Loading (dinput8 proxy)](#2-injection---loading-dinput8-proxy)
3. [Core Infrastructure (Patch / GameApi / IniConfig / Logger)](#3-core-infrastructure)
4. [Text Rendering System (TextRenderer + GdiFont)](#4-text-rendering-system)
5. [Campaigns & Movies (Cultures2 / Asgard / CampaignMovie)](#5-campaigns--movies)
6. [Per-Map Balance Table Reload (PerMapLogic)](#6-per-map-balance-table-reload-permaplogic)
7. [Community Static-Patch System (CulturesPatches / code cave)](#7-community-static-patch-system)
8. [Remaining Features (TitleOverride / UnlockAll / WarningLog / VersionStamp / UserCampaigns / MapLoaderExtra)](#8-remaining-features)
9. [Key Address Quick Reference](#9-key-address-quick-reference)
10. [Iron Rules & Lessons Learned](#10-iron-rules--lessons-learned)

---

## 1. Project Scope & Overall Architecture

`CulturesGameExtend` is a **community extension DLL** injected into the `Game.exe` process via a
`dinput8.dll` proxy. It layers a configurable set of features (CJK/UTF-8 text engine, campaign
extensions, per-map balance reload, patch system) on top of the original game **without modifying
`Game.exe` itself**.

### 1.1 Solution Layout

```
CulturesGameExtend/
├── CulturesGameExtend/        ; main project -> CulturesGameExtend.dll
│   ├── Core/                  ; game-agnostic infra (Patch / GameApi / IniConfig / GdiFont / ...)
│   ├── Features/              ; one .cpp per feature, self-registered via REGISTER_FEATURE
│   ├── dllmain.cpp            ; DllMain -> spawn thread -> RunExtend (avoids Loader Lock)
│   └── framework.h / pch.h
├── CulturesProxyDLL/          ; dinput8.dll proxy that injects CulturesGameExtend.dll
├── cultures-saga-patches/     ; authoritative patch-data source (matches Features/CulturesPatchesFeature)
├── Resource/                  ; mirror of game root (fonts / Data assets); build_deploy.sh copies root-to-root
└── build_deploy.sh            ; one-shot build + deploy (v141 cl.exe)
```

### 1.2 Feature Plugin Architecture (verified: `Core/Feature.h`, `Core/FeatureManager.cpp`)

- **Base class `Feature`**: three virtuals — `GetName()` (= INI `[Section]` name + log category prefix),
  `GetTarget()` (returns `GameTarget::Game` / `Any`), `OnInstall(IniConfig&, GameVersion&)` (install entry).
- **Self-registration macro `REGISTER_FEATURE(Cls)`** (`Feature.h:50`): defines a static object in an
  anonymous namespace whose constructor registers `new Cls()` into the global `FeatureRegistry`
  (Meyers singleton). **Consequence**: adding a feature only needs one `REGISTER_FEATURE(XxxFeature)` line
  at the bottom of its `.cpp`, plus a `#include` of that `.cpp` in `dllmain.cpp`; **do not also list the
  `.cpp` in the cl source list** (causes `LNK2005` duplicate definition).
- **`FeatureManager::InstallAll`** (`FeatureManager.cpp:27`): iterates the registry; for each feature reads
  `[Section] Enabled` (default 0) → skip if off; skip if `TargetMatch` fails; otherwise call `OnInstall`.
  Log reports `ok/attempted/registered/disabled`.

### 1.3 Three-Layer INI Merge (verified: `Core/IniConfig.cpp`)

Load order: `GlobalIniPath()` base → `GameIniPath()` override → `PatchesIniPath()` top override.
`IniConfig::Merge` (`IniConfig.cpp:67`) overwrites key-by-key; section/key names normalized to lowercase;
supports `0x`-prefixed hex (`GetInt`); comment lines cut at `;` / `#` (`StripComment`). Config changes take
effect only after a **game restart** (loaded once at startup, no hot reload).

### 1.4 Version Detection (verified: `Core/GameVersion.cpp`)

`GameVersion::Detect` resolves against the **exe's own directory** (`ge_paths::Resolve`, not CWD), default
`Game.exe`, overridable via `[Version] ExeName`; base `m_base = GetModuleHandle(NULL)` (= 0x400000).

---

## 2. Injection & Loading (dinput8 proxy)

> Verified: `CulturesProxyDLL/dllmain.cpp` (265 lines, with detailed English comments).

- **Proxy essence**: a renamed `dinput8.dll` placed in the game dir exports `DirectInput8Create`
  (`#pragma comment(linker,"/EXPORT:DirectInput8Create=_SHADOW_DirectInput8Create")`), whose stub
  `jmp [g_pOrigDIM8Create]` forwards to the real system `dinput8.dll` (`LoadLibraryW` + `GetProcAddress`).
  Without this forwarding the game fails with `0xc000007b`.
- **Injection list**: `plugins/dll_proxy.ini`, one DLL path per line (relative paths resolved against the
  **proxy's own directory**, not CWD); comments `#`/`;`; `LoadLibraryA` injects directly in-process
  (no `CreateRemoteThread` needed because the proxy already lives inside the game process).
- **Loader Lock avoidance**: `DllMain` only synchronously resolves the system `dinput8.dll` forward pointer
  (fast, no deadlock), deferring all heavy work (ini parsing, injecting CulturesGameExtend.dll) to a worker
  thread `ProxyWorker` created via `CreateThread`. Otherwise `LoadLibrary` into `CulturesGameExtend.dll`
  (whose own `DllMain` also needs the Loader Lock) → classic deadlock / silent failure.
- **Foreign-DLL warning**: `WarnIfForeignDll` logs a stability warning for any injected DLL other than
  `CulturesGameExtend.dll`.

---

## 3. Core Infrastructure

### 3.1 Patch (verified: `Core/Patch.cpp` / `Patch.h`)

- `WriteJmp(target, cave, nopCount)`: writes a 5-byte `E9 rel32` (`rel = cave-(target+5)`), padding
  `nopCount` extra bytes with `90`.
- `WriteBytes/WriteU8/WriteU32/WritePointer/ReadBytes/ReadMemory/WriteMemory/EnsureWritable`.
- `ProtectAndWrite` pattern: `VirtualProtect(PAGE_EXECUTE_READWRITE)` → patch bytes →
  `FlushInstructionCache` → restore original page protection. Every game-code write goes through this path.

### 3.2 GameApi (verified: `Core/GameApi.h` / `GameApi.cpp`)

- `g_imageBase` defaults to `0x400000`; set by `Init(base)`.
- Template `Va<T>(va) = g_imageBase + (va - 0x400000)`.
- Unified binding table `s_bindings` (`GameApi.cpp:23`): `{rva, (void**)&funcPtr}`; filled by `Init` per base.
  Bound: `LoadCampaignMap=0x410E6D`, `IniFile_Open=0x424EF8`, `IniFile_Close=0x425072`.
- **Known game symbols (confirmed by cpp comments)**:
  - `LoadCampaignMap` (0x410E6D): `__thiscall`, `this = *(DWORD*)Va(g_pCampaignStaticDataMgr)`,
    `g_pCampaignStaticDataMgr = 0x510BFC`. Arg `a4=1` = c2m mode, `a4=0` = Source-dir mode;
    `a6` = campaign ID (must be `<9`, see `0x411036 cmp eax,9`; out of range crashes).
  - `IniFile_Open` (0x424EF8): `__thiscall`, path at `[esp+4]` (PerMapLogic rewrites `[esp+0x28]`).
  - String tables: `StringTable_GetMainMenuText=0x4E15F6` (table 0),
    `StringTable_GetOdinText=0x4E1682` (table 11), `StringTable_GetSagaText=0x4E169E` (table 13).

### 3.3 GdiFont (verified: `Core/GdiFont.h` / `.cpp`)

A GDI glyph-rasterization engine, **game-agnostic and independently testable**:
- Input is a UTF-8 byte stream → `DecodeUtf8` decodes to Unicode code points (RFC 3629; out-of-range bytes skipped).
- Each code point is rasterized into an in-memory grayscale bitmap via Windows GDI from an installed/loaded TTF.
- **Rasterization uses DIB + `TextOutW`** (not `GetGlyphOutline`): `GetGlyphOutline`'s grayscale format is
  crash-prone without a display session; `TextOutW` is the most stable.
- **Must use `SetTextAlign(TA_BASELINE)`**: default `TA_TOP` treats `(x,y)` as the glyph's top edge, dropping
  the whole glyph below the canvas where it gets clipped.
- Glyph cache (`unordered_map` code-point → grayscale bitmap).
- `BlitGlyph`: alpha-blends the grayscale glyph into a **16bpp (RGB565)** or **32bpp (RGBA)** target buffer;
  supports `fbW/fbH` bounds clipping (prevents out-of-bounds write crashes), `clipX/Y/W/H` engine UI clip rect,
  and idempotent redraw (`idempotent`, avoids anti-alias darkening accumulation on uncleared surfaces).
- `CreateFromFile`: registers a `.ttf` into the process via `AddFontResourceEx(FR_PRIVATE)` (self-contained,
  no system font install needed).

---

## 4. Text Rendering System

> Verified: `Features/TextRendererFeature.cpp` (~1440 lines), `Core/GdiFont.*`.
> See also `SAGA_GAME_HACK/.workbuddy/memory/MEMORY.md` "TextRenderer final state".

### 4.1 Design Decisions (user decision 2026-08-12)

- **Full GDI takeover**: ASCII is also routed through GDI (not just CJK) for a unified render path.
- **Atomic convergence hook**: `sub_439610` (the single text-draw convergence, 3 callers: `sub_40ECF9`×2,
  `sub_4E1E3F`×1) + `sub_439633` (glyph width). `sub_4658C7` is the sole caller of `sub_439610`.
- **`sub_439610` calling convention (final)**: `__thiscall`, `ecx` = font object; 5 stack args
  `[ebp+8]`=color{aBGR}, `[ebp+0xC]`=**DrawContext**, `[ebp+0x10]`=ch, `[ebp+0x14]`=x, `[ebp+0x18]`=y.
  Trampoline copies 6 bytes → 0x439616.
- **DrawContext struct** (`+0x2C`=pixel base, `+0x30`=pitch(px), `+0x08..0x14`=clip{x,y,w,h},
  `+0x18`=width-1 (not height), `+0x38`=pitch(bytes) → bpp=`+0x38`/`+0x30`, **main UI is 16bpp**,
  32bpp dual path; `+0x50`=monospace advance). True height probed via `ProbeFbHeight`.

### 4.2 Key Fixes & Rules

- **Rich-text compactness**: `WordSplitPatch=1` (patch① 0xD053B single-byte → rich-text word = 1 byte →
  no overflow). `OnGlyphWidth` NUL fix (ch<=0→0) repairs the trailing `\0` spurious +9px.
- **Hyperlink whole-sentence highlight**: hook `sub_4C9FC4` (token redraw) entry → `HoverRepaintStub`,
  triggered only when `a4=1` and token+32 (link flag) ≠ 0; `sub_4C9FC4` is `__thiscall(this=layout obj,
  a2=surface, a3=token, a4=1)` — `a4` MUST be 1 to brighten and `surface` MUST be the same object; the
  layout `this` comes from ECX's `[esp+4]` (NOT `[esp+24]` ESI garbage — the 2884-crash root cause).
- **BlitGlyph must clip to upper bounds** (fbW/fbH); out-of-bounds write crashes.
- **UTF-8 source**: all fed text must be UTF-8 (GBK/1252 garbles); `SelfTest` can dump
  `logs/TextRenderer_selftest.bmp` without launching the game to verify.

---

## 5. Campaigns & Movies

### 5.1 Cultures2 Campaign (verified: `Features/Cultures2CampaignFeature.cpp`)

- **Reuses screen 5** (campaign-context swap); `Game.exe` is patched at only **7 immediates** + 1 jump-table slot:
  `R_SwitchScreen=0xD1E45`, `R_CmdJmpTable=0xD62BD` (slot[6]=`kAsgardSlot`), `R_Cons1Tail=0x0223B`,
  `R_CmdMgrSlot=0x169990`, `R_ScreenSlot=0x16967C`, `R_CmdCheck=0x0E0E4B`, `R_CmdClear=0x0E0EDB`,
  `R_CmdName1=0x108740`.
- Original tables: `R_NodesNordland=0x1089F8`, `R_RoutesNordland=0x0F7EE8`; in-DLL data tables
  `kNodesC2[]` (node 10..110), `kRoutesC2[]` (frame 26..34).
- **White-screen fix**: `AddUnlock(progress,1,10,1)` (`R_AddUnlock=0x016D62`) unlocks the first node (10).
  `R_ProgressSlot=0x111690`.
- **Sticky mode**: `g_pendingC2` held in `OnSwitchScreen`; `ApplyMode` writes the 7 immediates.

### 5.2 Asgard Campaign Button (verified: `Features/AsgardCampaignFeature.cpp`)

- hook `HOOK=0xD295E` (VA 0x4D295E, `call operator new` for the Nordland button) → `AsgardButtonStub`;
  `RET=0xD2963`; control ID `kAsgardCtlId=0x138E` (5006); references
  `F_New=0xE55BD`, `F_GetSagaTxt=0xE169E`, `F_CreateBtn=0xB7C2E`, `F_Sub439B9C=0x39B9C`,
  `F_AddBtn=0x768DA`, `D_UiRoot=0x154F20`; saga table IDs 25/26.

### 5.3 Campaign Movie (verified: `Features/CampaignMovieFeature.cpp`)

- hook `R_IntroConv=0x03184` (VA 0x403184, 8 bytes `cmp [esi],bl; je 0x403226`) → `IntroC2Stub`.
- `R_StrCpy=0x0E5400`: **strcpy is cdecl, pushes Source then Dest** (writing the stub reversed causes an AV).
- `R_GameStart=0x10F804` (`+0x2c` = intro mode flag), `R_Mgr=0x110BFC`,
  `R_FindMap=0x01086F` (`__thiscall` map-record lookup, `+0x124`=campaign, `+0x128`=node).
- **Fixed bug (2026-08-11)**: VA was wrongly written as `0x041086F` → `p_findmap=0x81086F` jumped outside
  the image (AV); corrected to `0x01086F`.
- campaign 1 + node 10 → strcpy `"intro00"`. Ending is a giant switch in `sub_41FB7A`, `seq_%4.4d`.

---

## 6. Per-Map Balance Table Reload (PerMapLogic)

> Verified: `Features/PerMapLogicFeature.cpp` (601 lines, with detailed comments and version history v2→v3.6).
> **This is the riskiest feature and directly confirms the "logic-table-reload iron rule" from memory.**

### 6.1 Goal

Logic travels **with the map package** (distributed with `.c2m` / map folder, transferred wholesale in
multiplayer). Override files live inside the map package's **`logic\`** subfolder, laid out exactly like
`data\logic` (`jobtypes.ini`, `goodtypes.ini`, `tribetypes\tribetypes.ini`, `atomicanimations\...`, etc.).
Missing file → fall back to global `data\logic\<same>`; no `logic\` at all → fully vanilla.

### 6.2 Two Hooks (all resident in the DLL)

1. **`IniFile_Open` (0x424EF8) entry trampoline**: during a redirect session (`g_redirect=1`) rewrites the
   12 `data\logic\X.ini` paths to `<map source dir>\logic\X.ini` (`PathRewrite`, `__cdecl`).
   Existence checked with plain Win32 `GetFileAttributesA` (**engine `sub_40667B` probing abandoned** — it
   opens/closes CRT fds and perturbs the file layer → goodtypes etc. fail to load, icons vanish).
2. **`MapLoader_LoadCurrentMap` tail `0x40AA13` (`call sub_407A21`) hook**: patches `E8` → `PrepCallStub`,
   which runs `ReloadLogicForMap` after the map ini/world/`[StaticObjects]` are fully processed and before
   game-prep, then `jmp`s to the original `sub_407A21`.
   - **v3 final trigger**: the v2.2–2.5 entry triggers `0x40A81A`/`0x40A6F4` made the following
     `$maproot$\map.ini` open fail (zero handle, missing section table) → `[StaticObjects]` skipped → empty map.
   - v3.5 additionally reloads **goodtypes only** at the `MapLoader` entry `0x40A6F4` (before `[StaticObjects]`
     parsing); the tail hook then skips goodtypes; if the entry hook fails, the tail hook falls back to shadow-safe.

### 6.3 The 12 Balance Loaders (order = dependency order; must be preserved on re-run)

| # | File | loader VA | manager global VA |
|---|------|-----------|-------------------|
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

### 6.4 ★★★ Root-Cause Iron Rule (v3.6, cpp-confirmed cause of "no icons / can't place / empty harvest")

- A goodtypes record's **+36** is a **runtime-filled derived handle**, not from the ini.
  During load the loader writes +36 as `-1` (unresolved); a one-time start-up guard (in `sub_401981`):
  `if (!this[40]) { sub_406C3E(); sub_47D3E6(); sub_407665(); this[40]=1; }`, where
  `sub_407665 → sub_415E8A` walks 56 records: `rec[+36] = sub_47FEF0(rec[+32])` (finds the index in the
  landscape graphics array `dword_568C24`, stride 140, count `dword_568C20` by landscapetype).
  **+36 = "good → landscape graphics index" cache, resolved exactly once.**
- ⇒ Any goodtypes reload sets +36 back to -1 → all goods lose their graphics handle →
  **no icons / can't place in world / can't harvest** (user-confirmed symptom). This also explains
  "override identical to global by md5 still breaks" — +36 is simply not in the ini.
- **Fix**: after reload, call `sub_415E8A()` to re-resolve +36 (the engine's own entry, 100% semantic match).
- **Addendum**: `dword_510C60` = land→goodid index, built by `sub_412B62` from the global table; the
  harvest/spawn system queries it for "item id on landscape X". Must re-run `sub_412B62` after reload
  (arg `idxMgr=*(0x510F20)`).
- **Shadow-table mechanism (v3.3)**: during goodtypes reload, the 6 `dword_511420` immediate references
  inside the loader are redirected to a DLL shadow global `g_shadowGood` so render threads always see the
  complete original table; after parsing, `memcpy` into the original table then restore the immmediates.
- **Copy-back (pointer identity preserved)**: after reload, copy new content back to the original table address
  and restore the original pointer (10 fixed-size tables, see `kTables[]`; skip the dynamic-size
  atomicanim/tribe/weapon tables) → subsystems caching the table pointer don't break.
- **Self-check**: whole-table `memcmp` (0x35A0) before/after reload; ideal result = only actually-edited
  fields differ; if other offsets turn 0/-1, another runtime-filled field was cleared (keep fixing).
- **Per-table switch**: `[PerMapLogic] Reload<Name> = 1/0`, for binary-search debugging.

### 6.5 Multiplayer Consistency

The map package (including `logic\`) is transferred wholesale to the peer → both sides on the same DLL
version + same map = same balance, consistent by construction.

---

## 7. Community Static-Patch System (CulturesPatches / code cave)

> Verified: `Features/CulturesPatchesFeature.cpp`, `cave_trampolines.h`, `caves_blob.h`.
> See also `SAGA_GAME_HACK/.workbuddy/memory/MEMORY.md` "CulturesPatches milestone".

- **`[CulturesPatches]` master `Enabled`**; authoritative data source = `cultures-saga-patches/` (Python static-patch tool).
- **`ApplyPatchDir`**: parses `plugins/config/patches/*.ini`, format
  `<addr> <hex...> [ | <verify hex> ]`, inline `;`/`#` comments; original bytes can be verified before writing.
- **`ApplyCodeCaves`**: `VirtualAlloc(NULL)` for an independent executable page (the image page at 0xF21CB is
  reserved-not-committed at load, so `MEM_COMMIT` in place returns `ERROR_ACCESS_DENIED(5)`); after copying
  the blob, perform two kinds of relocation — ① absolute self-reference +delta, ② relative outer-jump
  `rel32 −= delta`; otherwise it crashes on a zero page / err=5.
- **blob**: `kCaveBlob[]` 668 bytes, `kCaveOffset=0xF21CB` (`caves_blob.h`).
- **8 trampolines** (`cave_trampolines.h`, `CaveTrampoline{site,caveOff,verifyLen,verify[8]}`):
  `0xA2F42`(Shortcuts), `0xBB894`(Shortcuts), `0xC0C04`(AssistantCtrlClick), `0xC0E4B`(AssistantCtrlClick),
  `0xD4731`(MultiplayerVersionCheck), `0xDB625`(MultiplayerVersionCheck), `0xDCD97`(MultiplayerVersionCheck),
  `0xDF939`(MultiplayerStability).

---

## 8. Remaining Features

### 8.1 TitleOverride (verified: `Features/TitleOverrideFeature.cpp`)
Double **IAT hook** (patches the import-table slot, not the export entry — safer & reversible):
`R_IatCreateWindowExA=0xF31D4`, `R_IatSendMessageA=0xF31B8`; `WM_SETTEXT=0xC`; UTF-16 title
(`MultiByteToWideChar(CP_UTF8)`); a polling fallback thread re-applies every 0.5s.

### 8.2 UnlockAllCampaigns (verified: `Features/UnlockAllCampaignsFeature.cpp`)
hook `R_IsUnlocked=0x16E20` (`__thiscall`, linear scan of an 8-byte `{campaignId,nodeId}` table, `ret 8`);
stub `mov eax,1; ret 8`; default `Enabled=0` (does not touch save progress); all 6 call sites are in the
0x4Dxxxx campaign screen.

### 8.3 WarningLog (verified: `Features/WarningLogFeature.cpp`)
- Background: `Warning!` (IDA: `sub_47A14D`) is a developer assert dialog (`MessageBoxA(..,0x31=exclaim+OKCANCEL)`);
  `result = MessageBoxA - 2`; Cancel(=2)→0→`__debugbreak`(int 3)→process terminates when no debugger attached.
- `ForbidExit`: 1-byte NOP of the `int3` at `0x47A1C3` (CC→90) (safety net).
- `LogEnabled`: hook the `call ds:MessageBoxA` at `0x47A1B8` (6 bytes); the stub reads the already-formatted
  `lpText([ebp+8])` + caller return address `([ebp+4])`, logs them, fakes "OK pressed" (`eax=1`), and jumps
  back to `0x47A1BE` (skips the dialog and doesn't trigger int3).
- All `Warning!` callers (7 sites + vtable) go through this single `MessageBoxA` call → one hook covers all.

### 8.4 VersionStamp (verified: `Features/VersionStampFeature.cpp`)
Does not alter game code structure; only rewrites the three `push imm32` string addresses to the DLL's own
literal strings (version from the `CGE_VERSION_STR` compile macro, build date `__DATE__`/`__TIME__`):
`R_MenuPush=0xD189C` (menu template 0x5088B4), `R_NetBuildPush=0xDB322` (netlog Build 0x509860),
`R_NetHeadPush=0xDB2E7` (netlog header 0x509870).
Note: a version template near `0x4FF2EC` was confirmed **unreferenced** (dead template) by file-level pointer
scan and needs no handling.

### 8.5 UserCampaigns / MapLoaderExtra (verified: `Features/UserCampaignsFeature.cpp`, `MapLoaderExtraFeature.cpp`)
- Folder-map registration: `campaign00=7`, `campaign01=8`; `AllowCurrentUserMapFolder` also scans
  `currentusermap\map.ini`; `kCfgSize=1572*4`.
- Reuse `IniFile_Open + LoadCampaignMap(a4=0 Source)` to register folder maps; a polling thread waits for
  `g_pCampaignStaticDataMgr` to become non-null (up to 30s).
- **2026-08-13 refactor**: no longer hooks `l_IO_Load`; uses the native-function paradigm instead
  (byte-for-byte consistent with `l_IO_Load`'s built-in loop).

---

## 9. Key Address Quick Reference

> Image base is always `0x400000`; the VAs below are the `constexpr uintptr_t` values in the source (cpp-verified).

| Purpose | VA | Source file |
|---------|----|-------------|
| Image base | 0x400000 | GameApi.cpp |
| `g_pCampaignStaticDataMgr` | 0x510BFC | GameApi.h |
| `LoadCampaignMap` (thiscall) | 0x410E6D | GameApi.cpp |
| `IniFile_Open` (thiscall) | 0x424EF8 | GameApi.cpp |
| `IniFile_Close` | 0x425072 | GameApi.cpp |
| `StringTable_GetMainMenuText` (tbl 0) | 0x4E15F6 | GameApi.h |
| `StringTable_GetOdinText` (tbl 11) | 0x4E1682 | GameApi.h |
| `StringTable_GetSagaText` (tbl 13) | 0x4E169E | GameApi.h |
| `sub_439610` atomic blit convergence | 0x439610 | TextRendererFeature.cpp |
| `sub_439633` glyph width | 0x439633 | TextRendererFeature.cpp |
| `sub_4C9FC4` token redraw (hyperlink hover) | 0x4C9FC4 | TextRendererFeature.cpp |
| `MapLoader_LoadCurrentMap` entry | 0x40A6F4 | PerMapLogicFeature.cpp |
| `MapLoader` tail `call sub_407A21` | 0x40AA13 | PerMapLogicFeature.cpp |
| `sub_407A21` game prep | 0x407A21 | PerMapLogicFeature.cpp |
| `sub_415E8A` re-resolve good +36 handle | 0x415E8A | PerMapLogicFeature.cpp |
| `sub_412B62` rebuild land→goodid index | 0x412B62 | PerMapLogicFeature.cpp |
| `dword_568C20` landscape gfx table count | 0x568C20 | PerMapLogicFeature.cpp |
| `dword_510C60` land→goodid index | 0x510C60 | PerMapLogicFeature.cpp |
| `goodtypes` table ptr `dword_511420` | 0x511420 | PerMapLogicFeature.cpp |
| `sub_47A14D` Warning! assert box | 0x47A14D | WarningLogFeature.cpp |
| `0x47A1B8` call ds:MessageBoxA | 0x47A1B8 | WarningLogFeature.cpp |
| `0x47A1C3` int3 (ForbidExit NOP) | 0x47A1C3 | WarningLogFeature.cpp |
| `0x403184` intro movie convergence | 0x403184 | CampaignMovieFeature.cpp |
| `strcpy` (cdecl, Source→Dest) | 0x4E5400 | CampaignMovieFeature.cpp |
| `R_FindMap` (__thiscall) | 0x01086F | CampaignMovieFeature.cpp |
| `g_pGameStartRequest` (+0x2c mode) | 0x10F804 | CampaignMovieFeature.cpp |
| `0x4D295E` Asgard button hook site | 0x4D295E | AsgardCampaignFeature.cpp |
| `R_CmdJmpTable` (slot[6]) | 0xD62BD | Cultures2CampaignFeature.cpp |
| `R_SwitchScreen` | 0xD1E45 | Cultures2CampaignFeature.cpp |
| `R_AddUnlock` | 0x016D62 | Cultures2CampaignFeature.cpp |
| `R_ProgressSlot` | 0x111690 | Cultures2CampaignFeature.cpp |
| `R_IsUnlocked` (__thiscall) | 0x16E20 | UnlockAllCampaignsFeature.cpp |
| IAT `CreateWindowExA` | 0xF31D4 | TitleOverrideFeature.cpp |
| IAT `SendMessageA` | 0xF31B8 | TitleOverrideFeature.cpp |
| `0xD189C` menu version push | 0xD189C | VersionStampFeature.cpp |
| `0xDB322` netlog build push | 0xDB322 | VersionStampFeature.cpp |
| `0xDB2E7` netlog header push | 0xDB2E7 | VersionStampFeature.cpp |
| code-cave blob offset | 0xF21CB | caves_blob.h |
| 8 trampoline sites (see §7) | 0xA2F42..0xDF939 | cave_trampolines.h |

---

## 10. Iron Rules & Lessons Learned

1. **Logic-table reload must re-run the derived resolver**: `sub_415E8A` (good +36 graphics handle) +
   `sub_412B62` (`dword_510C60` index); memcpy-only without re-running → no icons / can't place / empty
   harvest. Shadow table → memcpy → resolver → whole-table diff self-check.
2. **A naked stub that calls a C function then `jmp`s to a `__thiscall` original must restore `ecx`(this)**
   (stash in ebx), else AV.
3. **BlitGlyph must clip to upper bounds** (fbW/fbH); out-of-bounds write crashes.
4. **Guard external object pointers with VirtualQuery before hooking** (wrong guess = no render + log, not a memory stomp).
5. **strcpy(0x4E5400) pushes Source then Dest**; `R_FindMap` was once wrongly written as `0x041086F` (jumped
   outside image → AV).
6. **Code-cave relocation must handle BOTH absolute self-references AND relative outer jumps**; doing only one
   crashes on a zero page / err=5.
7. **Loader Lock**: never `LoadLibrary` a dependent DLL synchronously inside `DllMain`; spawn a thread.
8. **Deploy discipline**: `build_deploy.sh` can silently fail to compile (grep swallows errors) → after every
   deploy, byte-level verify the DLL (search for key strings).
9. **c2m multiplayer map transfer packs the whole tree recursively with no allow-list** (including `logic\`,
   except dotfiles) → balance tables sync with the map by construction.
10. **Rich-text word = single byte** (WordSplitPatch=1) to avoid overflow; `OnGlyphWidth` NUL→0 fixes the
    trailing `\0` spurious +9px.
