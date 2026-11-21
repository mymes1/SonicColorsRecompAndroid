# SonicColorsRecomp

Native **Linux and Windows** static recompilation of **Sonic Colors (Wii)** — PAL `SNCP8P` revision 00.

You must own the game. No disc image or assets are shipped in this repository or in published builds.

**Android:** An unplayable launcher preview and debug APK workflow are available in [`android/`](android/README.md). The native Android game runtime is not yet implemented.

[![Get it on RecompLauncher](https://recomplauncher.com/badge/get-it-on-recomplauncher.svg)](https://recomplauncher.com)

## How to play

1. Own **Sonic Colors (Wii) Europe / PAL** — disc ID `SNCP8P` (ISO, RVZ, WIA, WBFS, or GCZ).
2. Install a build from [Releases](https://github.com/VoltTMC/SonicColorsRecomp/releases), or use [RecompLauncher](https://recomplauncher.com) (Easy Setup picks your disc and extracts game data).
3. Or run the built-in launcher and choose **Install from disc image…**.

## Play a build (no compile)

Download a package from [Releases](https://github.com/VoltTMC/SonicColorsRecomp/releases) when available:

| Platform | What to run |
|---|---|
| Linux | `chmod +x SonicColorsRecomp-*-linux-x86_64.AppImage` then run it |
| Windows | Run `SonicColorsRecomp-Setup-v*.exe` (Start Menu). Optional portable ZIP also available. |

The launcher installs from **your own** disc image (ISO / RVZ / WIA / WBFS). Do not redistribute game files.

Useful flags: `--help`, `--version`, `--portable`, `--check-updates`, `--download-update`, `--save-diagnostics`, `--list-gamepads`, `--verify-data`, `--prepare --game <disc>`.
If the game goes black after a graphics driver update, try System → Clear shader cache.

## Status (2026-11-20)

| Area | State |
|---|---|
| Recompilation | **Done** — Broadway/Gekko → native x86-64 |
| Runtime | OS / IOS / GX / AX / WPAD / PAD HLE |
| Boot → title → Tropical Resort Act 1 | **Verified** (Linux) |
| Windows | Cross-build + Wine smoke; **native** Windows play still checklist-open |
| Music Library (launcher) | Catalog, preview, WAV export, file-based replacement prep |
| PC polish | Launcher, RecompLauncher Easy Setup, update download + release notes, diagnostics (`--save-diagnostics`), log rotation, crash recovery banner, free-space install check, FPS overlay, save backups — see [`docs/DEVELOPMENT_ROADMAP.md`](docs/DEVELOPMENT_ROADMAP.md) |
| Known deferred | H7 black-world after stomp; AP1 opening spiral at 60 Hz; H8 in-game music listen |
| Full-game matrix | **Not** claimed — many stages / abilities `NT` |

Architecture: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) · Progress: [`docs/PROGRESS.md`](docs/PROGRESS.md) · Compatibility: [`docs/GAME_COMPATIBILITY.md`](docs/GAME_COMPATIBILITY.md) · Frame rate: [`docs/FRAME_RATE.md`](docs/FRAME_RATE.md) · Performance: [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md) · Parity: [`docs/WINDOWS_FEATURE_PARITY.md`](docs/WINDOWS_FEATURE_PARITY.md) · Music: [`docs/MUSIC_MODDING.md`](docs/MUSIC_MODDING.md) · Roadmap: [`docs/DEVELOPMENT_ROADMAP.md`](docs/DEVELOPMENT_ROADMAP.md)

## Foundation

Adapted from [BlackKnightRecomp](https://github.com/yunophilia/BlackKnightRecomp) (GPL-2.0-only): PPC→C++ tools and Wii runtime.  
[DolRecomp](https://github.com/ExpansionPak/DolRecomp) is pinned under `external/` for instruction-coverage cross-checks only.

## Prerequisites

- Clang/LLVM, CMake ≥ 3.21, Ninja, Python 3.10+, `wit` (Wiimms ISO Tools)
- Runtime link deps: see Black Knight’s Linux list in `external/BlackKnightRecomp/docs/linux.md` (Vulkan, Wayland/X11, audio, bison/flex for vkd3d)
- Optional for music tooling: `ffmpeg`, `vgmstream-cli` / cricodecs as documented in `docs/MUSIC_MODDING.md`

**CMake note:** Cursor’s AppImage can break stock `cmake` module discovery. Use:

```bash
./scripts/cmake.sh ...
```

## Quick start

```bash
# 1. Extract your disc (if game/SNCP8P is missing)
wit EXTRACT "/path/to/Sonic2010 [SNCP8P].iso" --DEST game/SNCP8P/ --overwrite

# 2. Configure + build (first run fetches SDL3 / dxvk / vkd3d / nod)
./scripts/cmake.sh --preset linux-clang
./scripts/cmake.sh --build --preset linux-clang

# 3. Launch the launcher (Install / locate data → Play)
./out/SonicColorsRecomp
```

### Launching the launcher

The same binary opens the **launcher** by default (omit `--skip-launcher`).

| Platform | Command |
|---|---|
| Linux (dev build) | `./out/SonicColorsRecomp` |
| Linux (staged AppDir) | `./dist/AppDir/AppRun` |
| Linux (AppImage) | `./SonicColorsRecomp-*.AppImage` |
| Windows (dev build) | `out\SonicColorsRecomp.exe` |

Windows build presets: `windows-clang` (preferred) or `windows-msvc` (output under `out\`).
MSVC builds use `-j 2` and target `SonicColorsRecomp` only by default — full parallel `/Zi` RelWithDebInfo OOMs `cl` (C1002) on the huge `ppc_recomp.*.cpp` TUs. If you still hit C1002, rebuild with `-j 1`.

Skip the launcher once data is configured:

```bash
# Linux
./out/SonicColorsRecomp --data game/SNCP8P --skip-launcher

# Windows
out\SonicColorsRecomp.exe --data game\SNCP8P --skip-launcher
```

User data (config, NAND saves, music cache): Windows **installer** → `%APPDATA%\SonicColorsRecomp`; Linux AppImage → `$XDG_DATA_HOME/SonicColorsRecomp`; portable ZIP / `--portable` / `SCR_PORTABLE=1` → next to the exe. See [`docs/WINDOWS_INSTALLER.md`](docs/WINDOWS_INSTALLER.md).

## Music replacement (file-based)

Do **not** use host MixInto / AX mute overlays. Workflow:

1. Open **Music Library** in the launcher.
2. Assign WAV replacements; prepare patched CPKs via `tools/music_asset`.
3. Runtime registers overrides with `Disc_RegisterHostOverride`.

See [`docs/MUSIC_MODDING.md`](docs/MUSIC_MODDING.md).

## Packaging (test shares / AppImage)

**Linux** (this host):

```bash
./scripts/cmake.sh --build --preset linux-clang
./tools/package_linux.sh -S          # stage dist/AppDir (no game data)
# optional: ./tools/package_linux.sh -A dist/AppDir -c /path/to/shadercache

# Shareable test archive (folder + .tar.zst/.tar.gz under dist/):
./tools/package_test_linux.sh
```

**Windows** (cross-build from Linux, or native):

```bash
cmake --build --preset windows-mingw --target SonicColorsRecomp music_asset
./tools/package_windows_installer.sh -v 0.3.7   # Setup.exe (primary; Wine+Inno or ISCC)
./tools/package_windows_cross.sh -v 0.3.7       # optional portable ZIP
./tools/package_windows_install_check.sh
```

Native Windows: `tools/package.ps1` still stages a folder; prefer the Inno Setup path above for distribution.

Redistributable builds must **not** include `game/` or copyrighted assets.

## Layout

```
tools/          PPC analysis + recomp + music_asset
runtime/        Wii OS / GX / AX / IOS / Linux platform + launcher UI
generated/      Recompiled SNCP8P C++ (gitignored)
game/           Extracted disc data (gitignored)
mods/music/     catalog.json (tracked); your tracks/manifest (local)
external/       Pinned upstream checkouts
docs/           Architecture, progress, compatibility, quality audit
out/            Built SonicColorsRecomp + shared libs
```

## Licence

Project runtime/tools derived from BlackKnightRecomp: **GPL-2.0-only** — see `licenses/`.  
Preserve upstream notices when copying further. Game code/data remain your property and must not be redistributed.
