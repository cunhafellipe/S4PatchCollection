#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <detours.h>
#include <intrin.h>
#include <cstdio>

// Links the ~74 App:: admin commands (App::CTestAIActorCMD, CMonsterCreate,
// CReloadScriptCMD, etc.) that the client registers on boot into the same
// shared command registry the F9-openable native console reads. See memory
// s4-admin-console-command-linking-solved for the why.
//
// The client's own registration (sub_F44770) runs during CClientApp::init,
// long before any console exists, so its inserts silently no-op (the
// registry's "+4" guard field isn't set up yet). We capture every command
// pointer as it's registered, then replay the inserts once the native
// console has been opened for real (which is what properly sets up that
// field) - the exe's own insert code does the rest, no struct-guessing.

static uintptr_t g_ac_base = 0;
static uintptr_t ac_base() {
    if (!g_ac_base) g_ac_base = (uintptr_t)GetModuleHandleW(nullptr);
    return g_ac_base;
}
#define S4(va) (ac_base() + ((va) - 0x00E80000u))

// Gate: only this Windows account gets the admin console/commands. Anyone
// else running this same compiled DLL gets nothing - F9 does nothing, no
// commands get linked, no capture happens at all.
static bool IsAuthorizedUser()
{
    char name[256];
    DWORD len = sizeof(name);
    if (!GetUserNameA(name, &len)) return false;
    return lstrcmpiA(name, "sneo") == 0;
}

namespace {
    constexpr uintptr_t kConsoleObject = 0x0025BF500u;      // dword_25BF500, shared registry (== the console object)
    constexpr uintptr_t kConsoleInit   = 0x001B4EED0u;      // sub_1B4EED0(this, HWND parent, const char* title)
    constexpr uintptr_t kBigCommandRegister = 0x000F44770u; // sub_F44770, registers ~74 App:: commands into the registry
    constexpr uintptr_t kInsertCommand = 0x001B4F050u;      // sub_1B4F050(registry, _, cmd) - guarded insert
    constexpr int kGetNameVtableSlot = 2;                   // vtable[2] (offset +8): App::CBaseConsoleCMD::GetName-ish

    // "test" fix: sub_1122DE0 is CAIActorManager::GetAIActor(x, z), a real
    // API used from 30+ places in the exe. CTestAIActorCMD::Execute has 3
    // call sites for it (one per test <2|3|4>) that never push x/z - a
    // compiled-in bug, confirmed via raw disasm (no push before the call,
    // though the callee still reads 2 stack args regardless). The x/z it
    // should have passed are sitting right there already, computed a few
    // instructions earlier and just never forwarded: dword_25BF558/55C.
    // Fix is scoped to only those 3 call sites (by return address) so the
    // other 30+ legitimate callers of GetAIActor are untouched.
    constexpr uintptr_t kActorSearch     = 0x01122DE0u; // sub_1122DE0 == CAIActorManager::GetAIActor(this, x, z)
    constexpr uintptr_t kSearchTargetX   = 0x0025BF558u; // dword_25BF558
    constexpr uintptr_t kSearchTargetZ   = 0x0025BF55Cu; // dword_25BF55C
    constexpr uintptr_t kBrokenCallers[3] = { 0x00F4E157u, 0x00F4E1DBu, 0x00F4E25Cu }; // return addrs right after CTestAIActorCMD's 3 calls

    // "test" force-spawn: CTestAIActorCMD::Execute's own search-based logic
    // needs a real P2P bot peer to match against, which doesn't exist in
    // this setup (see memory notes). Training TD/DM spawns bots through a
    // completely different, working path - the "AiSpawn" packet handler
    // (sub_1183720) - which boils down to: sub_1103A60(this, luaScriptPath,
    // name) builds the actor via its own Lua "CreateAIActor" callback, then
    // sub_115A390() gets the CAIActorManager singleton and sub_1122B00
    // registers the new actor into it. Replacing test's Execute entirely
    // with a direct call through that same real chain, using a script path
    // known to exist (monster_status.x7's ai_lua for the basic pawn).
    constexpr uintptr_t kTestExecute   = 0x00F4E040u; // App::CTestAIActorCMD::Execute (vtable[1])
    constexpr uintptr_t kActorFactory  = 0x01103A60u; // sub_1103A60(this, luaScriptPath, name) -> actor
    constexpr uintptr_t kManagerGetter = 0x0115A390u; // sub_115A390() -> CAIActorManager singleton
    constexpr uintptr_t kAddAIActor    = 0x01122B00u; // CAIActorManager::AddAIActor(this, actor)
    constexpr uintptr_t kSetSpawnPos   = 0x0142ED40u; // sub_142ED40(this=manager?, x, z) - called before the vtable setup, guess: CAIActorManager::SetSpawnPosition

    // reloadscr (CReloadScriptCMD::Execute, sub_F4A3F0) opens with a mode
    // check - `if (dword_25BD770[0x110/4] != sub_1292640(5)) return 0;` -
    // that silently no-ops the whole command outside whatever game state it
    // expects. `jz short loc_F4A44A` at this address is the branch; flipping
    // JZ (0x74) to an unconditional JMP (0xEB) with the same offset byte
    // always takes the "do the reload" path, everywhere.
    constexpr uintptr_t kReloadScriptCheckJz = 0x00F4A441u;

    using InitFn = void(__thiscall*)(void*, HWND, const char*);
    using BigCommandRegisterFn = int(__cdecl*)();
    using InsertCommandFn = int(__fastcall*)(int, int, int);
    using ActorSearchFn = void*(__thiscall*)(void*, int, int);
    using ActorFactoryFn = int(__thiscall*)(void*, char*, char*);
    using ManagerGetterFn = int(__cdecl*)();
    using AddActorFn = char(__thiscall*)(void*, int);
    using SetSpawnPosFn = int(__thiscall*)(void*, int, int);
    using TestExecuteFn = char(__thiscall*)(void*);
    TestExecuteFn original_test_execute = nullptr;
    ActorSearchFn original_actor_search = nullptr;
    using GetNameFn = const char*(__thiscall*)(void*);

    BigCommandRegisterFn original_big_command_register = nullptr;
    InsertCommandFn original_insert_command = nullptr;
    volatile bool g_consoleReady = false;
    volatile bool g_linked = false;
    HWND g_consoleHwnd = nullptr;
    HWND g_consoleInputEdit = nullptr;
    HANDLE g_openRequestEvent = nullptr;
}

void* g_adminCommands[128];
volatile LONG g_adminCommandCount = 0;

const char* AdminCommandName(void* cmd)
{
    if (!cmd) return "";
    __try {
        void** vtbl = *(void***)cmd;
        auto getName = (GetNameFn)vtbl[kGetNameVtableSlot];
        const char* name = getName(cmd);
        return name ? name : "";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "";
    }
}

namespace {
    void Log(const char* text)
    {
        (void)text;
    }

    bool IsReadable(uintptr_t address)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((void*)address, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        return (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
    }

    int __fastcall InsertCommandHook(int a1, int a2, int a3)
    {
        if (a3) {
            LONG idx = InterlockedIncrement(&g_adminCommandCount) - 1;
            if (idx < 128) g_adminCommands[idx] = (void*)a3;
        }
        return original_insert_command(a1, a2, a3);
    }

    int __cdecl BigCommandRegisterHook()
    {
        int result = original_big_command_register();
        char line[64];
        wsprintfA(line, "[admin-console] sub_F44770 returned, captured=%d", g_adminCommandCount);
        Log(line);
        return result;
    }

    void* __fastcall ActorSearchHook(void* this_, void* /*edx, unused*/, int a2, int a3)
    {
        uintptr_t ret = (uintptr_t)_ReturnAddress();
        for (uintptr_t brokenRva : kBrokenCallers) {
            if (ret == S4(brokenRva)) {
                a2 = *(int*)S4(kSearchTargetX);
                a3 = *(int*)S4(kSearchTargetZ);
                char line[96];
                wsprintfA(line, "[admin-console] GetAIActor: fixed test-cmd call, x=%d z=%d", a2, a3);
                Log(line);
                break;
            }
        }
        return original_actor_search(this_, a2, a3);
    }

    void InstallActorSearchFix()
    {
        original_actor_search = reinterpret_cast<ActorSearchFn>(S4(kActorSearch));
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(reinterpret_cast<PVOID*>(&original_actor_search), reinterpret_cast<PVOID>(ActorSearchHook));
        const LONG result = DetourTransactionCommit();
        Log(result == NO_ERROR ? "[admin-console] GetAIActor fix installed"
                               : "[admin-console] GetAIActor fix install FAILED");
    }

    void InstallReloadScriptModeCheckPatch()
    {
        uintptr_t addr = S4(kReloadScriptCheckJz);
        DWORD oldProtect = 0;
        if (!VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            Log("[admin-console] reloadscr patch: VirtualProtect failed");
            return;
        }
        unsigned char* b = (unsigned char*)addr;
        char line[96];
        if (*b != 0x74) {
            wsprintfA(line, "[admin-console] reloadscr patch: expected 0x74 (JZ) at target, found 0x%02X - skipped", *b);
            Log(line);
        } else {
            *b = 0xEBu; // JZ -> unconditional JMP, same offset byte
            wsprintfA(line, "[admin-console] reloadscr patch: mode check bypassed at %p", (void*)addr);
            Log(line);
        }
        DWORD ignore = 0;
        VirtualProtect((void*)addr, 1, oldProtect, &ignore);
    }

    // Full replacement, not a patch: "test"'s own logic is a dead end (needs
    // a P2P bot peer that doesn't exist here), so this bypasses it and goes
    // through the same real chain Training TD/DM uses to spawn its bots.
    volatile bool g_forceSpawnDone = false;

    char __fastcall TestExecuteHook(void* this_, void* /*edx, unused*/)
    {
        // sub_142ED40 -> sub_12936D0 doesn't look reentrant - a second call
        // deadlocked the whole process (even audio stopped). Guard until
        // that's understood better: only the very first "test" in a session
        // actually spawns anything.
        if (g_forceSpawnDone) {
            Log("[admin-console] force-AI: already spawned once this session, skipping (avoids a hang)");
            return 1;
        }
        g_forceSpawnDone = true;

        __try {
            auto factory = reinterpret_cast<ActorFactoryFn>(S4(kActorFactory));
            auto getManager = reinterpret_cast<ManagerGetterFn>(S4(kManagerGetter));
            auto addActor = reinterpret_cast<AddActorFn>(S4(kAddAIActor));
            // setSpawnPos (sub_142ED40) tried with this=manager: crashed or
            // hung every time, even on the very first call. Not calling it -
            // "this" guess was wrong, or (0,0) itself is unsafe there.
            // Reverting to the milder failure mode (crashes 1 frame later
            // reading position, not an immediate hang) until this can be
            // debugged live instead of guessed.

            static LONG s_spawnCounter = 0;
            char scriptPath[] = "Resources/Script/MonsterAI_Pawn_Normal.lua";
            char name[32];
            wsprintfA(name, "TestBot%d", InterlockedIncrement(&s_spawnCounter));

            int actor = factory(nullptr, scriptPath, name);
            char line[128];
            wsprintfA(line, "[admin-console] force-AI: factory(this=null) -> actor=%p", (void*)(uintptr_t)actor);
            Log(line);

            if (actor) {
                // The real spawn path (sub_1183720) also calls 2 vtable
                // methods on the actor (offsets 120/136 - position/rotation
                // in the original, fed from network packet fields we don't
                // have) before it's safe to touch. Without them, something
                // at actor+0x684 stays null and the next Update()/render
                // crashes reading it. Call them directly off the actor's own
                // (runtime, unknown-at-compile-time) vtable with zero args -
                // best guess at "spawn at a safe default" rather than
                // whatever garbage would otherwise sit there.
                void** vtbl = *(void***)(uintptr_t)actor;
                using Vtbl3Fn = void(__thiscall*)(void*, int, int, int);
                auto setA = reinterpret_cast<Vtbl3Fn>(vtbl[30]); // offset 120
                auto setB = reinterpret_cast<Vtbl3Fn>(vtbl[34]); // offset 136
                setA((void*)(uintptr_t)actor, 0, 0, 0);
                setB((void*)(uintptr_t)actor, 0, 0, 0);
                Log("[admin-console] force-AI: position/rotation vtable calls done");

                int manager = getManager();
                char added = addActor((void*)(uintptr_t)manager, actor);
                wsprintfA(line, "[admin-console] force-AI: manager=%p added=%d", (void*)(uintptr_t)manager, added);
                Log(line);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[admin-console] force-AI: exception during forced spawn");
        }
        return 1;
    }

    void InstallTestForceFix()
    {
        original_test_execute = reinterpret_cast<TestExecuteFn>(S4(kTestExecute));
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(reinterpret_cast<PVOID*>(&original_test_execute), reinterpret_cast<PVOID>(TestExecuteHook));
        const LONG result = DetourTransactionCommit();
        Log(result == NO_ERROR ? "[admin-console] force-AI spawn (test) installed"
                               : "[admin-console] force-AI spawn install FAILED");
    }

    HWND g_consoleOutputEdit = nullptr;

    // The single-line EDIT child is the input box; the log/output area is
    // multiline. Cheap enough to just check the style bit.
    BOOL CALLBACK FindInputEdit(HWND h, LPARAM out)
    {
        char cls[32]{};
        GetClassNameA(h, cls, sizeof(cls));
        LONG_PTR style = GetWindowLongPtrA(h, GWL_STYLE);
        char line[128];
        wsprintfA(line, "[admin-console] child hwnd=%p class=%s style=%08X", h, cls, (unsigned)style);
        Log(line);

        if (lstrcmpiA(cls, "EDIT") == 0) {
            if (style & ES_MULTILINE) g_consoleOutputEdit = h;
            else *(HWND*)out = h;
        } else if (lstrcmpiA(cls, "LISTBOX") == 0) {
            g_consoleOutputEdit = h; // probably the log area if it's not an EDIT
        }
        return TRUE; // enumerate everything, don't stop early - we want the full picture
    }

    void LinkCapturedCommands()
    {
        char line[96];
        wsprintfA(line, "[admin-console] linking %d captured commands", g_adminCommandCount);
        Log(line);

        int registryAsInt = (int)S4(kConsoleObject);
        __try {
            for (LONG i = 0; i < g_adminCommandCount && i < 128; ++i) {
                int result = original_insert_command(registryAsInt, 0, (int)(uintptr_t)g_adminCommands[i]);
                wsprintfA(line, "[admin-console] link[%d] %s -> result=%d",
                          i, AdminCommandName(g_adminCommands[i]), result);
                Log(line);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[admin-console] link: exception during replay");
        }
        g_linked = true;
    }

    DWORD WINAPI AutoInitWorker(void*)
    {
        // Opening this window (even hidden) works reliably only once the
        // game itself is well past boot and has its own message loop
        // running - tried right after sub_F44770 returns (still inside
        // CClientApp::init) and it came up blank/dead every time, on every
        // thread we tried. So: wait for an explicit request instead of a
        // timing guess - Overlay.cpp fires it the first time F9 is pressed,
        // which only happens once the player is actually in game.
        DWORD wait = WaitForSingleObject(g_openRequestEvent, INFINITE);
        if (wait != WAIT_OBJECT_0) return 0;
        if (g_adminCommandCount == 0) return 0;

        void* object = (void*)S4(kConsoleObject);
        auto init = (InitFn)S4(kConsoleInit);
        if (!IsReadable((uintptr_t)object) || !IsReadable((uintptr_t)init)) {
            Log("[admin-console] auto-init: offsets invalid");
            return 0;
        }

        __try {
            init(object, nullptr, "console");
            g_consoleReady = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[admin-console] auto-init: exception opening console");
            return 0;
        }

        g_consoleHwnd = FindWindowA(nullptr, "console");
        if (g_consoleHwnd) {
            EnumChildWindows(g_consoleHwnd, FindInputEdit, (LPARAM)&g_consoleInputEdit);
            // left visible on purpose - real-time feedback/log, same as before.
            // Whatever position the game picks for this window can land
            // mostly or entirely off-screen depending on the desktop layout -
            // pin it near the corner so it's always fully visible.
            SetWindowPos(g_consoleHwnd, nullptr, 20, 20, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        {
            char line[160];
            RECT rc{};
            if (g_consoleHwnd) GetWindowRect(g_consoleHwnd, &rc);
            wsprintfA(line, "[admin-console] consoleHwnd=%p inputEdit=%p outputEdit=%p rect=(%d,%d)-(%d,%d) %dx%d",
                      g_consoleHwnd, g_consoleInputEdit, g_consoleOutputEdit,
                      rc.left, rc.top, rc.right, rc.bottom, rc.right - rc.left, rc.bottom - rc.top);
            Log(line);
        }

        LinkCapturedCommands();

        // this thread also has to keep pumping messages for the (hidden)
        // console window, or Windows will mark it not-responding.
        for (;;) {
            MSG msg{};
            while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            Sleep(15);
        }
    }
}

void InstallAdminConsoleCapture()
{
    if (!IsAuthorizedUser()) return; // nothing installed at all for anyone else

    original_insert_command = reinterpret_cast<InsertCommandFn>(S4(kInsertCommand));
    original_big_command_register = reinterpret_cast<BigCommandRegisterFn>(S4(kBigCommandRegister));

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&original_insert_command), reinterpret_cast<PVOID>(InsertCommandHook));
    DetourAttach(reinterpret_cast<PVOID*>(&original_big_command_register), reinterpret_cast<PVOID>(BigCommandRegisterHook));
    const LONG result = DetourTransactionCommit();
    Log(result == NO_ERROR ? "[admin-console] capture hooks installed"
                           : "[admin-console] capture hooks install FAILED");

    InstallActorSearchFix();
    InstallTestForceFix();
    // InstallReloadScriptModeCheckPatch() tried and reverted: the mode check
    // it bypasses is a real guard, not a bug - removing it just traded a
    // silent no-op outside a scenario for a hard crash (null deref, the
    // reload logic needs scenario-only state). Use reloadscr unpatched,
    // while actually inside a scenario/arcade session.

    g_openRequestEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    CreateThread(nullptr, 0, AutoInitWorker, nullptr, 0, nullptr);
}

// Overlay.cpp calls this the first time F9 is pressed. Safe to call more
// than once - the event only needs setting once.
void RequestAdminConsoleOpen()
{
    if (g_openRequestEvent) SetEvent(g_openRequestEvent);
}

bool AdminConsoleLinked() { return g_linked; }

// Types text into the (hidden) native console's input box and submits it as
// Enter, so execution goes through the game's own real command dispatch -
// we don't have to reverse it ourselves.
// Mirrors the (hidden) console's real output log into caller's buffer.
int AdminConsoleReadOutput(char* buf, int bufSize)
{
    if (!buf || bufSize <= 0) return 0;
    buf[0] = 0;
    if (!g_consoleOutputEdit) return 0;

    char cls[32]{};
    GetClassNameA(g_consoleOutputEdit, cls, sizeof(cls));
    if (lstrcmpiA(cls, "LISTBOX") == 0) {
        int count = (int)SendMessageA(g_consoleOutputEdit, LB_GETCOUNT, 0, 0);
        int used = 0;
        int first = count > 40 ? count - 40 : 0; // each SendMessage crosses threads - keep it bounded
        for (int i = first; i < count; ++i) {
            char item[256];
            int len = (int)SendMessageA(g_consoleOutputEdit, LB_GETTEXT, i, (LPARAM)item);
            if (len <= 0) continue;
            if (used + len + 1 >= bufSize) break;
            memcpy(buf + used, item, len);
            used += len;
            buf[used++] = '\n';
        }
        buf[used] = 0;
        return used;
    }
    return GetWindowTextA(g_consoleOutputEdit, buf, bufSize);
}

bool AdminConsoleSubmit(const char* text)
{
    char line[192];
    wsprintfA(line, "[admin-console] submit target=%p text=\"%s\"", g_consoleInputEdit, text ? text : "");
    Log(line);
    if (!g_consoleInputEdit || !text) return false;

    // WM_SETTEXT is fast (a few characters) so it's fine as a blocking
    // SendMessage. The Enter keystroke is not: it triggers the actual
    // command execution on the worker thread, which can take a while (or
    // itself wait on the game's own thread), and SendMessage would freeze
    // our render thread for as long as that takes. Post those instead - the
    // render thread never waits on them.
    SetWindowTextA(g_consoleInputEdit, text);
    SendMessageA(g_consoleInputEdit, EM_SETSEL, (WPARAM)lstrlenA(text), (LPARAM)lstrlenA(text));
    PostMessageA(g_consoleInputEdit, WM_KEYDOWN, VK_RETURN, 0x1C0001);
    PostMessageA(g_consoleInputEdit, WM_CHAR, VK_RETURN, 0x1C0001);
    PostMessageA(g_consoleInputEdit, WM_KEYUP, VK_RETURN, (LPARAM)0xC01C0001);
    return true;
}
