#!/usr/bin/env python3
"""Repair stale import address table (IAT) slots in a Theme Park World tp.exe.

Some tp.exe files have the KERNEL32.dll and USER32.dll IAT slots pre-filled with
stale addresses (0x488exxxx, leftovers from a process dump) instead of the normal
"same as the import name table" values. On Windows 11 the loader leaves them alone,
so the very first API call jumps into unmapped memory (access violation at 0x488eec2b).

This rewrites every IAT slot that differs from its import-name-table entry so it
matches it, which is what an ordinary unbound executable contains.

Usage: fix_iat.py <in tp.exe> <out tp.exe>
Operates only on your own copy; nothing is distributed.
"""
import hashlib, struct, sys, pefile

src, dst = sys.argv[1:3]
data = bytearray(open(src, "rb").read())
pe = pefile.PE(data=bytes(data))
changed = {}
for d in pe.DIRECTORY_ENTRY_IMPORT:
    s = d.struct
    if not s.OriginalFirstThunk:
        continue
    n = 0
    k = 0
    while True:
        int_off = pe.get_offset_from_rva(s.OriginalFirstThunk + 4 * k)
        iat_off = pe.get_offset_from_rva(s.FirstThunk + 4 * k)
        v_int = struct.unpack_from("<I", data, int_off)[0]
        if v_int == 0:
            break
        if struct.unpack_from("<I", data, iat_off)[0] != v_int:
            struct.pack_into("<I", data, iat_off, v_int)
            n += 1
        k += 1
    # slot 0 of KERNEL32/USER32 was zeroed too; the loop above restores it
    if n:
        changed[d.dll.decode()] = n
open(dst, "wb").write(data)
print("in  sha256", hashlib.sha256(open(src, "rb").read()).hexdigest())
print("out sha256", hashlib.sha256(data).hexdigest())
print("slots repaired:", changed or "none")
