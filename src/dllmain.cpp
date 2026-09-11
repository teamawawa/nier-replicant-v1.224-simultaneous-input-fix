// NierConcurrentInput.asi
//
// Lets keyboard/mouse and a controller be used at the same time in
// NieR Replicant ver.1.22474487139, and can pin the on-screen button prompts
// to controller glyphs.
//
// The game keeps a "which device is active" flag (input context +0x8C) that is
// forced to 1 for as long as any pad button is held or a stick is off-centre.
// MouseUsable() ANDs that flag in, and MouseUsable() gates both the camera's
// mouse branch and the mouse capture/recentre mode.  Removing that one gate is
// enough for the camera: it then picks mouse-vs-right-stick per frame on its
// own, and every other pad input keeps working in the same frame.
//
// The same flag gates the keyboard/mouse contribution inside the axis and
// button readers, and is read again by the UI when it picks between keyboard
// and controller glyphs.  Those are the other two patches here.
//
// See docs/FINDINGS.md for the full analysis.

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------- logging --

FILE* g_log = nullptr;

void Log(const char* fmt, ...)
{
    if (!g_log) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

std::wstring ModuleDir(HMODULE mod)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(mod, path, MAX_PATH);
    std::wstring s(path);
    size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : s.substr(0, slash);
}

uint8_t* g_exeBase = nullptr;

// Offset from the image base, for log lines that stay meaningful across runs.
unsigned long long Rva(const void* p)
{
    return static_cast<unsigned long long>(static_cast<const uint8_t*>(p) - g_exeBase);
}

// --------------------------------------------------------- pattern scanner --

// "80 ?? 8C 00 00 00" style patterns; '?'/'??' is a wildcard byte.
struct Pattern
{
    std::vector<uint8_t> bytes;
    std::vector<bool>    mask;   // true = must match

    explicit Pattern(const char* sig)
    {
        for (const char* p = sig; *p;) {
            if (*p == ' ') { ++p; continue; }
            if (*p == '?') {
                bytes.push_back(0);
                mask.push_back(false);
                while (*p == '?') ++p;
            } else {
                bytes.push_back(static_cast<uint8_t>(strtoul(p, nullptr, 16)));
                mask.push_back(true);
                while (*p && *p != ' ') ++p;
            }
        }
    }
};

struct Section
{
    uint8_t* base = nullptr;
    size_t   size = 0;
};

Section GetSection(HMODULE mod, const char* name)
{
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(
        reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);

    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (strncmp(reinterpret_cast<const char*>(sec->Name), name, IMAGE_SIZEOF_SHORT_NAME) == 0)
            return { reinterpret_cast<uint8_t*>(mod) + sec->VirtualAddress,
                     sec->Misc.VirtualSize };
    }
    return {};
}

std::vector<uint8_t*> FindAll(const Section& text, const char* sig, size_t stopAfter = 0)
{
    Pattern pat(sig);
    std::vector<uint8_t*> hits;
    if (pat.bytes.empty() || pat.bytes.size() > text.size) return hits;

    const size_t last = text.size - pat.bytes.size();
    for (size_t i = 0; i <= last; ++i) {
        uint8_t* candidate = text.base + i;
        bool ok = true;
        for (size_t j = 0; j < pat.bytes.size(); ++j) {
            if (pat.mask[j] && candidate[j] != pat.bytes[j]) { ok = false; break; }
        }
        if (!ok) continue;
        hits.push_back(candidate);
        if (stopAfter && hits.size() >= stopAfter) break;
    }
    return hits;
}

// Returns the single match, or nullptr if there is none or more than one.
// Ambiguity is treated as failure on purpose: these patches are surgical and a
// second match would mean the signature no longer identifies what we think.
uint8_t* FindUnique(const Section& text, const char* label, const char* sig)
{
    std::vector<uint8_t*> hits = FindAll(text, sig, 2);
    if (hits.empty()) { Log("[!] %s: pattern not found", label); return nullptr; }
    if (hits.size() > 1) {
        Log("[!] %s: pattern is ambiguous (+0x%llx and +0x%llx), skipping",
            label, Rva(hits[0]), Rva(hits[1]));
        return nullptr;
    }
    Log("[+] %s: found at +0x%llx", label, Rva(hits[0]));
    return hits[0];
}

bool WriteBytes(uint8_t* addr, const uint8_t* data, size_t len)
{
    DWORD old = 0;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(addr, data, len);
    VirtualProtect(addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, len);
    return true;
}

bool Nop(uint8_t* addr, size_t len)
{
    std::vector<uint8_t> nops(len, 0x90);
    return WriteBytes(addr, nops.data(), len);
}

// ------------------------------------------------------------------ config --

struct Config
{
    bool mouseAlwaysActive    = true;
    bool keyboardAlwaysActive = false;
    bool cameraOnly           = false;
    bool forceControllerPrompts = true;
    bool logging              = true;
};

Config g_config;

bool IniBool(const wchar_t* section, const wchar_t* key, bool fallback, const std::wstring& ini)
{
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(section, key, fallback ? L"true" : L"false", buf, 32, ini.c_str());
    return _wcsicmp(buf, L"true") == 0 || _wcsicmp(buf, L"1") == 0 || _wcsicmp(buf, L"yes") == 0;
}

// ----------------------------------------------------- mouse always active --

// MouseUsable() at 0x1403D3F20 (base 0x140000000):
//   cmp byte [rsi+0x8C], 0 / jne  -> the "a pad is the active device" gate
//   cmp byte [rsi+0x8D], 0 / jne  -> the "mouse moved while disabled" latch
//   mov al, 1 / jmp / xor al, al
// NOPping the two jne's makes the function ignore both and always take mov al,1.
constexpr const char* kSigMouseUsableGate =
    "80 ?? 8C 00 00 00 00 75 0D 80 ?? 8D 00 00 00 00 75 04 B0 01 EB 02 32 C0";

// CameraShouldUseMouse() at 0x140653320:
//   sub rsp,0x58 / lea rcx,[rip+g_inputCtx] / call MouseUsable / test al,al / je return_false
// NOPping test+je makes only the camera ignore MouseUsable() (menus untouched),
// but it does not restore mouse capture/recentring while a pad is active.
constexpr const char* kSigCameraPredicate =
    "48 83 EC 58 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? BA 02 00 00 00";

// -------------------------------------------------- keyboard always active --

// Every axis/button reader in the input module (0x1403D36F0 .. 0x1403D4580)
// guards its keyboard and mouse contribution with the same two instructions:
//
//     cmp byte [<ctx>+0x8C], 0        80 B? 8C 00 00 00 00
//     [0-1 unrelated instructions]
//     jne skip_keyboard_and_mouse     75 xx   |   0F 85 xx xx xx xx
//
// The guarded code always ORs (buttons) or ADDSs (axes) its result into what
// the pad already produced, with one exception handled separately below, so
// NOPping the jne turns "pad or keyboard" into "pad and keyboard".
//
// The mouse wheel getter spells the same test as `sete al` feeding a `test`
// against the "mouse enabled" byte; there the fix is `mov al, 1` instead.
//
// Rather than carry fourteen near-identical signatures, the gates are found by
// scanning a window around MouseUsable() — the whole module sits within ±0x1000
// of it, and no other `cmp byte [reg+0x8C], 0` does.
constexpr size_t kInputModuleWindow = 0x1000;

// The two stick-axis readers are the exception: they *replace* the pad value
// with the keyboard one instead of adding it.
//
//     lea rcx, [rip+axisDesc] / call KeyboardAxis / movaps xmm6, xmm0
//     mov rcx, rbx / call MouseUsable / test al, al
//
// `movaps xmm6, xmm0` (0F 28 F0) and `addps xmm6, xmm0` (0F 58 F0) are both
// three bytes, so the replace becomes an add without needing a code cave.
// Only the low lane is ever read back; the upper three hold leftovers from the
// pad-axis call either way.
constexpr const char* kSigAxisKeyboardMerge =
    "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F 28 F0 48 8B CB E8 ?? ?? ?? ?? 84 C0";
constexpr size_t kAxisMergeOpcodeOffset = 13;   // the 0x28 of `movaps xmm6, xmm0`

// --------------------------------------------- force controller prompts --

// The UI reads the active-device flag directly (rip-relative, absolute address)
// wherever it picks between keyboard and controller artwork or wording.  Each
// of those reads is redirected to a read-only byte that holds 1, which is what
// the flag looks like while a pad is active.
//
// Only the sites that choose a glyph or a string are redirected.  The flag is
// also read by menu cursor handling and by keyboard key-repeat; those keep
// seeing the real device so mouse and keyboard menu control still work.
struct PromptSite
{
    const char* name;
    const char* sig;
    uint32_t    dispOffset;   // offset of the rip disp32 within the match
    uint32_t    insnLength;   // total instruction length
    bool        multiple;     // signature legitimately matches more than one site
};

// `cmp byte [rip+disp32], 0`   = 80 3D <disp32> 00
// `movzx r32, byte [rip+disp32]` = 0F B6 <modrm> <disp32>
constexpr PromptSite kPromptSites[] = {
    // GetButtonIconId(): maps a button id to a glyph in the key-help icon font.
    // This is what almost every on-screen prompt ends up calling.
    { "button icon id",      "80 3D ?? ?? ?? ?? 00 0F B6 C3 0F 84",                              2, 7, false },
    // The inline <button> tag renderer used inside dialogue and tutorial text.
    { "inline text icon",    "80 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 49 8B 06 41 B8 5C 01 00 00", 2, 7, false },
    // "is this action on a pad face button" — picks pad vs keyboard artwork.
    { "face button test",    "80 3D ?? ?? ?? ?? 00 74 7D 48 8D 0D",                              2, 7, false },
    // The key-help bar item builder; the keyboard path builds a different item.
    { "key help item",       "80 3D ?? ?? ?? ?? 00 8B FA 48 8B D9 75 10 80 79 6A 00",            2, 7, false },
    // Picks message id 0xA96 (pad) or 0xACB (keyboard) for a key-help line.
    { "key help message",    "80 3D ?? ?? ?? ?? 00 B8 CB 0A 00 00 B9 96 0A 00 00 0F 45 C1",      2, 7, false },
    // The cutscene "SKIP" prompt, twice (update and rebuild paths).
    { "skip prompt",         "0F B6 3D ?? ?? ?? ?? 74 36 48 8B 85 58 05 00 00",                  3, 7, false },
    { "skip prompt rebuild", "0F B6 3D ?? ?? ?? ?? 74 31 48 8B 83 58 05 00 00",                  3, 7, false },
    // Tutorial pop-up: selects the pad or keyboard wording of the body text.
    { "tutorial text",       "0F B6 05 ?? ?? ?? ?? 48 8D 72 04 48 8B FA 48 8B D9 3A 81 C9 02 00 00", 3, 7, false },
    { "tutorial text rebuild", "0F B6 05 ?? ?? ?? ?? 48 8D 8B D8 02 00 00 88 83 C9 02 00 00",    3, 7, false },
    // Memo / white-book screens: same, plus their cached copies of the flag.
    { "memo text",           "0F B6 1D ?? ?? ?? ?? 88 5F 4D 48 8B CD",                           3, 7, false },
    { "memo text sibling",   "0F B6 05 ?? ?? ?? ?? 88 46 4D 44 39 AE 40 06 00 00",               3, 7, false },
    { "memo device cache",   "0F B6 05 ?? ?? ?? ?? 3A 41 4D 74 4E 88 41 4D",                     3, 7, true  },
    { "book text cache",     "0F B6 05 ?? ?? ?? ?? 88 85 35 22 00 00",                           3, 7, false },
    { "book text",           "0F B6 1D ?? ?? ?? ?? 49 8B CF E8",                                 3, 7, false },
    { "book text rebuild",   "0F B6 35 ?? ?? ?? ?? 40 3A B5 35 22 00 00",                        3, 7, false },
};

uint8_t* RipTarget(const uint8_t* insn, const PromptSite& site)
{
    int32_t disp = 0;
    memcpy(&disp, insn + site.dispOffset, 4);
    return const_cast<uint8_t*>(insn) + site.insnLength + disp;
}

bool RedirectRip(uint8_t* insn, const PromptSite& site, const uint8_t* target)
{
    intptr_t delta = target - (insn + site.insnLength);
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    int32_t disp = static_cast<int32_t>(delta);
    return WriteBytes(insn + site.dispOffset, reinterpret_cast<uint8_t*>(&disp), 4);
}

// A byte in the exe's read-only data that holds 1. `.rdata` is mapped
// PAGE_READONLY, so whatever we pick can never change under us.
const uint8_t* FindConstantOne(HMODULE exe)
{
    Section rdata = GetSection(exe, ".rdata");
    if (!rdata.base) return nullptr;
    for (size_t i = 0; i < rdata.size; ++i)
        if (rdata.base[i] == 1) return rdata.base + i;
    return nullptr;
}

// ------------------------------------------------------------------ patches --

// Applies the keyboard/mouse gate removal across the input module.  `anchor`
// points at MouseUsable()'s own gate, which is left alone (it is the Mouse
// Always Active patch's business) and used to bound the search.
void PatchInputGates(const Section& text, uint8_t* anchor)
{
    uint8_t* lo = anchor - kInputModuleWindow;
    uint8_t* hi = anchor + kInputModuleWindow;
    if (lo < text.base)             lo = text.base;
    if (hi > text.base + text.size) hi = text.base + text.size;

    int patched = 0, skipped = 0;

    for (uint8_t* p = lo; p + 16 < hi; ++p) {
        // cmp byte [reg+0x8C], 0, with no REX prefix in front of it.
        if (p[0] != 0x80 || (p[1] & 0xF8) != 0xB8) continue;
        if (p[2] != 0x8C || p[3] || p[4] || p[5] || p[6]) continue;
        if (p > text.base && (p[-1] & 0xF0) == 0x40) continue;
        if (p == anchor) continue;

        // Step over the one instruction that MSVC sometimes schedules between
        // the compare and the branch, then NOP the branch.
        uint8_t* q = p + 7;
        if (q[0] == 0x0F && q[1] == 0xB6)              q += 3;   // movzx r32, r8
        else if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x5C && q[3] == 0x24) q += 5;   // mov rbx, [rsp+d8]

        if (q[0] == 0x0F && q[1] == 0x94 && q[2] == 0xC0) {
            // sete al -> mov al, 1 (+ a nop for the third byte)
            const uint8_t movTrue[3] = { 0xB0, 0x01, 0x90 };
            if (WriteBytes(q, movTrue, 3)) ++patched; else ++skipped;
            continue;
        }

        size_t len = 0;
        if (q[0] == 0x75)                      len = 2;  // jne rel8
        else if (q[0] == 0x0F && q[1] == 0x85) len = 6;  // jne rel32

        if (!len) {
            Log("[!] input gate at +0x%llx: unrecognised shape, skipped", Rva(p));
            ++skipped;
            continue;
        }
        if (Nop(q, len)) ++patched; else ++skipped;
    }

    Log("[%c] Keyboard Always Active: %d device gate(s) removed, %d skipped",
        skipped ? '!' : '+', patched, skipped);

    // The two stick-axis readers overwrite the pad value with the keyboard one;
    // turn that into an add.
    std::vector<uint8_t*> merges = FindAll(text, kSigAxisKeyboardMerge);
    if (merges.size() != 2) {
        Log("[!] Keyboard Always Active: expected 2 axis merge sites, found %d; "
            "keyboard sticks will override the pad instead of adding to it",
            static_cast<int>(merges.size()));
        return;
    }
    for (uint8_t* m : merges) {
        uint8_t addps = 0x58;
        if (WriteBytes(m + kAxisMergeOpcodeOffset, &addps, 1))
            Log("[+] axis keyboard merge at +0x%llx: movaps -> addps", Rva(m));
        else
            Log("[!] axis keyboard merge at +0x%llx: write failed", Rva(m));
    }
}

void PatchPrompts(HMODULE exe, const Section& text)
{
    const uint8_t* one = FindConstantOne(exe);
    if (!one) {
        Log("[!] Force Controller Prompts: no constant 1 byte in .rdata, skipping");
        return;
    }
    Log("[i] Force Controller Prompts: reading the device flag as 1 from +0x%llx", Rva(one));

    // Collect first so a disagreeing site can be dropped before anything is
    // written: every site must reference the same flag byte.
    struct Found { const PromptSite* site; uint8_t* insn; };
    std::vector<Found> found;
    uint8_t* flag = nullptr;

    for (const PromptSite& site : kPromptSites) {
        std::vector<uint8_t*> hits = FindAll(text, site.sig, site.multiple ? 0 : 2);
        if (hits.empty()) { Log("[!] prompt site '%s': pattern not found", site.name); continue; }
        if (!site.multiple && hits.size() > 1) {
            Log("[!] prompt site '%s': pattern is ambiguous, skipping", site.name);
            continue;
        }
        for (uint8_t* h : hits) {
            uint8_t* target = RipTarget(h, site);
            if (!flag) flag = target;
            if (target != flag) {
                Log("[!] prompt site '%s' at +0x%llx: reads +0x%llx, expected +0x%llx, skipping",
                    site.name, Rva(h), Rva(target), Rva(flag));
                continue;
            }
            found.push_back({ &site, h });
        }
    }

    if (found.empty()) {
        Log("[!] Force Controller Prompts: nothing to patch");
        return;
    }
    Log("[i] Force Controller Prompts: device flag is +0x%llx", Rva(flag));

    int patched = 0;
    for (const Found& f : found) {
        if (RedirectRip(f.insn, *f.site, one)) {
            ++patched;
        } else {
            Log("[!] prompt site '%s' at +0x%llx: write failed", f.site->name, Rva(f.insn));
        }
    }
    Log("[+] Force Controller Prompts: %d of %d read(s) redirected",
        patched, static_cast<int>(found.size()));
}

void ApplyPatches()
{
    HMODULE exe = GetModuleHandleW(nullptr);
    g_exeBase = reinterpret_cast<uint8_t*>(exe);
    Section text = GetSection(exe, ".text");
    if (!text.base) { Log("[!] could not locate .text"); return; }

    Log("[i] exe module %p, .text %p size 0x%llx", exe, text.base,
        static_cast<unsigned long long>(text.size));

    uint8_t* gate = nullptr;
    if (g_config.mouseAlwaysActive || g_config.keyboardAlwaysActive)
        gate = FindUnique(text, "MouseUsable device-mode gate", kSigMouseUsableGate);

    if (g_config.mouseAlwaysActive) {
        if (gate) {
            // +0x07: jne over the 0x8C test, +0x10: jne over the 0x8D test
            bool ok = Nop(gate + 0x07, 2) && Nop(gate + 0x10, 2);
            Log(ok ? "[+] Mouse Always Active: patched"
                   : "[!] Mouse Always Active: write failed");
        }
    } else {
        Log("[i] Mouse Always Active: disabled by config");
    }

    if (g_config.keyboardAlwaysActive) {
        if (gate) PatchInputGates(text, gate);
    } else {
        Log("[i] Keyboard Always Active: disabled by config");
    }

    if (g_config.forceControllerPrompts) {
        PatchPrompts(exe, text);
    } else {
        Log("[i] Force Controller Prompts: disabled by config");
    }

    if (g_config.cameraOnly) {
        uint8_t* pred = FindUnique(text, "Camera mouse predicate", kSigCameraPredicate);
        if (pred) {
            // +0x10: test al,al (2 bytes) + je rel32 (6 bytes)
            bool ok = Nop(pred + 0x10, 8);
            Log(ok ? "[+] Camera Only: patched"
                   : "[!] Camera Only: write failed");
        }
    }
}

DWORD WINAPI Main(LPVOID module)
{
    std::wstring dir = ModuleDir(static_cast<HMODULE>(module));
    std::wstring ini = dir + L"\\NierConcurrentInput.ini";

    g_config.mouseAlwaysActive    = IniBool(L"Concurrent Input", L"MouseAlwaysActive",    true,  ini);
    g_config.keyboardAlwaysActive = IniBool(L"Concurrent Input", L"KeyboardAlwaysActive", false, ini);
    g_config.cameraOnly           = IniBool(L"Concurrent Input", L"CameraOnly",           false, ini);
    g_config.forceControllerPrompts = IniBool(L"Prompts",        L"ForceControllerPrompts", true, ini);
    g_config.logging              = IniBool(L"Debug",            L"Logging",              true,  ini);

    if (g_config.logging) {
        std::wstring logPath = dir + L"\\NierConcurrentInput.log";
        g_log = _wfopen(logPath.c_str(), L"w");
    }

    Log("NierConcurrentInput " __DATE__ " " __TIME__);
    Log("[i] config: MouseAlwaysActive=%d KeyboardAlwaysActive=%d ForceControllerPrompts=%d CameraOnly=%d",
        g_config.mouseAlwaysActive, g_config.keyboardAlwaysActive,
        g_config.forceControllerPrompts, g_config.cameraOnly);
    Log("[i] ini: %ls", ini.c_str());

    ApplyPatches();

    if (g_log) { fclose(g_log); g_log = nullptr; }
    return 0;
}

} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        if (HANDLE t = CreateThread(nullptr, 0, Main, inst, 0, nullptr))
            CloseHandle(t);
    }
    return TRUE;
}
