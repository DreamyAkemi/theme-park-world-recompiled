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

## Next
- Reverse `secdrv.sys` (10 KB) to learn the IOCTL protocol and whether the ICD key depends on the disc.
- Emulate that device in the harness, continue tracing to the point where `TP.ICD` sections are decrypted.
