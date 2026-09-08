// Smoke-test host for NierConcurrentInput.asi.
//
// Embeds the exact byte sequences the plugin looks for into this exe's .text,
// loads the plugin, and verifies the expected bytes were patched. Run under
// wine: wine build/test_host.exe
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

// The MouseUsable() device-mode gate, verbatim from the game's .text at 0x1403D3F5A.
__attribute__((section(".text"), used, aligned(16)))
const unsigned char kGate[] = {
    0x80,0xBE,0x8C,0x00,0x00,0x00,0x00,  // cmp byte [rsi+0x8C], 0
    0x75,0x0D,                           // jne +0x0D
    0x80,0xBE,0x8D,0x00,0x00,0x00,0x00,  // cmp byte [rsi+0x8D], 0
    0x75,0x04,                           // jne +0x04
    0xB0,0x01,                           // mov al, 1
    0xEB,0x02,                           // jmp +2
    0x32,0xC0,                           // xor al, al
};

// The camera predicate prologue, verbatim from 0x140653320.
__attribute__((section(".text"), used, aligned(16)))
const unsigned char kCam[] = {
    0x48,0x83,0xEC,0x58,                     // sub rsp, 0x58
    0x48,0x8D,0x0D,0xD5,0xB0,0xDE,0x03,      // lea rcx, [rip+...]
    0xE8,0xF0,0x0B,0xD8,0xFF,                // call MouseUsable
    0x84,0xC0,                               // test al, al
    0x0F,0x84,0xB7,0x00,0x00,0x00,           // je  +0xB7
    0xBA,0x02,0x00,0x00,0x00,                // mov edx, 2
};

static int check(const char* name, const unsigned char* p, size_t off, size_t len,
                 unsigned char want)
{
    for (size_t i = 0; i < len; ++i) {
        if (p[off + i] != want) {
            printf("FAIL %s: byte +0x%zx is 0x%02X, expected 0x%02X\n",
                   name, off + i, p[off + i], want);
            return 1;
        }
    }
    printf("ok   %s: %zu byte(s) at +0x%zx == 0x%02X\n", name, len, off, want);
    return 0;
}

int main()
{
    printf("gate at %p, cam at %p\n", (void*)kGate, (void*)kCam);

    HMODULE m = LoadLibraryW(L"NierConcurrentInput.asi");
    if (!m) { printf("FAIL: LoadLibrary failed, err=%lu\n", GetLastError()); return 2; }
    Sleep(1500);  // the plugin patches from a worker thread

    int bad = 0;
    bad += check("gate jne #1 nopped", kGate, 0x07, 2, 0x90);
    bad += check("gate jne #2 nopped", kGate, 0x10, 2, 0x90);
    bad += check("gate cmp untouched", kGate, 0x00, 1, 0x80);
    bad += check("camera predicate untouched (CameraOnly=false)", kCam, 0x10, 1, 0x84);

    printf(bad ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", bad);
    return bad ? 1 : 0;
}
