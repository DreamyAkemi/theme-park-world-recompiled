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
