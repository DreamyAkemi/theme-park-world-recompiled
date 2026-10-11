# Build ddraw.dll (our DirectDraw layer) and the API test. Needs i686-w64-mingw32-g++.
MINGW_CXX ?= i686-w64-mingw32-g++
FLAGS = -O2 -std=c++17 -Wall -Wno-unused-function

all: ddraw.dll test_ddraw.exe

ddraw.dll: ddraw.cpp ddraw.def
	$(MINGW_CXX) $(FLAGS) -shared -static -static-libgcc -static-libstdc++ -s -o $@ ddraw.cpp ddraw.def -ldxguid -luuid -lgdi32 -luser32 -Wl,--kill-at,--enable-stdcall-fixup

test_ddraw.exe: test_ddraw.cpp
	$(MINGW_CXX) $(FLAGS) -static -o $@ test_ddraw.cpp -ldxguid -luuid -lgdi32 -luser32

clean:
	rm -f ddraw.dll test_ddraw.exe
