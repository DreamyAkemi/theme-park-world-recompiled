# Theme Park World, recompiled for Windows 11

Goal: make Bullfrog's *Theme Park World* (1999) run natively on Windows 11 with modern features (widescreen, higher
resolution, upscaled textures), **without shipping any copyrighted game files**. Everything here is our own code that
works on a copy of the game you already own.

## Status
- `patcher/tpw_patch.exe`: repairs the damaged import table in `tp.exe`, installs a small `weachatr.dll` shim,
  and sets the `Language` registry value. Backs up what it changes; `revert` undoes it.
- Still needed today: a DirectDraw wrapper (e.g. cnc-ddraw) next to `tp.exe`. Our own replacement is the next milestone.
- `tools/tp_debug.c`: diagnostic launcher (logs file access, registry, window creation, exits).
- `docs/findings.md`: reverse-engineering notes.

## Using the patcher
Put `tpw_patch.exe` anywhere, then (as Administrator, the registry value is machine-wide):

    tpw_patch status --game-dir "C:\Jeux\ThemeParkWorld"
    tpw_patch apply  --game-dir "C:\Jeux\ThemeParkWorld"
    tpw_patch revert --game-dir "C:\Jeux\ThemeParkWorld"

You also need `Data\Language\English` from your own disc.

## Building
Needs `i686-w64-mingw32-gcc` and `python3`:

    make -C patcher

Game files (`*.exe`, `*.dll`, data) are never committed; see `.gitignore`.
