# Cross-build the ASI plugin with mingw-w64.
CXX      := x86_64-w64-mingw32-g++
CXXFLAGS := -std=c++17 -O2 -municode -Wall -Wextra -Wno-cast-function-type
LDFLAGS  := -shared -s -static -static-libgcc -static-libstdc++ \
            -Wl,--enable-stdcall-fixup -Wl,--kill-at

TARGET  := build/NierConcurrentInput.asi
SOURCES := src/dllmain.cpp

GAMEDIR ?= /mnt/data/steam/steamapps/common/NieR Replicant ver.1.22474487139

# Ultimate ASI Loader. The x64 zip holds a single dinput8.dll; the game wants it
# as winmm.dll. Kept in its own subdirectory so it can't be picked up by the
# wine smoke test, which runs out of build/.
LOADER_VERSION := v9.7.4
LOADER_URL     := https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/$(LOADER_VERSION)/Ultimate-ASI-Loader_x64.zip
LOADER_SHA256  := 8272d83b2692662098746f2d0ad0e2d85f3c8358ab1d63f75fbe835c2c8135fd
LOADER_ZIP     := build/loader/Ultimate-ASI-Loader_x64.zip
LOADER         := build/loader/winmm.dll

.PHONY: all clean install install-loader loader
all: $(TARGET)

$(TARGET): $(SOURCES) | build
	$(CXX) $(CXXFLAGS) $(SOURCES) $(LDFLAGS) -o $@

build:
	mkdir -p build

build/loader:
	mkdir -p build/loader

# Downloads the pinned release and checks it against the hash above, so a
# retagged or truncated download fails here instead of in the game.
$(LOADER_ZIP): | build/loader
	curl -fSL -o $@.tmp $(LOADER_URL)
	echo "$(LOADER_SHA256)  $@.tmp" | sha256sum -c -
	mv $@.tmp $@

$(LOADER): $(LOADER_ZIP)
	unzip -p $< dinput8.dll > $@.tmp
	mv $@.tmp $@

loader: $(LOADER)

# Copies the ASI loader next to the game exe. Deliberately does *not* depend on
# $(LOADER): `make install` must never reach out to the network by itself, so a
# missing loader is a warning here rather than a download.
install-loader:
	@if [ -f "$(GAMEDIR)/winmm.dll" ]; then \
	    echo "ASI loader: winmm.dll already in the game folder, left alone."; \
	elif [ -f "$(LOADER)" ]; then \
	    cp "$(LOADER)" "$(GAMEDIR)/"; \
	    echo "ASI loader: installed as $(GAMEDIR)/winmm.dll"; \
	else \
	    echo "WARNING: no ASI loader found. The plugin will not load without one."; \
	    echo "         To fetch and install it, run:"; \
	    echo ""; \
	    echo "             make loader"; \
	    echo "             make install-loader GAMEDIR=\"$(GAMEDIR)\""; \
	    echo ""; \
	fi

# Copies the plugin, a default ini and the ASI loader next to the game exe.
# Touches nothing else, and never overwrites an ini or loader already there.
install: $(TARGET) install-loader
	cp $(TARGET) "$(GAMEDIR)/"
	[ -f "$(GAMEDIR)/NierConcurrentInput.ini" ] || cp dist/NierConcurrentInput.ini "$(GAMEDIR)/"

clean:
	rm -rf build

# Smoke test: builds a host exe that embeds the game's byte sequences in .text,
# loads the plugin under wine and verifies the patch landed.
build/test_host.exe: tests/host.cpp | build
	$(CXX) -std=c++17 -O0 -Wall -Wextra tests/host.cpp -o $@

# The test ini turns everything on; the shipped defaults leave
# KeyboardAlwaysActive off.
.PHONY: test
test: $(TARGET) build/test_host.exe
	printf '[Concurrent Input]\nMouseAlwaysActive = true\nKeyboardAlwaysActive = true\nCameraOnly = false\n\n[Prompts]\nForceControllerPrompts = true\n\n[Debug]\nLogging = true\n' > build/NierConcurrentInput.ini
	cd build && wine test_host.exe
