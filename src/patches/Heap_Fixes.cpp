#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <detours.h>
#include <cstdio>
#include "../s4_base.h"
#pragma comment(lib, "detours.lib")

static const DWORD CPP_EXC = 0xE06D7363;
static volatile LONG g_caught = 0;

static HANDLE g_known[128]; static int g_seen = 0;
static bool AlreadyKnown(HANDLE h) {
    for (int i = 0; i < g_seen; i++) if (g_known[i] == h) return true;
    if (g_seen < 128) g_known[g_seen++] = h;
    return false;
}
static void SweepHeapsLFH(bool first) {
    DWORD n = GetProcessHeaps(0, NULL); if (!n) return;
    HANDLE* heaps = (HANDLE*)HeapAlloc(GetProcessHeap(), 0, n * sizeof(HANDLE));
    if (!heaps) return;
    DWORD got = GetProcessHeaps(n, heaps);
    for (DWORD i = 0; i < got; i++) {
        AlreadyKnown(heaps[i]);
        ULONG info = 0; SIZE_T ret = 0;
        ULONG before = HeapQueryInformation(heaps[i], HeapCompatibilityInformation, &info, sizeof(info), &ret) ? info : (ULONG)-1;
        if (before != 2) { ULONG lfh = 2; HeapSetInformation(heaps[i], HeapCompatibilityInformation, &lfh, sizeof(lfh)); }
    }
    HeapFree(GetProcessHeap(), 0, heaps);
    (void)first;
}
static DWORD WINAPI LfhThread(LPVOID) {
    // Do NOT sweep during startup. GetProcessHeaps() takes a snapshot and we then call
    // HeapSetInformation on each handle; while the client is still creating and destroying
    // heaps, one can die in between and ntdll walks a freed heap structure. That faults
    // reading an address in ntdll's own range (0x77xxxxxx) a few milliseconds into the
    // process. Because it is a RACE it depends on the machine, not the build: same client,
    // same base, same DLL, and it only crashes where startup loses the race.
    Sleep(5000);
    for (;;) { SweepHeapsLFH(false); Sleep(5000); }
}

typedef void* (__fastcall* tA)(void*, void*, int);
#define HOOK_A(NAME, ADDR)                                                            \
  static tA o_##NAME = (tA)(ADDR);                                                    \
  static void* __fastcall h_##NAME(void* ecx, void* edx, int a1) {                    \
    __try { return o_##NAME(ecx, edx, a1); }                                          \
    __except (GetExceptionCode()==CPP_EXC ? EXCEPTION_EXECUTE_HANDLER                 \
                                          : EXCEPTION_CONTINUE_SEARCH) {              \
      InterlockedIncrement(&g_caught);                                                \
      return 0;                                                                       \
    }                                                                                 \
  }
HOOK_A(Alloc,      S4(0x01AE4910))
HOOK_A(f00f391d0,  S4(0x00F391D0))
HOOK_A(f014cb7d0,  S4(0x014CB7D0))
HOOK_A(f01a84f10,  S4(0x01A84F10))
HOOK_A(f01a85270,  S4(0x01A85270))
HOOK_A(f01a85570,  S4(0x01A85570))
HOOK_A(f01ac2690,  S4(0x01AC2690))
HOOK_A(f01adfbe0,  S4(0x01ADFBE0))
HOOK_A(f01adfd20,  S4(0x01ADFD20))
HOOK_A(f01adfeb0,  S4(0x01ADFEB0))
HOOK_A(f01ae87c0,  S4(0x01AE87C0))
HOOK_A(f01af0740,  S4(0x01AF0740))
HOOK_A(f01af5400,  S4(0x01AF5400))
HOOK_A(f01affda0,  S4(0x01AFFDA0))
HOOK_A(f01affeb0,  S4(0x01AFFEB0))
HOOK_A(f01b0e0a0,  S4(0x01B0E0A0))
HOOK_A(f01c7ee60,  S4(0x01C7EE60))
typedef void* (__fastcall* tB)(void*, void*, void*, unsigned);
static tB o_f01ae4dd0 = (tB)S4(0x01AE4DD0);
static void* __fastcall h_f01ae4dd0(void* ecx, void* edx, void* a1, unsigned a2) {
    __try { return o_f01ae4dd0(ecx, edx, a1, a2); }
    __except (GetExceptionCode() == CPP_EXC ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        InterlockedIncrement(&g_caught);
        return 0;
    }
}

static tA o_f01b0daf0 = (tA)S4(0x01B0DAF0);
static void* __fastcall h_f01b0daf0(void* ecx, void* edx, int a1) {
    if ((unsigned)a1 > 0x100000u) {
        InterlockedIncrement(&g_caught);
        return 0;
    }
    __try { return o_f01b0daf0(ecx, edx, a1); }
    __except (GetExceptionCode() == CPP_EXC ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        InterlockedIncrement(&g_caught);
        return 0;
    }
}

typedef unsigned(__cdecl* tC)(int);
#define HOOK_C(NAME, ADDR)                                                            \
  static tC o_##NAME = (tC)(ADDR);                                                    \
  static unsigned __cdecl h_##NAME(int a1) {                                          \
    __try { return o_##NAME(a1); }                                                    \
    __except (GetExceptionCode()==CPP_EXC ? EXCEPTION_EXECUTE_HANDLER                 \
                                          : EXCEPTION_CONTINUE_SEARCH) {              \
      InterlockedIncrement(&g_caught);                                                \
      return 0;                                                                       \
    }                                                                                 \
  }
HOOK_C(c017bb8e0, S4(0x017BB8E0)) HOOK_C(c00fba260, S4(0x00FBA260)) HOOK_C(c012ba620, S4(0x012BA620))
HOOK_C(c0127d780, S4(0x0127D780)) HOOK_C(c010bccb0, S4(0x010BCCB0)) HOOK_C(c01bf3af0, S4(0x01BF3AF0))
HOOK_C(c015f2350, S4(0x015F2350)) HOOK_C(c01ba9980, S4(0x01BA9980)) HOOK_C(c01129130, S4(0x01129130))
HOOK_C(c01129200, S4(0x01129200)) HOOK_C(c01be82a0, S4(0x01BE82A0)) HOOK_C(c01b6ec50, S4(0x01B6EC50))
HOOK_C(c011e0700, S4(0x011E0700)) HOOK_C(c012243b0, S4(0x012243B0)) HOOK_C(c01c1bf60, S4(0x01C1BF60))
HOOK_C(c01b98890, S4(0x01B98890)) HOOK_C(c01c1c140, S4(0x01C1C140)) HOOK_C(c01c1c050, S4(0x01C1C050))
HOOK_C(c013d1fc0, S4(0x013D1FC0)) HOOK_C(c01bd7c00, S4(0x01BD7C00)) HOOK_C(c00f57180, S4(0x00F57180))
HOOK_C(c018972d0, S4(0x018972D0)) HOOK_C(c01815f00, S4(0x01815F00)) HOOK_C(c01a8a670, S4(0x01A8A670))
HOOK_C(c015cfef0, S4(0x015CFEF0)) HOOK_C(c01bc3b80, S4(0x01BC3B80)) HOOK_C(c01642bb0, S4(0x01642BB0))
HOOK_C(c01642ae0, S4(0x01642AE0)) HOOK_C(c013c2750, S4(0x013C2750)) HOOK_C(c011c1f70, S4(0x011C1F70))
HOOK_C(c01906dd0, S4(0x01906DD0)) HOOK_C(c011c6e50, S4(0x011C6E50))

extern "C" void StartHeapFixes(int lfh, int failsoft19, int badAlloc32) {
    // no reinstalar: los punteros o_ ya apuntan a los trampolines de Detours,
    // enganchar de nuevo hookearia el trampolin y quedaria recursion infinita
    static bool installed = false;
    if (installed) return;
    installed = true;

    if (lfh) CreateThread(0, 0, LfhThread, 0, 0, 0);

    if (!failsoft19 && !badAlloc32) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (failsoft19) {
        DetourAttach(&(PVOID&)o_Alloc, h_Alloc);
        DetourAttach(&(PVOID&)o_f00f391d0, h_f00f391d0);
        DetourAttach(&(PVOID&)o_f014cb7d0, h_f014cb7d0);
        DetourAttach(&(PVOID&)o_f01a84f10, h_f01a84f10);
        DetourAttach(&(PVOID&)o_f01a85270, h_f01a85270);
        DetourAttach(&(PVOID&)o_f01a85570, h_f01a85570);
        DetourAttach(&(PVOID&)o_f01ac2690, h_f01ac2690);
        DetourAttach(&(PVOID&)o_f01adfbe0, h_f01adfbe0);
        DetourAttach(&(PVOID&)o_f01adfd20, h_f01adfd20);
        DetourAttach(&(PVOID&)o_f01adfeb0, h_f01adfeb0);
        DetourAttach(&(PVOID&)o_f01ae87c0, h_f01ae87c0);
        DetourAttach(&(PVOID&)o_f01af0740, h_f01af0740);
        DetourAttach(&(PVOID&)o_f01af5400, h_f01af5400);
        DetourAttach(&(PVOID&)o_f01affda0, h_f01affda0);
        DetourAttach(&(PVOID&)o_f01affeb0, h_f01affeb0);
        DetourAttach(&(PVOID&)o_f01b0daf0, h_f01b0daf0);
        DetourAttach(&(PVOID&)o_f01b0e0a0, h_f01b0e0a0);
        DetourAttach(&(PVOID&)o_f01c7ee60, h_f01c7ee60);
        DetourAttach(&(PVOID&)o_f01ae4dd0, h_f01ae4dd0);
    }
    if (badAlloc32) {
        DetourAttach(&(PVOID&)o_c017bb8e0, h_c017bb8e0); DetourAttach(&(PVOID&)o_c00fba260, h_c00fba260);
        DetourAttach(&(PVOID&)o_c012ba620, h_c012ba620); DetourAttach(&(PVOID&)o_c0127d780, h_c0127d780);
        DetourAttach(&(PVOID&)o_c010bccb0, h_c010bccb0); DetourAttach(&(PVOID&)o_c01bf3af0, h_c01bf3af0);
        DetourAttach(&(PVOID&)o_c015f2350, h_c015f2350); DetourAttach(&(PVOID&)o_c01ba9980, h_c01ba9980);
        DetourAttach(&(PVOID&)o_c01129130, h_c01129130); DetourAttach(&(PVOID&)o_c01129200, h_c01129200);
        DetourAttach(&(PVOID&)o_c01be82a0, h_c01be82a0); DetourAttach(&(PVOID&)o_c01b6ec50, h_c01b6ec50);
        DetourAttach(&(PVOID&)o_c011e0700, h_c011e0700); DetourAttach(&(PVOID&)o_c012243b0, h_c012243b0);
        DetourAttach(&(PVOID&)o_c01c1bf60, h_c01c1bf60); DetourAttach(&(PVOID&)o_c01b98890, h_c01b98890);
        DetourAttach(&(PVOID&)o_c01c1c140, h_c01c1c140); DetourAttach(&(PVOID&)o_c01c1c050, h_c01c1c050);
        DetourAttach(&(PVOID&)o_c013d1fc0, h_c013d1fc0); DetourAttach(&(PVOID&)o_c01bd7c00, h_c01bd7c00);
        DetourAttach(&(PVOID&)o_c00f57180, h_c00f57180); DetourAttach(&(PVOID&)o_c018972d0, h_c018972d0);
        DetourAttach(&(PVOID&)o_c01815f00, h_c01815f00); DetourAttach(&(PVOID&)o_c01a8a670, h_c01a8a670);
        DetourAttach(&(PVOID&)o_c015cfef0, h_c015cfef0); DetourAttach(&(PVOID&)o_c01bc3b80, h_c01bc3b80);
        DetourAttach(&(PVOID&)o_c01642bb0, h_c01642bb0); DetourAttach(&(PVOID&)o_c01642ae0, h_c01642ae0);
        DetourAttach(&(PVOID&)o_c013c2750, h_c013c2750); DetourAttach(&(PVOID&)o_c011c1f70, h_c011c1f70);
        DetourAttach(&(PVOID&)o_c01906dd0, h_c01906dd0); DetourAttach(&(PVOID&)o_c011c6e50, h_c011c6e50);
    }

    DetourTransactionCommit();
}

extern "C" void StopHeapFixes() { }
extern "C" __declspec(dllexport) long OomCaught() { return g_caught; }
