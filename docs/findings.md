# Findings so far (retail tp.exe, PE timestamp 1999-10-25)

- Plain x86 PE, no packer or copy protection visible in the exe. Imports DDRAW (DirectDrawCreate only),
  DINPUT, DSOUND, WINMM MIDI, QMIXER (QSound 3D audio), WSOCK32, USP10, and the EA online `wea*.dll` set + `sfman32.dll`.
- Custom MMX software renderer (strings: "MMX Software Renderer, by Martin Griffiths").
- **Launch failure on Win11** ("Entry Point Not Found: ?ParkShout@Chat@@..."): the exe is from Oct 1999 but the
  `weachatr.dll` found alongside is from Jan 2000 and no longer exports `Chat::ParkShout`, `CreatePark`, `ParkInfo`.
  Fix: `tools/make_chat_shim.py` builds a forwarding `weachatr.dll` that stubs those three.
- Data/_Resolution.sam (rename to Resolution.sam to enable) selects 512x384 .. 2048x1536 presets, all 4:3.
  Widescreen needs a patch for the aspect ratio / projection and UI layout.
- Detail presets are plain-text `.sam` scripts (low/med/high.sam).

Game files are never committed (see .gitignore).

## Crash on Win11 (WER dump analysis)
- Access violation at EIP 0x488eec2b (not inside any module) on the very first API call of the CRT startup (`call [GetVersion]` at 0x673776).
- Cause: the on-disk tp.exe has its KERNEL32.dll (124/125) and USER32.dll (34/35) IAT slots pre-filled with stale 0x488exxxx
  values (slot 0 zeroed); all other DLL imports are normal. In the crashed process those slots were still unresolved.
- `tools/fix_iat.py` rewrites every IAT slot to equal its import-name-table entry (like a normal unbound exe). 638 bytes change.
- Open question: why the Win11 loader leaves those two descriptors unsnapped. Untested until the user runs the patched exe.

## Startup path after the IAT fix (tp_debug traces)
- Game reads HKLM\SOFTWARE\WOW6432Node\Bullfrog Productions Ltd\Theme Park World, value `Language` as a 4-byte DWORD (bufsize=4).
  A REG_SZ there makes the game exit immediately (exit from WinMain, garbage exit code).
- With no value it falls back to the system locale (French on the user's PC) and exits when Data\Language\French is missing.
- The user's copy has per-language folders only at the CD root (English/French/...), not under Data\Language;
  need the contents of those folders to know what the installer copies where.

## Language check (code analysis)
- WinMain (0x459290) first calls 0x45dec0 -> 0x45df30, which opens HKLM\...\Bullfrog Productions Ltd\Theme Park World and reads the
  `Language` value as a DWORD. If it is missing, the game shows its error handler (0x4056b0) and returns from WinMain (silent exit).
- The value itself is not used by that function; the language folder (e.g. Data\Language\French) is chosen from the Windows locale.
- Workaround for non-installed copies: create the DWORD, and make Data\Language\<locale name> exist (e.g. copy English to French).

## Engine init (after Language fix, Language=9 -> Data\Language\English)
- Game now shows "Error: Engine failed to initialise" and crashes at 0x65addd (`mov ecx,[eax+0xc]` with a NULL `this` in a small
  pool/array container method) while cleaning up after the failed init.
- Engine diagnostics are written to `enginedebug.txt` in the game folder (fopen via 0x55a390; 0x55a400 writes a second log) -
  that file should say which init step failed.

## DirectDraw failure (enginedebug.txt / fallback.txt)
- Log: "Initializing DirectDraw ... NVIDIA GeForce RTX 4070 ... Fullscreen 640:480:16 ... Action not supported." (x2), then
  "Mommy! Where's fluffy?" (default branch of the HRESULT->text function at 0x560d64), fallback.txt: "FAILED, exiting (DD error code 80004001)".
- Init (0x5594xx-0x559cxx): QI IDirectDraw2 ({B3A6F3E0-...}) then IDirectDraw4 ({9C59509A-...}); GetDeviceIdentifier (vtbl+0x6c);
  SetCooperativeLevel(hwnd, 0x851) (+0x50); SetDisplayMode(640,480,16,0,0) (+0x54); CreateSurface with a 0x7c-byte DDSURFACEDESC2 (+0x18).
- Win11's built-in ddraw returns E_NOTIMPL (0x80004001) for one of these. Plan: shim ddraw with cnc-ddraw (or dgVoodoo2); long term
  a small ddraw replacement of our own is the natural place for widescreen / scaling.

## First park crash (with cnc-ddraw)
- cnc-ddraw has no Direct3D, so the game logs "Initializing Direct3D ... FAILED (80004005)" and uses its MMX software renderer
  (engine log: "Software renderer initialized succesfully", 640x480x16). That is fine.
- Entering the first park crashes at tp.exe+0xc75a0 (VA 0x4c75a0): a trivial getter `mov eax,[ecx+0xc]; ret` called with a bad `this`.
  Need the call chain (tp_debug now prints registers and return addresses on exceptions).

### Crash call chain (tp_debug v6 log)
- 0x52ef7d `call 0x4c75a0` with ecx = result of 0x50fb60: a getter on the big global object (0x7dbd54) that reads a 16-bit handle at
  [obj+0x1da726] and looks it up in an object table at 0x7dc088 (20-byte entries). The lookup returned NULL (object never created),
  so the park load (just after opening save\users\<n>Full Simulation\jungle\easymode.TPWI) dereferences null.
- Next: tp_debug now reports failing file opens, to find the file that did not load.

### World object never initialised (tp_debug v8 log)
- At the crash the world object (0x7dbd54 -> heap) has all handle words at +0x1da720 equal to 0 and the object table at 0x7dc088 is empty,
  i.e. World::Init (0x50bbc0) never ran. It is called from LoadLevel (0x407d70) only if 0x412f80 (level descriptor load: runs 0x421150 name lookups
  and 0x413770 per entry) returns non-zero. tp_debug v9 hooks these and logs arguments and return values.

### Level load failure (tp_debug v9 log)
- LoadLevel("data\levels\jungle") -> LevelDescLoad (0x412f80) -> ParseEntry (0x413770) returns 0 after scanning data\levels\jungle\Rides:
  bouncy, bumper, coaster1, coaster3, gokarts, incagod, lookout, minecart .wad all load, then the scan fails/ends with 0 (World::Init skipped).
- 0x413770 enumerates a directory (vtbl FindFirst/FindNext), skips non-matching names, builds an object per entry (0x41d740),
  validates it (0x41eb80), registers it (0x41e7f0) and loads it (0x413a60); any failure returns 0.
- tp_debug v10 logs each enumerated name and the return values of those four helpers.

### minecart.wad
- v10 log: all of bouncy..lookout load (LoadRide@413a60 returns 1); minecart.wad's LoadRide returns 0 -> ParseEntry 0 -> level load aborted -> world never created.
- The user's Data mixes file dates: minecart.wad 2000-01-21, coaster1.wad 1999-12-09, Rides.sam 1999-10-10, tp.exe built 1999-10-25 -> the data looks newer than the exe.
- LoadRide failure exits: 0x4140c2 via 413b65 (ride count==0x14a), 413c60 (lookup 0x401700 failed), or error reporter 0x4056b0 (code 3) paths.


## Our DirectDraw layer (ddraw/)
- Implements IDirectDraw/2/4/7 and IDirectDrawSurface/2/3/4/7 (shared vtable prefixes: one class serves v1-v3, one serves v4/v7), palettes,
  clippers; system-memory surfaces (any pixel format incl. 565/555/4444/8-bit/Z), flip chains by buffer rotation, colour-key/stretch/mirror Blt,
  BltFast, GetDC via DIB section, mip chains, EnumDisplayModes, GetDeviceIdentifier/GetCaps/GetAvailableVidMem.
- Presentation: primary -> 32-bit DIB -> StretchDIBits into the game's window (aspect-preserving), WM_PAINT/WM_ERASEBKGND and mouse coordinates
  handled by subclassing the window (mouse remap when scaled).
- Verified under Wine with test_ddraw.exe (34 checks) and a screenshot of the presented frame. Not yet verified with the real game.

### ddraw v2 (user test 1: window filled the screen, white flashes on the menu, mouse dead)
- tp.exe imports GetClientRect/GetWindowRect/ScreenToClient/MoveWindow/SetWindowPos/GetSystemMetrics and DirectInputCreateA (no GetCursorPos):
  it sizes its own window and derives mouse positions from window geometry. ddraw now patches the host exe's USER32 IAT entries so the game sees a W x H
  screen at (0,0), ignores the game's SetWindowPos/MoveWindow on the window it owns, and maps ScreenToClient into game coordinates (scale/letterbox aware).
- present() is now serialised (it used a shared static buffer from both the game thread and WM_PAINT); the window is brought to the foreground
  after sizing (DirectInput needs foreground); calls are logged once per distinct flag combination in ddraw_log.txt.
