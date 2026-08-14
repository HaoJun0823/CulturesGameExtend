# CulturesGameExtend

> **License: GNU General Public License v3.0-or-later (GPL-3.0-or-later)**
> Copyright (C) 2026 CulturesGameExtend & CulturesGameLocalization 贡献者
>
> This project is free software released under the GPL-3.0-or-later. You may
> redistribute and/or modify it under the terms of the GNU General Public License as
> published by the Free Software Foundation. See [`LICENSE.txt`](./LICENSE.txt).
>
> **Upstream / third-party attribution (license retained):**
> - Map format tooling (`cif2ini`, `c2mextracter`, `map.dat`/`c2m` converters) is based
>   on **Mikulus6/Cultures-map-editor** (GPL-3.0) and the **Cultures2-dat-format**
>   project, both derived from the 2010 XeNTaX forums cif research. Original copyright
>   notices of those projects are preserved in their respective source files under
>   `maptools/` and `Tools/`.
> - Any code added by the contributors of *CulturesGameExtend* / *CulturesGameLocalization*
>   is Copyright (C) 2026 the respective contributors and licensed under GPL-3.0-or-later.

A community extension DLL for **Cultures** / **Cultures 2: Gates of Asgard** (the
*Gates of Asgard* / *Die Saga* line by Funatics). It is loaded into the game
process by a `dinput8.dll` proxy and adds a configurable set of features on top
of the original game without modifying `Game.exe` itself.

Highlights:

- **Multilingual UTF-8 text rendering** — a self-contained GDI rasterizer
  (`Core/GdiFont`) that replaces the original render path and draws CJK + Latin
  text consistently.
- **Campaign enhancements** — entry button + campaign-screen loading for
  Cultures 2, intro/ending movie wiring, and an optional "unlock all campaigns"
  switch.
- **Per-map game balance** (`logic\` tables reloaded per map package).
- **A community patch drop-in system** (`plugins/config/patches/*.ini`).
- **Version stamping** on the main menu / network log.

> This is a fan modding / translation aid. It hooks a specific retail build of
> the game; use it with a legally owned copy.

---

## Related Projects / 关联项目

- **CulturesGameLocalization** — the Simplified-Chinese (`l10`) text & map
  localization pack this DLL is designed to render in-game. The two repos are
  developed separately; this extension provides the UTF-8 text engine that loads
  the `l10` content authored in that project.
  <https://github.com/HaoJun0823/CulturesGameLocalization>

## 1. Installation

### Prerequisites

- Windows (x86, the game is 32-bit).
- The target game installed.
- A MSVC toolchain that can build the **`v141_xp`** platform toolset for **Win32**
  (e.g. Visual Studio 2022 Build Tools with the *Windows XP targeting* component,
  or the full IDE). The proxy is built with the same toolchain.

### Build

**Option A — one-shot build & deploy script (recommended, Git Bash):**

```bash
bash build_deploy.sh
```

This script:

1. Compiles `CulturesGameExtend.dll` with `cl.exe` (Release / Win32 equivalent).
2. Copies the DLL into `<game>/plugins/CulturesGameExtend.dll`.
3. Mirrors the project's `Resource/` tree into both the game directory and
   `Release/` (configs, fonts, `Data/...` assets). `Resource/` is a mirror of the
   game root, so the copy is root-to-root.

**Option B — Visual Studio:**

Open `CulturesGameExtend.slnx` and build the two projects:

- `CulturesGameExtend` → produces `CulturesGameExtend.dll`.
- `CulturesProxyDLL` → produces the `dinput8.dll` proxy.

> The DLL build needs `_CRT_SECURE_NO_WARNINGS` (already set in both the project
> and `build_deploy.sh`) because `Core/GdiFont.cpp` uses `fopen`.

### Deploy into the game

1. **Proxy loader.** Place the built **`dinput8.dll`** (the proxy, *not* the
   system one) in the game directory, next to `Game.exe`. It forwards
   `DirectInput8Create` to the real system `dinput8.dll`, so the game still gets
   normal input, and on load it injects the DLLs listed in
   `plugins/dll_proxy.ini`.
2. **Extension DLL + resources.** Ensure these exist under the game directory:

   ```
   <game>/
     dinput8.dll                         ; the proxy
     plugins/
       dll_proxy.ini                     ; one DLL path per line (default: plugins/CulturesGameExtend.dll)
       CulturesGameExtend.dll            ; the extension
       config/                           ; *.ini configuration (see §2)
       fonts/                            ; .ttf fonts + per-language .ini (see §3)
     Data/gui/lang/ger/bobs/ls_menu_logos.bmd   ; campaign-2 screen asset (see §4)
   ```

3. **Game.ini switch.** The game's own `Game.ini` (in the game root, next to
   `Game.exe`) must contain:

   ```ini
   AllowCurrentUserMapFolder = 1
   ```

   This lets the extension load folder-style / extracted campaign maps. (If you
   only use the text-rendering and patch features you can leave it, but it is
   required by the campaign features.)

4. Launch the game normally. The proxy's worker thread injects
   `CulturesGameExtend.dll`; logs are written to `logs/dinput8.log` and the
   feature logs under `logs/`.

> If text does not appear at all, check `logs/dinput8.log` — a missing
> `plugins/CulturesGameExtend.dll` (listed in `dll_proxy.ini`) is reported there.

---

## 2. Configuration

Configuration lives in **`plugins/config/`** and is loaded at startup:

| File | Purpose |
|------|---------|
| `CulturesGameExtend_Global.ini` | Global defaults + log level. |
| `CulturesGameExtend_Game.ini`   | Per-game overrides (merged *on top of* Global). |
| `CulturesGameExtend_Patches.ini`| Top-level toggle for the byte-patch system. |
| `patches/*.ini`                 | Individual community patches (each file = one patch; enabled when the global switch is on). |

Every feature is a `[Section]` with an `Enabled = 0|1` key. Common sections in
`CulturesGameExtend_Game.ini`:

- `[UserCampaigns]` — folder-style custom campaigns (`AllowCurrentUserMapFolder`
  must be `1`; optional `CampaignDirs` mapping).
- `[Cultures2Campaign]` — Cultures 2 entry button + campaign-screen loading
  (`Enabled`, `AddButton`).
- `[UnlockAllCampaigns]` — always-treat levels as unlocked (does not touch saved
  progress).
- `[CampaignMovie]` — Cultures 2 (campaign 1) intro movie wiring.
- `[VersionStamp]` — replace the version/build string on the menu / netlog.
- `[PerMapLogic]` — reload `logic\` balance tables per map; each table has its
  own `Reload<Name>` switch (handy for binary-search debugging when a table
  reload seems to cause missing icons/resources).
- `[TextRenderer]` — see §3 / §5.
- `[CulturesPatches]` — master `Enabled` for `patches/*.ini`.

> Editing `.ini` files takes effect after a **game restart** (config is read at
> load). There is no hot reload.

---

## 3. `plugins/fonts/` — what it is for

This folder holds the **fonts and per-language render overrides** used by the
TextRenderer feature. It does **not** affect the engine's own native font path.

### 3.1 Font files (`*.ttf`)

TextRenderer does not use any font installed in Windows. Instead it loads a TTF
from this folder, selected automatically by the game's language:

```
plugins/fonts/<code>.ttf
```

`<code>` comes from the game's `Game.ini` `set_language` value via the internal
language table:

| `set_language` | code | Font |
|---|---|---|
| 0 | `ger` | Roboto (Latin) |
| 1 | `eng` | Roboto (Latin) |
| 2 | `fra` | Roboto (Latin) |
| 3 | `ita` | Roboto (Latin) |
| 4 | `cze` | Roboto (Latin) |
| 5 | `rus` | Roboto (Latin) |
| 6 | `pol` | Roboto (Latin) |
| 7 | `spa` | Roboto (Latin) |
| 8 | `por` | Roboto (Latin) |
| 9 | `hun` | Roboto (Latin) |
| 10 | `l10` | Source Han Sans SC (CJK — Chinese) |
| … | `l11` … `l19` | extend as needed |

To change the language, edit the game's `Game.ini` `set_language`. The matching
`plugins/fonts/<code>.ttf` is loaded automatically.

You can override the auto-selection with the `[TextRenderer]` keys
`FontName =` (a system font face) or `FontFile =` (an explicit `.ttf` path) in
the config (see §2) or in the per-language `.ini` (see below).

### 3.2 Per-language override (`<code>.ini`)

For each language you can ship a **`plugins/fonts/<code>.ini`** (e.g.
`l10.ini`, `eng.ini`) whose `[TextRenderer]` section **overrides**
`CulturesGameExtend_Game.ini` for that language only (higher priority). This lets
you tune font size, color, spacing, anti-aliasing, or point at a different font
**per language** without touching the global config.

Keys you can set in `[TextRenderer]` (all optional; uncomment to activate):

```ini
[TextRenderer]
; Enabled = 1
; FontName =            ; system font face (overrides language-driven ttf)
; FontFile =            ; explicit .ttf path (highest priority)
; FontSize = 14         ; glyph size in pixels (try 12–14 first)
; TextColor = FFFFFF    ; 0xRRGGBB, used only when UseEngineColor=0
; UseEngineColor = 1    ; 1 = keep engine's per-UI colors/shadow (recommended)
; HookEnabled = 1       ; enable the in-game glyph redraw
; AntiAlias = 1         ; 1 = smooth, 0 = crisp hard edges
; BlendIdempotent = 1   ; avoid cross-frame darkening on uncleared surfaces
; TextXOffset = 0       ; horizontal nudge (px)
; TextYOffset = 0       ; vertical nudge (px)
; HalfCellExtra = 2     ; extra Latin half-cell width
; SelfTest = 1          ; render SelfTestText to logs/TextRenderer_selftest.bmp
; SelfTestText =        ; UTF-8 sample text (great for verifying a language)
```

> All keys are **commented out by default** in the shipped `l10.ini` template, so
> it has no effect until you uncomment and set a value.

---

## 4. `Resource/.../*.bmd` — Campaign 2 screen images

The repository's `Resource/` tree is a **mirror of the game root** and is copied
into the game by `build_deploy.sh`. Among its assets:

```
Resource/Data/gui/lang/ger/bobs/ls_menu_logos.bmd
```

This `.bmd` is the **Cultures 2 (Gates of Asgard) main-menu logo / campaign-screen
animation** that the `[Cultures2Campaign]` feature displays.

> **Important:** `ls_menu_logos.bmd` **must be the 35-frame version**. The
> campaign-screen route frames are hardcoded to indices 26..34 (0-based) of that
> 35-frame asset. If you replace this file, keep it at 35 frames or the
> campaign screen will not line up.

Deployed path (after build): `<game>/Data/gui/lang/ger/bobs/ls_menu_logos.bmd`.

---

## 5. TextRenderer requires the **original text to be UTF-8**

The GDI rasterizer decodes source strings as **UTF-8**
(`GdiFontRasterizer::DecodeUtf8`). For text to render correctly:

- **The text fed to the renderer must be UTF-8 encoded** (CJK, accented Latin,
  symbols — all as UTF-8 bytes).
- If a string table / `*.ini` / briefing text is saved in a legacy codepage
  (e.g. GBK, Windows-1252, Latin-1) instead of UTF-8, the glyphs will come out
  wrong or garbled, because the decoder will misinterpret the bytes.

Practical guidance for translators / modders:

- Save edited string files and any custom `SelfTestText` as **UTF-8 (no BOM)**.
- Use the `SelfTest` / `SelfTestText` keys (§3.2) to render a sample to
  `logs/TextRenderer_selftest.bmp` **without launching the game** — a fast way to
  confirm your UTF-8 text decodes and draws correctly before testing in-engine.
- The engine font itself is game-agnostic; the only hard requirement on your
  content side is the UTF-8 encoding of the source text.

---

## 6. Logs & troubleshooting

- `logs/dinput8.log` — proxy injection trace (did it find `dll_proxy.ini`? did it
  load `CulturesGameExtend.dll`?).
- `logs/` — per-feature logs (e.g. `[TextRenderer]`, `[Cultures2Campaign]`,
  `[PerMapLogic]`). `PerMapLogic` prints, per map entry, which `logic\` table was
  reloaded or skipped — useful when a reload seems to cause missing
  resources/icons.
- No text at all? Check `logs/dinput8.log` for a missing/failed DLL, and confirm
  `plugins/dll_proxy.ini` lists `plugins/CulturesGameExtend.dll`.

---

## License

See `LICENSE.txt`.

---

## Upstream Credits / 上游致谢

This project's map-format tooling (and documentation) is derived from the
**Cultures2-dat-format** project. The exact scope of what was reused depends on
the files and links referenced in that upstream repository; we thank its
contributors for their research and tooling.

### Contributors（上游贡献者）

- [Mikulus](https://github.com/Mikulus6): Managed project and wrote Python code.
- [Basssiiie](https://github.com/Basssiiie): Decompiled parts of the game's engine via Ghidra.
- [Rumu](https://github.com/Rumu121/): Helped with empirical verifications in game.
- [Push42](https://github.com/push42): Helped with walk sectors data interpretation.

### Literature（上游文献）

- [Watto](https://github.com/wattostudios): "*Game Extractor*" (2004)
- [Bacter](mailto:the.bacter@gmail.com): "*Unknown Encryption In Cultures Game*" (2010) — <https://web.archive.org/web/20210724220815/https://forum.xentax.com/viewtopic.php?t=3711>
- [Red Blob Games](https://www.redblobgames.com/): "*Hexagonal Grids*" (2013) — <https://www.redblobgames.com/grids/hexagons/>
- [Siguza](https://github.com/Siguza): "*Cultures 2 file formats*" (2013) — <https://web.archive.org/web/20210724220815/https://forum.xentax.com/viewtopic.php?t=10705>
- [Nithanim](https://github.com/Nithanim): "*Northland or 8th Wonder of the World map.dat file format*" (2019) — <https://gist.github.com/Nithanim/766c31475377b0bd594bab974a1de8d2>
- [Martianboy](https://github.com/martianboy): "*cultures2-engine*" (2020) — <https://github.com/martianboy/cultures2-engine>
- [Mikulus](https://github.com/Mikulus6): "*Cultures map editor*" (2025) — <https://github.com/Mikulus6/Cultures-map-editor>

> *Cultures* is the property of [Funatics Development](https://www.funatics.de/) with all
> rights reserved; it is not covered by the GPL-3.0-or-later license.
