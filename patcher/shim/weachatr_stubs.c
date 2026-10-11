/*
 * Stubs for Chat methods that the retail tp.exe imports from weachatr.dll
 * but that the later (Jan 2000) weachatr.dll no longer exports.
 *
 * All are __thiscall (callee pops args): ret N = 4 * (number of stack args).
 * Online services are dead, so each stub just returns a nonzero error code.
 * The return value semantics are an assumption (DWORD status, 0 = OK).
 */
__asm__(
    ".text\n"
    ".globl _stub_ParkShout\n"  "_stub_ParkShout:\n"  "  movl $1, %eax\n" "  ret $12\n"
    ".globl _stub_CreatePark\n" "_stub_CreatePark:\n" "  movl $1, %eax\n" "  ret $24\n"
    ".globl _stub_ParkInfo\n"   "_stub_ParkInfo:\n"   "  movl $1, %eax\n" "  ret $8\n"
);

#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID x) { (void)h; (void)r; (void)x; return TRUE; }
