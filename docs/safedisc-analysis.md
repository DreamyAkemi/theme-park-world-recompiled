# Theme Park World: SafeDisc analysis notes

Findings from the user-supplied retail files (no game assets are stored in this repo).

## Protection
- SafeDisc **1.41.000** (string in `clcd32.dll`; `drvmgt.dll`, `clcd16.dll`, `clokspl.exe`, `secdrv.sys`).
- `tp.exe` (254 KB) is the SafeDisc loader; `TP.ICD` (3.7 MB) is the real game, with `.text` and `CSEG`
  (and some data sections) encrypted (entropy ~7.99).
- Loader has a 6191-byte overlay at file offset 0x3C800: starts with the reversed string `atad motsuc`
  ("custom data"), then a table keyed by language ID (0x40c, 0x406, 0x407, 0x41d, 0x809).
  It is per-language data, not obviously key material.

## Game executable (TP.ICD) imports (visible without decrypting)
- DDRAW (`DirectDrawCreate`, `DirectDrawEnumerateA`) only; Direct3D is reached via COM. No Glide import.
- DINPUT, DSOUND, QMIXER (QSound), WSOCK32, and 8 `wea*.dll` EA online modules (servers are gone).

## Loader behaviour observed under emulation (tools/sd_emu.py)
1. Named shared memory `ladybird` (0x30 bytes); checks `SAFEDISC_ERROR_<pid>` mapping.
2. Probes existence of CLCD32.DLL, CLCD16.DLL, SECDRV.SYS in its own directory.
3. Reads the overlay from its own exe.
4. Anti-debug: `NtQueryInformationProcess(ProcessDebugPort)`, `\\.\SICE`, `\\.\NTICE`,
   `IsDebuggerPresent`, parses the in-memory kernel32 export table, resolves
   `ReadProcessMemory/WriteProcessMemory/VirtualProtect/CreateProcessA/W`.
5. Repeatedly tries to open `\\.\Secdrv`, the kernel driver. **Current blocker.**

## Update: licensed copy confirmed identical
The user's own retail copy (EA Classics, FR/UK) has byte-identical SHA-256 for tp.exe, TP.ICD, clcd32.dll,
clcd16.dll, drvmgt.dll, clokspl.exe, secdrv.sys, so the analysis above applies to it.

## secdrv.sys
- Imports contain no disc/SCSI/IoCallDriver functions, so it does NOT read the disc. It is a computation helper.
- DriverEntry reads `CurrentVersion`/`CSDVersion` (HKLM\Software\Microsoft\Windows NT\CurrentVersion)
  and **only loads on NT4/Windows 2000** (5.0/4.0); it returns STATUS_UNSUCCESSFUL on XP and later.
  This is why the game can't start on modern Windows even before graphics come into it.
- `tools/secdrv_host.py` runs the real driver in Unicorn (presenting as Win2000 SP4, build 2195).
- Loader sends IOCTL `0xEF002407` (1300-byte in / 1552-byte out) 256 times; the reply differs only in a
  tick-count-derived field (timing sampling).

## Loader behaviour after the driver
- Uses `int 1` traps + MSVC SEH handler (0x416fd0) as part of its control flow; the harness implements
  Windows-style exception dispatch.
- Its own `.text`-like section `.txt` (0x401000-0x40ba00, entropy 8.0) is encrypted and is entered at 0x40986a
  before being decrypted in the current emulation: **current blocker** (probably an incomplete SEH/unwind emulation or
  a missing environment value).

## Next
- Reverse `secdrv.sys` (10 KB) to learn the IOCTL protocol and whether the ICD key depends on the disc.
- Emulate that device in the harness, continue tracing to the point where `TP.ICD` sections are decrypted.
