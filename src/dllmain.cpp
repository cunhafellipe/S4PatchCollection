#include <windows.h>

static LONG WINAPI s4fixes_unhandled_exception(EXCEPTION_POINTERS* ep)
{
    static LONG entered = 0;
    if (InterlockedExchange(&entered, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    char line[512];
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    const ULONG_PTR address = ep && ep->ExceptionRecord ?
        (ULONG_PTR)ep->ExceptionRecord->ExceptionAddress : 0;
    wsprintfA(line, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] exception=0x%08lX address=0x%p\r\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        now.wMilliseconds, code, (void*)address);

    HANDLE file = CreateFileA("s4fixes_crash.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, line, lstrlenA(line), &written, nullptr);
        CloseHandle(file);
    }
    OutputDebugStringA(line);
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallArcadeStackOverflowFix();
void InstallEnchantOOBFix();
void InstallHttpImageCacheFix();
void InstallStructVectorCapFix();
void InstallArcadeAllocFix();
void InstallLoadingTipCache();
void InstallX7DecryptGuard();
void InstallBlobTreeGuard();
void InstallFileExistsCache();
void InstallMemoryJumpFix();
void InstallSceneLeakFix();
void InstallS4hdMountGuard();
void InstallResWrapperLeakFix();
void InstallLzoS4SizeGuard();
void InstallP2PContainerDoSGuard();
void InstallIDocumentReloadLeakFix();
void InstallXmlOverReadGuard();
void InstallBattlEyeBypass();
void InstallCollectionBookCyanFix();
void StartMatchmakingHotkeys();
void StartGameEnhancements();
void StartChaserTrainingTab();
void StartOverlay();
void StartNewActorState();
void StartXuiHotReload();
void StartX7HotReload();
void InstallAdminConsoleCapture();
void StartResourceLogTest();
void StartHdText();
extern "C" void StartHeapFixes(int lfh, int failsoft19, int badAlloc32);

static DWORD WINAPI InitThread(LPVOID)
{
   // SetUnhandledExceptionFilter(s4fixes_unhandled_exception);

    // sub_F44770 (registers the ~74 admin commands) fires early during
    // CClientApp::init, so this has to be armed before anything else can
    // possibly delay it, or the capture misses everything.
    InstallAdminConsoleCapture();
    StartResourceLogTest();

    // First, before anything else: the client preloads every screen while booting, and these
    // hooks have to be in place to capture each Create's `this`. Installed any later they miss
    // the whole sweep and F6 finds nothing to reload.
    //StartXuiHotReload();
    // Disabled while it is being iterated on as the standalone x7_hot_reload.asi in the client
    // folder. Both loaded at once means whichever installs second finds the prologues already
    // patched and refuses to hook anything.
    //StartX7HotReload();

    InstallBattlEyeBypass();
    InstallCollectionBookCyanFix();
   StartMatchmakingHotkeys();

   //

    InstallArcadeStackOverflowFix();
   InstallFileExistsCache();
    InstallMemoryJumpFix();
   InstallEnchantOOBFix();
   InstallHttpImageCacheFix();
   InstallStructVectorCapFix();
    InstallArcadeAllocFix();
    InstallLoadingTipCache();
   InstallX7DecryptGuard();
    InstallBlobTreeGuard();
    // known side effect: the per-slot anti-reload (500ms window over the 10 SetSce ne
    // slots) sometimes skips a reload and leaves a collectionbook item on a stale
    // scene, which shows up as a red/broken block. ANTIRELOAD=false or a lower
    // ANTIRELOAD_MS in Scene_Leak_Fix.cpp if it gets annoying.
   InstallSceneLeakFix();
    InstallS4hdMountGuard();
   InstallResWrapperLeakFix();
    InstallLzoS4SizeGuard();
    InstallP2PContainerDoSGuard();
    InstallIDocumentReloadLeakFix();
    InstallXmlOverReadGuard();
   //// y el heapfixes me dio crash
  StartHeapFixes(1, 1, 0);
    StartHdText();
    StartChaserTrainingTab();
    StartGameEnhancements();
   // actor crash
   StartNewActorState();
   StartOverlay();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        //LoadLibraryA("mutex.dll");
       // LoadLibraryA("sciter.dll");
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
