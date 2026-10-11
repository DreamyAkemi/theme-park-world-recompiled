#!/usr/bin/env python3
"""Run the user's own secdrv.sys inside Unicorn as a minimal kernel-driver host.

Lets the SafeDisc loader's DeviceIoControl requests be answered by the real driver
code, without loading anything into the host OS.  Nothing is written to disk.
"""
import struct

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UcError, UC_PROT_ALL
from unicorn.x86_const import *

PAGE = 0x1000
THUNKS = 0x7E000000
STACK_TOP = 0x00800000
HEAP = 0x20000000
DATA_IMPORTS = {'NtBuildNumber', 'KeTickCount'}
NARGS = dict(IoCreateDevice=7, IoCreateSymbolicLink=2, IoDeleteDevice=1, IoDeleteSymbolicLink=1,
             RtlInitUnicodeString=2, RtlEqualUnicodeString=3, RtlQueryRegistryValues=5,
             IofCompleteRequest=0)  # fastcall: args in ecx/edx

IRP_MJ_CREATE, IRP_MJ_CLOSE, IRP_MJ_DEVICE_CONTROL = 0, 2, 14


class DriverHost:
    # The driver refuses anything newer than Windows 2000, so present as Win2000 SP4.
    REG_VALUES = {'CurrentVersion': '5.0', 'CSDVersion': 'Service Pack 4'}
    NT_BUILD = 2195

    def __init__(self, sys_path, verbose=False):
        self.v = verbose
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.heap = HEAP
        self.thunks = {}
        self.pe = pefile.PE(sys_path)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.device = None
        self.tick = 0x1000
        self._load()

    # -- helpers
    def r32(self, a):
        return struct.unpack('<I', self.uc.mem_read(a, 4))[0]

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack('<I', v & 0xFFFFFFFF))

    def map(self, start, size):
        s = start & ~(PAGE - 1)
        e = (start + size + PAGE - 1) & ~(PAGE - 1)
        for a in range(s, e, PAGE):
            try:
                self.uc.mem_map(a, PAGE, UC_PROT_ALL)
            except UcError:
                pass

    def alloc(self, n):
        n = (n + 15) & ~15
        p = self.heap
        self.heap += n
        self.map(p, n)
        return p

    def log(self, *a):
        if self.v:
            print('[drv]', *a)

    def wstr(self, p):
        length, _, buf = struct.unpack('<HHI', self.uc.mem_read(p, 8))
        return bytes(self.uc.mem_read(buf, length)).decode('utf-16le', 'replace') if buf else ''

    # -- loading
    def _load(self):
        uc = self.uc
        img = self.pe.get_memory_mapped_image()
        self.map(self.base, len(img))
        uc.mem_write(self.base, bytes(img))
        self.map(THUNKS, 0x10000)
        self.map(STACK_TOP - 0x100000, 0x100000 + PAGE)
        self.map(HEAP, 0x1000)
        self.nt_build = self.alloc(8)
        self.w32(self.nt_build, 0xF0000000 | self.NT_BUILD)
        self.ke_tick = self.alloc(16)
        for ent in self.pe.DIRECTORY_ENTRY_IMPORT:
            for imp in ent.imports:
                name = imp.name.decode()
                if name == 'NtBuildNumber':
                    self.w32(imp.address, self.nt_build)
                elif name == 'KeTickCount':
                    self.w32(imp.address, self.ke_tick)
                else:
                    a = THUNKS + 16 * len(self.thunks)
                    self.thunks[a] = name
                    self.w32(imp.address, a)
        uc.hook_add(UC_HOOK_CODE, self._on_thunk, begin=THUNKS, end=THUNKS + 0x10000)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_unmapped)

    def _on_unmapped(self, uc, access, addr, size, value, _):
        print('[drv] unmapped access type=%d addr=%#x eip=%#x' % (access, addr, uc.reg_read(UC_X86_REG_EIP)))
        return False

    def _on_thunk(self, uc, addr, size, _):
        name = self.thunks.get(addr)
        if not name:
            return
        esp = uc.reg_read(UC_X86_REG_ESP)
        n = NARGS.get(name, 0)
        args = [self.r32(esp + 4 + 4 * i) for i in range(n)]
        ret = self.r32(esp)
        rv = 0
        if name == 'IoCreateDevice':
            dev = self.alloc(0x100)
            self.w32(dev + 0x2C, 0)                    # Flags
            self.w32(dev + 0x28, self.alloc(max(args[1], 16)))  # DeviceExtension
            self.w32(args[6], dev)
            self.device = dev
            self.log('IoCreateDevice(%r) -> %#x' % (self.wstr(args[2]) if args[2] else '', dev))
        elif name == 'IoCreateSymbolicLink':
            self.log('IoCreateSymbolicLink(%r -> %r)' % (self.wstr(args[0]), self.wstr(args[1])))
        elif name == 'RtlInitUnicodeString':
            s, src = args
            n16 = 0
            if src:
                while bytes(uc.mem_read(src + 2 * n16, 2)) != b'\0\0':
                    n16 += 1
            uc.mem_write(s, struct.pack('<HHI', 2 * n16, 2 * n16 + 2, src))
        elif name == 'RtlEqualUnicodeString':
            rv = int(self.wstr(args[0]).lower() == self.wstr(args[1]).lower())
        elif name == 'RtlQueryRegistryValues':
            rel, path, table = args[0], args[1], args[2]
            pth = bytes(uc.mem_read(path, 200)).decode('utf-16le', 'ignore').split('\0')[0] if path > 0x10000 else hex(path)
            self.log('RtlQueryRegistryValues(rel=%d, path=%r)' % (rel, pth))
            off = table
            while True:
                qr, fl, nm, ctx, dtype, ddata, dlen = struct.unpack('<IIIIIII', bytes(uc.mem_read(off, 28)))
                if qr == 0 and nm == 0:
                    break
                nme = bytes(uc.mem_read(nm, 80)).decode('utf-16le', 'ignore').split('\0')[0] if nm else None
                self.log('   entry: routine=%#x flags=%#x name=%r ctx=%#x deftype=%d defdata=%#x deflen=%d' % (qr, fl, nme, ctx, dtype, ddata, dlen))
                if fl & 0x20 and nme in self.REG_VALUES and dtype == 1:
                    val = self.REG_VALUES[nme].encode('utf-16le')
                    ln, mx, buf = struct.unpack('<HHI', bytes(uc.mem_read(ctx, 8)))
                    if buf == 0 or mx < len(val) + 2:
                        buf = self.alloc(len(val) + 2)
                        mx = len(val) + 2
                    uc.mem_write(buf, val + b'\0\0')
                    uc.mem_write(ctx, struct.pack('<HHI', len(val), mx, buf))
                off += 28
            rv = 0
        elif name == 'IofCompleteRequest':
            self.log('IofCompleteRequest')
        else:
            self.log('stub', name, [hex(a) for a in args])
        uc.reg_write(UC_X86_REG_EAX, rv & 0xFFFFFFFF)
        uc.reg_write(UC_X86_REG_ESP, esp + 4 + 4 * n)
        uc.reg_write(UC_X86_REG_EIP, ret)

    # -- calling into driver code
    def call(self, fn, args, max_insn=5_000_000):
        uc = self.uc
        sp = STACK_TOP - 0x1000
        for a in reversed(args):
            sp -= 4
            self.w32(sp, a)
        sp -= 4
        self.w32(sp, THUNKS + 0xF000)                  # return address (magic stop)
        uc.reg_write(UC_X86_REG_ESP, sp)
        uc.reg_write(UC_X86_REG_EBP, 0)
        try:
            uc.emu_start(fn, THUNKS + 0xF000, count=max_insn)
        except UcError as e:
            print('[drv] emulation error: %s eip=%#x' % (e, uc.reg_read(UC_X86_REG_EIP)))
            return None
        return uc.reg_read(UC_X86_REG_EAX)

    def driver_entry(self):
        drv = self.alloc(0xA8)
        reg = self.alloc(16)
        name = 'REGISTRY\\MACHINE\\SYSTEM\\CurrentControlSet\\Services\\Secdrv'.encode('utf-16le') + b'\0\0'
        nb = self.alloc(len(name))
        self.uc.mem_write(nb, name)
        self.uc.mem_write(reg, struct.pack('<HHI', len(name) - 2, len(name), nb))
        self.driver_obj = drv
        ep = self.base + self.pe.OPTIONAL_HEADER.AddressOfEntryPoint
        st = self.call(ep, [drv, reg])
        self.major = {i: self.r32(drv + 0x38 + 4 * i) for i in range(28)}
        self.log('DriverEntry -> %s' % (hex(st) if st is not None else None))
        self.log('MajorFunction:', {k: hex(v) for k, v in self.major.items() if v})
        return st

    def dispatch(self, major, ioctl=0, inbuf=b'', outlen=0):
        irp = self.alloc(0x100)
        stack = self.alloc(0x40)
        sysbuf = self.alloc(max(len(inbuf), outlen, 16) + 16)
        self.uc.mem_write(sysbuf, inbuf)
        self.w32(irp + 0x0C, sysbuf)
        self.w32(irp + 0x3C, sysbuf)
        self.w32(irp + 0x60, stack)
        self.uc.mem_write(stack, bytes([major, 0, 0, 0]))
        self.w32(stack + 4, outlen)
        self.w32(stack + 8, len(inbuf))
        self.w32(stack + 0xC, ioctl)
        self.w32(stack + 0x10, sysbuf)
        self.w32(self.ke_tick, self.tick)
        self.tick += 7
        fn = self.major.get(major)
        if not fn:
            return None
        st = self.call(fn, [self.device, irp])
        status = self.r32(irp + 0x18)
        info = self.r32(irp + 0x1C)
        out = bytes(self.uc.mem_read(sysbuf, min(max(info, 0), 0x10000)))
        return st, status, info, out


if __name__ == '__main__':
    import sys
    h = DriverHost(sys.argv[1], verbose=True)
    h.driver_entry()
    print('create ->', h.dispatch(IRP_MJ_CREATE))
