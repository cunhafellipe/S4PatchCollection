#include <Windows.h>
#include <cstdint>
#include "detours.h"

using CardCheck = uint32_t(__thiscall*)(void* bookManager, int mode, uint32_t index);
using IndexToBookKey = int(__thiscall*)(void* bookManager, uint32_t index);
using FindProgressRow = void*(__thiscall*)(void* manager, int bookKey);

static CardCheck g_originalCardCheck = (CardCheck)0x01809EE0;
static IndexToBookKey g_indexToBookKey = (IndexToBookKey)0x01809BB0;
static FindProgressRow g_findProgressRow = (FindProgressRow)0x01248050;

static bool IsReadable(const void* ptr, size_t size = sizeof(void*))
{
    if (!ptr)
        return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;

    const uintptr_t begin = (uintptr_t)ptr;
    const uintptr_t end = begin + size;
    const uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return end >= begin && end <= regionEnd;
}

static void* GetCollectBookManager()
{
    __try {
        const uintptr_t root = *(uintptr_t*)0x025BD770;
        if (!IsReadable((void*)root, 0xFC))
            return nullptr;

        void* manager = *(void**)(root + 0xF8);
        return IsReadable(manager) ? manager : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static int GetProgressState(void* manager, int bookKey)
{
    if (!manager || bookKey <= 0)
        return -1;

    __try {
        void* row = g_findProgressRow(manager, bookKey);
        return IsReadable(row, 0x10) ? *(int*)((uint8_t*)row + 0x0C) : -1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static uint32_t __fastcall CardCheckHook(void* ecx, void* edx, int mode, uint32_t index)
{
    (void)edx;
    const uint32_t result = g_originalCardCheck(ecx, mode, index);
    if (mode != 1 || result != 0)
        return result;

    __try {
        const int bookKey = g_indexToBookKey(ecx, index);
        if (GetProgressState(GetCollectBookManager(), bookKey) == 4)
            return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    return result;
}

void InstallCollectionBookCyanFix()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_originalCardCheck, (PVOID)CardCheckHook);
    DetourTransactionCommit();
}
