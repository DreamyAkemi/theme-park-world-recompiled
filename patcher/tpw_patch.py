#!/usr/bin/env python3
"""Theme Park World (Bullfrog, 1999) Windows 11 patcher.

Works on YOUR OWN copy of the game. It contains no game code or assets: it repairs a damaged import table in
tp.exe by an algorithm, installs a small compatibility DLL built from this repo, and writes one registry value.

    python tpw_patch.py status --game-dir "C:\\Jeux\\ThemeParkWorld"
    python tpw_patch.py apply  --game-dir ...
    python tpw_patch.py revert --game-dir ...

Standard library only, so it can be bundled into a single .exe.
"""
import argparse, hashlib, os, shutil, struct, sys

# sha256 of tp.exe builds we have analysed -> description
KNOWN_ORIGINAL = {"00f9dfedcb2d4c01c3098f94fccbef6486a7f8aa40494d0921e8cdb8649f1409": "tp.exe 1999-10-25 (stale KERNEL32/USER32 IAT)"}
KNOWN_PATCHED = {"b097f3ad9f0b69542697fe7d5307141ddb47d1d9dbebf0060047ea5f4385e32a": "tp.exe with repaired IAT"}
# Functions the Oct 1999 exe imports from weachatr.dll that the Jan 2000 DLL no longer exports (see shim/)
SHIM_FUNCS = ("ParkShout", "CreatePark", "ParkInfo")

REG_PATH = r"SOFTWARE\Bullfrog Productions Ltd\Theme Park World"  # 32-bit view -> WOW6432Node
LANGUAGE_ENGLISH = 9


class PE:
    """Just enough PE32 parsing for imports and exports."""
    def __init__(self, data):
        self.d = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        nsec, = struct.unpack_from("<H", data, pe + 6)
        optsz, = struct.unpack_from("<H", data, pe + 20)
        opt = pe + 24
        if struct.unpack_from("<H", data, opt)[0] != 0x10B:
            raise ValueError("not a 32-bit PE")
        self.base, = struct.unpack_from("<I", data, opt + 28)
        self.dirs = [struct.unpack_from("<II", data, opt + 96 + 8 * i) for i in range(16)]
        self.sections = []
        sec = opt + optsz
        for i in range(nsec):
            vs, va, rs, rp = struct.unpack_from("<IIII", data, sec + 8 + 40 * i)
            self.sections.append((va, max(vs, rs), rp, rs))

    def off(self, rva):
        for va, vs, rp, rs in self.sections:
            if va <= rva < va + vs:
                return rp + rva - va
        raise ValueError("rva 0x%x not in any section" % rva)

    def cstr(self, rva):
        o = self.off(rva)
        return self.d[o:self.d.index(b"\0", o)].decode("latin1")

    def imports(self):
        """[(dll, oft_off, ft_off, [(name or None)...])]"""
        out = []
        rva, size = self.dirs[1]
        if not rva:
            return out
        o = self.off(rva)
        while True:
            oft, _, _, name, ft = struct.unpack_from("<IIIII", self.d, o)
            if not name:
                break
            names = []
            if oft:
                k = 0
                while True:
                    v, = struct.unpack_from("<I", self.d, self.off(oft + 4 * k))
                    if not v:
                        break
                    names.append(None if v & 0x80000000 else self.cstr((v & 0x7FFFFFFF) + 2))
                    k += 1
            out.append((self.cstr(name), oft, ft, names))
            o += 20
        return out

    def exports(self):
        rva, size = self.dirs[0]
        if not rva:
            return set()
        o = self.off(rva)
        nnames, = struct.unpack_from("<I", self.d, o + 24)
        names_rva, = struct.unpack_from("<I", self.d, o + 32)
        return {self.cstr(struct.unpack_from("<I", self.d, self.off(names_rva) + 4 * i)[0]) for i in range(nnames)}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def iat_stale_slots(data):
    """Slots where FirstThunk differs from OriginalFirstThunk -> {dll: count}."""
    pe = PE(data)
    res = {}
    for dll, oft, ft, names in pe.imports():
        if not oft:
            continue
        n = 0
        for k in range(len(names)):
            a = struct.unpack_from("<I", data, pe.off(oft + 4 * k))[0]
            b = struct.unpack_from("<I", data, pe.off(ft + 4 * k))[0]
            n += a != b
        if n:
            res[dll] = n
    return res


def repair_iat(data):
    data = bytearray(data)
    pe = PE(bytes(data))
    for dll, oft, ft, names in pe.imports():
        if not oft:
            continue
        for k in range(len(names)):
            a = struct.unpack_from("<I", data, pe.off(oft + 4 * k))[0]
            struct.pack_into("<I", data, pe.off(ft + 4 * k), a)
    return bytes(data)


def find(game, name):
    for fn in os.listdir(game):
        if fn.lower() == name.lower():
            return os.path.join(game, fn)
    return None


def chat_shim_needed(game):
    """-> (needed, reason)"""
    exe, dll = find(game, "tp.exe"), find(game, "weachatr.dll")
    if not dll:
        return False, "weachatr.dll not found"
    pe = PE(open(exe, "rb").read())
    need = {n for d, _, _, names in pe.imports() if d.lower() == "weachatr.dll" for n in names if n}
    have = PE(open(dll, "rb").read()).exports()
    missing = sorted(n for n in need if n not in have)
    if not missing:
        return False, "weachatr.dll already exports everything tp.exe needs"
    unknown = [n for n in missing if not any(n.startswith("?" + f + "@") for f in SHIM_FUNCS)]
    if unknown:
        return None, "weachatr.dll lacks exports the shim cannot cover: " + ", ".join(unknown)
    return True, "weachatr.dll lacks %d export(s) the exe imports" % len(missing)


def registry_language(write):
    if sys.platform != "win32":
        return "skipped (not Windows)"
    import winreg
    access = winreg.KEY_READ | winreg.KEY_WOW64_32KEY
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, REG_PATH, 0, access) as k:
            v, t = winreg.QueryValueEx(k, "Language")
            if t == winreg.REG_DWORD:
                return "present (DWORD %d)" % v
            if write:
                pass  # wrong type: overwrite below
    except OSError:
        pass
    if not write:
        return "missing or wrong type"
    try:
        with winreg.CreateKeyEx(winreg.HKEY_LOCAL_MACHINE, REG_PATH, 0, winreg.KEY_WRITE | winreg.KEY_WOW64_32KEY) as k:
            winreg.SetValueEx(k, "Language", 0, winreg.REG_DWORD, LANGUAGE_ENGLISH)
        return "written (DWORD %d = English)" % LANGUAGE_ENGLISH
    except PermissionError:
        return "FAILED: run this tool as Administrator (it writes HKLM)"


def cmd_status(game, args):
    exe = find(game, "tp.exe")
    if not exe:
        print("tp.exe not found in", game); return 1
    h = sha256(exe)
    data = open(exe, "rb").read()
    print("tp.exe sha256", h)
    print("  known:", KNOWN_ORIGINAL.get(h) or KNOWN_PATCHED.get(h) or "UNKNOWN build")
    stale = iat_stale_slots(data)
    print("  stale import slots:", stale or "none")
    if find(game, "weachatr_orig.dll"):
        print("weachatr shim: installed")
    else:
        need, why = chat_shim_needed(game)
        print("weachatr shim:", {True: "needed", False: "not needed", None: "PROBLEM"}[need], "-", why)
    lang = find(os.path.join(game, "Data"), "Language") if find(game, "Data") else None
    print("Data\\Language\\English:", bool(lang and find(lang, "English")))
    print("registry Language:", registry_language(False))
    print("ddraw.dll in game folder:", bool(find(game, "ddraw.dll")), "(a DirectDraw wrapper is still required for now)")
    return 0


def cmd_apply(game, args):
    exe = find(game, "tp.exe")
    if not exe:
        print("tp.exe not found in", game); return 1
    h = sha256(exe)
    if h not in KNOWN_ORIGINAL and h not in KNOWN_PATCHED and not args.force:
        print("Unknown tp.exe build (%s). Re-run with --force to patch anyway." % h); return 2
    data = open(exe, "rb").read()
    stale = iat_stale_slots(data)
    if stale:
        backup = exe + ".orig"
        if not os.path.exists(backup):
            shutil.copy2(exe, backup)
            print("backup:", backup)
        new = repair_iat(data)
        assert not iat_stale_slots(new)
        open(exe, "wb").write(new)
        print("repaired import slots:", stale)
    else:
        print("tp.exe import table already fine")

    need, why = chat_shim_needed(game) if not find(game, "weachatr_orig.dll") else (False, "shim already installed")
    if need is None:
        print("ERROR:", why); return 3
    if need:
        payload = os.path.join(os.path.dirname(os.path.abspath(__file__)), "payload", "weachatr.dll")
        if not os.path.exists(payload):
            print("ERROR: payload/weachatr.dll missing; build it with build_shim.py"); return 4
        dll = find(game, "weachatr.dll")
        os.replace(dll, os.path.join(game, "weachatr_orig.dll"))
        shutil.copy2(payload, os.path.join(game, "weachatr.dll"))
        print("installed weachatr shim (original kept as weachatr_orig.dll)")
    else:
        print("weachatr:", why)

    print("registry Language:", registry_language(True))
    if not find(game, "ddraw.dll"):
        print("NOTE: no DirectDraw wrapper in the game folder yet. Windows 11 cannot run the game's display setup without one.")
    return 0


def cmd_revert(game, args):
    exe = find(game, "tp.exe")
    backup = (exe or os.path.join(game, "tp.exe")) + ".orig"
    if os.path.exists(backup):
        os.replace(backup, exe); print("restored tp.exe")
    orig = find(game, "weachatr_orig.dll")
    if orig:
        cur = find(game, "weachatr.dll")
        if cur:
            os.remove(cur)
        os.replace(orig, os.path.join(game, "weachatr.dll")); print("restored weachatr.dll")
    print("registry value left in place")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["status", "apply", "revert"])
    ap.add_argument("--game-dir", required=True)
    ap.add_argument("--force", action="store_true", help="patch a tp.exe build we have not analysed")
    args = ap.parse_args(argv)
    return {"status": cmd_status, "apply": cmd_apply, "revert": cmd_revert}[args.command](args.game_dir, args)


if __name__ == "__main__":
    sys.exit(main())
