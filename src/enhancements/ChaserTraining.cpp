#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <detours.h>
#include <intrin.h>
#include <new>

static uintptr_t g_s4_base = 0;

static uintptr_t s4_base() {
    if (!g_s4_base)
        g_s4_base = (uintptr_t)GetModuleHandleW(nullptr);
    return g_s4_base;
}

#define S4(va) (s4_base() + ((va) - 0x00E80000u))

using hard_map_parser_t = void(__thiscall*)(void*, void*);
static hard_map_parser_t original_hard_map_parser = nullptr;

using map_index_t = int(__thiscall*)(void*, int);
static map_index_t original_map_index = nullptr;
using update_map_t = void(__thiscall*)(void*, int);
static update_map_t original_update_map = nullptr;
using map_string_t = char*(__thiscall*)(void*, int);
static map_string_t original_map_image = nullptr;
static map_string_t original_map_name = nullptr;
using map_mode_lookup_t = uint32_t(__thiscall*)(void*, int, int);
static map_mode_lookup_t original_map_mode_lookup = nullptr;
using map_mode_string_t = void*(__thiscall*)(void*, int, int);
static map_mode_string_t original_map_mode_string = nullptr;
static map_mode_string_t original_map_mode_info = nullptr;
using game_mode_lookup_t = void*(__thiscall*)(void*, int);
static game_mode_lookup_t original_game_mode_lookup = nullptr;
using ai_register_t = void(__thiscall*)(void*, int*);
static ai_register_t original_ai_register = nullptr;
using ai_command_t = void(__thiscall*)(void*, int);
static ai_command_t original_ai_command = nullptr;
// TrainingSpawnAI_pathA/pathB: vtable slot+0 of two Training dialog classes, each ends up
// calling Lua CreateAIActor via FUN_01103390/FUN_01103920. Zero direct callers found in Ghidra
// (called through a vtable) -- hooked directly here to see if either ever fires in a real
// Chaser Training match, since the downstream ai_register/ai_command hooks never fire.
using training_spawn_t = void(__thiscall*)(void*);
static training_spawn_t original_training_spawn_a = nullptr;
static training_spawn_t original_training_spawn_b = nullptr;
using ai_create_t = void*(__cdecl*)(char*, char*);
static ai_create_t original_ai_create = nullptr;
using ai_spawn_many_t = void(__thiscall*)(void*, char*, char*, uint32_t);
static ai_spawn_many_t original_ai_spawn_many = nullptr;
using ai_spawn_one_t = void*(__thiscall*)(void*, char*, char*);
static ai_spawn_one_t original_ai_spawn_one = nullptr;
using room_enter_ack_t = uint8_t(__thiscall*)(void*, void*, void*, int);
static room_enter_ack_t original_room_enter_ack_old = nullptr;
static room_enter_ack_t original_room_enter_ack2_short = nullptr;
using room_enter_ack2_t = uint8_t(__thiscall*)(void*, void*, void*, void*, int);
static room_enter_ack2_t original_room_enter_ack_old2 = nullptr;
static room_enter_ack2_t original_room_enter_ack2 = nullptr;
static bool mode18_fallback_logged = false;
// CTrainingManager singleton accessor: called from every training-UI function we've
// hooked so far (map-index, update-map, mode-lookup...). Logging the caller (return
// address) of every FIRST-SEEN call site tells us which functions actually run during
// a real room flow (TD/DM working vs Chaser broken), without guessing the roster/bot
// fill function's address ahead of time -- diff the caller sets between a TD run and
// a Chaser run afterwards.
using training_manager_get_t = int(__cdecl*)(void);
static training_manager_get_t original_training_manager_get = nullptr;
static void* seen_mgr_callers[512] = {};
static int seen_mgr_caller_count = 0;
static FILE* chaser_log = nullptr;
static char chaser_image[] = "Resources\\mapselect\\Office.tga";
static char chaser_name[] = "Office";
static void trace(const char* format, ...);

static void __fastcall chaser_ai_register(void* list, void*, int* actor) {
    original_ai_register(list, actor);
    trace("[ai] registered list=%p actor=%p\n", list, actor);
}

static void __fastcall chaser_ai_command(void* actor, void*, int args) {
    trace("[ai] command actor=%p args=%p\n", actor, (void*)(uintptr_t)(uint32_t)args);
    original_ai_command(actor, args);
}

static void __fastcall chaser_training_spawn_a(void* self, void*) {
    trace("[spawn-a] called self=%p\n", self);
    original_training_spawn_a(self);
    trace("[spawn-a] returned self=%p\n", self);
}

static void __fastcall chaser_training_spawn_b(void* self, void*) {
    trace("[spawn-b] called self=%p\n", self);
    original_training_spawn_b(self);
    trace("[spawn-b] returned self=%p\n", self);
}

static void* __cdecl chaser_ai_create(char* name, char* type) {
    trace("[ai-create] name=%s type=%s\n", name ? name : "<null>",
          type ? type : "<null>");
    void* actor = original_ai_create(name, type);
    trace("[ai-create] returned actor=%p\n", actor);
    return actor;
}

static void __fastcall chaser_ai_spawn_many(void* self, void*, char* name,
                                             char* type, uint32_t count) {
    trace("[ai-spawn-many] self=%p name=%s type=%s count=%u\n", self,
          name ? name : "<null>", type ? type : "<null>", count);
    original_ai_spawn_many(self, name, type, count);
    trace("[ai-spawn-many] returned self=%p count=%u\n", self, count);
}

static void* __fastcall chaser_ai_spawn_one(void* self, void*, char* name,
                                             char* type) {
    trace("[ai-spawn-one] self=%p name=%s type=%s\n", self,
          name ? name : "<null>", type ? type : "<null>");
    void* actor = original_ai_spawn_one(self, name, type);
    trace("[ai-spawn-one] returned actor=%p\n", actor);
    return actor;
}

static uint8_t __fastcall chaser_room_enter_ack(void* self, void*, void* p1,
                                                 void* p2, int dto) {
    void* caller = _ReturnAddress();
    trace("[room-enter-ack-old] caller=%p rva=%p dto=%p\n", caller,
          (void*)((uintptr_t)caller - s4_base() + 0x00E80000u), (void*)(uintptr_t)dto);
    return original_room_enter_ack_old(self, p1, p2, dto);
}

static uint8_t __fastcall chaser_room_enter_ack2(void* self, void*, void* p1,
                                                  void* p2, void* p3, int dto) {
    void* caller = _ReturnAddress();
    trace("[room-enter-ack2-old] caller=%p rva=%p dto=%p\n", caller,
          (void*)((uintptr_t)caller - s4_base() + 0x00E80000u), (void*)(uintptr_t)dto);
    return original_room_enter_ack_old2(self, p1, p2, p3, dto);
}

static uint8_t __fastcall chaser_room_enter_ack2_short(void* self, void*, void* p1,
                                                        void* p2, int dto) {
    void* caller = _ReturnAddress();
    trace("[room-enter-ack2-short] caller=%p rva=%p dto=%p\n", caller,
          (void*)((uintptr_t)caller - s4_base() + 0x00E80000u), (void*)(uintptr_t)dto);
    return original_room_enter_ack2_short(self, p1, p2, dto);
}

static uint8_t __fastcall chaser_room_enter_ack2_full(void* self, void*, void* p1,
                                                       void* p2, void* p3, int dto) {
    void* caller = _ReturnAddress();
    trace("[room-enter-ack2] caller=%p rva=%p dto=%p\n", caller,
          (void*)((uintptr_t)caller - s4_base() + 0x00E80000u), (void*)(uintptr_t)dto);
    return original_room_enter_ack2(self, p1, p2, p3, dto);
}

// map.x7 registers map 91 under mode 8. Wire rule 18 is the training wrapper;
// keep 18 on the network, but resolve its local map data as mode 8.
static int local_map_mode(int mode, int map_id) {
    return mode == 18 && map_id == 91 ? 8 : mode;
}

static uint32_t __fastcall chaser_map_mode_lookup(void* manager, void*, int mode, int map_id) {
    return original_map_mode_lookup(manager, local_map_mode(mode, map_id), map_id);
}

static void* __fastcall chaser_map_mode_string(void* manager, void*, int mode, int map_id) {
    return original_map_mode_string(manager, local_map_mode(mode, map_id), map_id);
}

static void* __fastcall chaser_map_mode_info(void* manager, void*, int mode, int map_id) {
    return original_map_mode_info(manager, local_map_mode(mode, map_id), map_id);
}

static void register_chaser_game_mode(void* manager) {
    if (!manager || !original_game_mode_lookup)
        return;

    auto* existing = (uint8_t*)original_game_mode_lookup(manager, 18);
    if (existing)
        return;

    auto* source_field = (uint8_t*)original_game_mode_lookup(manager, 8);
    if (!source_field)
        return;

    auto* source = source_field - 4;
    auto* table = (uint8_t*)manager;
    for (int i = 0; i <= 0x15; ++i) {
        auto* slot = table + i * 0x9c;
        auto* mode = (int*)(slot + 4);
        if (*mode != 0)
            continue;
        std::memcpy(slot, source, 0x9c);
        *mode = 18;
        trace("[mode-register] registered mode=18 from mode=8 slot=%d\n", i);
        return;
    }

    if (!mode18_fallback_logged) {
        trace("[mode-register] table full; mode=18 aliases mode=8\n");
        mode18_fallback_logged = true;
    }
}

static void* __fastcall chaser_game_mode_lookup(void* manager, void*, int mode) {
    if (mode == 18) {
        register_chaser_game_mode(manager);
        auto* result = original_game_mode_lookup(manager, 18);
        return result ? result : original_game_mode_lookup(manager, 8);
    }
    return original_game_mode_lookup(manager, mode);
}

static int __cdecl chaser_training_manager_get() {
    void* caller = _ReturnAddress();
    bool known = false;
    for (int i = 0; i < seen_mgr_caller_count; ++i) {
        if (seen_mgr_callers[i] == caller) { known = true; break; }
    }
    if (!known && seen_mgr_caller_count < 512) {
        seen_mgr_callers[seen_mgr_caller_count++] = caller;
        trace("[mgr-get] new caller=%p (rva=%p)\n", caller,
              (void*)((uintptr_t)caller - s4_base() + 0x00E80000u));
    }
    return original_training_manager_get();
}

static void trace(const char* format, ...) {
    (void)format;
}

static void open_chaser_console() {
}

static void dump_map_vector(void* manager, int mode) {
    auto* vector = (uint8_t*)manager + 0x190 + mode * 0x0c;
    auto* begin = *(uint8_t**)vector;
    auto* end = *(uint8_t**)(vector + 4);
    const ptrdiff_t bytes = end - begin;
    if (!begin || !end || end < begin || bytes % 0x28 != 0 || bytes > 0x28 * 64) {
        trace("[map-list] mode=%d invalid begin=%p end=%p\n", mode, begin, end);
        return;
    }
    trace("[map-list] mode=%d count=%td begin=%p end=%p records=",
          mode, bytes / 0x28, begin, end);
    for (auto* record = begin; record != end; record += 0x28)
        trace("id=%u rule=%u active=%u;",
              *(uint32_t*)record, *(uint32_t*)(record + 0x20), record[0x24]);
    trace("\n");
}

static void create_chaser_map_record(void* manager) {
    auto* touchdown = (uint8_t*)manager + 0x190;
    auto* chaser = (uint8_t*)manager + 0x1a8;
    if (*(uint8_t**)chaser || *(uint8_t**)(chaser + 4))
        return;

    auto* source = *(uint8_t**)touchdown;
    auto* source_end = *(uint8_t**)(touchdown + 4);
    if (!source || !source_end || source_end - source < 0x28)
        return;

    auto* record = static_cast<uint8_t*>(::operator new(0x28, std::nothrow));
    if (!record)
        return;
    std::memcpy(record, source, 0x28);
    *(uint32_t*)record = 91;          // Office: Chaser training map
    *(uint32_t*)(record + 0x20) = 18; // Chaser training rule
    record[0x24] = 1;

    *(uint8_t**)chaser = record;
    *(uint8_t**)(chaser + 4) = record + 0x28;
    *(uint8_t**)(chaser + 8) = record + 0x28;
}

static int __fastcall chaser_map_index(void* training_manager, void*, int page) {
    const bool chaser = training_manager &&
        *(int*)((uint8_t*)training_manager + 0x1d0) == 2;
    if (chaser)
        create_chaser_map_record(training_manager);
    const int old_mode = chaser ? 2 : 0;
    if (chaser)
        *(int*)((uint8_t*)training_manager + 0x1d0) = 0;
    const int result = original_map_index(training_manager, page);
    if (chaser)
        *(int*)((uint8_t*)training_manager + 0x1d0) = old_mode;
    trace("[map-index] manager=%p page=%d result=%d type=%d mode=%d\n",
          training_manager, page, result,
          training_manager ? *(int*)((uint8_t*)training_manager + 0x1cc) : -1,
          training_manager ? *(int*)((uint8_t*)training_manager + 0x1d0) : -1);
    if (training_manager && *(int*)((uint8_t*)training_manager + 0x1cc) == 3) {
        dump_map_vector(training_manager, 0);
        dump_map_vector(training_manager, 1);
        dump_map_vector(training_manager, 2);
    }
    if (result == -1 && page == 0 && training_manager &&
        *(int*)((uint8_t*)training_manager + 0x1d0) == 2)
        return 0;
    return result;
}

static void __fastcall chaser_update_map(void* manager, void*, int page) {
    trace("[update-map] manager=%p page=%d mode=%d\n", manager, page,
          manager ? *(int*)((uint8_t*)manager + 0x1d0) : -1);
    if (manager && *(int*)((uint8_t*)manager + 0x1d0) == 2) {
        const int old_mode = *(int*)((uint8_t*)manager + 0x1d0);
        *(int*)((uint8_t*)manager + 0x1d0) = 0;
        original_update_map(manager, page);
        *(int*)((uint8_t*)manager + 0x1d0) = old_mode;
        return;
    }
    original_update_map(manager, page);
}

static char* __fastcall chaser_map_image(void* manager, void*, int index) {
    trace("[map-image] manager=%p index=%d mode=%d\n", manager, index,
          manager ? *(int*)((uint8_t*)manager + 0x1d0) : -1);
    if (manager && *(int*)((uint8_t*)manager + 0x1d0) == 2) {
        trace("[map-image] returned=%s\n", chaser_image);
        return chaser_image;
    }
    return original_map_image(manager, index);
}

static char* __fastcall chaser_map_name(void* manager, void*, int index) {
    trace("[map-name] manager=%p index=%d mode=%d\n", manager, index,
          manager ? *(int*)((uint8_t*)manager + 0x1d0) : -1);
    if (manager && *(int*)((uint8_t*)manager + 0x1d0) == 2) {
        trace("[map-name] returned=%s\n", chaser_name);
        return chaser_name;
    }
    return original_map_name(manager, index);
}

static void __fastcall enable_chaser_training_map(void* training_manager, void*, void* xml) {
    original_hard_map_parser(training_manager, xml);

    // FUN_018b4d10 stores Hard_Map_Info pointers in this+0x1c.
    // Rule 18 is Chaser training; the shipped entry is inactive.
    auto** begin = *(uint32_t***)(static_cast<uint8_t*>(training_manager) + 0x1c);
    auto** end = *(uint32_t***)(static_cast<uint8_t*>(training_manager) + 0x20);
    if (!begin || !end || end < begin)
        return;

    trace("[map-parser] records=%td\n", end - begin);
    for (auto** it = begin; it != end; ++it) {
        uint32_t* record = *it;
        if (record)
            trace("[map-parser] id=%u rule=%u active=%u\n",
                  record[0], record[8], *(uint8_t*)((uint8_t*)record + 36));
        if (record && record[8] == 18)
            *(uint8_t*)((uint8_t*)record + 36) = 1;
    }
}

void StartChaserTrainingTab() {
    open_chaser_console();
    // FUN_01c69ec0: force only the slaughter training entry enabled.
    // Original bytes at 01c6a37b: MOVZX EDX,AL.
    uint8_t* patch = (uint8_t*)S4(0x01c6a37b);
    const uint8_t expected[] = { 0x0F, 0xB6, 0xD0 };
    const uint8_t force_enabled[] = { 0x31, 0xD2, 0x42 }; // XOR EDX,EDX; INC EDX

    if (memcmp(patch, expected, sizeof(expected)) == 0) {
        DWORD oldProtect = 0;
        if (VirtualProtect(patch, sizeof(force_enabled), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(patch, force_enabled, sizeof(force_enabled));
            FlushInstructionCache(GetCurrentProcess(), patch, sizeof(force_enabled));
            VirtualProtect(patch, sizeof(force_enabled), oldProtect, &oldProtect);
        }
    }

    original_hard_map_parser = (hard_map_parser_t)S4(0x018b4d10);
    original_map_index = (map_index_t)S4(0x018af920);
    original_update_map = (update_map_t)S4(0x0177d080);
    original_map_image = (map_string_t)S4(0x018afd20);
    original_map_name = (map_string_t)S4(0x018afde0);
    original_map_mode_lookup = (map_mode_lookup_t)S4(0x01a8eb80);
    original_map_mode_string = (map_mode_string_t)S4(0x01a8fef0);
    original_map_mode_info = (map_mode_string_t)S4(0x01a8f580);
    original_game_mode_lookup = (game_mode_lookup_t)S4(0x01a8ea90);
    // AI path observed in the client: FUN_01163740 registers an actor;
    // FUN_010FE9A0 dispatches its Command() state.
    original_ai_register = (ai_register_t)S4(0x01163740);
    original_ai_command = (ai_command_t)S4(0x010fe9a0);
    original_training_spawn_a = (training_spawn_t)S4(0x01183720);
    original_training_spawn_b = (training_spawn_t)S4(0x01181c20);
    original_ai_create = (ai_create_t)S4(0x01103a60);
    original_ai_spawn_many = (ai_spawn_many_t)S4(0x01103920);
    original_ai_spawn_one = (ai_spawn_one_t)S4(0x01103390);
    original_room_enter_ack_old = (room_enter_ack_t)S4(0x019be2e0);
    original_room_enter_ack_old2 = (room_enter_ack2_t)S4(0x019be390);
    original_room_enter_ack2_short = (room_enter_ack_t)S4(0x019c9480);
    original_room_enter_ack2 = (room_enter_ack2_t)S4(0x019c9530);
    original_training_manager_get = (training_manager_get_t)S4(0x013280a0);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)original_hard_map_parser, (PVOID)enable_chaser_training_map);
    DetourAttach(&(PVOID&)original_map_index, (PVOID)chaser_map_index);
    DetourAttach(&(PVOID&)original_update_map, (PVOID)chaser_update_map);
    DetourAttach(&(PVOID&)original_map_image, (PVOID)chaser_map_image);
    DetourAttach(&(PVOID&)original_map_name, (PVOID)chaser_map_name);
    DetourAttach(&(PVOID&)original_map_mode_lookup, (PVOID)chaser_map_mode_lookup);
    DetourAttach(&(PVOID&)original_map_mode_string, (PVOID)chaser_map_mode_string);
    DetourAttach(&(PVOID&)original_map_mode_info, (PVOID)chaser_map_mode_info);
    DetourAttach(&(PVOID&)original_game_mode_lookup, (PVOID)chaser_game_mode_lookup);
    DetourAttach(&(PVOID&)original_ai_register, (PVOID)chaser_ai_register);
    DetourAttach(&(PVOID&)original_ai_command, (PVOID)chaser_ai_command);
    DetourAttach(&(PVOID&)original_training_spawn_a, (PVOID)chaser_training_spawn_a);
    DetourAttach(&(PVOID&)original_training_spawn_b, (PVOID)chaser_training_spawn_b);
    trace("[hook] ai-spawn-one=%ld ai-spawn-many=%ld ai-create=%ld\n",
          DetourAttach(&(PVOID&)original_ai_spawn_one, (PVOID)chaser_ai_spawn_one),
          DetourAttach(&(PVOID&)original_ai_spawn_many, (PVOID)chaser_ai_spawn_many),
          DetourAttach(&(PVOID&)original_ai_create, (PVOID)chaser_ai_create));
    trace("[hook] room-old=%ld room-old2=%ld room2-short=%ld room2=%ld\n",
          DetourAttach(&(PVOID&)original_room_enter_ack_old, (PVOID)chaser_room_enter_ack),
          DetourAttach(&(PVOID&)original_room_enter_ack_old2, (PVOID)chaser_room_enter_ack2),
          DetourAttach(&(PVOID&)original_room_enter_ack2_short, (PVOID)chaser_room_enter_ack2_short),
          DetourAttach(&(PVOID&)original_room_enter_ack2, (PVOID)chaser_room_enter_ack2_full));
    DetourAttach(&(PVOID&)original_training_manager_get, (PVOID)chaser_training_manager_get);
    DetourTransactionCommit();

}
