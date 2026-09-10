# NieR Replicant ver.1.22474487139 — simultaneous mouse + controller

Small plugin that stops *NieR Replicant ver.1.22474487139* from disabling the mouse while a controller is in use, so the mouse can drive the camera at the same time as the pad.

Aim is to make it work well with the Steam Controller touchpads.

This mod was developed by claude in an impressive time, this includes reversing, documentation and building:

> ✻ Baked for 22m 34s

Details of that are available in [`docs/PROMPT.md`](docs/PROMPT.md). If you have any reason to be unhappy with the use of LLMs, please simply do not use this mod and move on, sending grievances is highly discouraged.

## Install

### Pre-built (recommended)

See instructions in the [latest release](https://github.com/teamawawa/nier-replicant-v1.224-simultaneous-input-fix/releases/latest).

You can also use [nexus mods](https://www.nexusmods.com/nierreplicant/mods/123) if you want, though there's no difference between the two options and difficulty.

### Manual build & Install (for advanced users)

- Download [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/tag/v9.7.4), specifically [x64 variant of v9.7.4](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/v9.7.4/Ultimate-ASI-Loader_x64.zip).
- Extract the dll and rename it to `winmm.dll`, put it into your game folder next to the exe. (I'm not linking to winmm zip directly, as that's linking to latest version vs just v9.7.4)
- If you're on linux, set your launch command for the game in steam to `WINEDLLOVERRIDES="winmm=n,b" %command%`.
- Run commands below to compile and install the actual mod:

```sh
make
make install GAMEDIR="/path/to/NieR Replicant ver.1.22474487139"
```

Make sure you replace the path before you run the install command. If you don't want to, you can also manually copy the .asi and .ini files from `build/` to your game folder, next to the exe.

### Uninstalling

To uninstall, delete `NierConcurrentInput.asi`, `NierConcurrentInput.ini` and
`NierConcurrentInput.log` from the game folder. No original game file is modified.

## Settings — `NierConcurrentInput.ini`

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

## What it does

The game keeps a single "active input device" flag. It is forced to *controller* for as long as any
pad button is held or a stick is off-centre, and everything mouse-related is gated behind it — the
camera's mouse branch, and also mouse capture (cursor hiding, clipping and per-frame recentring).
So the moment you hold the left stick to run, the mouse is dead.

The plugin NOPs the two branches that AND that flag into the game's `MouseUsable()` predicate
(4 bytes total). After that:

* the camera picks mouse-vs-right-stick per frame, using the game's own rule: mouse if the mouse
  moved this frame, right stick otherwise;
* everything else on the pad — left stick, buttons, triggers — keeps working in the same frame,
  untouched;
* controller button prompts still switch correctly: the active-device flag itself is not modified,
  only the mouse gate that read it;
* mouse capture and cursor recentring stay in their normal "mouse is active" state, so touchpad-driven
  look does not die at the screen edge.

Full reverse-engineering write-up, including addresses and the byte-level reasoning:
[`docs/FINDINGS.md`](docs/FINDINGS.md).

## Installing without an ASI loader

`tools/patch_exe.py` applies the same 4-byte change statically, always to a **copy**:

```sh
python3 tools/patch_exe.py "game/NieR Replicant ver.1.22474487139.exe" -o build/patched.exe
python3 tools/patch_exe.py build/patched.exe --verify
```

## Build and test

These are primarily for ensuring the setup works under linux.

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
docs/PROMPT.md      details of the model and prompt used to develop this mod
```

## Thanks

- [NierReplicantFix by Lyall](https://codeberg.org/Lyall/NierReplicantFix) served as a very useful inspiration, and a good base for patching the game

