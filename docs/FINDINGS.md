# NieR Replicant ver.1.22474487139 input device switching — reverse-engineering notes

Target: `NieR Replicant ver.1.22474487139.exe`, Steam build
(MD5 `89e4c3d0af6c86db2312fb911dafec91`, 23,723,240 bytes, PE32+, image base `0x140000000`).
No Denuvo or packer; a plain MSVC binary with intact `.pdata` (46,634 functions) and MSVC RTTI.

All addresses below are **virtual addresses at the default image base `0x140000000`**.
Subtract `0x140000000` to get an RVA.

## 1. Engine layout

RTTI names show the engine is Toylogic's "tp" framework with a "cgl" rendering/input layer:

| Class | Meaning |
| --- | --- |
| `tp::hid::Input` | platform HID aggregate (singleton, instance at `0x1415B01A8`) |
| `tp::hid::Mouse::RawDevice` / `MouseRawDeviceWin32` | Win32/DirectInput8 mouse backend |
| `tp::hid::Keyboard::RawDevice` / `KeyboardRawDeviceWin32` | keyboard backend |
| `tp::hid::Pad::RawDevice` / `PadRawDeviceWin32` | XInput + DirectInput pad backend |
| `tp::hid::DirectInput` | DirectInput8 wrapper |
| `InputManager` (`tp::fnd::Singleton<InputManager>`) | game-side input manager, instance `0x1415B01A0` |
| `cgl::input::Pad`, `cgl::input::IInputDevice` | pad state cache |

Imports confirm the input stack: `DINPUT8!DirectInput8Create`, `XINPUT9_1_0!XInputGetState/SetState`,
and raw Win32 cursor handling (`GetCursorPos`, `SetCursorPos`, `ShowCursor`, `ClipCursor`,
`ScreenToClient`, `GetClientRect`). There is **no** RawInput usage — mouse deltas are derived from
`GetCursorPos` + a recentre via `SetCursorPos`.

## 2. The global input context

`0x14443E400` is the game's input context ("player 0" input object). Relevant fields:

| Offset | VA | Meaning |
| --- | --- | --- |
| `+0x80..` | `0x14443E480` | per-axis "keyboard mapping enabled" flags |
| `+0x88` | `0x14443E488` | active pad index |
| `+0x8C` | `0x14443E48C` | **`usingGamepad`** — 1 = pad is the active device, 0 = KB/mouse |
| `+0x8D` | `0x14443E48D` | "mouse moved while the mouse was disabled" latch |
| `+0x8E` | `0x14443E48E` | mouse input globally enabled (set/cleared by `FUN_140068D80` / `FUN_140068E50`) |
| `+0x8F` | `0x14443E48F` | cached result of `MouseUsable()` from the previous frame |
| `+0x90` | `0x14443E490` | "force re-apply cursor mode" flag |
| `+0xA8` | `0x14443E4A8` | embedded `cgl::input::Pad` (per-pad records of `0x214` bytes at `+0x6C`) |

`0x14443E48C` is read from 35+ call sites across the game, and drives button-prompt glyphs
(KB vs. controller icons), UI behaviour, etc. Anything that changes it changes the glyphs, so the
fix below leaves it alone.

Related globals: `0x14443E394` (mouse button state block), `0x14443E3B0` (keyboard state block),
`0x14443E3C4` (mouse wheel, float).

## 3. `UpdateActiveDevice` — `0x1403D3410`

Runs every frame, and is what "disables the mouse when a controller is detected":

```c
void UpdateActiveDevice(ctx) {
    if (PadConnected(&ctx->pad, ctx->padIndex)) {
        // four button/axis state masks at pad record +0x70/+0x74/+0x78/+0x7C
        if (PadMask0() || PadMask1() || PadMask2() || PadMask3()) {
            *(uint16_t*)&ctx->f8C = 1;   // ctx->0x8C = 1 (pad), ctx->0x8D = 0
            return;                      // <-- EARLY OUT: KB/mouse never even polled
        }
    }
    kbOrMouseButton = any of ~20 keyboard keys, or any of 4 mouse buttons;
    mouseMoved      = mouseDX || mouseDY || wheel != 0;

    if (kbOrMouseButton || mouseMoved) {
        ctx->f8C = 0;                                   // KB/mouse becomes active device
        if (kbOrMouseButton)          ctx->f8D = 0;
        else if (mouseMoved && ctx->f8F == 0) ctx->f8D = 1;   // mouse-move alone is NOT enough
    }
}
```

Two things follow:

1. While any pad button is held or a stick is off-centre, the function returns early, so
   `0x8C` stays `1` for as long as you are, say, holding the left stick to run. Moving the mouse in
   that frame is simply never looked at.
2. Once `0x8C` is 1, `MouseUsable()` returns false, so `0x8F` becomes 0, and then mouse *movement*
   alone sets `0x8D = 1`, which keeps the mouse disabled. You have to press a key or click a mouse
   button to get back to KB/mouse. This is the "controller detected → mouse dead" behaviour.

## 4. `MouseUsable` — `0x1403D3F20`

```c
bool MouseUsable(ctx) {
    r  = g_mousePresent(0x14130B72C) & hidInput->mouseConnected(+0x2F);
    r &= (ctx->f8C == 0 && ctx->f8D == 0);      // <-- THE DEVICE-MODE GATE (patched by this repo)
    r &= ctx->f8E;                              // mouse globally enabled
    r &= UIManager_AllowsMouse();
    if (hidInput->mouseConnected && ctx->f8F == 0)
        r &= cursor is inside the client rect;  // only checked while the mouse is not already active
    return r;
}
```

The gate lives at `0x1403D3F5A`:

```
1403D3F5A  80 BE 8C 00 00 00 00   cmp byte [rsi+0x8C], 0
1403D3F61  75 0D                  jne  1403D3F70          <-- patched to 90 90
1403D3F63  80 BE 8D 00 00 00 00   cmp byte [rsi+0x8D], 0
1403D3F6A  75 04                  jne  1403D3F70          <-- patched to 90 90
1403D3F6C  B0 01                  mov  al, 1
1403D3F6E  EB 02                  jmp  1403D3F72
1403D3F70  32 C0                  xor  al, al
1403D3F72  ...
```

`MouseUsable()`'s result is also what drives the mouse capture mode. `FUN_1403D4886` compares it to
the cached `ctx->0x8F` and, on change, pushes three booleans into `MouseRawDeviceWin32`:

| Raw device field | Set from | Effect in `MouseRawDeviceWin32::Update` (`0x1407C45C0`) |
| --- | --- | --- |
| `+0x48` | `!usable` | `ShowCursor()` — hardware cursor visible |
| `+0x49` | `usable` | `ClipCursor()` — cursor confined to the window |
| `+0x4A` | `usable` | recentre the cursor with `SetCursorPos` each frame and derive deltas from it |

So when the pad takes over, the game also turns off mouse-look recentring, which is why even
"partial" fixes that only unblock the camera feel bad: the cursor drifts to a screen edge and the
deltas die. Patching `MouseUsable()` itself keeps capture/recentring alive.

## 5. Camera path — `0x14064E320`, predicate `0x140653320`

```c
bool CameraShouldUseMouse() {          // 0x140653320
    if (!MouseUsable(&g_inputCtx)) return false;
    padY = GetAxis(&g_inputCtx, 2, -1);        // right stick
    padX = GetAxis2(&g_inputCtx, 2, -1);
    mdx  = MouseDeltaX();  mdy = MouseDeltaY();
    if (mdx*mdx + mdy*mdy <= 0 && padX*padX + padY*padY > 0) return false;  // stick wins only if mouse idle
    return true;
}
```

The camera update `0x14064E320` calls it once and then takes **one** of two branches:
`false` → right-stick path (`FUN_140652580`), `true` → mouse path (`MouseDeltaX/Y` at
`0x1403D2E40` / `0x1403D2E70`, negated and scaled).

This is the "mouse XOR right stick, decided per frame" behaviour we want, and it is already the
game's own design. Nothing else in the camera path consults `0x8C`.

The mouse delta getters `0x1403D2E40` / `0x1403D2E70` only check that a mouse is connected
(`hidInput->+0x2F`). They do **not** consult the active-device flag, so raw deltas keep flowing
while a pad is in use.

## 6. The fix

Patch the two `jne`s at `0x1403D3F61` and `0x1403D3F6A` to `nop nop` (4 bytes total). `MouseUsable()`
then ignores `usingGamepad` / the mouse-move latch, and the mouse behaves exactly as it does for a
keyboard-and-mouse player:

* camera: mouse if the mouse moved this frame, right stick otherwise — decided per frame;
* everything else on the pad (left stick, buttons, triggers) is untouched and works in the same frame;
* `0x8C` still tracks the last-used device, so button-prompt glyphs keep switching correctly;
* mouse capture/recentring/cursor hiding stay in their normal "mouse active" state.

Signature used to locate it at runtime (unique in `.text`):

```
80 ?? 8C 00 00 00 00 75 0D 80 ?? 8D 00 00 00 00 75 04 B0 01 EB 02 32 C0
```

### Narrower alternative

If touching `MouseUsable()` globally is not wanted, the camera predicate alone can be neutered by
NOPping the `test al,al` / `je` at `0x140653330` (8 bytes). That leaves menus untouched, but it does
**not** re-enable mouse capture/recentring while the pad is active, so the cursor is not recentred
and the deltas stop at the screen edge. Shipped as an opt-in (`CameraOnly`) for comparison only.

## 7. Interaction with NierReplicantFix

`NierReplicantFix`'s **Hide Cursor** option installs a mid-hook at `0x1403D4084` (the tail of
`MouseUsable()`) that forces the return value to 1 whenever `ctx->0x8C == 1`. That was written to
hide the hardware cursor while a pad is in use, but as a side effect it *partially* does what we
want. It is not equivalent to the patch here:

* it only covers `0x8C == 1`, not the `0x8D == 1` latch, so the "mouse move alone can't re-enable the
  mouse" trap can still be entered when Hide Cursor is off or when the state machine passes through
  `0x8D = 1`;
* it forces the value only at the very end of the function, after the in-client-rect test, so the
  behaviour depends on where the cursor happens to be at the moment the mode flips.

The two do not overlap in memory (`0x1403D3F5A` vs `0x1403D4084`), so this ASI and
`NierReplicantFix.asi` can be installed together in any load order.

## 8. Not done: keyboard/pad concurrency

Analogous switches exist for the axis and button readers, keyed on the same `0x8C`:

* `0x1403D36F0` `GetAxis` — at `0x1403D377C`, `if (ctx->0x8C == 0)` the pad axis value in `xmm6` is
  **replaced** by the keyboard value (`0x1403D37E9: movaps xmm6, xmm0`) and then optionally by the
  mouse axis. Removing the gate alone would zero the stick, so true concurrency needs
  `movaps xmm6, xmm0` → `addss xmm6, xmm0` (3 bytes → 4, so it needs a code cave).
* `0x1403D3840` — the same shape for the other axis component.
* `0x1403D3B00` — already *adds* the keyboard contribution (`fVar3 + fVar2`), gated on `0x8C == 0`;
  here removing the gate is a 2-byte NOP and is additive.

Left out for now: the camera requirement is met without it, and mixing keyboard movement into the
pad axes changes the glyph-switching feel more than it helps.

## 9. Tooling in this repo

Static analysis was done against a copy of the exe in `game/` (originals untouched):

* `tools/patscan.py` — IDA-style byte-pattern scan over `.text`, reports file offset + VA.
* `tools/disas.py` — capstone disassembly at a VA.
* `tools/disrange.py` — disassemble every `.pdata` function in a VA range.
* `tools/funcs.py` — `.pdata` function index / containing-function lookup.
* `tools/xref.py` — direct `E8`/`E9` and absolute-pointer xrefs (no Ghidra needed).
* `tools/xref_iat.py` — call sites of a given import thunk.
* `tools/rtti.py` — MSVC RTTI → vtable + vfunc list for a mangled class name.
* `tools/decomp.sh`, `tools/xrefs.sh` — Ghidra headless decompilation / xrefs
  (`scripts/ghidra/Decomp.java`, `scripts/ghidra/Xrefs.java`).
