# Handover: continue this project in a local session on the owner's Windows PC

You are Claude Code running on the owner's Windows 11 PC (RTX 4070, French-locale Windows, 2048x1152 work area, two monitors).
The project was developed so far in a cloud session that could not run the game, which caused a lot of slow
"swap the DLL, launch, report back" ping-pong. **You can run the game here. Use that: build, launch, screenshot, iterate
yourself, and only show the owner builds you have already seen working.**

## Goal
Make Theme Park World (Bullfrog 1999) a proper modern remaster on Windows 11: true widescreen at high resolution,
good-looking presentation, UI that fits, upscaled textures, and eventually a real GPU renderer. No third-party wrappers
(dgVoodoo / cnc-ddraw) in the final result: everything is our own code operating on the owner's own copy of the game.

## Hard rule: no copyrighted content in the repo
Never commit game files (exe, dll, data, textures, extracted assets, dumps). `.gitignore` already blocks `*.exe`, `*.dll`,
`*.zip`, `*.bin`, `*.dmp`, `game/`, `work/`. The repo contains only our code (patcher, ddraw layer, tools, docs).
Work on extracted assets in a folder outside the repo or under an ignored folder.

## Paths
- Repo: `C:\Projects\ThemeParkWorld\theme-park-world-recompiled` (branch `claude/friendly-sagan-v5qam6`; `git pull` first)
- Game (the owner's patched install, outside the repo): `C:\Projects\ThemeParkWorld\ThemeParkWorld`
  - If the owner's older copy at `C:\Jeux\ThemeParkWorld` still exists, treat it as the reference install and do not break it.
- The game folder is outside the repo folder: ask the owner to allow access to it (`/add-dir C:\Projects\ThemeParkWorld\ThemeParkWorld`).
- Work on a COPY of the game folder when experimenting with exe patches; keep a backup of the original `tp.exe`.

## Where things stand (all verified by the owner on real hardware unless noted)
- The owner's `tp.exe` is the patched build (SHA-256 `D0399ACFC1BCBDCBE5836814B19E386486BFA06F057646AE7821BC25E9927AC9`, PE
  timestamp 2022-11-15). The original 1999 build (SHA-256 `00f9dfed...1409`) is also supported by parts of the code. The game
  data is from the early-2000 patch. Using the matching exe fixed the park-load crashes (the old 1999 exe could not read the
  patched ride WADs `coasta.wad`, `minecart.wad`, ...).
- `ddraw/ddraw.cpp` + `ddraw/gpu.cpp`: our own DirectDraw layer. It owns the window (windowed, scaled, never maximised),
  proxies DirectInput (synthetic absolute mouse computed from the real pointer, buffered data, non-exclusive, Windows key
  left enabled), IAT-hooks window-geometry APIs, patches the exe's resolution table in memory, and presents through Direct3D 11
  (pass 1 scaling, pass 2 FXAA + deband + sharpen; automatic GDI fallback). Options are in `ddraw/ddraw.ini` (documented).
- Widescreen: the patched exe reads `Resolution.sam` as an index (1=512x384, 2=640x480, 3=800x600, 4=1024x768,
  5=1280x1024, 6=1600x1200, 7=2048x1536). `ddraw.dll` rewrites entry 7's immediates (at 0x44a0ed) to `width`/`height` from the ini.
  With `width=1920 height=1080` and `Resolution.sam` = 7 the game runs at true 16:9; HUD sits correctly in a park; mouse works. See `docs/findings.md`.
- `patcher/`: single-exe patcher (IAT repair for the 1999 exe, shim DLL for the online libs, registry `Language` DWORD,
  installs ddraw.dll). It was written for the original 1999 exe; it must be reworked for the patched exe.
- `tools/tp_debug.c`: debug launcher with breakpoints on engine functions (used to trace the park crash).
- `docs/findings.md`: running reverse-engineering notes. Read it first.

## Open problems (in the owner's priority order)
1. **Input after minimise / Alt-Tab.** After leaving the window or minimising and coming back, the mouse stops working in the game
   and the game's own cursor is not visible. Not diagnosed yet. The latest `ddraw.dll` logs every focus/size/DInput
   Acquire/Unacquire event with a `[focus]` prefix when `log=1`. Reproduce it yourself, read `ddraw_log.txt` next to `tp.exe`, and fix.
   Suspects: the game does not re-Acquire, `M.snap` handshake needs the pointer inside the picture while the window is foreground,
   WM_SIZE/ShowWindow handling on restore, swap chain or the 0x0 client size while minimised.
2. **Main menu is stretched at 16:9** (button layout is positioned in screen proportions). The in-park HUD is anchored
   correctly but looks huge: UI art is pixel-based (the exe picks a UI art set by screen width; function near 0x41a5a0
   buckets <=512, <=640, <=800, else largest). Find the menu layout code and add aspect-ratio correction; try forcing a
   smaller UI set for 1080p.
3. **Textures / look.** Decode texture formats (TGA, `.bf4` fonts, WAD archives), build an extract -> AI upscale -> repack
   tool (the owner's RTX 4070 can run the upscaler locally), loading replaced textures without touching originals.
4. **The big project: a real GPU renderer.** The game uses a 16-bit software rasteriser (MMX, "Martin Griffiths"
   renderer). Trace where the engine submits polygons to it and feed them to Direct3D 11 instead (MSAA, filtering, hi-res
   textures, no 16-bit banding). Start with analysis and a design note before coding. The old 1999 exe analysis
   (`text.bin`-style disassembly with objdump, VA base 0x401000) is the working method; Ghidra is fine too.
5. Packaging: one patcher that repairs/installs everything for the patched exe, sets `Resolution.sam`, installs `ddraw.dll` + ini.

## Build (Windows)
The ddraw layer is C++17 and builds with MinGW-w64 32-bit (the game is 32-bit): install MSYS2, then
`pacman -S mingw-w64-i686-gcc make`, open the "MSYS2 MinGW 32-bit" shell, and from `ddraw/` run
`make MINGW_CXX=g++` (default `i686-w64-mingw32-g++` is the cross-compiler name from Linux). Output: `ddraw.dll`.
`ddraw/test_ddraw.cpp` is an API test (52 checks passed under Wine); `make` also builds `test_ddraw.exe`.
Copy `ddraw.dll` + `ddraw.ini` next to `tp.exe` to test. Set `log=1` in the ini for `ddraw_log.txt`.

## Working style the owner wants
- Do significant work autonomously for long stretches; run and verify yourself; avoid asking the owner to swap files repeatedly.
- Be honest about what is verified and what is a guess. Commit small, push to the branch, no PR unless asked.
- The owner is French-locale; they are not a programmer by trade but follow along well and are enthusiastic. Be direct, not wordy.
