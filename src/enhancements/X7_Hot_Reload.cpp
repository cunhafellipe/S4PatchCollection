// X7_Hot_Reload.cpp -- reload a .x7 and have the client re-apply it, no restart.
//
// F7 reloads whatever x7_hot_reload.ini lists, F8 prints every .x7 the client has loaded.
// Verified end to end on Language/message_box.x7: edit the file, repack resource.s4hd, press F7,
// and the popup text changes with the client running.
//
// The three things that make it work, none of which are obvious:
//
//  1. The container index keeps the size each file had AT BOOT. After a repack it no longer
//     matches, and the read is rejected. Patching the je at 01B84FF9 that catches this is NOT
//     the fix, it is the trigger: it lets the reader work the wrong number of bytes and corrupts
//     the heap, which dies by fail-fast without reaching SetUnhandledExceptionFilter -- the
//     "crash with no log". Passing the real file size is the fix.
//
//  2. The parsers do not share a calling convention. sub_1BD8AC0 (item.x7) is a clean
//     __thiscall(this, path); sub_17BC300 (message_box) is __userpurge with its first argument
//     in ESI. Both registers are loaded before calling, and ECX only when the captured value is
//     plausible, because forcing stack garbage into it breaks the ESI kind.
//
//  3. The consumers only insert when the key is absent, so a reload parses correctly and changes
//     nothing. The entry has to be overwritten in place.
//
// Prologue lengths are measured with a disassembler and carried in the table: assuming five
// bytes split sub esp,0x30 in item.x7's parser and left a stray byte running as garbage.

//
// Where the ".x7 reload does nothing" comes from
// ---------------------------------------------
// Calling the resource loader for a data file just hands back a buffer nobody looks at. The
// piece that matters is the function that ASKED for it, because it is also the one that feeds
// the result to whatever consumes it:
//
//   char __thiscall sub_1BD8AC0(void* this, char* path)      // xml/item.x7
//   {
//       res = FUN_01B79480(DAT_25BD094, path, 1, 0);
//       if (!res) return 0;
//       doc = <new xml document>;
//       if (doc->parse(buffer)) {
//           FUN_01B79FB0(res);        // release the resource
//           this->vtbl[2](doc);       // <-- hands the parsed data to its manager
//           doc->release();
//           return 1;
//       }
//   }
//
// So there is no separate manager to hunt down: it is already inside, and the path is a
// parameter, which makes the whole thing re-callable as it stands. Reloading is
// parser(this, path) with the size check bypassed.
//
// Those parsers were found by logging the loader's RETURN ADDRESS per file (see x7_watch), then
// walking back to each function start. Several files share one: 01B5A210 serves every language
// string table, 014407F0 every tooltip. That is why the hooks are keyed by parser address and
// the (path, this) pair is recorded at runtime -- hooking per file would detour the same
// function several times over.
//
// F7 reloads. F6 belongs to the xui reload, so the two can run side by side.
//
// Build: cl /LD /O2 /MT x7_hot_reload.cpp user32.lib /Fe:x7_hot_reload.dll -> rename to .asi

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

namespace {

const uintptr_t GH        = 0x00E80000;
const uintptr_t kSizeCheck = 0x01B84FF9;   // the je that rejects a repacked file

uintptr_t g_base = 0;
bool  g_checkPatched = false;
volatile LONG g_pending = 0;
HWND    g_window = nullptr;
WNDPROC g_oldWndProc = nullptr;

void Log(const char* fmt, ...)
{
    char line[512];
    va_list a; va_start(a, fmt); wvsprintfA(line, fmt, a); va_end(a);
    lstrcatA(line, "\r\n");
    OutputDebugStringA(line);

    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) lstrcpyA(slash + 1, "x7_hot_reload.log"); else lstrcpyA(path, "x7_hot_reload.log");
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w; SetFilePointer(f, 0, nullptr, FILE_END);
    WriteFile(f, line, lstrlenA(line), &w, nullptr);
    CloseHandle(f);
}

uintptr_t Trampoline(uintptr_t fn, int len)
{
    uint8_t* t = (uint8_t*)VirtualAlloc(0, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    memcpy(t, (void*)fn, len);
    t[len] = 0xE9;
    *(int32_t*)(t + len + 1) = (int32_t)((fn + len) - (uintptr_t)(t + len + 5));
    return (uintptr_t)t;
}

void Detour(uintptr_t at, void* dst, int len)
{
    DWORD old;
    VirtualProtect((void*)at, len, PAGE_EXECUTE_READWRITE, &old);
    *(uint8_t*)at = 0xE9;
    *(int32_t*)(at + 1) = (int32_t)((uint8_t*)dst - (uint8_t*)(at + 5));
    for (int i = 5; i < len; ++i) *(uint8_t*)(at + i) = 0x90;
    VirtualProtect((void*)at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, len);
}

typedef char(__fastcall* tParser)(void*, void*, char*);

// file -> parser, derived from the loader return addresses (see x7_watch). Only the rows the
// ini asks for get hooked: assuming all 77 share sub_1BD8AC0's __thiscall(this, char*) shape is
// what crashed the client on the way up, with a misaligned ESP in one of the trampolines. That
// signature is verified for xml/item.x7 and nothing else.
struct Known { const char* file; uintptr_t parser; int prologue; };
const Known kParsers[] = {
    { "language/_eu_default_option.x7", 0x0158E150, 5 },
    { "Language/_eu_ingame_loading_key.x7", 0x013D9E60, 5 },
    { "Language/arcade_text.x7", 0x01158DD0, 5 },
    { "Language/arcade_text.x7", 0x01353F30, 5 },
    { "Language/autonomic_regulation_messages.x7", 0x01188510, 5 },
    { "Language/ban_reason_text.x7", 0x011C5980, 5 },
    { "Language/club_string.x7", 0x01B5A210, 6 },
    { "Language/club_string.x7", 0x0123DAC0, 5 },
    { "Language/conquest_text.x7", 0x01158DD0, 5 },
    { "Language/develop_social.x7", 0x0158ED70, 5 },
    { "Language/message_box.x7", 0x017BC300, 5 },
    { "Language/respawn_guides.x7", 0x013CC600, 5 },
    { "Language/script/defence_char_string_table.x7", 0x01B5A210, 6 },
    { "language/static_string_table.x7", 0x01B29FF0, 6 },
    { "language/xml/arcade_item_string_table.x7", 0x01B5A210, 6 },
    { "language/xml/cardinfo_string_table.x7", 0x01B5A210, 6 },
    { "language/xml/challenge_arcade_string_table.x7", 0x01B5A210, 6 },
    { "Language/xml/club_create_string_table.x7", 0x01B5A210, 6 },
    { "Language/xml/club_fix_string.x7", 0x0123D580, 5 },
    { "language/xml/default_item_string_table.x7", 0x01B5A210, 6 },
    { "Language/xml/default_option_string_table.x7", 0x01B5A210, 6 },
    { "language/xml/experience_string_table.x7", 0x01B5A210, 6 },
    { "Language/xml/game_help_string_table.x7", 0x01B5A210, 6 },
    { "language/xml/gameinfo_string_table.x7", 0x01B5A210, 6 },
    { "language/xml/item_effect_string_table.x7", 0x01B5A210, 6 },
    { "language/xml/iteminfo_string_table.x7", 0x01B5A210, 6 },
    { "language/xml/master_experience_string_table.x7", 0x01B5A210, 6 },
    { "language/xml/randomshopinfo_string_table.x7", 0x01B5A210, 6 },
    { "language/xml/tooltipinfo_string_table.x7", 0x01B5A210, 6 },
    { "language\font_config.x7", 0x00F549B0, 6 },
    { "Resources/Intro/_eu_intro.x7", 0x013229A0, 5 },
    { "xml/_eu_card_system_info.x7", 0x01212440, 5 },
    { "xml/_eu_club_create_info.x7", 0x0123D1E0, 5 },
    { "xml/_eu_coin_info.x7", 0x01B93AA0, 5 },
    { "xml/_eu_combination_info.x7", 0x01C31990, 5 },
    { "xml/_eu_decomposition_info.x7", 0x01C38F90, 5 },
    { "xml/_eu_game_help.x7", 0x014289A0, 5 },
    { "xml/_eu_gameinfo.x7", 0x01A91E30, 5 },
    { "xml/_eu_item_tooltip_addcapsule.x7", 0x014407F0, 6 },
    { "xml/_eu_item_tooltip_base.x7", 0x014407F0, 6 },
    { "xml/_eu_item_tooltip_weapongraph.x7", 0x014407F0, 6 },
    { "xml/_eu_make_Character_info.x7", 0x014A53D0, 5 },
    { "xml/_eu_promotion_info.x7", 0x00F1A500, 5 },
    { "xml/_eu_reward_conquest.x7", 0x01C47730, 6 },
    { "xml/_rc_promotion_info.x7", 0x012887B0, 5 },
    { "xml/arcade_item.x7", 0x01176A30, 5 },
    { "xml/arcade_reward.x7", 0x0114F410, 5 },
    { "xml/arcade_scenario.x7", 0x01C023A0, 6 },
    { "xml/attackRangeLimit.x7", 0x0108CD90, 5 },
    { "xml/battle_training_center_info.x7", 0x01C65460, 5 },
    { "xml/battle_training_center_info.x7", 0x01C667C0, 5 },
    { "xml/battle_training_center_info.x7", 0x01C68B40, 5 },
    { "xml/battle_training_center_info.x7", 0x01C69EC0, 5 },
    { "xml/book_scenario.x7", 0x012463C0, 5 },
    { "xml/BTCAIActorType.x7", 0x011DE290, 5 },
    { "xml/btcdm_point.x7", 0x011E6D90, 5 },
    { "xml/card_Info.x7", 0x01212890, 5 },
    { "xml/challenge_arcade_list.x7", 0x014A9750, 5 },
    { "xml/challenge_arcade_ranking.x7", 0x014AC8A0, 5 },
    { "xml/challenge_arcade_string.x7", 0x014ABC00, 5 },
    { "xml/combine_element_info.x7", 0x01471000, 5 },
    { "xml/common_game_option.x7", 0x01C1E8C0, 5 },
    { "xml/content_setting.x7", 0x01BA56C0, 5 },
    { "xml/decomposition_element_info.x7", 0x01471370, 5 },
    { "xml/default_item.x7", 0x01467240, 5 },
    { "xml/effect_match_list.x7", 0x01C09030, 5 },
    { "xml/enchant_extractkey.x7", 0x01C1E430, 5 },
    { "xml/equiped_weapon.x7", 0x0145A8D0, 5 },
    { "xml/esper_enchant_price.x7", 0x01C207C0, 5 },
    { "xml/EsperAction.x7", 0x0146D4B0, 5 },
    { "xml/experience.x7", 0x01BEA940, 6 },
    { "xml/game_tempos.x7", 0x01332560, 5 },
    { "xml/ghost_trail.x7", 0x01335C60, 5 },
    { "xml/item.x7", 0x01BD8AC0, 6 },
    { "xml/item_grade.x7", 0x01453AD0, 5 },
    { "xml/item_super_rare_set.x7", 0x01475030, 5 },
    { "xml/item_tooltip_addcontrol.x7", 0x014407F0, 6 },
    { "xml/item_tooltip_addfeature.x7", 0x014407F0, 6 },
    { "xml/keycount.x7", 0x0145BEF0, 5 },
    { "xml/map.x7", 0x01A93550, 5 },
    { "xml/master_experience.x7", 0x01BEA940, 6 },
    { "xml/mission.x7", 0x01C3DE60, 5 },
    { "xml/mission.x7", 0x01C3E6B0, 5 },
    { "xml/mission.x7", 0x01C3EF00, 5 },
    { "xml/mission.x7", 0x01C41B60, 5 },
    { "xml/mission.x7", 0x01C44150, 5 },
    { "xml/mission.x7", 0x01C42FA0, 5 },
    { "xml/mission.x7", 0x01C451F0, 5 },
    { "xml/mission_list.x7", 0x01BF8180, 5 },
    { "xml/parse_string.x7", 0x00F59290, 5 },
    { "xml/randomshop_ani_info.x7", 0x0187D6F0, 5 },
    { "xml/randomshop_package.x7", 0x015D8290, 5 },
    { "xml/Stadium_info.x7", 0x01C21D00, 5 },
    { "xml/TimeDelayInfo.x7", 0x015EF540, 5 },
    { "xml/Training_Info.x7", 0x018B41E0, 5 },
    { "xml/tran_effect_Item_Level.x7", 0x01C25600, 5 },
    { "xml/weapon_attach.x7", 0x016738D0, 5 },
    { "xml/weapon_lua.x7", 0x0145AD00, 5 },
};
const int kNParsers = (int)(sizeof(kParsers) / sizeof(kParsers[0]));
void* g_orig[kNParsers];

// One row per file actually seen, filled from the hooks: which parser handled it, and with
// which `this`. Fixed size and no allocation - this runs inside the parser.
struct Seen { char path[96]; uintptr_t parser; void* self; unsigned esi; };
Seen g_seen[256];
int  g_nSeen = 0;

void Remember(const char* path, uintptr_t parser, void* self, unsigned esi)
{
    if (!path) return;
    for (int i = 0; i < g_nSeen; ++i) {
        if (lstrcmpiA(g_seen[i].path, path) != 0) continue;
        g_seen[i].parser = parser;
        g_seen[i].self = self;
        g_seen[i].esi = esi;
        return;
    }
    if (g_nSeen >= 256) return;
    lstrcpynA(g_seen[g_nSeen].path, path, sizeof(g_seen[0].path));
    g_seen[g_nSeen].parser = parser;
    g_seen[g_nSeen].self = self;
    g_seen[g_nSeen].esi = esi;
    ++g_nSeen;
}

// The parsers do not agree on which register carries their first argument:
//
//   sub_1BD8AC0  __thiscall(this, path)          this in ECX
//   sub_17BC300  __userpurge(a1@<esi>, path)     a1   in ESI
//
// Both take one stack argument and clean it themselves. Reading ECX as `this` for the ESI kind
// produced the nonsense 0019EA27 in the listing, and it is why only the FIRST reload of a
// session worked: on that one ESI still held something usable, and the parse succeeded.
//
// ESI is only READ here, never written: writing it and returning left the caller with a
// clobbered register, since MSVC does not save it around an __asm block. The recording happens
// after the call so nothing sits between the read and the forward.
unsigned g_esi[kNParsers];

#pragma optimize("", off)
template<int I> char __fastcall ParserHook(void* self, void* edx, char* path)
{
    unsigned incoming;
    __asm mov incoming, esi

    char r = ((tParser)g_orig[I])(self, edx, path);

    g_esi[I] = incoming;
    Remember(path, kParsers[I].parser, self, incoming);
    return r;
}
#pragma optimize("", on)

// Both registers are loaded, because each parser ignores the one it does not use.
#pragma optimize("", off)
static char CallParser(uintptr_t fn, void* ecxValue, unsigned esiValue, char* path)
{
    char result = 0;
    // ECX is only forced when the captured value is plausible. For the ESI-style parsers the
    // recorded `this` is stack garbage (0019EA27), and writing that into ECX broke message_box,
    // which had been reloading fine: sub_17BC300 does use ECX -- it calls sub_1B428D0() with no
    // arguments, i.e. __thiscall. Leaving the register alone is what the working build did.
    bool useEcx = ((uintptr_t)ecxValue >= 0x00400000) && !IsBadReadPtr(ecxValue, 4);
    __asm {
        push ebx
        mov  ebx, esi
        mov  esi, esiValue
        cmp  useEcx, 0
        je   skip_ecx
        mov  ecx, ecxValue
    skip_ecx:
        mov  eax, path
        push eax
        mov  eax, fn
        call eax
        mov  result, al
        mov  esi, ebx
        pop  ebx
    }
    return result;
}
#pragma optimize("", on)

void* const g_hooks[] = {
    &ParserHook<0>,
    &ParserHook<1>,
    &ParserHook<2>,
    &ParserHook<3>,
    &ParserHook<4>,
    &ParserHook<5>,
    &ParserHook<6>,
    &ParserHook<7>,
    &ParserHook<8>,
    &ParserHook<9>,
    &ParserHook<10>,
    &ParserHook<11>,
    &ParserHook<12>,
    &ParserHook<13>,
    &ParserHook<14>,
    &ParserHook<15>,
    &ParserHook<16>,
    &ParserHook<17>,
    &ParserHook<18>,
    &ParserHook<19>,
    &ParserHook<20>,
    &ParserHook<21>,
    &ParserHook<22>,
    &ParserHook<23>,
    &ParserHook<24>,
    &ParserHook<25>,
    &ParserHook<26>,
    &ParserHook<27>,
    &ParserHook<28>,
    &ParserHook<29>,
    &ParserHook<30>,
    &ParserHook<31>,
    &ParserHook<32>,
    &ParserHook<33>,
    &ParserHook<34>,
    &ParserHook<35>,
    &ParserHook<36>,
    &ParserHook<37>,
    &ParserHook<38>,
    &ParserHook<39>,
    &ParserHook<40>,
    &ParserHook<41>,
    &ParserHook<42>,
    &ParserHook<43>,
    &ParserHook<44>,
    &ParserHook<45>,
    &ParserHook<46>,
    &ParserHook<47>,
    &ParserHook<48>,
    &ParserHook<49>,
    &ParserHook<50>,
    &ParserHook<51>,
    &ParserHook<52>,
    &ParserHook<53>,
    &ParserHook<54>,
    &ParserHook<55>,
    &ParserHook<56>,
    &ParserHook<57>,
    &ParserHook<58>,
    &ParserHook<59>,
    &ParserHook<60>,
    &ParserHook<61>,
    &ParserHook<62>,
    &ParserHook<63>,
    &ParserHook<64>,
    &ParserHook<65>,
    &ParserHook<66>,
    &ParserHook<67>,
    &ParserHook<68>,
    &ParserHook<69>,
    &ParserHook<70>,
    &ParserHook<71>,
    &ParserHook<72>,
    &ParserHook<73>,
    &ParserHook<74>,
    &ParserHook<75>,
    &ParserHook<76>,
    &ParserHook<77>,
    &ParserHook<78>,
    &ParserHook<79>,
    &ParserHook<80>,
    &ParserHook<81>,
    &ParserHook<82>,
    &ParserHook<83>,
    &ParserHook<84>,
    &ParserHook<85>,
    &ParserHook<86>,
    &ParserHook<87>,
    &ParserHook<88>,
    &ParserHook<89>,
    &ParserHook<90>,
    &ParserHook<91>,
    &ParserHook<92>,
    &ParserHook<93>,
    &ParserHook<94>,
    &ParserHook<95>,
    &ParserHook<96>,
    &ParserHook<97>,
};

char g_wanted[8192];

void LoadWantedList()
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) lstrcpyA(slash + 1, "x7_hot_reload.ini"); else lstrcpyA(path, "x7_hot_reload.ini");

    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        static const char* seed =
            "# One .x7 per line, exactly as it appears in the log.\r\n"
            "# A line containing only an asterisk reloads everything seen so far, which is a\r\n"
            "# bad idea: some of these feed global structures other objects already point at.\r\n"
            "xml/item.x7\r\n";
        f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w; WriteFile(f, seed, lstrlenA(seed), &w, nullptr); CloseHandle(f);
        }
        lstrcpyA(g_wanted, "xml/item.x7\n");
        return;
    }
    DWORD read = 0;
    ReadFile(f, g_wanted, sizeof(g_wanted) - 1, &read, nullptr);
    g_wanted[read] = 0;
    CloseHandle(f);
}

bool Wanted(const char* file)
{
    const char* p = g_wanted;
    size_t n = strlen(file);
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (*p == 0) break;
        if (*p == '#' || *p == ';') { while (*p && *p != '\n') ++p; continue; }
        const char* start = p;
        while (*p && *p != '\r' && *p != '\n') ++p;
        size_t len = (size_t)(p - start);
        while (len && (start[len - 1] == ' ' || start[len - 1] == '\t')) --len;
        if (len == 1 && start[0] == '*') return true;
        if (len == n && _strnicmp(start, file, n) == 0) return true;
    }
    return false;
}

// Repacking changes the file but not the size the container index remembers, so the read is
// rejected unless the edit happens to keep the exact same length. Bypassed only while reloading.
void ForceSizeCheck(bool off)
{
    uint8_t* q = (uint8_t*)(g_base + (kSizeCheck - GH));
    DWORD old;
    __try {
        if (off) {
            if (q[0] != 0x74) return;
            VirtualProtect(q, 1, PAGE_EXECUTE_READWRITE, &old);
            q[0] = 0xEB;
            VirtualProtect(q, 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), q, 1);
            g_checkPatched = true;
        } else if (g_checkPatched) {
            VirtualProtect(q, 1, PAGE_EXECUTE_READWRITE, &old);
            q[0] = 0x74;
            VirtualProtect(q, 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), q, 1);
            g_checkPatched = false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Which size no longer matches the file
// -------------------------------------
// The backing file under _resources is COMPRESSED. A one-character edit leaves the uncompressed
// size alone -- which is why expected == read and the size check passes -- but it does change
// the compressed length, and that still comes from the index built at boot:
//
//   sub_1B84DA0(entry, ..., entry[0x108], 0)   // the size the index remembers
//       sub_1B8E590(&size, dst, capacity)      // decompresses
//           sub_1B7C760(dst, size, scratch, &size)
//
// Handing the decompressor a source length that no longer matches the file reads or writes past
// the buffer, which corrupts the heap. That explains all three symptoms at once: identical
// content reloads forever, any edit kills the client, and nothing is logged because a heap
// fail-fast never reaches SetUnhandledExceptionFilter.
//
// So this dumps the entry fields around the hash (+0x100) and the size (+0x108) next to the real
// size of the file on disk. Whichever field stopped matching after a repack is the one to fix,
// and then the fix is to write the correct value rather than to bypass the je.
const uintptr_t kOpen = 0x01B84DA0;   // 55 8B EC 6A FF -- five bytes
typedef char(__fastcall* tOpen)(void*, void*, char*, char*, unsigned, int);
tOpen o_open = nullptr;
volatile LONG g_probe = 0;

// Where the correct size comes from
// ---------------------------------
// The container never has to be parsed for this. The client's own decompressing read reports the
// true size on its way out:
//
//   bool sub_1B8E590(rsize_t *a1, void *Destination, unsigned a3)
//   {
//     if (a3 && a3 < 0xFA000) {              // entries at or above 1,024,000 are read raw
//       DestinationSize = *a1;
//       v6 = sub_1B7C760(Destination, DestinationSize, Source, &DestinationSize) == 0;
//       *a1 = DestinationSize;               // <-- the decompressed size, handed back
//       ...
//
// That threshold is also why message_box behaves differently from item.x7: at 1,155,004 bytes it
// is over the line and never goes through LZO, so writing bytes into it without changing its
// length keeps the recorded size valid. item.x7, at 834,504, is under the line and compressed,
// so once ItemManager rewrites the entry the recorded size no longer describes it.
//
// The prologue here is SIX bytes: 55 8B EC 83 EC 10. Copying five splits the sub esp,0x10 and
// leaves the tail running as garbage, which is what jumped to 00E00008 last night.
const uintptr_t kDecompRead = 0x01B8E590;
typedef char(__cdecl* tDecomp)(unsigned*, void*, unsigned);
tDecomp o_decomp = nullptr;
volatile LONG g_trueSize = 0;

char __cdecl decomp_hook(unsigned* pSize, void* dst, unsigned expected)
{
    char r = o_decomp(pSize, dst, expected);
    if (pSize && !IsBadReadPtr(pSize, 4))
        InterlockedExchange(&g_trueSize, (LONG)*pSize);
    return r;
}

unsigned FileSizeOnDisk(const char* rel)
{
    if (!rel) return 0;
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) lstrcpynA(slash + 1, rel, (int)(MAX_PATH - (slash + 1 - path)));
    else lstrcpynA(path, rel, MAX_PATH);

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return 0;
    return fad.nFileSizeLow;
}

char __fastcall open_hook(void* self, void* edx, char* str, char* file, unsigned size, int mode)
{
    // No size is forced here any more. The record's own value is correct: the client never
    // repacks, so record+0x108 still holds the size that entry has always had. An earlier version
    // overrode it with the size of the blob on disk, which is a different quantity entirely --
    // for a stored entry the two happen to be equal, which is the only reason that ever appeared
    // to work.
    // The index record is the SECOND argument, not the first. The loader calls
    //
    //     sub_1B84DA0(v15, v31, name, *((_DWORD *)v31 + 66), 0)
    //
    // where v15 is a freshly built reader and v31 is the record sub_1B83840(path) looked up.
    // The size it passes is v31[66], i.e. record+0x108, and the hash that names the extracted
    // _resources/<hash> file sits at record+0x100. Dumping `self` was dumping the new reader,
    // which is why the size field kept reading back as zero.
    if (g_probe && str) {
        __try {
            unsigned* rec = (unsigned*)str;
            unsigned onDisk = FileSizeOnDisk(file);
            Log("   record %08X  file=\"%s\"  blob on disk=%u", (unsigned)(uintptr_t)str,
                file ? file : "(null)", onDisk);
            Log("   arg size=%u  mode=%d", size, mode);
            for (unsigned off = 0xF8; off <= 0x114; off += 4) {
                unsigned v = rec[off / 4];
                const char* mark = "";
                if (off == 0x100)     mark = "   <== hash, low half";
                else if (off == 0x104) mark = "   <== hash, high half";
                else if (off == 0x108) mark = "   <== the size the loader passes";
                else if (v == onDisk)  mark = "   <== matches the blob on disk";
                Log("      +0x%03X  %10u  (%08X)%s", off, v, v, mark);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    InterlockedExchange(&g_trueSize, 0);
    char r = o_open(self, edx, str, file, size, mode);

    // A refusal means the recorded size no longer describes the entry. The decompressor has just
    // said how big it really is, so the record is corrected and the open repeated. This is the
    // opposite of patching the je: the check still runs, it simply gets told the truth. The
    // record is the one the loader itself will read again (it passes record+0x108), so a single
    // write here fixes every later load of this file too.
    // The decompressor's report is NOT a safe source for this. It is the output of a read that
    // may itself have stopped early, so feeding it back into record+0x108 drifts the value
    // downward on every reload -- 869852, 869792, 869788 -- until the entry is short of the real
    // 869824 and the client refuses the data outright ("item loading failed"). The correct length
    // is the one in the container index, which ItemManager writes correctly; decoding the entry
    // offline gives the complete XML, 5639 items, ending in </itemlist>. It has to come from
    // there, not from here.
    if (g_probe && self) {
        __try {
            unsigned* e = (unsigned*)self;
            Log("   open -> %s   buffer %08X  bytes %u", r ? "OK" : "REFUSED", e[7], e[8]);
            if (r && e[7]) {
                char head[49] = { 0 };
                memcpy(head, (void*)e[7], 48);
                for (int i = 0; i < 48; ++i)
                    if ((unsigned char)head[i] < 32 || (unsigned char)head[i] > 126) head[i] = '.';
                Log("   first bytes: %s", head);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return r;
}

// Making message_box actually change on screen
// -------------------------------------------
// The reload reads, decompresses and parses correctly and still shows the old text, because the
// consumer only ever inserts:
//
//   sub_17B9DE0(this, name, text):        __thiscall, ret 8
//       id = sub_17BABD0(name)            // name -> numeric id, ret 0x18 (a string by value)
//       if (map.find(id) != end) return 0 // <-- already there, nothing written
//       ...build and insert...
//
// So the entry is overwritten in place instead. Everything here was read off the disassembly
// rather than the decompiler, which has been wrong about registers and offsets all night: it
// claimed sub_17BA0A0 returned a bool, when it actually ends with
//
//   017BA0F7  mov ecx, [ebp-0xc]    ; the node
//   017BA0FA  add ecx, 0x10         ; the pair
//   017BA103  add eax, 4            ; past the id -> the CMessageData
//
// CMessageData, per sub_17BB330 and sub_17B89D0:
//
//   +0  vtable                +4  refcounted pointer
//   +8  flag                  +12 a 0x40 block
//
// The insert's third argument IS that refcounted pointer already -- sub_17B89D0 does
// *(this+4) = a2 then AddRef -- so nothing needs constructing.
//
// The old reference is deliberately NOT released: the release slot for this class is unverified,
// and getting it wrong corrupts the heap and kills the client with no log, which is the failure
// mode that cost most of tonight. Leaking one reference per reload cannot corrupt anything.
static const uintptr_t kMsgId     = 0x017BABD0;   // name -> id, ret 0x18
static const uintptr_t kMsgInsert = 0x017B9DE0;   // insert-if-absent, ret 8
static const uintptr_t kMsgFind   = 0x017BA0A0;   // id -> CMessageData*, ret 4

typedef int(__stdcall* tMsgId)(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
typedef char(__fastcall* tMsgIns)(void*, void*, char*, void*);
typedef void*(__fastcall* tMsgFind)(void*, void*, unsigned);

static tMsgId  o_msgid = nullptr;
static tMsgIns o_msgins = nullptr;

// The id builder runs inside the insert, so what it returns is the key of the entry the insert
// is about to reject.
__declspec(thread) unsigned g_lastMsgId = 0;

static int __stdcall msgid_hook(unsigned a, unsigned b, unsigned c, unsigned d, unsigned e, unsigned f)
{
    int id = o_msgid(a, b, c, d, e, f);
    g_lastMsgId = (unsigned)id;
    return id;
}

static char __fastcall msgins_hook(void* self, void* edx, char* name, void* text)
{
    char r = o_msgins(self, edx, name, text);
    if (r || !g_probe) return r;      // inserted normally, or not a reload: leave it alone

    __try {
        void* cmd = ((tMsgFind)(g_base + (kMsgFind - GH)))(self, nullptr, g_lastMsgId);
        if (!cmd || IsBadWritePtr(cmd, 0x54)) return r;

        if (text && !IsBadReadPtr(text, 4)) {
            unsigned* vt = *(unsigned**)text;
            if (vt && !IsBadReadPtr(vt, 8) && vt[1])
                ((void(__thiscall*)(void*))vt[1])(text);   // AddRef
        }
        *(void**)((char*)cmd + 4) = text;                  // old ref left behind on purpose
        *(char*)((char*)cmd + 8) = 1;
        memset((char*)cmd + 12, 0, 0x40);
        r = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("   overwrite: exception %08X", (unsigned)GetExceptionCode());
    }
    return r;
}

// Making item.x7 actually change on screen
// ---------------------------------------
// Same shape as message_box: the file parses fine and nothing changes, because the manager only
// inserts. sub_1BD9100 walks <itemlist> calling sub_1BD9450 per <item>, and that ends in a
// std::map insert whose tail is
//
//   if ( v13 >= v8 )        // the key is already in the map
//   {
//     sub_1BDC710(a5);      // the freshly built item is thrown away
//     *(BYTE *)(a2 + 4) = 0;//   and the pair comes back with inserted = false
//   }
//
// so an item that exists keeps whatever it had at boot.
//
// The layout, measured rather than assumed:
//
//   sub_1BDB860   node+12 colour, node+13 isnil, the pair constructed at node+16  (a _Tree node)
//   sub_1BDBFB0   a1[3] = *a3      the value is ONE POINTER, at pair+12
//   sub_F4E670    key = [2] ^ [0] with [0] == ~[1]      the key is 12 guarded bytes
//
//   node + 16 = pair,  node + 16 + 12 = node + 28 = the item pointer
//
// Discarding is safe to work around because sub_1BDC8D0 does not release anything: it pushes the
// node onto a free list (*a1 = *head; *head = a1). The item object itself survives, so its
// pointer can be saved before the call and planted afterwards, with no AddRef needed. As with the
// messages the previous pointer is left behind rather than released -- nothing here is verified
// to be refcounted, and guessing wrong corrupts the heap and kills the client with no log.
const uintptr_t kItemInsert = 0x01BDBBD0;   // 55 8B EC 6A FF -- five bytes lands on a boundary
typedef void*(__fastcall* tItemIns)(void*, void*, void*, int, void*, void*);
tItemIns o_iteminsert = nullptr;
volatile LONG g_replaced = 0;

void* __fastcall iteminsert_hook(void* map, void* edx, void* result, int hint,
                                 void* keyPair, void* newNode)
{
    void* fresh = nullptr;
    if (g_probe && newNode && !IsBadReadPtr((char*)newNode + 28, 4))
        fresh = *(void**)((char*)newNode + 28);

    void* r = o_iteminsert(map, edx, result, hint, keyPair, newNode);

    if (g_probe && fresh && result && !IsBadReadPtr(result, 5)) {
        __try {
            if (*((char*)result + 4) == 0) {          // refused: the key was already there
                void* node = *(void**)result;
                if (node && !IsBadWritePtr((char*)node + 28, 4)) {
                    *(void**)((char*)node + 28) = fresh;
                    InterlockedIncrement(&g_replaced);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return r;
}

// Telling the client the truth about the index
// -------------------------------------------
// Saving from ItemManager rewrites an entry's length AND its checksum, and the checksum is what
// names the extracted blob (_resources/<checksum>). The client read the index once, at startup,
// so it keeps opening the old file at the old size and the read is refused -- or worse, accepted
// at a wrong length.
//
// Neither value can be recomputed inside the client without reimplementing four layers of
// container encryption (swapBlocks -> extractKeys -> decryptVersion2 -> seedCtr), so s4zip is
// asked instead: x7_index_dump.cjs writes name/length/checksum for every entry, and this reads it
// back. An earlier attempt used the size the decompressor reported, which is the output of a read
// that may have stopped early -- it drifted downward on every reload until the data was short and
// the client refused it outright ("item loading failed"). This value is the authoritative one.
//
// sub_1B83840 is where it goes: __thiscall(this, path) returning the record, called by the loader
// BEFORE it builds the blob filename and reads the length out of the record. Patch it here and
// everything downstream picks the new values up on its own. Prologue 55 8B EC 6A FF -- five bytes
// landing exactly on an instruction boundary.
char*  g_index = nullptr;
size_t g_indexBytes = 0;

void LoadIndex()
{
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) *slash = 0;

    char script[MAX_PATH];
    wsprintfA(script, "%s\\x7_index_dump.cjs", dir);
    if (GetFileAttributesA(script) != INVALID_FILE_ATTRIBUTES) {
        char cmd[MAX_PATH * 2];
        wsprintfA(cmd, "node.exe \"%s\"", script);
        STARTUPINFOA si; ZeroMemory(&si, sizeof si); si.cb = sizeof si;
        PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof pi);
        if (CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, dir, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 60000);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        } else {
            Log("could not run node for x7_index_dump.cjs -- using whatever x7_index.txt holds");
        }
    }

    char path[MAX_PATH];
    wsprintfA(path, "%s\\x7_index.txt", dir);
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Log("no x7_index.txt: sizes cannot be refreshed"); return; }

    DWORD size = GetFileSize(f, nullptr);
    if (g_index) { VirtualFree(g_index, 0, MEM_RELEASE); g_index = nullptr; g_indexBytes = 0; }
    g_index = (char*)VirtualAlloc(nullptr, size + 1, MEM_COMMIT, PAGE_READWRITE);
    if (g_index) {
        DWORD got = 0;
        ReadFile(f, g_index, size, &got, nullptr);
        g_index[got] = 0;
        g_indexBytes = got;
    }
    CloseHandle(f);
}

bool LookupEntry(const char* name, unsigned* outLen, unsigned* outLo, unsigned* outHi)
{
    if (!g_index || !name || !*name) return false;
    size_t n = strlen(name);

    for (const char* p = g_index; p < g_index + g_indexBytes; ) {
        const char* eol = strchr(p, '\n');
        if (!eol) eol = g_index + g_indexBytes;
        const char* tab = (const char*)memchr(p, '\t', eol - p);
        if (tab && (size_t)(tab - p) == n) {
            size_t i = 0;
            for (; i < n; ++i) {
                char a = p[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a == '\\') a = '/';
                if (b == '\\') b = '/';
                if (a != b) break;
            }
            if (i == n) {
                *outLen = (unsigned)strtoul(tab + 1, nullptr, 10);
                const char* tab2 = (const char*)memchr(tab + 1, '\t', eol - tab - 1);
                if (!tab2 || eol - tab2 < 17) return false;
                char hi[9] = { 0 }, lo[9] = { 0 };
                memcpy(hi, tab2 + 1, 8);
                memcpy(lo, tab2 + 9, 8);
                *outHi = (unsigned)strtoul(hi, nullptr, 16);
                *outLo = (unsigned)strtoul(lo, nullptr, 16);
                return true;
            }
        }
        p = (eol < g_index + g_indexBytes) ? eol + 1 : g_index + g_indexBytes;
    }
    return false;
}

const uintptr_t kIndexLookup = 0x01B83840;   // 55 8B EC 6A FF
typedef void*(__fastcall* tLookup)(void*, void*, char*);
tLookup o_lookup = nullptr;

void* __fastcall lookup_hook(void* self, void* edx, char* path)
{
    void* rec = o_lookup(self, edx, path);
    if (g_probe && rec && !IsBadWritePtr(rec, 0x10C)) {
        __try {
            unsigned len = 0, lo = 0, hi = 0;
            if (LookupEntry((const char*)rec, &len, &lo, &hi)) {
                unsigned* r = (unsigned*)rec;
                if (r[0x108 / 4] != len || r[0x100 / 4] != lo || r[0x104 / 4] != hi) {
                    Log("   index refresh %s: %u %08X%08X -> %u %08X%08X", (const char*)rec,
                        r[0x108 / 4], r[0x104 / 4], r[0x100 / 4], len, hi, lo);
                    r[0x100 / 4] = lo;
                    r[0x104 / 4] = hi;
                    r[0x108 / 4] = len;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return rec;
}

void ReloadAll()
{
    // The je stays put: bypassing it does not fix a stale length, it lets a mismatched read
    // through, which is what was corrupting the heap.
    LoadIndex();
    InterlockedExchange(&g_probe, 1);
    InterlockedExchange(&g_replaced, 0);
    int done = 0;
    for (int i = 0; i < g_nSeen; ++i) {
        if (!Wanted(g_seen[i].path) || !g_seen[i].self) continue;
        uintptr_t fn = g_base + (g_seen[i].parser - GH);
        char r = 0;
        // sub_1BD8AC0 ends with (*(_DWORD *)this + 8)(this, doc): the parsed document is handed
        // to vtbl[2] of the consumer, and THAT is what decides whether an entry already present
        // gets replaced or quietly ignored. Its address only exists at runtime, so it is read off
        // the object here and printed rebased, ready to look up.
        if (g_seen[i].self && !IsBadReadPtr(g_seen[i].self, 4)) {
            __try {
                unsigned* vt = *(unsigned**)g_seen[i].self;
                if (vt && !IsBadReadPtr(vt, 12) && vt[2])
                    Log("   consumer vtbl[2] = %08X  (rebased %08X)", vt[2],
                        (unsigned)(GH + (vt[2] - g_base)));
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        __try {
            r = CallParser(fn, g_seen[i].self, g_seen[i].esi, g_seen[i].path);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("%s: exception in the parser, code %08X", g_seen[i].path,
                (unsigned)GetExceptionCode());
            continue;
        }
        Log("%-40s parser %08X ecx %08X esi %08X -> %s", g_seen[i].path,
            (unsigned)g_seen[i].parser, (unsigned)(uintptr_t)g_seen[i].self, g_seen[i].esi,
            r ? "OK" : "FAILED");
        ++done;
    }
    InterlockedExchange(&g_probe, 0);
    if (g_replaced) Log("%d entries already present were overwritten in place", (int)g_replaced);
    if (!done) Log("nothing reloaded: check x7_hot_reload.ini against the names in the log");
}

// Runs on the thread owning the window, which is the client's main thread.
LRESULT CALLBACK WndProcHook(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_TIMER && wp == 0xF7F7 && InterlockedExchange(&g_pending, 0))
        ReloadAll();
    return CallWindowProcA(g_oldWndProc, h, msg, wp, lp);
}

DWORD WINAPI Keys(LPVOID)
{
    for (;;) {
        Sleep(50);
        if (GetAsyncKeyState(VK_F7) & 1) InterlockedExchange(&g_pending, 1);
        if (GetAsyncKeyState(VK_F8) & 1) {
            Log("---- %d .x7 files loaded so far ----", g_nSeen);
            for (int i = 0; i < g_nSeen; ++i)
                Log("  %-40s parser %08X  ecx %08X  esi %08X%s", g_seen[i].path,
                    (unsigned)g_seen[i].parser, (unsigned)(uintptr_t)g_seen[i].self,
                    g_seen[i].esi, Wanted(g_seen[i].path) ? "   [in the ini]" : "");
        }
    }
}

BOOL CALLBACK FindGameWindow(HWND h, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    if (h == GetConsoleWindow()) return TRUE;
    char cls[64] = {};
    GetClassNameA(h, cls, sizeof(cls));
    if (lstrcmpiA(cls, "ConsoleWindowClass") == 0) return TRUE;
    g_window = h;
    return FALSE;
}

DWORD WINAPI Install(LPVOID)
{
    g_base = (uintptr_t)GetModuleHandleA(nullptr);
    LoadWantedList();

    int hooked = 0;
    for (int i = 0; i < kNParsers; ++i) {
        if (!Wanted(kParsers[i].file)) continue;
        // a shared parser must only be detoured once
        bool already = false;
        for (int j = 0; j < i; ++j)
            if (kParsers[j].parser == kParsers[i].parser && g_orig[j]) { already = true; break; }
        if (already) continue;

        uintptr_t at = g_base + (kParsers[i].parser - GH);
        uint8_t* q = (uint8_t*)at;
        // The length comes from the table, measured with a disassembler. Assuming five bytes
        // split sub esp,0x30 in item.x7's parser and left a stray 30 running as garbage.
        int len = kParsers[i].prologue;
        if (IsBadReadPtr(q, len) || !(q[0] == 0x55 && q[1] == 0x8B && q[2] == 0xEC)) {
            Log("%s: parser %08X has an unexpected prologue, skipped", kParsers[i].file, (unsigned)kParsers[i].parser);
            continue;
        }
        g_orig[i] = (void*)Trampoline(at, len);
        if (!g_orig[i]) continue;
        Detour(at, g_hooks[i], len);
        Log("hooked %-40s parser %08X (prologue %d bytes)", kParsers[i].file, (unsigned)kParsers[i].parser, len);
        ++hooked;
    }
    Log("=== x7 hot reload === %d parsers hooked (of %d known, per x7_hot_reload.ini)", hooked, kNParsers);

    {
        uintptr_t at = g_base + (kIndexLookup - GH);
        uint8_t* q = (uint8_t*)at;
        if (!IsBadReadPtr(q, 5) && q[0] == 0x55 && q[1] == 0x8B && q[2] == 0xEC
            && q[3] == 0x6A && q[4] == 0xFF) {
            o_lookup = (tLookup)Trampoline(at, 5);
            if (o_lookup) {
                Detour(at, (void*)lookup_hook, 5);
                Log("index lookup %08X hooked, records get refreshed from x7_index.txt",
                    (unsigned)kIndexLookup);
            }
        } else {
            Log("index lookup %08X: unexpected prologue, records cannot be refreshed",
                (unsigned)kIndexLookup);
        }
    }

    {
        uintptr_t at = g_base + (kItemInsert - GH);
        uint8_t* q = (uint8_t*)at;
        if (!IsBadReadPtr(q, 5) && q[0] == 0x55 && q[1] == 0x8B && q[2] == 0xEC
            && q[3] == 0x6A && q[4] == 0xFF) {
            o_iteminsert = (tItemIns)Trampoline(at, 5);
            if (o_iteminsert) {
                Detour(at, (void*)iteminsert_hook, 5);
                Log("item insert %08X hooked", (unsigned)kItemInsert);
            }
        } else {
            Log("item insert %08X: unexpected prologue, items will not be replaced",
                (unsigned)kItemInsert);
        }
    }

    {
        uintptr_t at = g_base + (kDecompRead - GH);
        uint8_t* q = (uint8_t*)at;
        if (!IsBadReadPtr(q, 6) && q[0] == 0x55 && q[1] == 0x8B && q[2] == 0xEC && q[3] == 0x83) {
            o_decomp = (tDecomp)Trampoline(at, 6);
            if (o_decomp) {
                Detour(at, (void*)decomp_hook, 6);
                Log("decompressing read %08X hooked, it reports the true size", (unsigned)kDecompRead);
            }
        } else {
            Log("decompressing read %08X: unexpected prologue, sizes cannot be corrected",
                (unsigned)kDecompRead);
        }
    }

    {
        uintptr_t at = g_base + (kOpen - GH);
        uint8_t* q = (uint8_t*)at;
        if (!IsBadReadPtr(q, 5) && q[0] == 0x55 && q[1] == 0x8B && q[2] == 0xEC && q[3] == 0x6A) {
            o_open = (tOpen)Trampoline(at, 5);
            if (o_open) { Detour(at, (void*)open_hook, 5); Log("container open %08X hooked for the size probe", (unsigned)kOpen); }
        } else {
            Log("container open %08X: unexpected prologue, no size probe", (unsigned)kOpen);
        }
    }

    {
        struct { uintptr_t va; void* hook; void** orig; const char* name; } mh[] = {
            { kMsgId,     (void*)msgid_hook,  (void**)&o_msgid,  "message id"     },
            { kMsgInsert, (void*)msgins_hook, (void**)&o_msgins, "message insert" },
        };
        for (int k = 0; k < 2; ++k) {
            uintptr_t ma = g_base + (mh[k].va - GH);
            uint8_t* mp = (uint8_t*)ma;
            if (IsBadReadPtr(mp, 5) || mp[0] != 0x55 || mp[1] != 0x8B || mp[2] != 0xEC || mp[3] != 0x6A) {
                Log("%s %08X: unexpected prologue, not hooked", mh[k].name, (unsigned)mh[k].va);
                continue;
            }
            *mh[k].orig = (void*)Trampoline(ma, 5);
            if (!*mh[k].orig) continue;
            Detour(ma, mh[k].hook, 5);
            Log("%s %08X hooked", mh[k].name, (unsigned)mh[k].va);
        }
    }

    for (int i = 0; i < 200 && !g_window; ++i) {
        EnumWindows(FindGameWindow, 0);
        if (!g_window) Sleep(250);
    }
    if (!g_window) { Log("game window not found, F7 will do nothing"); return 0; }

    g_oldWndProc = (WNDPROC)SetWindowLongPtrA(g_window, GWLP_WNDPROC, (LONG_PTR)WndProcHook);
    SetTimer(g_window, 0xF7F7, 100, nullptr);
    CreateThread(nullptr, 0, Keys, nullptr, 0, nullptr);
    Log("F7 reloads what x7_hot_reload.ini lists, F8 prints every .x7 seen");
    return 0;
}

} // namespace

void StartX7HotReload()
{
    CreateThread(nullptr, 0, Install, nullptr, 0, nullptr);
}
