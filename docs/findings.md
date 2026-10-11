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
