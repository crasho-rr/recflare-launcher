<p align="center"><img src="res/logo.png" width="360" alt="RecFlare"></p>

# RecFlare Launcher

one click launcher for RecFlare on rec room **20260323.14**. no injector!!

**download:** grab `RecFlare.Launcher.zip` from [releases](../../releases/latest), unzip, run `RecFlare Launcher.exe`, press play.

the game isn't included, it comes from steam (steam's current rec room is 20260323.14). the launcher finds it in your steam
libraries, and if it's missing or a different build it gives you **install / update via steam** buttons and picks the game
up when steam is done. fallback if steam ever moves on: **old build via steam console** copies
`download_depot 471710 471711 8867414017562658247` and opens the steam console. your steam login never leaves steam.

on a normal steam install it offers to make a separate copy first so steam updates can't break it, and there's an
**undo recflare** button that puts the stock exe back.

## how it works

- **the launcher** (`src/launcher.cpp`) checks your install is 20260323.14 (sha256 of GameAssembly / Referee / UnityPlayer, only once),
  drops `2026loader.dll` + `2026patch.dll` (+ `2026patch.ini` if you don't have one) into the game folder, and adds an import of
  `2026loader.dll` to `RecRoom.exe` (stock one kept as `RecRoom.original.exe`). then it starts the game and closes when the patch says it's hooked.
- **the loader** (`loader/loader.cpp`) does what Injector.exe used to do, but from inside the game:
  - sets up the local Referee arena from a DLL load notification, because Referee sets the slot and uses it during its own startup
    while holding the loader lock, so a normal thread can't get in there
  - prewarms the game modules in the file cache, because the patch hashes ~384 MB before hooking and on a slow drive the game hits the
    name server first
  - loads `2026patch.dll` once GameAssembly is in, and holds the main thread for a moment until the patch log says `hooks enabled` (20 s cap)
- **the patch** is [recflare/patch-2026](https://github.com/recflare/patch-2026), embedded as-is (`payload/`)

command line: `--play`, `--vr`, `--folder "<game folder>"`

## building

needs visual studio 2022 with the c++ x64 tools. run `build.cmd`, you get `build/RecFlare Launcher.exe`.
`res/make-assets.ps1` remakes the logo + icon from the source art.

## credits

- [recflare/patch-2026](https://github.com/recflare/patch-2026) for the patch (MIT), which builds on
  [Carpetsoft/Rec-Room-2025-Patch](https://github.com/Carpetsoft/Rec-Room-2025-Patch) and [recflare/patch-2025](https://github.com/recflare/patch-2025)
- [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-clause, inside the patch)

see `THIRD-PARTY-NOTICES.txt`. not affiliated with rec room inc / against gravity.
