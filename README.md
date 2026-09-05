<h1 align="center">Xenia Seamless Toypad Build</h1>

<p align="center">A <a href="https://github.com/xenia-canary/xenia-canary">xenia-canary</a> fork with a built-in <b>emulated LEGO Dimensions ToyPad</b> — play the full game with all DLC, Title Update 23 and a 60 FPS unlock, no physical portal needed.</p>

**Download: [latest release](https://github.com/NeverCookFirst/Xenia-Seamless-Toypad-Build/releases/latest)** — the zip ships a preconfigured, portable build. The release notes contain the full step-by-step install guide (game, DLC and title update).

Thanks [LEGO Dimensions Discord](https://discord.gg/PuXpBMFE4P) for support!

## Video guide

<p align="center">
    <a href="https://youtube.com/watch?v=RPRY2tEzZt4">
        <img src="https://img.youtube.com/vi/RPRY2tEzZt4/maxresdefault.jpg" width="640" alt="Lego Dimensions Xenia Emulator Setup 2026 (Xbox 360 Emulator)" />
    </a>
</p>

<p align="center"><i>Click on image to watch on YouTube.</i></p>

## What this fork adds

- **Emulated ToyPad** (`src/xenia/hid/portal/emulated_toypad.*`) — a complete software implementation of the LEGO Dimensions portal: crypto handshake, tag reads/writes, LED commands. The game detects it as real hardware. Protocol logic is ported from RPCS3's `dimensions_toypad`.
- **Companion app support** — a loopback TCP listener (127.0.0.1:9191, same wire contract as the Cemu / RPCS3 / shadPS4 seamless builds) lets the [LegoToypad](https://github.com/harrysof/LegoToypad) overlay app place, move and remove characters while the game is running.
- **Working DLC + Title Update installation.** Regular xenia fails LEGO Dimensions' post-update data install at 96%. Three fixes in this fork make it complete:
  - `IoDismountVolume` / `IoDismountVolumeByFileHandle` / `IoDismountVolumeByName` kernel exports are implemented (safe success no-ops) — the game dismounts the content volume to finalize the install.
  - Content package headers are written at creation time instead of on close — the installer enumerates its freshly created `appdata` package while it is still open, matching real hardware behavior.
  - `XamContentCreate` on an already-mapped root name now returns `ERROR_ALREADY_EXISTS` (the XDK-documented code) instead of `ERROR_INVALID_PARAMETER` — the installer re-opens its own open package to validate the install and only continues on that exact code.
- **`Display > Internal Resolution` menu** — switch the render resolution between 1x (720p), 2x (1440p) and 3x (2160p) without editing the config. Applies on the next launch.
- **`Display > Toggle 60 FPS Unlock` menu** — runs the game at 60 FPS by doubling the emulated vblank rate (the same approach RPCS3 uses for the PS3 version). Applies **live**, no restart; toggle again to go back to 30. Note: the emulator is demanding — if your PC can't hold a stable 60, use a frame-generation tool (e.g. Lossless Scaling / LSFG) instead.
- **`Mods` menu / `M` hotkey** — enable and disable file mods while the game runs. A mod is a folder in `mods\` with a `mod.txt` describing byte patches applied to files inside the game's content packages as they are read: `patch = PATCH.DAT | 0x807F8 | orig_splash.bin | new_splash.bin`, i.e. `<file inside the package> | <offset in that file> | <original bytes> | <replacement bytes>`. The original bytes are verified against what is on disk first, so a mod built for different data is flagged incompatible and skipped instead of corrupting anything; `orig` and `new` must be the same length. Nothing on disk is ever modified. Three mods ship in the release, all off by default: **Quick Startup** (boot splashes cut to 0.1 s each), **Xenia Text Test** and **Super Sonic Infinite** (needs the Sonic Level Pack DLC). **Mods currently work on Title Update 23 only** — a patch names the exact bytes it expects at an offset, and any other title update moves that data, so on TU24 (or with no update installed) the check fails and the mod is skipped instead of applied. That is the safe outcome rather than a bug, but it does mean mods do nothing until you are on TU23.
- **Roughly twice the frame rate at 1440p** (`readback_resolve_max_kb`) — the game reads resolved render targets back on the CPU for HDR eye adaptation, but only needs a small downsample chain; the emulator was copying back every resolve, including full-screen surfaces nothing ever looks at. Readback is now skipped above a size limit (bundled value `256` KB), which on the test machine took LEGO Dimensions from 23.5 to 43.9 FPS and the 1% low from 21.0 to 37.9, with the picture unchanged. The resolve itself still happens — only the wasted copy to system memory is dropped. Set the value to `0` to restore the old behavior exactly.
- **`Performance` menu / Right Shift** — a live panel with FPS, frame time, CPU load for both the emulator and the whole system, RAM and video memory. Upstream xenia has no frame rate display at all, so this doubles as one. Nothing is sampled until you tick **Measure performance** in the panel (or set `perf_monitor = true`); the checkbox starts and stops the sampler while the game runs and remembers the choice. `perf_log_to_file` additionally appends one row per minute to `perf_session.csv` next to the executable — average, minimum, maximum and 1% low FPS — with the row interval set by `perf_monitor_interval`; it is off by default, and the file belongs to a single session (the previous one is deleted when logging starts).
- **Stability fixes** — the toypad response queue is bounded (fixes a memory leak during long sessions), and the bundled config ships the correct GPU readback settings for the TT Games engine (without them the picture accumulates artifacts within seconds).

## Quick start

1. Grab the [latest release](https://github.com/NeverCookFirst/Xenia-Seamless-Toypad-Build/releases/latest) and follow its install guide (game + DLC + TU23 sources and folder layout are described there).
2. Launch the game — the title bar should read `v0.0.23.3` (update applied), let it install its data when asked.
3. Run the [LegoToypad](https://github.com/harrysof/LegoToypad) companion app and play.

Known quirks: don't switch the render target path to ROV (the game hangs on loading with it); saves made before the title update may black-screen — start a fresh save.

## Building

Same as upstream xenia-canary — see [building](docs/building.md). The toypad code lives in `src/xenia/hid/portal/`. CI builds run via the `Toypad_build.yml` workflow on the `toypad` branch.

---

This is a fork of [Xenia Canary](https://github.com/xenia-canary/xenia-canary), an experimental fork of the [Xenia](https://xenia.jp/) Xbox 360 emulator. Huge thanks to the xenia team — all the heavy lifting is theirs. See the [Xenia Canary wiki](https://github.com/xenia-canary/xenia-canary/wiki) and [FAQ](https://github.com/xenia-canary/xenia-canary/wiki/FAQ) for general emulator questions.
