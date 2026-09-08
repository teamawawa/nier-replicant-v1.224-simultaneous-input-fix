# NieR Replicant ver.1.22474487139 — simultaneous mouse + controller

> ✻ Baked for 22m 34s

Small ASI plugin that stops *NieR Replicant ver.1.22474487139* from disabling the mouse while a
controller is in use, so the mouse can drive the camera at the same time as the pad.

Motivation: playing with a Steam Controller, whose touchpad is mapped to the mouse for camera look.

## What it does

The game keeps a single "active input device" flag. It is forced to *controller* for as long as any
pad button is held or a stick is off-centre, and everything mouse-related is gated behind it — the
camera's mouse branch, and also mouse capture (cursor hiding, clipping and per-frame recentring).
So the moment you hold the left stick to run, the mouse is dead.

The plugin NOPs the two branches that AND that flag into the game's `MouseUsable()` predicate
(4 bytes total). After that:

* the **camera** picks mouse-vs-right-stick per frame, using the game's own rule: mouse if the mouse
  moved this frame, right stick otherwise;
* **everything else on the pad** — left stick, buttons, triggers — keeps working in the same frame,
  untouched;
* controller **button prompts still switch correctly**: the active-device flag itself is not
  modified, only the mouse gate that read it;
* mouse capture and cursor recentring stay in their normal "mouse is active" state, so touchpad-driven
  look does not die at the screen edge.

Full reverse-engineering write-up, including addresses and the byte-level reasoning:
[`docs/FINDINGS.md`](docs/FINDINGS.md).

## Install

Requires an ASI loader in the game folder. If you already use
[NierReplicantFix](https://codeberg.org/Lyall/NierReplicantFix), its `winmm.dll` is one — the two
plugins patch different bytes and coexist in any load order.

```sh
make
make install            # copies the .asi + default .ini next to the game exe
```

`GAMEDIR` overrides the destination:

```sh
make install GAMEDIR="/path/to/NieR Replicant ver.1.22474487139"
```

To uninstall, delete `NierConcurrentInput.asi`, `NierConcurrentInput.ini` and
`NierConcurrentInput.log` from the game folder. No original game file is modified.

### Settings — `NierConcurrentInput.ini`

| Key | Default | Meaning |
| --- | --- | --- |
| `MouseAlwaysActive` | `true` | The fix described above. |
| `CameraOnly` | `false` | Narrower variant: only the camera ignores the active-device flag, menus and cursor handling are left completely alone. Does *not* restore cursor recentring while a pad is active, so look input stops once the cursor hits a screen edge. For comparison only. |
| `Logging` | `true` | Writes `NierConcurrentInput.log` next to the exe listing what was found and patched. |

On startup the log should read:

```
[+] MouseUsable device-mode gate: found at ...
[+] Mouse Always Active: patched
```

If a game update moves the code, the byte signature will stop matching; the plugin then logs
`pattern not found` and changes nothing rather than corrupting the executable.

## Without an ASI loader

`tools/patch_exe.py` applies the same 4-byte change statically, always to a **copy**:

```sh
python3 tools/patch_exe.py "game/NieR Replicant ver.1.22474487139.exe" -o build/patched.exe
python3 tools/patch_exe.py build/patched.exe --verify
```

## Build and test

```sh
make        # cross-compiles the plugin with mingw-w64
make test   # builds a host exe that embeds the game's byte sequences in its own
            # .text, loads the plugin under wine and asserts the patch landed
```

## Repo layout

```
src/dllmain.cpp     the plugin
dist/               default ini shipped next to the plugin
tests/host.cpp      wine smoke test
tools/              static analysis helpers + the standalone exe patcher
scripts/ghidra/     headless Ghidra decompile / xref scripts
docs/FINDINGS.md    reverse-engineering notes
game/               local copies of the game files (gitignored, not redistributable)
```

Analysis was done entirely against copies in `game/`; the Steam installation itself is only ever
added to, never modified.
