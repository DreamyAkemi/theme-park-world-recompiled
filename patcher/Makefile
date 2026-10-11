# Build tpw_patch.exe (single file, shim embedded). Needs i686-w64-mingw32-gcc and python3.
MINGW_CC ?= i686-w64-mingw32-gcc

all: tpw_patch.exe

payload/weachatr.dll: build_shim.py shim/weachatr_stubs.c shim/weachatr_imports.txt
	python3 build_shim.py

payload_weachatr.h: payload/weachatr.dll
	python3 -c "d=open('payload/weachatr.dll','rb').read(); print('static const unsigned char shim_dll[] = {'+','.join(map(str,d))+'};\nstatic const unsigned shim_dll_len = %d;' % len(d))" > $@

tpw_patch.exe: tpw_patch.c payload_weachatr.h
	$(MINGW_CC) -O2 -Wall -static -o $@ tpw_patch.c -lbcrypt -ladvapi32

clean:
	rm -f tpw_patch.exe payload_weachatr.h payload/weachatr.dll
