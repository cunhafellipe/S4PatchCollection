#define _CRT_SECURE_NO_WARNINGS

#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <intrin.h>
#include "detours.h"

#pragma intrinsic(_ReturnAddress)

using tUiFn = void(__thiscall*)(void*);
using tUpdateFn = void(__thiscall*)(void*);
using tGetNativeMatch = void* (__cdecl*)();
using tSendNative92 = void(__stdcall*)(int);
using tSendNative94 = void(__stdcall*)(int);
using tClubName = const char* (__thiscall*)(void*);
using tFindChild = void* (__cdecl*)(void*, const char*, const char*);
using tReMatchAckHandler = void(__thiscall*)(void*, void*, void*, int*, int*, int*, int*, int*);

static tUiFn o0179C6F0 = (tUiFn)0x0179C6F0;       // quick search open, state 1
static tUiFn o0179A2F0 = (tUiFn)0x0179A2F0;       // full reset/hide, clears native busy flag
static tUiFn o0179A440 = (tUiFn)0x0179A440;       // hide panels
static tUiFn o0179B1A0 = (tUiFn)0x0179B1A0;       // base popup open
static tUiFn o0179B590 = (tUiFn)0x0179B590;       // VS/rematch popup open
static tUpdateFn o0179C800 = (tUpdateFn)0x0179C800; // quick search update
static tUpdateFn o0179D750 = (tUpdateFn)0x0179D750; // waiting room update state2/state3
static tGetNativeMatch GetNativeMatch = (tGetNativeMatch)0x011C5830;
static tSendNative92 o018CE0F0 = (tSendNative92)0x018CE0F0; // base Start/cancel native 0x92
static tSendNative94 o018CE110 = (tSendNative94)0x018CE110; // rematch/cancel native 0x94
static tClubName GetClubName1 = (tClubName)0x018C53C0; // native side1/alpha club name
static tClubName GetClubName2 = (tClubName)0x018C5580; // native side2/beta club name
static tFindChild FindChild = (tFindChild)0x0134EC80; // XUI FindChild(group, name, class)
static tReMatchAckHandler o01A29800 = (tReMatchAckHandler)0x01A29800; // S2C 1159 ReMatch_Ack

static FILE* g_log = nullptr;
static void* const g_rankViewer1 = (void*)0x2C54DF88;
static void* const g_rankViewer2 = (void*)0x2C54D380;
static void* volatile g_waitingRoom = nullptr;
static DWORD g_lastLogTick = 0;
static uint32_t g_lastLoggedState = 0xFFFFFFFF;
static uint32_t g_lastQuickTime = 0;
static uint32_t g_lastQuickLimit = 30000;
static DWORD g_quickStartedTick = 0;
static DWORD g_suppressEmptyPopupUntil = 0;
static int g_lastCustomRematchStatus = 0;
static DWORD g_lastCustomRematchTick = 0;

static void Log(const char* fmt, ...)
{
    (void)fmt;
}

static int LogException(EXCEPTION_POINTERS* ep, const char* tag)
{
    if (ep && ep->ExceptionRecord) {
        Log("[%s] SEH code=%08X addr=%p\n",
            tag,
            ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
    }
    else {
        Log("[%s] SEH no exception info\n", tag);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

static uint32_t GetUiState(void* self)
{
    uint32_t state = 0xFFFFFFFF;

    __try {
        if (self)
            state = *(uint32_t*)((uint8_t*)self + 0x238);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        state = 0xFFFFFFFF;
    }

    return state;
}

static void RememberThis(void* self, const char* source)
{
    if (!self)
        return;

    g_waitingRoom = self;

    __try {
        uint32_t state = GetUiState(self);
        DWORD now = GetTickCount();
        if (state != g_lastLoggedState || now - g_lastLogTick > 1000) {
            g_lastLoggedState = state;
            g_lastLogTick = now;
            Log("[%s] this=%p state=%u\n", source, self, state);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[%s] this=%p state=<seh>\n", source, self);
    }
}

static const char* ReadStdString(void* object, char* out, size_t outSize)
{
    if (!out || outSize == 0)
        return "";

    out[0] = 0;

    __try {
        if (!object)
            return out;

        uint8_t* s = (uint8_t*)object;
        uint32_t size = *(uint32_t*)(s + 0x10);
        uint32_t capacity = *(uint32_t*)(s + 0x14);
        const char* text = capacity < 0x10 ? (const char*)s : *(const char**)s;

        if (!text || size > 4096)
            return out;

        size_t copy = size < outSize - 1 ? size : outSize - 1;
        memcpy(out, text, copy);
        out[copy] = 0;
    }
    __except (LogException(GetExceptionInformation(), "ReadStdString")) {
        out[0] = 0;
    }

    return out;
}

static bool HasVisibleUiText(const char* text)
{
    if (!text)
        return false;

    bool inTag = false;
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
        unsigned char c = *p;
        if (c == '{') {
            inTag = true;
            continue;
        }
        if (c == '}') {
            inTag = false;
            continue;
        }
        if (!inTag && c > ' ')
            return true;
    }

    return false;
}

static bool IsTickBefore(DWORD deadline)
{
    return (LONG)(GetTickCount() - deadline) < 0;
}

static bool IsCustomRematchStatus(int status)
{
    return status >= 10 && status <= 13;
}

static bool HasFreshCustomRematchStatus(int* status)
{
    DWORD tick = g_lastCustomRematchTick;
    int value = g_lastCustomRematchStatus;

    if (!IsCustomRematchStatus(value) || !tick)
        return false;

    if (GetTickCount() - tick > 60000) {
        g_lastCustomRematchStatus = 0;
        g_lastCustomRematchTick = 0;
        return false;
    }

    if (status)
        *status = value;

    return true;
}

static int ReadIntPtr(int* value, int fallback)
{
    int result = fallback;

    __try {
        if (value)
            result = *value;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        result = fallback;
    }

    return result;
}

static const char* SafeCStr(const char* text)
{
    __try {
        return text ? text : "";
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return "";
    }
}

static int ReadIntOffset(void* object, size_t offset, int fallback)
{
    int result = fallback;

    __try {
        if (object)
            result = *(int*)((uint8_t*)object + offset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        result = fallback;
    }

    return result;
}

static void ResetRematchPopupTimer(void* self, const char* source)
{
    __try {
        if (!self || GetUiState(self) != 4)
            return;

        *(uint32_t*)((uint8_t*)self + 0x25C) = 0;
        *(uint32_t*)((uint8_t*)self + 0x260) = 60000;
        *(uint32_t*)((uint8_t*)self + 0x264) = 0;

        Log("[%s] reset rematch timer 60s this=%p\n", source, self);
    }
    __except (LogException(GetExceptionInformation(), "ResetRematchPopupTimer")) {
    }
}

static void ResetCustomRematchLocal(void* self)
{
    g_lastCustomRematchStatus = 0;
    g_lastCustomRematchTick = 0;
    g_suppressEmptyPopupUntil = GetTickCount() + 6000;

    __try {
        if (self) {
            *(uint32_t*)((uint8_t*)self + 0x238) = 0;
            *(uint32_t*)((uint8_t*)self + 0x23C) = 0;
            *(uint32_t*)((uint8_t*)self + 0x244) = 0;
            *(uint32_t*)((uint8_t*)self + 0x25C) = 0;
            *(uint32_t*)((uint8_t*)self + 0x264) = 0;
        }

        void* nativeMatch = GetNativeMatch();
        if (nativeMatch) {
            *(uint32_t*)((uint8_t*)nativeMatch + 0xBC) = 0;
            *(uint32_t*)((uint8_t*)nativeMatch + 0xC0) = 0;
            *(uint32_t*)((uint8_t*)nativeMatch + 0xC4) = 0;
            *(uint32_t*)((uint8_t*)nativeMatch + 0xC8) = 0;
        }
    }
    __except (LogException(GetExceptionInformation(), "ResetCustomRematchLocal")) {
    }

    __try {
        if (self)
            o0179A2F0(self);
    }
    __except (LogException(GetExceptionInformation(), "ResetCustomRematchLocal.full")) {
    }
}

static void SetVisible(void* widget, int visible)
{
    __try {
        if (!widget)
            return;

        void** vtbl = *(void***)widget;
        auto setVisible = (void(__thiscall*)(void*, int))vtbl[0x1B4 / 4];
        setVisible(widget, visible);
    }
    __except (LogException(GetExceptionInformation(), "SetVisible")) {
    }
}

static void SetText(void* widget, const char* text)
{
    __try {
        if (!widget)
            return;

        void** vtbl = *(void***)widget;
        auto setText = (void(__thiscall*)(void*, const char*))vtbl[0x70 / 4];
        setText(widget, text ? text : "");
    }
    __except (LogException(GetExceptionInformation(), "SetText")) {
    }
}

static void* FindTextViewer(void* group, const char* name)
{
    void* widget = nullptr;

    __try {
        if (group)
            widget = FindChild(group, name, "CGUI_TextViewer");
    }
    __except (LogException(GetExceptionInformation(), "FindTextViewer")) {
        Log("[FindTextViewer] failed group=%p name='%s'\n", group, SafeCStr(name));
    }

    return widget;
}

static void FillMiddleRematchClubNames(void* self, const char* source)
{
    __try {
        if (!self)
            return;

        void* nativeMatch = GetNativeMatch();
        if (!nativeMatch) {
            Log("[%s] middle labels skipped native=NULL\n", source);
            return;
        }

        const char* name1 = GetClubName1(nativeMatch);
        const char* name2 = GetClubName2(nativeMatch);
        void* nameViewer1 = *(void**)((uint8_t*)self + 0x2C8);
        void* nameViewer2 = *(void**)((uint8_t*)self + 0x2CC);

        SetText(nameViewer1, SafeCStr(name1));
        SetText(nameViewer2, SafeCStr(name2));
        SetVisible(nameViewer1, 1);
        SetVisible(nameViewer2, 1);

        Log("[%s] middle dead labels names='%s'/'%s' viewers=%p/%p\n",
            source, SafeCStr(name1), SafeCStr(name2), nameViewer1, nameViewer2);
    }
    __except (LogException(GetExceptionInformation(), "FillMiddleRematchClubNames")) {
    }
}

static void FillVsRankLabels(void* self, const char* source)
{
    __try {
        if (!self)
            return;

        void* nativeMatch = GetNativeMatch();
        if (!nativeMatch) {
            Log("[%s] rank labels skipped native=NULL\n", source);
            return;
        }

        int rank1 = ReadIntOffset(nativeMatch, 0x104, 0);
        int rank2 = ReadIntOffset(nativeMatch, 0x108, 0);
        char text1[32];
        char text2[32];
        sprintf_s(text1, "%d", rank1);
        sprintf_s(text2, "%d", rank2);

        void* teamClassGroup = *(void**)((uint8_t*)self + 0x2F4);
        void* rankViewer1 = FindTextViewer(teamClassGroup, "club_rank1_TextViewer");
        void* rankViewer2 = FindTextViewer(teamClassGroup, "club_rank2_TextViewer");

        if (!rankViewer1)
            rankViewer1 = g_rankViewer1;
        if (!rankViewer2)
            rankViewer2 = g_rankViewer2;

        SetText(rankViewer1, text1);
        SetText(rankViewer2, text2);
        SetVisible(rankViewer1, 1);
        SetVisible(rankViewer2, 1);

        Log("[%s] rank labels db/native=%d/%d viewers=%p/%p\n",
            source, rank1, rank2, rankViewer1, rankViewer2);
    }
    __except (LogException(GetExceptionInformation(), "FillVsRankLabels")) {
    }
}

static void ShowBaseMessage(void* self, const char* text, const char* tag)
{
    if (!self)
        return;

    __try {
        Log("[%s] show this=%p text='%s'\n", tag, self, text ? text : "");

        o0179A440(self);
        *(uint32_t*)((uint8_t*)self + 0x23C) = 0;
        *(uint32_t*)((uint8_t*)self + 0x244) = 0;
        *(uint32_t*)((uint8_t*)self + 0x238) = 0;

        void* baseGroup = *(void**)((uint8_t*)self + 0x2A0);
        void* startButton = *(void**)((uint8_t*)self + 0x2D8);
        void* textViewer = *(void**)((uint8_t*)self + 0x2E4);

        SetVisible(baseGroup, 1);
        SetText(textViewer, text);
        SetVisible(textViewer, 1);
        SetVisible(startButton, 1);
    }
    __except (LogException(GetExceptionInformation(), tag)) {
    }
}

static const char* GetQuickEmptyMessage()
{
    uint32_t limit = g_lastQuickLimit ? g_lastQuickLimit : 30000;
    DWORD elapsed = g_quickStartedTick ? GetTickCount() - g_quickStartedTick : 0;
    if (g_lastQuickTime + 1500 >= limit || elapsed >= 25000)
        return "{F-2002_20}{S-S}{W}No match was found.{N}Try Quick Match again.";

    return "{F-2002_20}{S-S}{W}Your match has entered the room.";
}

static const char* GetCustomRematchMessage(int status)
{
    switch (status) {
    case 10:
        return "{F-2002_20}{S-M}{W}You have won this clan battle.";
    case 11:
        return "{F-2002_20}{S-M}{W}You have lost this clan battle.";
    case 12:
        return "{F-2002_20}{S-S}{W}No match was found.{N}Try Quick Match again.";
    case 13:
        return "{F-2002_20}{S-S}{W}Your match has entered the room.";
    default:
        return nullptr;
    }
}

void __fastcall hk0179C6F0(void* ecx, void* edx)
{
    (void)edx;
    RememberThis(ecx, "0179C6F0");
    g_lastQuickTime = 0;
    g_lastQuickLimit = 30000;
    g_quickStartedTick = GetTickCount();
    o0179C6F0(ecx);
}

void __fastcall hk0179A440(void* ecx, void* edx)
{
    (void)edx;

    int customStatus = 0;
    if (HasFreshCustomRematchStatus(&customStatus)) {
        Log(" PRESIONASTE EL BOTON,\n");
        Log("[0179A440] custom ok/hide status=%d this=%p\n", customStatus, ecx);

        ResetCustomRematchLocal(ecx);
        o0179A440(ecx);

        if (customStatus == 11) {
            Log("[0179A440] custom lost -> send room leave through ReMatch cancel id=6\n");
            o018CE110(6);
        }
        return;
    }

    o0179A440(ecx);
}

void __fastcall hk0179C800(void* ecx, void* edx)
{
    (void)edx;
    RememberThis(ecx, "0179C800");

    o0179C800(ecx);

    __try {
        if (GetUiState(ecx) == 1) {
            g_lastQuickTime = *(uint32_t*)((uint8_t*)ecx + 0x244);
            g_lastQuickLimit = *(uint32_t*)((uint8_t*)ecx + 0x240);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall hk0179D750(void* ecx, void* edx)
{
    (void)edx;
    RememberThis(ecx, "0179D750");
    o0179D750(ecx);
}

void __fastcall hk0179B1A0(void* ecx, void* edx)
{
    (void)edx;
    RememberThis(ecx, "0179B1A0");

    uint32_t stateBefore = GetUiState(ecx);
    char text[512];
    text[0] = 0;

    __try {
        void* nativeMatch = GetNativeMatch();
        if (!nativeMatch) {
            o0179B1A0(ecx);
            return;
        }

        ReadStdString((uint8_t*)nativeMatch + 0xCC, text, sizeof(text));

        if (!HasVisibleUiText(text)) {
            if (stateBefore == 2) {
                Log("[0179B1A0] allow empty request-list popup this=%p caller=%p\n",
                    ecx, _ReturnAddress());
                o0179B1A0(ecx);
                return;
            }

            if (stateBefore != 1) {
                Log("[0179B1A0] reset empty non-quick popup this=%p state=%u caller=%p\n",
                    ecx, stateBefore, _ReturnAddress());
                ResetCustomRematchLocal(ecx);
                o0179A440(ecx);
                return;
            }

            if (IsTickBefore(g_suppressEmptyPopupUntil)) {
                Log("[0179B1A0] suppress empty popup this=%p state=%u caller=%p\n",
                    ecx, stateBefore, _ReturnAddress());
                ResetCustomRematchLocal(ecx);
                o0179A440(ecx);
                return;
            }

            Log("[0179B1A0] replace empty popup this=%p state=%u caller=%p timer=%u/%u\n",
                ecx, stateBefore, _ReturnAddress(), g_lastQuickTime, g_lastQuickLimit);
            const char* message = GetQuickEmptyMessage();
            ShowBaseMessage(ecx, message, "quick result");
            g_lastQuickTime = 0;
            g_lastQuickLimit = 30000;
            g_quickStartedTick = 0;
            return;
        }
    }
    __except (LogException(GetExceptionInformation(), "0179B1A0.precheck")) {
    }

    o0179B1A0(ecx);
}

void __fastcall hk0179B590(void* ecx, void* edx)
{
    (void)edx;
    RememberThis(ecx, "0179B590");

    __try {
        o0179B590(ecx);
        ResetRematchPopupTimer(ecx, "0179B590.after");
        FillMiddleRematchClubNames(ecx, "0179B590.after");
        FillVsRankLabels(ecx, "0179B590.after");
    }
    __except (LogException(GetExceptionInformation(), "0179B590")) {
    }
}

void __fastcall hk01A29800(void* ecx, void* edx, void* arg1, void* arg2, int* unk1, int* unk2, int* unk3, int* unk4, int* status)
{
    (void)edx;

    int statusValue = ReadIntPtr(status, -1);
    if (statusValue == 0) {
        o01A29800(ecx, arg1, arg2, unk1, unk2, unk3, unk4, status);
        Log("[01A29800] reset rematch state this=%p\n", g_waitingRoom);
        ResetCustomRematchLocal(g_waitingRoom);
        return;
    }

    if (statusValue == 4) {
        o01A29800(ecx, arg1, arg2, unk1, unk2, unk3, unk4, status);
        return;
    }

    int mark0 = ReadIntPtr(unk1, 0);
    int mark1 = ReadIntPtr(unk2, 0);
    int mark2 = ReadIntPtr(unk3, 0);
    int mark3 = ReadIntPtr(unk4, 0);

    const char* message = GetCustomRematchMessage(statusValue);
    if (!message) {
        o01A29800(ecx, arg1, arg2, unk1, unk2, unk3, unk4, status);
        return;
    }

    o01A29800(ecx, arg1, arg2, unk1, unk2, unk3, unk4, status);

    void* self = g_waitingRoom;
    g_lastCustomRematchStatus = statusValue;
    g_lastCustomRematchTick = GetTickCount();
    if (statusValue == 10 || statusValue == 11)
        g_suppressEmptyPopupUntil = GetTickCount() + 6000;

    Log("[01A29800] custom rematch status=%d marks=%d,%d,%d,%d this=%p\n",
        statusValue, mark0, mark1, mark2, mark3, self);
    ShowBaseMessage(self, message, "custom rematch");
}

void __stdcall hk018CE0F0(int value)
{
    void* self = g_waitingRoom;
    int customStatus = 0;

    if (HasFreshCustomRematchStatus(&customStatus)) {
        Log(" PRESIONASTE EL BOTON,\n");
        Log("[018CE0F0] custom ok value=%d status=%d this=%p send status\n",
            value, customStatus, self);

        ResetCustomRematchLocal(self);

        __try {
            if (self) {
                o0179A440(self);
            }
        }
        __except (LogException(GetExceptionInformation(), "018CE0F0.custom")) {
        }

        if (customStatus == 11) {
            Log("[018CE0F0] custom lost -> send ReMatch cancel id=6\n");
            o018CE110(6);
        }
        else {
            o018CE0F0(customStatus);
        }
        return;
    }

    uint32_t state = GetUiState(self);

    if (state == 1) {
        g_suppressEmptyPopupUntil = GetTickCount() + 2500;
        g_quickStartedTick = 0;
        Log("[018CE0F0] quick cancel/start value=%d this=%p suppress empty popup\n",
            value, self);
    }

    o018CE0F0(value);
}

static void OpenConsole()
{
}

static void AttachHooks()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)o0179C6F0, hk0179C6F0);
    DetourAttach(&(PVOID&)o0179A440, hk0179A440);
    DetourAttach(&(PVOID&)o0179C800, hk0179C800);
    DetourAttach(&(PVOID&)o0179D750, hk0179D750);
    DetourAttach(&(PVOID&)o0179B1A0, hk0179B1A0);
    DetourAttach(&(PVOID&)o0179B590, hk0179B590);
    DetourAttach(&(PVOID&)o018CE0F0, hk018CE0F0);
    DetourAttach(&(PVOID&)o01A29800, hk01A29800);
    LONG result = DetourTransactionCommit();

    Log("[hooks] Detour commit=%ld\n", result);
}

static void DetachHooks()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)o0179C6F0, hk0179C6F0);
    DetourDetach(&(PVOID&)o0179A440, hk0179A440);
    DetourDetach(&(PVOID&)o0179C800, hk0179C800);
    DetourDetach(&(PVOID&)o0179D750, hk0179D750);
    DetourDetach(&(PVOID&)o0179B1A0, hk0179B1A0);
    DetourDetach(&(PVOID&)o0179B590, hk0179B590);
    DetourDetach(&(PVOID&)o018CE0F0, hk018CE0F0);
    DetourDetach(&(PVOID&)o01A29800, hk01A29800);
    DetourTransactionCommit();

    if (g_log) {
        fclose(g_log);
        g_log = nullptr;
    }
}

void StartMatchmakingHotkeys()
{
    OpenConsole();
    AttachHooks();
}
