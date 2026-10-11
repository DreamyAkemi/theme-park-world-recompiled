#!/usr/bin/env python3
"""Build a weachatr.dll shim for a mismatched exe/DLL pair.

The retail tp.exe (Oct 1999) imports Chat::ParkShout/CreatePark/ParkInfo, which
the Jan 2000 weachatr.dll no longer exports, so Windows refuses to start the game
("Entry Point Not Found"). This forwards every export the user's own DLL has to
a renamed copy of it and adds do-nothing stubs for the missing ones.

Usage: make_chat_shim.py <tp.exe> <orig weachatr.dll> <outdir>
Only export *names* are read from the game files; nothing is copied.
"""
import os, subprocess, sys, pefile

exe_path, dll_path, outdir = sys.argv[1:4]
CC = os.environ.get("CC", "i686-w64-mingw32-gcc")
ORIG = "weachatr_orig"

exe, dll = pefile.PE(exe_path), pefile.PE(dll_path)
have = [e.name.decode() for e in dll.DIRECTORY_ENTRY_EXPORT.symbols if e.name]
need = [i.name.decode() for d in exe.DIRECTORY_ENTRY_IMPORT
        if d.dll.decode().lower() == "weachatr.dll" for i in d.imports if i.name]
missing = [n for n in need if n not in have]
lines = ["LIBRARY weachatr", "EXPORTS"] + [f"  {n} = {ORIG}.{n}" for n in have]
for n in missing:
    key = n.split("@")[0].lstrip("?")
    if key not in ("ParkShout", "CreatePark", "ParkInfo"):
        sys.exit(f"no stub for missing export {n}")
    lines.append(f"  {n} = stub_{key}")
os.makedirs(outdir, exist_ok=True)
defp = os.path.join(outdir, "weachatr.def")
open(defp, "w").write("\n".join(lines) + "\n")
src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "shim", "weachatr_stubs.c")
subprocess.check_call([CC, "-shared", "-nostdlib", "-s", "-o", os.path.join(outdir, "weachatr.dll"),
                       src, defp, "-lkernel32", "-Wl,-e,_DllMain@12"])
print(f"forwarded {len(have)} exports, stubbed {len(missing)}; put your original DLL next to it as {ORIG}.dll")
