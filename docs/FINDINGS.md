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

`0x14443E48C` is read from 19 places inside the input module (through the context pointer) and
from 34 more across the rest of the game (rip-relative against the absolute address), where it
drives button-prompt glyphs, menu cursor handling and prompt wording. Anything that changes the
byte itself changes all of them at once, so none of the patches here do — see §8 and §9 for how
the two groups are separated.

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

## 8. Keyboard/pad concurrency (`KeyboardAlwaysActive`)

The axis and button readers in the input module all gate their keyboard and mouse-button
contribution on the same `0x8C`, and they all spell it the same way:

```
cmp byte [<ctx>+0x8C], 0      80 B? 8C 00 00 00 00
<0 or 1 unrelated instruction>
jne skip_keyboard_and_mouse   75 xx   |   0F 85 xx xx xx xx
```

The guarded code ORs (buttons) or `addss`es (axes) its result into what the pad already produced,
so NOPping the `jne` turns "pad *or* keyboard" into "pad *and* keyboard". Eighteen of those gates
sit between `0x1403D377C` and `0x1403D45ED`; nothing else within ±0x1000 of `MouseUsable()`
matches the byte pattern, which is how the plugin finds them without carrying eighteen
signatures. Note that several are leaf functions with no `.pdata` entry, so a `.pdata`-driven
sweep misses them — scanning raw bytes does not.

Two of them need more than the NOP:

* **The two stick-axis readers** (`0x1403D36F0` and `0x1403D3840`) *replace* the pad value in
  `xmm6` with the keyboard one at `0x1403D37E9` / `0x1403D3939` instead of adding it, so removing
  the gate alone would let a keyboard reading of zero cancel the stick. `movaps xmm6, xmm0`
  (`0F 28 F0`) and `addps xmm6, xmm0` (`0F 58 F0`) are both three bytes, so this is a one-byte
  opcode swap rather than the code cave an `addss` would need. Only the low lane is ever read
  back; the upper three carry leftovers from the pad-axis call either way.
* **The mouse wheel getter** `0x1403D3990` has no branch at all: it writes the test as `sete al`
  feeding a `test` against the "mouse enabled" byte at `+0x8E`. There the fix is
  `sete al` → `mov al, 1` + `nop`.

Where a pad axis and a keyboard axis are pushed at once the two add, so `GetAxis()` can return ±2
instead of ±1. Nothing downstream goes faster for it — both consumers saturate:

* **movement** (`0x1406BCB40`): each component is divided by `0.9`, clamped per-component to
  `[-1, +1]`, and the resulting vector is renormalised if its length exceeds 1
  (`0x1406BCFAB`..`0x1406BD03D`);
* **camera** (`FUN_140648210`, the response curve behind `0x140652580`): anything past the outer
  threshold returns exactly `±1.0`.

What is lost is analog resolution. With a keyboard direction held, the stick is already saturated
at any deflection, so a gentle push that would have been a walk becomes a full run. That, plus the
fact that §6 does not need it, is why it is off by default.

## 9. Pinning the button glyphs (`ForceGlyphs`)

`0x14443E48C` is read from 34 places outside the input module, always rip-relative against the
absolute address (the input module reaches the same byte through the context pointer instead).
Those 34 split into two groups:

* code that picks **artwork or wording** — which is what the option is for;
* code that drives **input behaviour**: the mouse hit-test over menu widgets at `0x1404C16A8`
  (`FUN_140082780`, which tests the cursor position against the widget list), keyboard key-repeat
  in `FUN_1403A5B60` / `FUN_1403A6600` / `FUN_1403A7360`, the menu click path in `FUN_1404CDF00`,
  and the rumble gate in `FUN_1400760B0` / `FUN_140076120` (`FUN_140078390` drives a vibration
  envelope).

The split does not follow function boundaries. `FUN_1404C12E0` contains both: one read feeds the
mouse hit-test, and four more switch the system/save menu's key-help bar between its keyboard and
controller presentations. Classifying per function rather than per read is what left the save menu
switching after the first pass — those four, plus two more of the same shape in `FUN_1404C1B10`,
have to be pinned individually.

Forcing the flag itself — or blanket-redirecting all 34 reads — would take the second group with
it and break mouse control of menus, which is exactly what `MouseAlwaysActive` exists to enable.
So only the first group is redirected, twenty-two reads in all:

| Site | Function | What it picks |
| --- | --- | --- |
| `0x14008EC42` | `0x14008EC00` | button id → glyph in the key-help icon font. Nearly every prompt funnels through here |
| `0x14008FDFC` | `0x14008F9A0` | the inline `<button>` tag renderer used inside dialogue and tutorial text |
| `0x140085699` | `0x140085680` | "is this action on a pad face button", pad artwork vs keyboard character |
| `0x1400BF274` | `0x1400BF250` | key-help bar item builder; the keyboard path builds a different item |
| `0x140495566` | `0x140495180` | message id `0xA96` (pad) or `0xACB` (keyboard) |
| `0x1400CE7AF`, `0x1400CE995` | → `0x140451310` | the cutscene SKIP prompt |
| `0x14046A2F9`, `0x14046A6BB` | → `0x14046CE20` | tutorial pop-up body text variant |
| `0x1404D576C`, `0x1404A6A6A`, `0x1404A7083`, `0x1404D6743` | → `0x1404DE3B0` | memo screen text variant and its cached copies of the flag |
| `0x140509AF7`, `0x140509B81`, `0x140509C21` | → `0x1405336A0` | white-book memo text variant and its cache |
| `0x1404C193D`, `0x1404C199E`, `0x1404C19E5`, `0x1404C1A08` | `0x1404C12E0` | system/save menu key-help bar: shows one of two whole presentations rather than re-picking glyphs, plus its cached copy of the flag |
| `0x1404C2016`, `0x1404C2034` | `0x1404C1B10` | the same switch on the menu's init path |

Each is `cmp byte [rip+disp32], 0` (`80 3D … 00`) or `movzx r32, byte [rip+disp32]`
(`0F B6 /r …`). Rather than rewrite the instructions, the patch rewrites the four displacement
bytes so they point at a byte in `.rdata` holding the device id to pin to — `1` for controller,
`0` for keyboard, exactly the values the flag itself carries. `.rdata` is mapped `PAGE_READONLY`,
so whatever byte is picked cannot change under the game. Semantics are unchanged, instruction
lengths are unchanged, and the sites that were left alone keep reading the real flag.

The cached-copy sites matter: several UI classes compare the flag against a cached copy to decide
when to rebuild a widget. Redirecting the read without redirecting the cache write in the same
class would make the comparison fail every frame and rebuild the widget forever, so within each
class it is all or nothing.

Before writing anything the patch checks that every site it found resolves to the *same* address;
a site that disagrees is dropped rather than patched.

## 10. Tooling in this repo

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
