// Smoke-test host for NierConcurrentInput.asi.
//
// Embeds the exact byte sequences the plugin looks for into this exe's .text,
// loads the plugin, and verifies the expected bytes were patched. Run under
// wine: wine build/test_host.exe
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

#define TEXT __attribute__((section(".text"), used, aligned(16)))

// ---------------------------------------------------------------------------
// Input module. The plugin finds MouseUsable()'s own device gate by signature
// and then sweeps ±0x1000 around it for the per-reader gates, so everything
// here lives in one array to keep the distances fixed.
//
// Offset 0 is the MouseUsable() gate, verbatim from the game's .text at
// 0x1403D3F5A. The rest are the reader gates, one of each shape the game uses.
// ---------------------------------------------------------------------------
TEXT unsigned char kInput[] = {
    // +0x00  MouseUsable() device-mode gate (the anchor; not a reader gate)
    0x80,0xBE,0x8C,0x00,0x00,0x00,0x00,  // cmp byte [rsi+0x8C], 0
    0x75,0x0D,                           // jne +0x0D
    0x80,0xBE,0x8D,0x00,0x00,0x00,0x00,  // cmp byte [rsi+0x8D], 0
    0x75,0x04,                           // jne +0x04
    0xB0,0x01,                           // mov al, 1
    0xEB,0x02,                           // jmp +2
    0x32,0xC0,                           // xor al, al

    // +0x18  axis reader: jne rel32, nothing in between
    0x80,0xBB,0x8C,0x00,0x00,0x00,0x00,  // cmp byte [rbx+0x8C], 0
    0x0F,0x85,0x90,0x00,0x00,0x00,       // jne rel32

    // +0x25  axis reader: `mov rbx, [rsp+0x40]` scheduled before the branch
    0x80,0xBB,0x8C,0x00,0x00,0x00,0x00,
    0x48,0x8B,0x5C,0x24,0x40,
    0x75,0x15,                           // jne rel8

    // +0x33  button reader: `movzx esi, al` scheduled before the branch
    0x80,0xBB,0x8C,0x00,0x00,0x00,0x00,
    0x0F,0xB6,0xF0,
    0x75,0x22,

    // +0x3F  button reader: jne rel8, nothing in between
    0x80,0xBF,0x8C,0x00,0x00,0x00,0x00,
    0x75,0x19,

    // +0x48  leaf "keyboard only" getter
    0x80,0xB9,0x8C,0x00,0x00,0x00,0x00,
    0x75,0x0C,

    // +0x51  mouse wheel getter: sete al instead of a branch
    0x80,0xB9,0x8C,0x00,0x00,0x00,0x00,
    0x0F,0x94,0xC0,                      // sete al
    0x84,0x05,0x00,0x00,0x00,0x00,       // test byte [rip+...], al
};
constexpr size_t kReaderGates = 6;
constexpr size_t kGateOffsets[kReaderGates] = { 0x18, 0x25, 0x33, 0x3F, 0x48, 0x51 };

// The stick-axis keyboard merge, verbatim from 0x1403D37DD and 0x1403D392D.
// The plugin insists on finding exactly two, so both are here.
TEXT unsigned char kAxisMerge[2][25] = {
  { 0x48,0x8D,0x0D,0xB0,0xAB,0x06,0x04,  // lea rcx, [rip+axisDesc]
    0xE8,0xC7,0xE9,0xFF,0xFF,            // call KeyboardAxis
    0x0F,0x28,0xF0,                      // movaps xmm6, xmm0
    0x48,0x8B,0xCB,                      // mov rcx, rbx
    0xE8,0x2C,0x07,0x00,0x00,            // call MouseUsable
    0x84,0xC0 },                         // test al, al
  { 0x48,0x8D,0x0D,0x60,0xAA,0x06,0x04,
    0xE8,0x57,0xE9,0xFF,0xFF,
    0x0F,0x28,0xF0,
    0x48,0x8B,0xCB,
    0xE8,0xDC,0x05,0x00,0x00,
    0x84,0xC0 },
};

// The camera predicate prologue, verbatim from 0x140653320.
TEXT unsigned char kCam[] = {
    0x48,0x83,0xEC,0x58,                     // sub rsp, 0x58
    0x48,0x8D,0x0D,0xD5,0xB0,0xDE,0x03,      // lea rcx, [rip+...]
    0xE8,0xF0,0x0B,0xD8,0xFF,                // call MouseUsable
    0x84,0xC0,                               // test al, al
    0x0F,0x84,0xB7,0x00,0x00,0x00,           // je  +0xB7
    0xBA,0x02,0x00,0x00,0x00,                // mov edx, 2
};

// ---------------------------------------------------------------------------
// Prompt sites. Every one of these reads the active-device flag rip-relative;
// the four displacement bytes are filled in at startup so they all point at
// g_deviceFlag, and the plugin is expected to repoint them at a byte holding 1.
// ---------------------------------------------------------------------------
volatile unsigned char g_deviceFlag = 0;

#define PROMPT(name, disp, ...) TEXT unsigned char name[] = { __VA_ARGS__ }; \
                                constexpr size_t name##_disp = disp;

PROMPT(kIconId, 2,          // cmp byte [rip], 0 / movzx eax, bl / je
    0x80,0x3D,0,0,0,0,0x00, 0x0F,0xB6,0xC3, 0x0F,0x84,0xAC,0x00,0x00,0x00)
PROMPT(kInlineIcon, 2,
    0x80,0x3D,0,0,0,0,0x00, 0x0F,0x85,0x4E,0xFD,0xFF,0xFF,
    0x49,0x8B,0x06, 0x41,0xB8,0x5C,0x01,0x00,0x00)
PROMPT(kFaceButton, 2,
    0x80,0x3D,0,0,0,0,0x00, 0x74,0x7D, 0x48,0x8D,0x0D,0x57,0x8D,0x3B,0x04)
PROMPT(kKeyHelpItem, 2,
    0x80,0x3D,0,0,0,0,0x00, 0x8B,0xFA, 0x48,0x8B,0xD9, 0x75,0x10,
    0x80,0x79,0x6A,0x00)
PROMPT(kKeyHelpMsg, 2,
    0x80,0x3D,0,0,0,0,0x00, 0xB8,0xCB,0x0A,0x00,0x00, 0xB9,0x96,0x0A,0x00,0x00,
    0x0F,0x45,0xC1)
PROMPT(kSkip, 3,            // movzx edi, byte [rip]
    0x0F,0xB6,0x3D,0,0,0,0, 0x74,0x36, 0x48,0x8B,0x85,0x58,0x05,0x00,0x00)
PROMPT(kSkipRebuild, 3,
    0x0F,0xB6,0x3D,0,0,0,0, 0x74,0x31, 0x48,0x8B,0x83,0x58,0x05,0x00,0x00)
PROMPT(kTutorial, 3,
    0x0F,0xB6,0x05,0,0,0,0, 0x48,0x8D,0x72,0x04, 0x48,0x8B,0xFA, 0x48,0x8B,0xD9,
    0x3A,0x81,0xC9,0x02,0x00,0x00)
PROMPT(kTutorialRebuild, 3,
    0x0F,0xB6,0x05,0,0,0,0, 0x48,0x8D,0x8B,0xD8,0x02,0x00,0x00,
    0x88,0x83,0xC9,0x02,0x00,0x00)
PROMPT(kMemo, 3,
    0x0F,0xB6,0x1D,0,0,0,0, 0x88,0x5F,0x4D, 0x48,0x8B,0xCD)
PROMPT(kMemoSibling, 3,
    0x0F,0xB6,0x05,0,0,0,0, 0x88,0x46,0x4D, 0x44,0x39,0xAE,0x40,0x06,0x00,0x00)
// This signature legitimately matches two sites in the game; both must land.
PROMPT(kMemoCacheA, 3,
    0x0F,0xB6,0x05,0,0,0,0, 0x3A,0x41,0x4D, 0x74,0x4E, 0x88,0x41,0x4D)
PROMPT(kMemoCacheB, 3,
    0x0F,0xB6,0x05,0,0,0,0, 0x3A,0x41,0x4D, 0x74,0x4E, 0x88,0x41,0x4D)
PROMPT(kBookCache, 3,
    0x0F,0xB6,0x05,0,0,0,0, 0x88,0x85,0x35,0x22,0x00,0x00)
PROMPT(kBookText, 3,
    0x0F,0xB6,0x1D,0,0,0,0, 0x49,0x8B,0xCF, 0xE8,0xC0,0xBE,0xF6,0xFF)
PROMPT(kBookRebuild, 3,
    0x0F,0xB6,0x35,0,0,0,0, 0x40,0x3A,0xB5,0x35,0x22,0x00,0x00)

struct PromptRef { const char* name; unsigned char* insn; size_t disp; };
static PromptRef g_prompts[] = {
    { "button icon id",        kIconId,          kIconId_disp },
    { "inline text icon",      kInlineIcon,      kInlineIcon_disp },
    { "face button test",      kFaceButton,      kFaceButton_disp },
    { "key help item",         kKeyHelpItem,     kKeyHelpItem_disp },
    { "key help message",      kKeyHelpMsg,      kKeyHelpMsg_disp },
    { "skip prompt",           kSkip,            kSkip_disp },
    { "skip prompt rebuild",   kSkipRebuild,     kSkipRebuild_disp },
    { "tutorial text",         kTutorial,        kTutorial_disp },
    { "tutorial text rebuild", kTutorialRebuild, kTutorialRebuild_disp },
    { "memo text",             kMemo,            kMemo_disp },
    { "memo text sibling",     kMemoSibling,     kMemoSibling_disp },
    { "memo device cache #1",  kMemoCacheA,      kMemoCacheA_disp },
    { "memo device cache #2",  kMemoCacheB,      kMemoCacheB_disp },
    { "book text cache",       kBookCache,       kBookCache_disp },
    { "book text",             kBookText,        kBookText_disp },
    { "book text rebuild",     kBookRebuild,     kBookRebuild_disp },
};
constexpr size_t kPromptInsnLen = 7;

static int g_failures = 0;

static void unprotect(void* p, size_t n)
{
    DWORD old = 0;
    VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
}

static unsigned char* ripTarget(const PromptRef& r)
{
    int32_t disp = 0;
    memcpy(&disp, r.insn + r.disp, 4);
    return r.insn + kPromptInsnLen + disp;
}

static void check(bool ok, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    printf(ok ? "ok   " : "FAIL ");
    vprintf(fmt, args);
    va_end(args);
    putchar('\n');
    if (!ok) ++g_failures;
}

static bool allBytes(const unsigned char* p, size_t n, unsigned char want)
{
    for (size_t i = 0; i < n; ++i) if (p[i] != want) return false;
    return true;
}

int main()
{
    unprotect(kInput, sizeof kInput);
    unprotect(kAxisMerge, sizeof kAxisMerge);
    unprotect(kCam, sizeof kCam);
    for (PromptRef& r : g_prompts) {
        unprotect(r.insn, 32);
        int32_t disp = static_cast<int32_t>(
            reinterpret_cast<intptr_t>(&g_deviceFlag) -
            reinterpret_cast<intptr_t>(r.insn + kPromptInsnLen));
        memcpy(r.insn + r.disp, &disp, 4);
    }

    printf("input block at %p, flag at %p\n", (void*)kInput, (void*)&g_deviceFlag);

    HMODULE m = LoadLibraryW(L"NierConcurrentInput.asi");
    if (!m) { printf("FAIL: LoadLibrary failed, err=%lu\n", GetLastError()); return 2; }
    Sleep(1500);  // the plugin patches from a worker thread

    // Mouse Always Active: both jne's inside the MouseUsable gate are gone,
    // the compares themselves are untouched.
    check(allBytes(kInput + 0x07, 2, 0x90), "MouseUsable jne #1 nopped");
    check(allBytes(kInput + 0x10, 2, 0x90), "MouseUsable jne #2 nopped");
    check(kInput[0x00] == 0x80 && kInput[0x09] == 0x80, "MouseUsable compares untouched");

    // Keyboard Always Active: every reader gate lost its branch, and the two
    // axis merges turned from movaps into addps.
    static const size_t kBranchAt[kReaderGates] = { 7, 12, 10, 7, 7, 7 };
    static const size_t kBranchLen[kReaderGates] = { 6, 2, 2, 2, 2, 3 };
    for (size_t i = 0; i < kReaderGates; ++i) {
        const unsigned char* g = kInput + kGateOffsets[i] + kBranchAt[i];
        bool ok = (i == kReaderGates - 1)
                    ? (g[0] == 0xB0 && g[1] == 0x01 && g[2] == 0x90)   // sete al -> mov al, 1
                    : allBytes(g, kBranchLen[i], 0x90);
        check(ok, "reader gate %zu at +0x%zx neutralised", i, kGateOffsets[i]);
        check(kInput[kGateOffsets[i]] == 0x80, "reader gate %zu compare untouched", i);
    }
    for (size_t i = 0; i < 2; ++i)
        check(kAxisMerge[i][13] == 0x58, "axis merge %zu is addps", i);

    // Force Controller Prompts: every site now reads a byte that holds 1, and
    // no longer the flag this host owns.
    for (const PromptRef& r : g_prompts) {
        const unsigned char* t = ripTarget(r);
        check(t != &g_deviceFlag && *t == 1, "prompt site '%s' reads 1 (at %p)", r.name, (void*)t);
    }

    // CameraOnly is off in the test ini, so the predicate must be untouched.
    check(kCam[0x10] == 0x84, "camera predicate untouched (CameraOnly=false)");

    printf(g_failures ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
