# Cross-build the ASI plugin with mingw-w64.
CXX      := x86_64-w64-mingw32-g++
CXXFLAGS := -std=c++17 -O2 -municode -Wall -Wextra -Wno-cast-function-type
LDFLAGS  := -shared -s -static -static-libgcc -static-libstdc++ \
            -Wl,--enable-stdcall-fixup -Wl,--kill-at

TARGET  := build/NierConcurrentInput.asi
SOURCES := src/dllmain.cpp

GAMEDIR ?= /mnt/data/steam/steamapps/common/NieR Replicant ver.1.22474487139

.PHONY: all clean install
all: $(TARGET)

$(TARGET): $(SOURCES) | build
	$(CXX) $(CXXFLAGS) $(SOURCES) $(LDFLAGS) -o $@

build:
	mkdir -p build

# Copies the plugin + a default ini next to the game exe. Touches nothing else.
install: $(TARGET)
	cp $(TARGET) "$(GAMEDIR)/"
	[ -f "$(GAMEDIR)/NierConcurrentInput.ini" ] || cp dist/NierConcurrentInput.ini "$(GAMEDIR)/"

clean:
	rm -rf build

# Smoke test: builds a host exe that embeds the game's byte sequences in .text,
# loads the plugin under wine and verifies the patch landed.
build/test_host.exe: tests/host.cpp | build
	$(CXX) -std=c++17 -O0 -Wall -Wextra tests/host.cpp -o $@

.PHONY: test
test: $(TARGET) build/test_host.exe dist/NierConcurrentInput.ini
	cp dist/NierConcurrentInput.ini build/
	cd build && wine test_host.exe
