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
