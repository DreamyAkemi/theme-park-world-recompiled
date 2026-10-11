#!/usr/bin/env python3
"""Minimal Win32 emulation harness for tracing a SafeDisc 1.x loader (tp.exe).

Loads a 32-bit PE into Unicorn, resolves its imports to hookable thunks, and
logs every API call.  Operates on the user's own game files; never writes
anything back into the game directory.

Usage: python3 -I tools/sd_emu.py <game_dir> [--max-insn N] [--quiet]
"""
import argparse
import os
import struct
import sys

import pefile
from unicorn import (Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED,
                     UC_HOOK_MEM_WRITE, UcError, UC_PROT_ALL)
from unicorn.x86_const import *

PAGE = 0x1000
STACK_TOP = 0x00200000
STACK_SIZE = 0x00100000
HEAP_BASE = 0x10000000
THUNK_BASE = 0x7F000000
TEB_BASE = 0x7FFDE000
PEB_BASE = 0x7FFDF000

# stdcall argument counts (bytes popped = 4*n); None = cdecl (caller cleans)
NARGS = dict(
    FreeLibrary=1, GetLastError=0, CreateFileMappingA=6, MapViewOfFile=5, UnmapViewOfFile=1,
    CloseHandle=1, GetFileAttributesA=1, GetLogicalDriveStringsA=2, GetModuleFileNameA=3,
    GetProfileIntA=3, GetSystemDefaultLCID=0, ReadFile=5, SetFilePointer=4, CreateFileA=7,
    FlushFileBuffers=1, GetVersionExA=1, GetProfileStringA=5, CopyFileA=3, GetExitCodeProcess=2,
    GetProcAddress=2, GetDriveTypeA=1, Sleep=1, FileTimeToLocalFileTime=2, GetFileTime=4,
    FindClose=1, FindNextFileA=2, FindFirstFileA=2, SetFileAttributesA=2, SetThreadContext=2,
    GetThreadContext=2, ResumeThread=1, GetCommandLineA=0, DeviceIoControl=8,
    SetEnvironmentVariableA=2, GetEnvironmentVariableA=3, LoadLibraryExA=3, GetCurrentProcessId=0,
    OpenFileMappingA=3, WriteFile=5, CreateEventA=4, FileTimeToSystemTime=2, LoadLibraryA=1,
    VirtualFree=3, InitializeCriticalSection=1, DeleteCriticalSection=1, HeapDestroy=1,
    HeapCreate=3, GetLocaleInfoA=4, GetEnvironmentStringsW=0, GetEnvironmentStrings=0,
    FreeEnvironmentStringsW=1, FreeEnvironmentStringsA=1, WaitForSingleObject=2, VirtualAlloc=4,
    GetStdHandle=1, SetHandleCount=1, SetEndOfFile=1, HeapReAlloc=4, CompareStringW=6,
    CompareStringA=6, GetLocaleInfoW=4, GetModuleHandleA=1, GetCurrentProcess=0, HeapFree=3,
    HeapAlloc=3, InterlockedDecrement=1, InterlockedIncrement=1, GetTimeZoneInformation=1,
    GetSystemTime=1, GetLocalTime=1, ExitProcess=1, TerminateProcess=2, GetFileType=1,
    GetStartupInfoA=1, GetVersion=0, RtlUnwind=4, UnhandledExceptionFilter=1, TlsSetValue=2,
    TlsAlloc=0, GetOEMCP=0, EnterCriticalSection=1, LeaveCriticalSection=1,
    MultiByteToWideChar=6, WideCharToMultiByte=8, LCMapStringA=6, LCMapStringW=6,
    GetCurrentThreadId=0, GetACP=0, SetStdHandle=2, SetLastError=1, TlsGetValue=1,
    GetStringTypeA=5, GetStringTypeW=4, GetCPInfo=2,
    MessageBoxA=4, CreateWindowExA=12, wvsprintfA=3, WaitForInputIdle=2, GetDesktopWindow=0,
    wsprintfA=None, MsgWaitForMultipleObjects=5, PeekMessageA=5, GetMessageA=4,
    TranslateMessage=1, DispatchMessageA=1, DefWindowProcA=4, RegisterClassA=1, ShowWindow=2,
    GetKeyboardType=1,
    RegQueryValueExA=6, RegOpenKeyExA=5, RegCloseKey=1, RegEnumKeyExA=8, RegOpenKeyA=3,
    NtQueryInformationProcess=5, IsDebuggerPresent=0, ReadProcessMemory=5, WriteProcessMemory=5,
    VirtualProtect=4, CreateProcessA=10, CreateProcessW=10, GetStartupInfoW=1, VirtualQuery=3,
    GetModuleHandleW=1, CheckRemoteDebuggerPresent=2, OutputDebugStringA=1, GetFileVersionInfoA=4, VerQueryValueA=4, GetFileVersionInfoSizeA=2,
)


class Emu:
    def __init__(self, game_dir, quiet=False):
        self.dir = game_dir
        self.quiet = quiet
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.heap_ptr = HEAP_BASE
        self.thunks = {}      # addr -> (dll, name)
        self.next_thunk = THUNK_BASE
        self.calls = 0
        self.tls = {}
        self.last_error = 0
        self.handles = {}
        self.next_handle = 0x100
        self.cmdline = None
        self.env = None
        self.modules = {}
        self.trace_all = False
        self.named_maps = {}
        self.ring = []

    # ---- memory helpers -------------------------------------------------
    def r32(self, a):
        return struct.unpack('<I', self.uc.mem_read(a, 4))[0]

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack('<I', v & 0xFFFFFFFF))

    def cstr(self, a, n=260):
        if not a:
            return ''
        out = bytearray()
        for i in range(n):
            c = self.uc.mem_read(a + i, 1)[0]
            if c == 0:
                break
            out.append(c)
        return out.decode('latin1')

    def alloc(self, size):
        size = (size + 15) & ~15
        p = self.heap_ptr
        self.heap_ptr += size
        self.map_range(p, size)
        return p

    def map_range(self, start, size):
        s = start & ~(PAGE - 1)
        e = (start + size + PAGE - 1) & ~(PAGE - 1)
        a = s
        while a < e:
            try:
                self.uc.mem_map(a, PAGE, UC_PROT_ALL)
            except UcError:
                pass
            a += PAGE

    # ---- PE loading -------------------------------------------------------
    def load_pe(self, path):
        pe = pefile.PE(path)
        base = pe.OPTIONAL_HEADER.ImageBase
        img = pe.get_memory_mapped_image()
        size = (len(img) + PAGE - 1) & ~(PAGE - 1)
        self.uc.mem_map(base, size, UC_PROT_ALL)
        self.uc.mem_write(base, bytes(img))
        self.pe, self.base = pe, base
        for ent in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
            dll = ent.dll.decode().lower()
            for imp in ent.imports:
                name = imp.name.decode() if imp.name else '#%d' % imp.ordinal
                self.w32(imp.address, self.make_thunk(dll, name))
        return pe

    def make_thunk(self, dll, name):
        for a, (d, n) in self.thunks.items():
            if d == dll and n == name:
                return a
        a = self.next_thunk
        self.next_thunk += 16
        self.thunks[a] = (dll, name)
        return a

    # ---- setup ---------------------------------------------------------------
    def setup(self, exe_name='tp.exe'):
        uc = self.uc
        uc.mem_map(THUNK_BASE, 0x100000, UC_PROT_ALL)
        uc.mem_map(STACK_TOP - STACK_SIZE, STACK_SIZE + PAGE, UC_PROT_ALL)
        uc.mem_map(HEAP_BASE, 0x1000, UC_PROT_ALL)
        uc.mem_map(TEB_BASE, 0x2000, UC_PROT_ALL)
        # TEB: SEH chain @0, stack base @4, limit @8, self @0x18, PEB @0x30
        self.w32(TEB_BASE + 0x00, 0xFFFFFFFF)
        self.w32(TEB_BASE + 0x04, STACK_TOP)
        self.w32(TEB_BASE + 0x08, STACK_TOP - STACK_SIZE)
        self.w32(TEB_BASE + 0x18, TEB_BASE)
        self.w32(TEB_BASE + 0x30, PEB_BASE)
        self.w32(PEB_BASE + 0x08, 0)  # ImageBaseAddress patched after load
        # GDT/FS setup
        self.setup_fs()
        self.load_pe(os.path.join(self.dir, exe_name))
        self.w32(PEB_BASE + 0x08, self.base)
        self.cmdline = self.alloc(64)
        uc.mem_write(self.cmdline, b'"C:\\TP\\tp.exe"\0')
        self.exe_path = 'C:\\TP\\' + exe_name
        uc.reg_write(UC_X86_REG_ESP, STACK_TOP - 0x100)
        uc.reg_write(UC_X86_REG_EBP, 0)
        self.setup_modules()
        uc.hook_add(UC_HOOK_CODE, self.on_code, begin=THUNK_BASE, end=THUNK_BASE + 0x100000)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self.on_unmapped)
        uc.hook_add(UC_HOOK_CODE, self.on_ring, begin=0, end=0x7EFFFFFF)

    def setup_fs(self):
        uc = self.uc
        GDT = 0x7FFC0000
        uc.mem_map(GDT, 0x1000)
        def entry(base, limit, access, flags):
            e = limit & 0xFFFF
            e |= (base & 0xFFFFFF) << 16
            e |= access << 40
            e |= ((limit >> 16) & 0xF) << 48
            e |= flags << 52
            e |= ((base >> 24) & 0xFF) << 56
            return struct.pack('<Q', e)
        gdt = b'\0' * 8
        gdt += entry(0, 0xFFFFF, 0x9B, 0xC)   # 0x08 code
        gdt += entry(0, 0xFFFFF, 0x93, 0xC)   # 0x10 data
        gdt += entry(TEB_BASE, 0xFFF, 0xF3, 0x4)  # 0x18 fs (ring3)
        uc.mem_write(GDT, gdt)
        uc.reg_write(UC_X86_REG_GDTR, (0, GDT, len(gdt) - 1, 0))
        uc.reg_write(UC_X86_REG_CS, 0x08)
        uc.reg_write(UC_X86_REG_SS, 0x10)
        uc.reg_write(UC_X86_REG_DS, 0x10)
        uc.reg_write(UC_X86_REG_ES, 0x10)
        uc.reg_write(UC_X86_REG_FS, 0x18)

    # ---- hooks -----------------------------------------------------------------
    def on_unmapped(self, uc, access, addr, size, value, _):
        eip = uc.reg_read(UC_X86_REG_EIP)
        print('[!] unmapped access %d at %#x (size %d) from eip=%#x' % (access, addr, size, eip))
        return False

    def on_ring(self, uc, addr, size, _):
        self.ring.append(addr)
        if len(self.ring) > 40:
            self.ring.pop(0)

    def on_code(self, uc, addr, size, _):
        t = self.thunks.get(addr)
        if t is None:
            return
        dll, name = t
        esp = uc.reg_read(UC_X86_REG_ESP)
        nargs = NARGS.get(name, 0)
        n = nargs if nargs is not None else 6
        args = [self.r32(esp + 4 + 4 * i) for i in range(n)]
        ret = self.r32(esp)
        self.calls += 1
        if self.trace_all:
            print('[api] %s!%s(%s)' % (dll, name, ', '.join(hex(a) for a in args)))
        handler = getattr(self, 'api_' + name, None)
        if handler:
            rv = handler(*args)
        else:
            rv = 0
            print('[stub] %s!%s%s -> 0' % (dll, name, tuple(hex(a) for a in args)))
        if rv is None:
            rv = 0
        uc.reg_write(UC_X86_REG_EAX, rv & 0xFFFFFFFF)
        pop = 0 if nargs is None else 4 * nargs
        uc.reg_write(UC_X86_REG_ESP, esp + 4 + pop)
        uc.reg_write(UC_X86_REG_EIP, ret)

    def log(self, msg):
        if not self.quiet:
            print(msg)

    # ---- API implementations (just enough to start the loader) -----------
    def api_GetVersion(self):
        return 0x0A280105  # Windows XP 5.1 build 2600

    def api_GetCommandLineA(self):
        return self.cmdline

    def api_GetModuleFileNameA(self, mod, buf, size):
        self.uc.mem_write(buf, self.exe_path.encode() + b'\0')
        self.log('GetModuleFileNameA -> %s' % self.exe_path)
        return len(self.exe_path)

    def api_HeapCreate(self, a, b, c):
        return self.alloc(0x1000)

    def api_HeapAlloc(self, h, flags, size):
        return self.alloc(max(size, 16))

    def api_HeapFree(self, *a):
        return 1

    def api_HeapReAlloc(self, h, flags, p, size):
        q = self.alloc(max(size, 16))
        try:
            data = bytes(self.uc.mem_read(p, size))
            self.uc.mem_write(q, data)
        except UcError:
            pass
        return q

    def api_VirtualAlloc(self, addr, size, typ, prot):
        if addr:
            self.map_range(addr, size)
            self.log('VirtualAlloc(%#x,%#x)' % (addr, size))
            return addr
        return self.alloc(size)

    def api_VirtualFree(self, *a):
        return 1

    def api_GetLastError(self):
        return self.last_error

    def api_SetLastError(self, e):
        self.last_error = e

    def api_TlsAlloc(self):
        return len(self.tls)

    def api_TlsSetValue(self, i, v):
        self.tls[i] = v
        return 1

    def api_TlsGetValue(self, i):
        return self.tls.get(i, 0)

    def api_InitializeCriticalSection(self, *a):
        pass

    def api_EnterCriticalSection(self, *a):
        pass

    def api_LeaveCriticalSection(self, *a):
        pass

    def api_GetCurrentProcess(self):
        return 0xFFFFFFFF

    def api_GetCurrentProcessId(self):
        return 0x1234

    def api_GetCurrentThreadId(self):
        return 0x5678

    def api_InterlockedIncrement(self, p):
        v = self.r32(p) + 1
        self.w32(p, v)
        return v

    def api_InterlockedDecrement(self, p):
        v = self.r32(p) - 1
        self.w32(p, v)
        return v

    def api_GetStartupInfoA(self, p):
        self.uc.mem_write(p, b'\0' * 68)
        self.w32(p, 68)

    def api_GetStdHandle(self, n):
        return 0x10 + (n & 0xF)

    def api_GetFileType(self, h):
        return 2

    def api_SetHandleCount(self, n):
        return n

    def api_GetACP(self):
        return 1252

    def api_GetOEMCP(self):
        return 437

    def api_GetCPInfo(self, cp, p):
        self.uc.mem_write(p, struct.pack('<I', 1) + b'?\0' + b'\0' * 14)
        return 1

    def api_GetEnvironmentStringsW(self):
        p = self.alloc(64)
        self.uc.mem_write(p, 'SYSTEMROOT=C:\\WINDOWS'.encode('utf-16le') + b'\0\0\0\0')
        return p

    def api_GetEnvironmentStrings(self):
        p = self.alloc(64)
        self.uc.mem_write(p, b'SYSTEMROOT=C:\\WINDOWS\0\0')
        return p

    def api_FreeEnvironmentStringsW(self, p):
        return 1

    def api_FreeEnvironmentStringsA(self, p):
        return 1

    def api_WideCharToMultiByte(self, cp, fl, src, ncw, dst, ncb, defc, usedef):
        if ncw == 0xFFFFFFFF:
            raw = bytearray()
            i = 0
            while True:
                c = bytes(self.uc.mem_read(src + 2 * i, 2))
                raw += c
                i += 1
                if c == b'\0\0':
                    break
            ncw = i
        else:
            raw = bytes(self.uc.mem_read(src, 2 * ncw))
        out = bytes(raw).decode('utf-16le', 'replace').encode('latin1', 'replace')
        if ncb == 0:
            return len(out)
        self.uc.mem_write(dst, out[:ncb])
        return min(len(out), ncb)

    def api_MultiByteToWideChar(self, cp, fl, src, ncb, dst, ncw):
        if ncb == 0xFFFFFFFF:
            raw = self.cstr(src, 4096).encode('latin1') + b'\0'
        else:
            raw = bytes(self.uc.mem_read(src, ncb))
        out = raw.decode('latin1').encode('utf-16le')
        if ncw == 0:
            return len(out) // 2
        self.uc.mem_write(dst, out[:ncw * 2])
        return min(len(out) // 2, ncw)

    def api_GetStringTypeW(self, t, src, n, dst):
        n = n if n != 0xFFFFFFFF else 1
        self.uc.mem_write(dst, b'\x01\x00' * n)
        return 1

    def api_GetStringTypeA(self, loc, t, src, n, dst):
        n = n if n != 0xFFFFFFFF else 1
        self.uc.mem_write(dst, b'\x01\x00' * n)
        return 1

    def api_LCMapStringA(self, loc, fl, src, n, dst, nd):
        if nd == 0:
            return n
        self.uc.mem_write(dst, bytes(self.uc.mem_read(src, n))[:nd])
        return min(n, nd)

    def api_LCMapStringW(self, loc, fl, src, n, dst, nd):
        if nd == 0:
            return n
        self.uc.mem_write(dst, bytes(self.uc.mem_read(src, 2 * n))[:2 * nd])
        return min(n, nd)

    def api_GetLocaleInfoA(self, loc, t, buf, n):
        return 0

    def api_GetSystemDefaultLCID(self):
        return 0x409

    # ---- time / misc -----------------------------------------------------------
    def _systime(self, p):
        # 2005-06-15 12:00:00 (any plausible value will do)
        self.uc.mem_write(p, struct.pack('<8H', 2005, 6, 3, 15, 12, 0, 0, 0))

    def api_GetLocalTime(self, p):
        self._systime(p)

    def api_GetSystemTime(self, p):
        self._systime(p)

    def api_GetTimeZoneInformation(self, p):
        self.uc.mem_write(p, b'\0' * 172)
        return 0

    def api_Sleep(self, ms):
        pass

    # ---- shared memory / handles ---------------------------------------------
    def new_handle(self, obj):
        h = self.next_handle
        self.next_handle += 4
        self.handles[h] = obj
        return h

    def api_CreateFileMappingA(self, hfile, sec, prot, hi, lo, name):
        n = self.cstr(name)
        self.log('CreateFileMappingA(file=%#x, size=%#x, name=%r)' % (hfile, lo, n))
        if hfile == 0xFFFFFFFF:
            if n in self.named_maps:
                self.last_error = 183  # ERROR_ALREADY_EXISTS
                return self.named_maps[n]
            buf = self.alloc(max(lo, 16))
            h = self.new_handle(('map', buf, lo, n))
            if n:
                self.named_maps[n] = h
            self.last_error = 0
            return h
        obj = self.handles.get(hfile)
        if obj and obj[0] == 'file':
            data = obj[1]
            buf = self.alloc(len(data) + 16)
            self.uc.mem_write(buf, data)
            return self.new_handle(('map', buf, len(data), n))
        return 0

    def api_OpenFileMappingA(self, access, inherit, name):
        n = self.cstr(name)
        self.log('OpenFileMappingA(%r)' % n)
        if n in self.named_maps:
            return self.named_maps[n]
        self.last_error = 2
        return 0

    def api_MapViewOfFile(self, h, access, hi, lo, n):
        obj = self.handles.get(h)
        self.log('MapViewOfFile(%#x) -> %s' % (h, hex(obj[1]) if obj else None))
        return obj[1] if obj else 0

    def api_UnmapViewOfFile(self, p):
        return 1

    def api_CloseHandle(self, h):
        return 1

    def api_CreateEventA(self, *a):
        return self.new_handle(('event',))

    # ---- host-backed filesystem ---------------------------------------------
    def host_path(self, win):
        w = win.replace('\\', '/')
        if len(w) > 2 and w[1] == ':':
            w = w[2:]
        parts = [x for x in w.split('/') if x and x.lower() not in ('tp',)]
        cur = self.dir
        for part in parts:
            try:
                names = os.listdir(cur)
            except OSError:
                return None
            hit = [n for n in names if n.lower() == part.lower()]
            if not hit:
                return os.path.join(cur, part)  # nonexistent
            cur = os.path.join(cur, hit[0])
        return cur

    def api_GetFileAttributesA(self, name):
        n = self.cstr(name)
        hp = self.host_path(n)
        ok = hp is not None and os.path.exists(hp)
        self.log('GetFileAttributesA(%r) -> %s' % (n, 'ok' if ok else 'missing'))
        if not ok:
            self.last_error = 2
            return 0xFFFFFFFF
        return 0x10 if os.path.isdir(hp) else 0x20

    def api_CreateFileA(self, name, access, share, sec, disp, flags, tmpl):
        n = self.cstr(name)
        hp = self.host_path(n)
        self.log('CreateFileA(%r, access=%#x, disp=%d)' % (n, access, disp))
        if n.startswith('\\\\.\\'):
            return 0xFFFFFFFF  # device (SecDrv etc.)
        if hp and os.path.isfile(hp):
            with open(hp, 'rb') as f:
                data = f.read()
            return self.new_handle(['file', data, 0, hp])
        self.last_error = 2
        return 0xFFFFFFFF

    def api_ReadFile(self, h, buf, n, pread, ov):
        obj = self.handles.get(h)
        if not obj or obj[0] != 'file':
            return 0
        data, pos = obj[1], obj[2]
        chunk = data[pos:pos + n]
        self.uc.mem_write(buf, chunk)
        obj[2] = pos + len(chunk)
        if pread:
            self.w32(pread, len(chunk))
        return 1

    def api_SetFilePointer(self, h, lo, hi, method):
        obj = self.handles.get(h)
        if not obj or obj[0] != 'file':
            return 0xFFFFFFFF
        lo = lo - (1 << 32) if lo & 0x80000000 else lo
        base = (0, obj[2], len(obj[1]))[method]
        obj[2] = max(0, base + lo)
        return obj[2]

    def api_GetDriveTypeA(self, name):
        n = self.cstr(name)
        self.log('GetDriveTypeA(%r)' % n)
        return 5 if n.upper().startswith('D') else 3

    def api_GetLogicalDriveStringsA(self, n, buf):
        s = b'C:\\\0D:\\\0\0'
        self.uc.mem_write(buf, s)
        return len(s) - 1

    def api_GetProfileIntA(self, *a):
        return a[2]

    def api_GetVersionExA(self, p):
        # OSVERSIONINFOA: XP SP2
        self.uc.mem_write(p + 4, struct.pack('<IIII', 5, 1, 2600, 2) + b'Service Pack 2\0')
        return 1

    def api_RegOpenKeyExA(self, *a):
        self.log('RegOpenKeyExA(%r)' % self.cstr(a[1]))
        return 2  # ERROR_FILE_NOT_FOUND

    def api_RegOpenKeyA(self, *a):
        self.log('RegOpenKeyA(%r)' % self.cstr(a[1]))
        return 2

    def api_NtQueryInformationProcess(self, h, cls, buf, n, retlen):
        # ProcessDebugPort(7) etc: report "no debugger" (zeros)
        self.uc.mem_write(buf, b'\0' * n)
        if retlen:
            self.w32(retlen, n)
        return 0

    def api_MessageBoxA(self, h, text, cap, typ):
        print('[MessageBox] %r: %r' % (self.cstr(cap), self.cstr(text, 1024)))
        return 1

    # ---- fake system DLLs with real export tables -----------------------------
    MOD_BASES = {'kernel32.dll': 0x77000000, 'ntdll.dll': 0x77200000,
                 'user32.dll': 0x77400000, 'advapi32.dll': 0x77600000}
    KERNEL32_EXTRA = ['IsDebuggerPresent', 'ReadProcessMemory', 'WriteProcessMemory',
                      'VirtualProtect', 'CreateProcessA', 'CreateProcessW', 'GetStartupInfoW',
                      'VirtualQuery', 'GetModuleHandleW', 'CheckRemoteDebuggerPresent',
                      'OutputDebugStringA']
    NTDLL_NAMES = ['NtQueryInformationProcess']

    def build_fake_dll(self, dll, names):
        base = self.MOD_BASES[dll]
        size = 0x10000
        self.uc.mem_map(base, size, UC_PROT_ALL)
        names = sorted(set(names))
        img = bytearray(size)
        struct.pack_into('<2sH', img, 0, b'MZ', 0)
        struct.pack_into('<I', img, 0x3c, 0x80)
        o = 0x80
        img[o:o + 4] = b'PE\0\0'
        struct.pack_into('<HHIIIHH', img, o + 4, 0x14c, 1, 0x3B9ACA00, 0, 0, 0xE0, 0x2102)
        opt = o + 24
        struct.pack_into('<H', img, opt, 0x10b)
        struct.pack_into('<I', img, opt + 28, base)
        struct.pack_into('<II', img, opt + 32, 0x1000, 0x200)
        struct.pack_into('<I', img, opt + 56, size)
        struct.pack_into('<I', img, opt + 60, 0x400)
        struct.pack_into('<I', img, opt + 92, 16)
        exp_rva, exp_size = 0x1000, 0x8000
        struct.pack_into('<II', img, opt + 96, exp_rva, exp_size)
        sh = opt + 0xE0
        struct.pack_into('<8sIIIIIIHHI', img, sh, b'.text', 0xF000, 0x1000, 0, 0, 0, 0, 0, 0, 0x60000020)
        n = len(names)
        d = exp_rva
        funcs, nms, ords = d + 40, d + 40 + 4 * n, d + 40 + 8 * n
        strs = d + 40 + 10 * n + 16
        dllname = strs
        img[dllname:dllname + len(dll) + 1] = dll.encode() + b'\0'
        p = dllname + len(dll) + 1
        struct.pack_into('<IIHHIIIIIII', img, d, 0, 0x3B9ACA00, 0, 0, dllname, 1, n, n, funcs, nms, ords)
        for i, name in enumerate(names):
            thunk = self.make_thunk(dll, name)
            struct.pack_into('<I', img, funcs + 4 * i, thunk - base)
            struct.pack_into('<I', img, nms + 4 * i, p)
            struct.pack_into('<H', img, ords + 2 * i, i)
            img[p:p + len(name) + 1] = name.encode() + b'\0'
            p += len(name) + 1
        self.uc.mem_write(base, bytes(img))
        self.modules[dll] = base

    def setup_modules(self):
        k32 = [n for n in NARGS if n[0].isupper() and n not in ('MessageBoxA', 'CreateWindowExA', 'wvsprintfA',
               'WaitForInputIdle', 'GetDesktopWindow', 'MsgWaitForMultipleObjects', 'PeekMessageA',
               'GetMessageA', 'TranslateMessage', 'DispatchMessageA', 'DefWindowProcA', 'RegisterClassA',
               'ShowWindow', 'GetKeyboardType') and not n.startswith(('Reg', 'GetFileVersion', 'VerQuery', 'Nt'))]
        self.build_fake_dll('kernel32.dll', k32 + self.KERNEL32_EXTRA)
        self.build_fake_dll('ntdll.dll', self.NTDLL_NAMES + ['RtlUnwind'])
        self.build_fake_dll('user32.dll', ['MessageBoxA', 'CreateWindowExA', 'GetDesktopWindow', 'wsprintfA'])
        self.build_fake_dll('advapi32.dll', ['RegOpenKeyExA', 'RegQueryValueExA', 'RegCloseKey'])

    def module_by_name(self, name):
        n = name.lower()
        if not n.endswith('.dll'):
            n += '.dll'
        return n if n in self.modules else None

    def module_name_of(self, base):
        for k, v in self.modules.items():
            if v == base:
                return k
        return None

    def api_GetModuleHandleA(self, name):
        s = self.cstr(name)
        self.log('GetModuleHandleA(%r)' % s)
        if not name:
            return self.base
        m = self.module_by_name(s)
        return self.modules[m] if m else 0

    def api_GetModuleHandleW(self, name):
        s = bytes(self.uc.mem_read(name, 64)).decode('utf-16le', 'ignore').split('\0')[0]
        m = self.module_by_name(s)
        return self.modules[m] if m else 0

    def api_LoadLibraryA(self, name):
        s = self.cstr(name)
        self.log('LoadLibraryA(%r)' % s)
        m = self.module_by_name(s)
        return self.modules[m] if m else 0

    def api_GetProcAddress(self, mod, name):
        n = self.cstr(name) if name > 0x10000 else '#%d' % name
        dll = self.module_name_of(mod) or 'dyn'
        self.log('GetProcAddress(%s, %r)' % (dll, n))
        return self.make_thunk(dll, n)

    def api_IsDebuggerPresent(self):
        return 0

    def api_CheckRemoteDebuggerPresent(self, h, p):
        self.w32(p, 0)
        return 1

    def api_OutputDebugStringA(self, p):
        pass

    def api_VirtualProtect(self, addr, size, prot, old):
        if old:
            self.w32(old, 0x20)
        return 1

    def api_VirtualQuery(self, addr, buf, n):
        return 0

    def api_ReadProcessMemory(self, h, addr, buf, n, pread):
        try:
            self.uc.mem_write(buf, bytes(self.uc.mem_read(addr, n)))
        except UcError:
            return 0
        if pread:
            self.w32(pread, n)
        return 1

    def api_WriteProcessMemory(self, h, addr, buf, n, pw):
        self.log('WriteProcessMemory(h=%#x, addr=%#x, n=%#x)' % (h, addr, n))
        try:
            self.uc.mem_write(addr, bytes(self.uc.mem_read(buf, n)))
        except UcError:
            return 0
        if pw:
            self.w32(pw, n)
        return 1

    def api_CreateProcessA(self, app, cmd, *rest):
        print('[CreateProcessA] app=%r cmd=%r flags=%#x' % (self.cstr(app), self.cstr(cmd), rest[3]))
        return 0

    def api_GetStartupInfoW(self, p):
        self.uc.mem_write(p, b'\0' * 68)
        self.w32(p, 68)

    def _format(self, fmt, getarg):
        out = ''
        i = 0
        while i < len(fmt):
            c = fmt[i]
            if c != '%':
                out += c
                i += 1
                continue
            i += 1
            flags = ''
            while i < len(fmt) and fmt[i] in '-+ #0':
                flags += fmt[i]
                i += 1
            width = ''
            while i < len(fmt) and fmt[i].isdigit():
                width += fmt[i]
                i += 1
            while i < len(fmt) and fmt[i] in 'lh':
                i += 1
            t = fmt[i] if i < len(fmt) else ''
            i += 1
            if t == '%':
                out += '%'
                continue
            v = getarg()
            if t == 's':
                txt = self.cstr(v, 1024)
            elif t in 'di':
                txt = str(v - (1 << 32) if v & 0x80000000 else v)
            elif t == 'u':
                txt = str(v)
            elif t in 'xX':
                txt = '%x' % v if t == 'x' else '%X' % v
            elif t == 'c':
                txt = chr(v & 0xFF)
            else:
                txt = ''
            w = int(width) if width else 0
            if '-' in flags:
                txt = txt.ljust(w)
            else:
                txt = txt.rjust(w, '0' if '0' in flags and t != 's' else ' ')
            out += txt
        return out

    def api_wsprintfA(self, buf, fmt, *args):
        # cdecl: re-read args from the stack beyond the 2 fixed ones
        esp = self.uc.reg_read(UC_X86_REG_ESP)
        it = iter(self.r32(esp + 12 + 4 * i) for i in range(32))
        txt = self._format(self.cstr(fmt, 1024), lambda: next(it))
        self.uc.mem_write(buf, txt.encode('latin1') + b'\0')
        return len(txt)

    def api_wvsprintfA(self, buf, fmt, va):
        n = [0]
        def get():
            v = self.r32(va + 4 * n[0])
            n[0] += 1
            return v
        txt = self._format(self.cstr(fmt, 1024), get)
        self.uc.mem_write(buf, txt.encode('latin1') + b'\0')
        return len(txt)

    def api_ExitProcess(self, code):
        print('[ExitProcess(%d)] after %d API calls' % (code, self.calls))
        self.uc.emu_stop()

    # ---- run ---------------------------------------------------------------------
    def run(self, max_insn):
        ep = self.base + self.pe.OPTIONAL_HEADER.AddressOfEntryPoint
        print('entry %#x' % ep)
        # return address for the entry point -> fake ExitProcess thunk
        self.uc.reg_write(UC_X86_REG_ESP, self.uc.reg_read(UC_X86_REG_ESP) - 4)
        self.w32(self.uc.reg_read(UC_X86_REG_ESP), self.make_thunk('kernel32.dll', 'ExitProcess'))
        try:
            self.uc.emu_start(ep, 0, count=max_insn)
        except UcError as e:
            eip = self.uc.reg_read(UC_X86_REG_EIP)
            print('[!] emulation stopped:', e, 'eip=%#x' % eip, self.thunks.get(eip))
            print('    esp=%#x ' % self.uc.reg_read(UC_X86_REG_ESP) + ' '.join('%s=%#x' % (n, self.uc.reg_read(r)) for n, r in (('eax', UC_X86_REG_EAX), ('ebx', UC_X86_REG_EBX), ('ecx', UC_X86_REG_ECX), ('edx', UC_X86_REG_EDX), ('esi', UC_X86_REG_ESI), ('edi', UC_X86_REG_EDI), ('ebp', UC_X86_REG_EBP))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game_dir')
    ap.add_argument('--max-insn', type=int, default=5_000_000)
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--trace', action='store_true')
    a = ap.parse_args()
    e = Emu(a.game_dir, a.quiet)
    e.trace_all = a.trace
    e.setup()
    e.run(a.max_insn)
    print('API calls: %d, EIP=%#x' % (e.calls, e.uc.reg_read(UC_X86_REG_EIP)))
    print('last insns:', ' '.join('%x' % x for x in e.ring[-25:]))


if __name__ == '__main__':
    sys.exit(main())
