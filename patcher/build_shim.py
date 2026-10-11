#!/usr/bin/env python3
"""Build payload/weachatr.dll, the shim that lets the Oct 1999 tp.exe load next to the Jan 2000 weachatr.dll.

Needs a 32-bit MinGW cross compiler (i686-w64-mingw32-gcc). The result is our own code only: it forwards the
exports tp.exe uses to a renamed copy of the user's DLL (weachatr_orig.dll) and answers three removed
functions itself.
"""
import os, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
CC = os.environ.get("CC", "i686-w64-mingw32-gcc")
ORIG = "weachatr_orig"

lines = ["LIBRARY weachatr", "EXPORTS"]
for raw in open(os.path.join(here, "shim", "weachatr_imports.txt")):
    raw = raw.strip()
    if not raw or raw.startswith("#"):
        continue
    kind, name = raw.split(None, 1)
    if kind == "fwd":
        lines.append(f"  {name} = {ORIG}.{name}")
    else:
        key = name.split("@")[0].lstrip("?")
        lines.append(f"  {name} = stub_{key}")

out = os.path.join(here, "payload")
os.makedirs(out, exist_ok=True)
defp = os.path.join(out, "weachatr.def")
open(defp, "w").write("\n".join(lines) + "\n")
dll = os.path.join(out, "weachatr.dll")
subprocess.check_call([CC, "-shared", "-nostdlib", "-s", "-o", dll,
                       os.path.join(here, "shim", "weachatr_stubs.c"), defp, "-lkernel32", "-Wl,-e,_DllMain@12"])
os.remove(defp)
print("built", dll)
