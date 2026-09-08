// NierConcurrentInput.asi
//
// Lets keyboard/mouse and a controller be used at the same time in
// NieR Replicant ver.1.22474487139.
//
// The game keeps a "which device is active" flag (input context +0x8C) that is
// forced to 1 for as long as any pad button is held or a stick is off-centre.
// MouseUsable() ANDs that flag in, and MouseUsable() gates both the camera's
// mouse branch and the mouse capture/recentre mode.  Removing that one gate is
// enough: the camera then picks mouse-vs-right-stick per frame on its own, and
// every other pad input keeps working in the same frame.
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

struct TextSection
{
    uint8_t* base = nullptr;
    size_t   size = 0;
};

TextSection GetTextSection(HMODULE mod)
{
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(
        reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);

    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5) == 0)
            return { reinterpret_cast<uint8_t*>(mod) + sec->VirtualAddress,
                     sec->Misc.VirtualSize };
    }
    return {};
}

// Returns the single match, or nullptr if there is none or more than one.
// Ambiguity is treated as failure on purpose: these patches are surgical and a
// second match would mean the signature no longer identifies what we think.
uint8_t* FindUnique(const TextSection& text, const char* label, const char* sig)
{
    Pattern pat(sig);
    if (pat.bytes.empty() || pat.bytes.size() > text.size) return nullptr;

    uint8_t* hit = nullptr;
    const size_t last = text.size - pat.bytes.size();

    for (size_t i = 0; i <= last; ++i) {
        uint8_t* candidate = text.base + i;
        bool ok = true;
        for (size_t j = 0; j < pat.bytes.size(); ++j) {
            if (pat.mask[j] && candidate[j] != pat.bytes[j]) { ok = false; break; }
        }
        if (!ok) continue;
        if (hit) {
            Log("[!] %s: pattern is ambiguous (%p and %p), skipping", label, hit, candidate);
            return nullptr;
        }
        hit = candidate;
    }

    if (!hit) Log("[!] %s: pattern not found", label);
    else      Log("[+] %s: found at %p (+0x%llx)", label, hit,
                  static_cast<unsigned long long>(hit - reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr))));
    return hit;
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
    bool mouseAlwaysActive = true;
    bool cameraOnly        = false;
    bool logging           = true;
};

Config g_config;

bool IniBool(const wchar_t* section, const wchar_t* key, bool fallback, const std::wstring& ini)
{
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(section, key, fallback ? L"true" : L"false", buf, 32, ini.c_str());
    return _wcsicmp(buf, L"true") == 0 || _wcsicmp(buf, L"1") == 0 || _wcsicmp(buf, L"yes") == 0;
}

// ------------------------------------------------------------------ patches --

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

void ApplyPatches()
{
    HMODULE exe = GetModuleHandleW(nullptr);
    TextSection text = GetTextSection(exe);
    if (!text.base) { Log("[!] could not locate .text"); return; }

    Log("[i] exe module %p, .text %p size 0x%llx", exe, text.base,
        static_cast<unsigned long long>(text.size));

    if (g_config.mouseAlwaysActive) {
        uint8_t* gate = FindUnique(text, "MouseUsable device-mode gate", kSigMouseUsableGate);
        if (gate) {
            // +0x07: jne over the 0x8C test, +0x10: jne over the 0x8D test
            bool ok = Nop(gate + 0x07, 2) && Nop(gate + 0x10, 2);
            Log(ok ? "[+] Mouse Always Active: patched"
                   : "[!] Mouse Always Active: write failed");
        }
    } else {
        Log("[i] Mouse Always Active: disabled by config");
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

    g_config.mouseAlwaysActive = IniBool(L"Concurrent Input", L"MouseAlwaysActive", true,  ini);
    g_config.cameraOnly        = IniBool(L"Concurrent Input", L"CameraOnly",        false, ini);
    g_config.logging           = IniBool(L"Debug",            L"Logging",           true,  ini);

    if (g_config.logging) {
        std::wstring logPath = dir + L"\\NierConcurrentInput.log";
        g_log = _wfopen(logPath.c_str(), L"w");
    }

    Log("NierConcurrentInput " __DATE__ " " __TIME__);
    Log("[i] config: MouseAlwaysActive=%d CameraOnly=%d",
        g_config.mouseAlwaysActive, g_config.cameraOnly);
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
