// Xui_Hot_Reload.cpp -- reload any .xui and rebind its screen without restarting the client.
//
// How a screen is built
// ---------------------
// Every screen class has a ::Create that reloads its .xui and then resolves each widget by name:
//
//   Game::CServerListConnector::Create(this):           01768700
//       this->vtbl[8]("Resources/XUI/UI/4server.xui")   // load + parse
//       this[2] = FindChild(this[1], "Server_Window")   // the root, then one per widget
//
// Create does NOT replace the previous tree, it appends a second one, and FindChild binds
// whichever comes first -- the stale one. So a plain replay reloads the file and changes
// nothing on screen. The sequence that works is:
//
//   1. Create        parses the file and appends a fresh tree
//   2. count the ui manager children carrying the root name; if there are two, rename the
//      stale one (one byte) so FindChild stops matching it
//   3. Create again  now it binds the fresh tree
//   4. hide every leftover sibling, show the bound one
//
// Renaming without checking that a replacement exists crashes inside Create at 01768AA5,
// dereferencing the null FindChild hands back.
//
// The size check
// --------------
// sub_1B84DA0 compares the size the container index remembers against what the read produced:
//
//   01B84FF0  mov edx, [ecx+0x20]     ; size from the index, captured at boot
//   01B84FF3  cmp edx, [ebp-0x98]     ; against the bytes actually read
//   01B84FF9  je  0x1B85027           ; 74 2C -- equal, carry on; otherwise free and fail
//
// Repacking the container changes the file but not the in-memory index, so the two only match
// when the edit keeps the exact same length. je -> jmp for the duration of a reload and
// restored right after, so ordinary loading keeps its check.
//
// Offsets, all verified against this client: widget name at +0x28 (VS2013 std::string, length
// +0x10, capacity +0x14, inline while capacity < 16), label text at +0x21C, children at +0x208
// (a POINTER to the vector, not the vector itself), visible flag at +0xAA, and vtbl[0x1B4] is
// SetVisible(bool) -- it takes an argument even though the decompiler prints none, and calling
// it without one unbalances the stack.
//
// F6 reloads every screen whose Create has already run, so only what has actually been visited.
//
// Which screens get hooked
// ------------------------
// Only the ones listed in xui_hot_reload.ini next to the exe, one name per line, and the file
// is created with just 4server.xui the first time. That is the only entry actually verified
// end to end; the other 113 addresses were derived from the string each Create pushes and are a
// reasonable inference, not a measurement. Detouring all of them at once crashed the client a
// second into booting -- it only takes one row that is not really a Create. Add names as they
// are confirmed, or put * on a line to hook everything and find out.

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "../s4_base.h"

namespace {

const uintptr_t kSizeCheck = 0x01B84FF9;   // the je to neutralise while reloading

struct Screen {
    const char* xui;
    uintptr_t   create;
    void*       instance;   // captured from the client own call
    void*       original;   // trampoline back to the real Create
};

Screen g_screens[] = {
    { "1Login.xui", 0x01729D90, nullptr, nullptr },
    { "2makecharacter.xui", 0x0172B9E0, nullptr, nullptr },
    { "3d_waitingroom.xui", 0x0178F400, nullptr, nullptr },
    { "3layout.xui", 0x01AB05A0, nullptr, nullptr },
    { "3Main_DownNavi.xui", 0x01736D90, nullptr, nullptr },
    { "4server.xui", 0x01768700, nullptr, nullptr },
    { "4server_notice.xui", 0x0176AB40, nullptr, nullptr },
    { "5channel_list.xui", 0x01697D50, nullptr, nullptr },
    { "5channel_notice.xui", 0x016991C0, nullptr, nullptr },
    { "button_option.xui", 0x01741710, nullptr, nullptr },
    { "card.xui", 0x01694090, nullptr, nullptr },
    { "Collection_Book.xui", 0x016C4BC0, nullptr, nullptr },
    { "Collection_Item_Popup.xui", 0x016CB010, nullptr, nullptr },
    { "enchant.xui", 0x016E6BC0, nullptr, nullptr },
    { "enchant_result.xui", 0x016FB420, nullptr, nullptr },
    { "Lobby_Arcade_MakeRoom.xui", 0x0175B210, nullptr, nullptr },
    { "Lobby_Character.xui", 0x0167FBD0, nullptr, nullptr },
    { "Lobby_Chat.xui", 0x0169A580, nullptr, nullptr },
    { "Lobby_Club.xui", 0x0169F0D0, nullptr, nullptr },
    { "Lobby_Community.xui", 0x016D1720, nullptr, nullptr },
    { "Lobby_Community_MessageBOX.xui", 0x016D5980, nullptr, nullptr },
    { "Lobby_EventMessage.xui", 0x017050C0, nullptr, nullptr },
    { "Lobby_help.xui", 0x01714BF0, nullptr, nullptr },
    { "Lobby_InfoCharacter.xui", 0x017853C0, nullptr, nullptr },
    { "Lobby_list.xui", 0x015BC9A0, nullptr, nullptr },
    { "Lobby_MakeRoom.xui", 0x017559E0, nullptr, nullptr },
    { "Lobby_MakeRoom.xui", 0x0175F560, nullptr, nullptr },
    { "Lobby_Mission_new.xui", 0x0172F7D0, nullptr, nullptr },
    { "Lobby_myinfo.xui", 0x0167D150, nullptr, nullptr },
    { "Lobby_myinfo.xui", 0x0177FC50, nullptr, nullptr },
    { "Lobby_Option.xui", 0x01743DB0, nullptr, nullptr },
    { "Lobby_QuickMatchSetting.xui", 0x0175CD00, nullptr, nullptr },
    { "Lobby_Roominfo.xui", 0x017543D0, nullptr, nullptr },
    { "Lobby_RoomList.xui", 0x01761980, nullptr, nullptr },
    { "Lobby_ScoreInfo.xui", 0x017829E0, nullptr, nullptr },
    { "Lobby_ScoreInfo.xui", 0x0178BEE0, nullptr, nullptr },
    { "Lobby_trainning.xui", 0x0177A2A0, nullptr, nullptr },
    { "lobby_trainning_make.xui", 0x0177EED0, nullptr, nullptr },
    { "Main_UpNavi.xui", 0x01737B10, nullptr, nullptr },
    { "Mode_Loading.xui", 0x013D86B0, nullptr, nullptr },
    { "Mode_Loading.xui", 0x01729430, nullptr, nullptr },
    { "new_club_main.xui", 0x016A20F0, nullptr, nullptr },
    { "New_Club_Manager.xui", 0x016AF0E0, nullptr, nullptr },
    { "Popup/Basic_TextBox_Popup.xui", 0x017676B0, nullptr, nullptr },
    { "Popup/basicpopup.xui", 0x017B9150, nullptr, nullptr },
    { "Popup/Club_popup_question.xui", 0x016B0190, nullptr, nullptr },
    { "Popup/ClubInfo_Popup.xui", 0x016A1310, nullptr, nullptr },
    { "Popup/Community_AddByName_Popup.xui", 0x016CDAA0, nullptr, nullptr },
    { "popup/Community_Combi_OK_Popup.xui", 0x016CE6D0, nullptr, nullptr },
    { "popup/Community_CombiAdd_Popup.xui", 0x016D0980, nullptr, nullptr },
    { "popup/Community_CombiDel_OK_Popup.xui", 0x016CFAF0, nullptr, nullptr },
    { "popup/Community_CombiLvUP_Popup.xui", 0x016CF170, nullptr, nullptr },
    { "Popup/Community_Message_club_Popup.xui", 0x016B7C00, nullptr, nullptr },
    { "Popup/Community_Message_OK_Popup.xui", 0x016D8580, nullptr, nullptr },
    { "Popup/Community_Message_Popup.xui", 0x016D9DA0, nullptr, nullptr },
    { "Popup/Daily_Bonus_Popup.xui", 0x016DF080, nullptr, nullptr },
    { "Popup/daily_present_popup.xui", 0x016CCA00, nullptr, nullptr },
    { "Popup/enchant_change_check_popup.xui", 0x016E2F70, nullptr, nullptr },
    { "Popup/enchant_change_fail_popup.xui", 0x016E4F10, nullptr, nullptr },
    { "Popup/enchant_Change_Fix_Popup.xui", 0x016E3980, nullptr, nullptr },
    { "Popup/enchant_change_popup.xui", 0x016F9380, nullptr, nullptr },
    { "Popup/enchant_extract_popup.xui", 0x016F9E10, nullptr, nullptr },
    { "Popup/enchant_extracton_check_popup.xui", 0x016F5E80, nullptr, nullptr },
    { "Popup/enchant_extracton_fail_popup.xui", 0x016F8AA0, nullptr, nullptr },
    { "Popup/enchant_extracton_popup.xui", 0x016F7370, nullptr, nullptr },
    { "Popup/Enchant_option_del.xui", 0x016F43D0, nullptr, nullptr },
    { "Popup/enchant_Popup.xui", 0x016E57A0, nullptr, nullptr },
    { "Popup/enchant_try_Multipopup_Window.xui", 0x016E1AF0, nullptr, nullptr },
    { "Popup/enchant_try_popup_Window.xui", 0x016E0F40, nullptr, nullptr },
    { "Popup/Escape_Penalty_Popup.xui", 0x0174DBC0, nullptr, nullptr },
    { "Popup/event_play_popup.xui", 0x016FD2A0, nullptr, nullptr },
    { "Popup/event_play_xmas_popup.xui", 0x01705A60, nullptr, nullptr },
    { "Popup/event_popup.xui", 0x01707210, nullptr, nullptr },
    { "Popup/GMWindow.xui", 0x01710F30, nullptr, nullptr },
    { "Popup/Invite_popup.xui", 0x016D3F70, nullptr, nullptr },
    { "Popup/Item_Compensatin_popup.xui", 0x01725690, nullptr, nullptr },
    { "Popup/Item_DayFinish_popup.xui", 0x01727C90, nullptr, nullptr },
    { "Popup/Item_Durability_popup.xui", 0x01726460, nullptr, nullptr },
    { "Popup/Lobby_RoomInfoPopup.xui", 0x01779CF0, nullptr, nullptr },
    { "Popup/LvUP_popup.xui", 0x017125F0, nullptr, nullptr },
    { "Popup/MakeCharacter_Payment_Popup.xui", 0x0172DEB0, nullptr, nullptr },
    { "Popup/mission_arcade_reward_Popup.xui", 0x01711B30, nullptr, nullptr },
    { "Popup/mission_arcade_reward_Popup.xui", 0x01713A60, nullptr, nullptr },
    { "Popup/New_Club_Make_Popup.xui", 0x016B5600, nullptr, nullptr },
    { "Popup/NicknameChange_Popup.xui", 0x01716830, nullptr, nullptr },
    { "Popup/QM_EXIST_Popup.xui", 0x0175BDB0, nullptr, nullptr },
    { "Popup/random_Compensation_popup.xui", 0x01696880, nullptr, nullptr },
    { "Popup/Ranking_popup.xui", 0x017AF330, nullptr, nullptr },
    { "Popup/Server_Rating.xui", 0x01753BA0, nullptr, nullptr },
    { "Popup/Shop_buy_OK_popup.xui", 0x01692F00, nullptr, nullptr },
    { "Popup/Shop_Buy_Popup.xui", 0x01690590, nullptr, nullptr },
    { "Popup/Shop_Del_popup.xui", 0x017173F0, nullptr, nullptr },
    { "Popup/Shop_Del_popup.xui", 0x01736190, nullptr, nullptr },
    { "Popup/shop_firsthand_wear_popup.xui", 0x0171EF20, nullptr, nullptr },
    { "Popup/Shop_multibuy_OK_popup.xui", 0x0168EA40, nullptr, nullptr },
    { "Popup/Shop_MultiBuy_Popup.xui", 0x0168B160, nullptr, nullptr },
    { "Popup/Shop_MultiCapsule_OK_popup.xui", 0x01723C20, nullptr, nullptr },
    { "Popup/Shop_MultiRepair_popup.xui", 0x0171FF50, nullptr, nullptr },
    { "Popup/Shop_Present_Get_Popup.xui", 0x01708380, nullptr, nullptr },
    { "Popup/Shop_Present_Give_Popup.xui", 0x0170D4A0, nullptr, nullptr },
    { "Popup/Shop_Repair_popup.xui", 0x01722B60, nullptr, nullptr },
    { "Popup/Shop_ReSell_popup.xui", 0x01721B40, nullptr, nullptr },
    { "Popup/tutorial_complete.xui", 0x01781FB0, nullptr, nullptr },
    { "Popup/tutorial_fail.xui", 0x017815D0, nullptr, nullptr },
    { "Popup/userpopmenu.xui", 0x016DC0D0, nullptr, nullptr },
    { "Shop_ Inven.xui", 0x01718470, nullptr, nullptr },
    { "Shop_Fumbi.xui", 0x0174EA30, nullptr, nullptr },
    { "Shop_Fumbi_Scn.xui", 0x01751740, nullptr, nullptr },
    { "shop_list.xui", 0x01771200, nullptr, nullptr },
    { "Tooltip_Basic.xui", 0x01775540, nullptr, nullptr },
    { "Tooltip_Item.xui", 0x01775C00, nullptr, nullptr },
    { "watingroom.xui", 0x017A3730, nullptr, nullptr },
    { "Watingroom_roominfo.xui", 0x017A1710, nullptr, nullptr },
    { "WatingRoom_TeamChange.xui", 0x017B1D90, nullptr, nullptr },
};
const int kScreens = (int)(sizeof(g_screens) / sizeof(g_screens[0]));

volatile LONG g_pending = 0;
bool    g_checkPatched = false;
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
    if (slash) lstrcpyA(slash + 1, "xui_hot_reload.log"); else lstrcpyA(path, "xui_hot_reload.log");

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

typedef void(__fastcall* tCreate)(void*, void*);

// One instantiation per row: that is how every screen gets its own hook body without writing
// one by hand for each.
template<int I> void __fastcall CreateHook(void* self, void* edx)
{
    Screen& s = g_screens[I];
    if (s.instance != self) {
        s.instance = self;
        Log("[%s] instance %08X", s.xui, (unsigned)(uintptr_t)self);
    }
    ((tCreate)s.original)(self, edx);
}

void* const g_hooks[] = {
    &CreateHook<0>,
    &CreateHook<1>,
    &CreateHook<2>,
    &CreateHook<3>,
    &CreateHook<4>,
    &CreateHook<5>,
    &CreateHook<6>,
    &CreateHook<7>,
    &CreateHook<8>,
    &CreateHook<9>,
    &CreateHook<10>,
    &CreateHook<11>,
    &CreateHook<12>,
    &CreateHook<13>,
    &CreateHook<14>,
    &CreateHook<15>,
    &CreateHook<16>,
    &CreateHook<17>,
    &CreateHook<18>,
    &CreateHook<19>,
    &CreateHook<20>,
    &CreateHook<21>,
    &CreateHook<22>,
    &CreateHook<23>,
    &CreateHook<24>,
    &CreateHook<25>,
    &CreateHook<26>,
    &CreateHook<27>,
    &CreateHook<28>,
    &CreateHook<29>,
    &CreateHook<30>,
    &CreateHook<31>,
    &CreateHook<32>,
    &CreateHook<33>,
    &CreateHook<34>,
    &CreateHook<35>,
    &CreateHook<36>,
    &CreateHook<37>,
    &CreateHook<38>,
    &CreateHook<39>,
    &CreateHook<40>,
    &CreateHook<41>,
    &CreateHook<42>,
    &CreateHook<43>,
    &CreateHook<44>,
    &CreateHook<45>,
    &CreateHook<46>,
    &CreateHook<47>,
    &CreateHook<48>,
    &CreateHook<49>,
    &CreateHook<50>,
    &CreateHook<51>,
    &CreateHook<52>,
    &CreateHook<53>,
    &CreateHook<54>,
    &CreateHook<55>,
    &CreateHook<56>,
    &CreateHook<57>,
    &CreateHook<58>,
    &CreateHook<59>,
    &CreateHook<60>,
    &CreateHook<61>,
    &CreateHook<62>,
    &CreateHook<63>,
    &CreateHook<64>,
    &CreateHook<65>,
    &CreateHook<66>,
    &CreateHook<67>,
    &CreateHook<68>,
    &CreateHook<69>,
    &CreateHook<70>,
    &CreateHook<71>,
    &CreateHook<72>,
    &CreateHook<73>,
    &CreateHook<74>,
    &CreateHook<75>,
    &CreateHook<76>,
    &CreateHook<77>,
    &CreateHook<78>,
    &CreateHook<79>,
    &CreateHook<80>,
    &CreateHook<81>,
    &CreateHook<82>,
    &CreateHook<83>,
    &CreateHook<84>,
    &CreateHook<85>,
    &CreateHook<86>,
    &CreateHook<87>,
    &CreateHook<88>,
    &CreateHook<89>,
    &CreateHook<90>,
    &CreateHook<91>,
    &CreateHook<92>,
    &CreateHook<93>,
    &CreateHook<94>,
    &CreateHook<95>,
    &CreateHook<96>,
    &CreateHook<97>,
    &CreateHook<98>,
    &CreateHook<99>,
    &CreateHook<100>,
    &CreateHook<101>,
    &CreateHook<102>,
    &CreateHook<103>,
    &CreateHook<104>,
    &CreateHook<105>,
    &CreateHook<106>,
    &CreateHook<107>,
    &CreateHook<108>,
    &CreateHook<109>,
    &CreateHook<110>,
    &CreateHook<111>,
    &CreateHook<112>,
    &CreateHook<113>,
};

bool ReadName(void* obj, unsigned off, char* out, size_t max)
{
    __try {
        unsigned char* b = (unsigned char*)obj + off;
        if (IsBadReadPtr(b, 0x18)) return false;
        size_t len = *(size_t*)(b + 0x10);
        size_t cap = *(size_t*)(b + 0x14);
        if (len == 0 || len > 200 || cap < len || cap > 0x10000) return false;
        char* text = (cap < 16) ? (char*)b : *(char**)b;
        if (!text || IsBadReadPtr(text, len + 1) || text[len] != 0) return false;
        for (size_t i = 0; i < len; ++i)
            if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7E) return false;
        if (len + 1 > max) return false;
        memcpy(out, text, len + 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Children(void* w, unsigned char** begin, unsigned char** end)
{
    void** direct = (void**)((unsigned char*)w + 0x208);
    if (IsBadReadPtr(direct, 4)) return false;
    void** vec = (void**)direct[0];                 // the field holds a pointer to the vector
    if (!vec || IsBadReadPtr(vec, 12)) vec = direct;
    unsigned char* b = (unsigned char*)vec[0];
    unsigned char* e = (unsigned char*)vec[1];
    if (!b || e < b || (e - b) % 4 || (e - b) > 0x4000) return false;
    if (IsBadReadPtr(b, (size_t)(e - b))) return false;
    *begin = b; *end = e;
    return true;
}

void SetVisible(void* w, int visible)
{
    if (!w) return;
    __try {
        unsigned* vt = *(unsigned**)w;
        if (IsBadReadPtr(vt, 0x1B8)) return;
        void* fn = (void*)vt[0x1B4 / 4];
        if (fn) ((void(__thiscall*)(void*, int))fn)(w, visible);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

int CountNamed(void* parent, const char* name, void** first)
{
    int n = 0;
    if (first) *first = nullptr;
    if (!parent) return 0;
    __try {
        unsigned char* b = nullptr; unsigned char* e = nullptr;
        if (!Children(parent, &b, &e)) return 0;
        int total = (int)((e - b) / 4);
        for (int i = 0; i < total && i < 64; ++i) {
            void* h = ((void**)b)[i];
            if (!h || IsBadReadPtr(h, 0x30)) continue;
            char nm[128] = {};
            if (!ReadName(h, 0x28, nm, sizeof(nm)) || lstrcmpA(nm, name) != 0) continue;
            if (n == 0 && first) *first = h;
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}

void HideOthers(void* parent, const char* name, void* keep)
{
    if (!parent) return;
    __try {
        unsigned char* b = nullptr; unsigned char* e = nullptr;
        if (!Children(parent, &b, &e)) return;
        int total = (int)((e - b) / 4);
        for (int i = 0; i < total && i < 64; ++i) {
            void* h = ((void**)b)[i];
            if (!h || h == keep || IsBadReadPtr(h, 0x30)) continue;
            char nm[128] = {};
            if (ReadName(h, 0x28, nm, sizeof(nm)) && lstrcmpA(nm, name) == 0) SetVisible(h, 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Overwriting the first character is enough for FindChild to stop matching, and it costs
// nothing: no allocation, no tree surgery, nothing freed.
bool RenameFirstChar(void* widget)
{
    __try {
        unsigned char* b = (unsigned char*)widget + 0x28;
        if (IsBadReadPtr(b, 0x18)) return false;
        size_t len = *(size_t*)(b + 0x10);
        size_t cap = *(size_t*)(b + 0x14);
        if (len == 0 || cap < len) return false;
        char* text = (cap < 16) ? (char*)b : *(char**)b;
        if (!text || IsBadWritePtr(text, 1)) return false;
        text[0] = '_';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Every reload leaves spare trees behind. They were hidden but kept their name, so the next
// press found one of those first and rebound to stale content -- which is why it took three or
// four presses before the edit showed up. The children vector appends, so the freshest tree is
// always the LAST one carrying the name: rename every earlier one and Create binds the right
// one on the first press.
int RenameAllButLast(void* parent, const char* name, void** last)
{
    int renamed = 0;
    if (last) *last = nullptr;
    if (!parent) return 0;
    __try {
        unsigned char* b = nullptr; unsigned char* e = nullptr;
        if (!Children(parent, &b, &e)) return 0;
        int total = (int)((e - b) / 4);
        if (total > 64) total = 64;

        void* found[64];
        int n = 0;
        for (int i = 0; i < total; ++i) {
            void* h = ((void**)b)[i];
            if (!h || IsBadReadPtr(h, 0x30)) continue;
            char nm[128] = {};
            if (ReadName(h, 0x28, nm, sizeof(nm)) && lstrcmpA(nm, name) == 0) found[n++] = h;
        }
        if (n == 0) return 0;
        if (last) *last = found[n - 1];
        for (int i = 0; i < n - 1; ++i) {
            if (RenameFirstChar(found[i])) { SetVisible(found[i], 0); ++renamed; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return renamed;
}

void ForceSizeCheck(bool off)
{
    uint8_t* q = (uint8_t*)S4(kSizeCheck);
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

// The ui manager keeps focus and mouse-over pointers to specific widgets. After the swap they
// still address the stale tree, so clicks land nowhere until something else takes focus -- the
// screen only responded again through the Join Now button. vtbl[0xAC] is the client's own reset
// for exactly that: it forwards to vtbl[0x318] and vtbl[0x314], which handle focus and hover.
// Called BEFORE a reload it wrecks the load; called after the rebind it is what is needed.
void ResetFocus(void* uiMgr)
{
    if (!uiMgr) return;
    __try {
        unsigned* vt = *(unsigned**)uiMgr;
        if (IsBadReadPtr(vt, 0xB0)) return;
        void* fn = (void*)vt[0xAC / 4];
        if (fn) ((void(__thiscall*)(void*))fn)(uiMgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void ReloadScreen(Screen& s)
{
    if (!s.instance || !s.original) return;

    unsigned* m = (unsigned*)s.instance;
    void* uiMgr = nullptr;
    void* rootBefore = nullptr;
    char rootName[128] = {};
    __try {
        uiMgr = (void*)m[1];
        rootBefore = (void*)m[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    // The root name comes off the live widget instead of being hardcoded, so each screen works
    // with its own: Server_Window, Lobby_Window, and so on.
    if (!rootBefore || !ReadName(rootBefore, 0x28, rootName, sizeof(rootName))) return;

    __try {
        ((tCreate)s.original)(s.instance, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[%s] exception in Create, code %08X", s.xui, (unsigned)GetExceptionCode());
        return;
    }

    void* fresh = nullptr;
    int renamed = RenameAllButLast(uiMgr, rootName, &fresh);
    if (renamed > 0 && fresh) {
        __try {
            ((tCreate)s.original)(s.instance, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[%s] exception in the second pass, code %08X", s.xui, (unsigned)GetExceptionCode());
            return;
        }
    }

    __try {
        void* bound = (void*)m[2];
        if (bound) {
            HideOthers(uiMgr, rootName, bound);   // each pass leaves a spare tree drawing on top
            SetVisible(bound, 1);
            ResetFocus(uiMgr);
        }
        Log("[%s] %s -> %08X (%s, %d stale renamed)", s.xui, rootName,
            (unsigned)(uintptr_t)bound, bound != rootBefore ? "rebound" : "unchanged", renamed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void ReloadAll()
{
    ForceSizeCheck(true);
    int n = 0;
    for (int i = 0; i < kScreens; ++i) {
        if (!g_screens[i].instance) continue;
        ReloadScreen(g_screens[i]);
        ++n;
    }
    ForceSizeCheck(false);
    Log("reload done, %d screens built so far", n);
}

// Runs on the thread that owns the window, which is the client main thread. Rebuilding a widget
// tree from the hotkey thread, underneath the renderer, is what crashes.
LRESULT CALLBACK WndProcHook(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_TIMER && wp == 0xF6F6 && InterlockedExchange(&g_pending, 0))
        ReloadAll();
    return CallWindowProcA(g_oldWndProc, h, msg, wp, lp);
}

DWORD WINAPI HotkeyThread(LPVOID)
{
    for (;;) {
        Sleep(40);
        if (GetAsyncKeyState(VK_F6) & 1) InterlockedExchange(&g_pending, 1);
    }
}

// A console owned by this process is also a visible, ownerless, same-process top level window
// and enumerates first, so it is skipped explicitly.
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

// Reads the allowlist. Missing file means: write one with the single verified entry and use it.
char g_wanted[8192];

void LoadWantedList()
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) lstrcpyA(slash + 1, "xui_hot_reload.ini"); else lstrcpyA(path, "xui_hot_reload.ini");

    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        static const char* seed =
            "# One .xui per line, exactly as it appears in the log.\r\n"
            "# A line containing only an asterisk hooks every screen.\r\n"
            "# Only 4server.xui is verified end to end; the rest are inferred from the binary,\r\n"
            "# and hooking all of them at once crashed the client a second into booting.\r\n"
            "4server.xui\r\n";
        f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w; WriteFile(f, seed, lstrlenA(seed), &w, nullptr); CloseHandle(f);
        }
        lstrcpyA(g_wanted, "4server.xui\n");
        return;
    }
    DWORD read = 0;
    ReadFile(f, g_wanted, sizeof(g_wanted) - 1, &read, nullptr);
    g_wanted[read] = 0;
    CloseHandle(f);
}

bool Wanted(const char* xui)
{
    // The wildcard only counts as a line of its own. Searching the whole buffer for '*' matched
    // the asterisk inside the seed comment, so every screen got hooked regardless of the file
    // and the client died on the way up.
    const char* p = g_wanted;
    size_t n = strlen(xui);
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (*p == 0) break;
        if (*p == '#' || *p == ';') { while (*p && *p != '\n') ++p; continue; }
        const char* start = p;
        while (*p && *p != '\r' && *p != '\n') ++p;
        size_t len = (size_t)(p - start);
        while (len && (start[len - 1] == ' ' || start[len - 1] == '\t')) --len;
        if (len == 1 && start[0] == '*') return true;
        if (len == n && _strnicmp(start, xui, n) == 0) return true;
    }
    return false;
}

DWORD WINAPI InstallThread(LPVOID)
{
    LoadWantedList();
    int hooked = 0;
    for (int i = 0; i < kScreens; ++i) {
        if (!Wanted(g_screens[i].xui)) continue;
        uintptr_t at = S4(g_screens[i].create);
        uint8_t* q = (uint8_t*)at;
        if (IsBadReadPtr(q, 5) || !(q[0] == 0x55 && q[1] == 0x8B && q[2] == 0xEC)) continue;
        g_screens[i].original = (void*)Trampoline(at, 5);
        if (!g_screens[i].original) continue;
        Detour(at, g_hooks[i], 5);
        ++hooked;
    }
    Log("=== xui hot reload === %d screens hooked (of %d known, per xui_hot_reload.ini)",
        hooked, kScreens);

    for (int i = 0; i < 200 && !g_window; ++i) {
        EnumWindows(FindGameWindow, 0);
        if (!g_window) Sleep(250);
    }
    if (!g_window) { Log("game window not found, F6 will do nothing"); return 0; }

    g_oldWndProc = (WNDPROC)SetWindowLongPtrA(g_window, GWLP_WNDPROC, (LONG_PTR)WndProcHook);
    SetTimer(g_window, 0xF6F6, 100, nullptr);
    CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
    Log("F6 reloads every screen visited so far");
    return 0;
}

} // namespace

void StartXuiHotReload()
{
    CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
}
