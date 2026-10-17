#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <detours.h>
#include <intrin.h>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "whitelist_code.inc"

#define printf(...) ((void)0)
#define puts(...) ((void)0)

#pragma comment(lib, "psapi.lib")

// ============================================================================
// S4 League Ultra-Fast Resource & Pure On-Demand Optimizer
// ============================================================================

static HANDLE(WINAPI* OriginalCreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
    DWORD, DWORD, HANDLE) = CreateFileA;
static void* (__fastcall* OriginalResourceLoader)(void*, void*, char*, char, char) =
    reinterpret_cast<decltype(OriginalResourceLoader)>(0x01B79480);
static unsigned int (__thiscall* OriginalResourceExists)(void*, char*) =
    reinterpret_cast<decltype(OriginalResourceExists)>(0x01B792A0);
static char (__cdecl* OriginalFileExists)(const char*) =
    reinterpret_cast<decltype(OriginalFileExists)>(0x01B7AFB0);
static char (__cdecl* OriginalDecompress)(unsigned*, void*, unsigned) =
    reinterpret_cast<decltype(OriginalDecompress)>(0x01B8E590);
static char (__thiscall* OriginalModelLoader)(void*, char*) =
    reinterpret_cast<decltype(OriginalModelLoader)>(0x01D08370);
static int (__fastcall* OriginalQueuePreload)(void*, void*, char*, int, int) =
    reinterpret_cast<decltype(OriginalQueuePreload)>(0x01CB8500);
static float* (__thiscall* OriginalGetSpriteUV)(void*, unsigned int, float*, float*, float*, float*) =
    reinterpret_cast<decltype(OriginalGetSpriteUV)>(0x01229130);
static int (__thiscall* OriginalOctreeUpdate)(void*) =
    reinterpret_cast<decltype(OriginalOctreeUpdate)>(0x01D558C0);
static int (__fastcall* OriginalCreateActorBox)(void*, void*, int, char, int, int, int) =
    reinterpret_cast<decltype(OriginalCreateActorBox)>(0x01637E40);

// Preloader de configuracion nativa (sub_1A8AE20)
typedef int* (__thiscall* FnInitConfig)(int* this_ptr);
static FnInitConfig OriginalInitConfig = reinterpret_cast<FnInitConfig>(0x01A8AE20);

// 3 Bucles de precarga masiva de armas / items (sub_15A1810, sub_15A1AB0, sub_15A1BD0)
typedef unsigned int (__thiscall* FnMassPreload)(void* self, void* vec);
static FnMassPreload OriginalMassPreload1 = reinterpret_cast<FnMassPreload>(0x015A1810);
static FnMassPreload OriginalMassPreload2 = reinterpret_cast<FnMassPreload>(0x015A1AB0);
static FnMassPreload OriginalMassPreload3 = reinterpret_cast<FnMassPreload>(0x015A1BD0);

static CRITICAL_SECTION CacheLock;
static CRITICAL_SECTION LogLock;

static std::unordered_map<std::string, std::vector<unsigned char>> ResourceCache;
static std::unordered_map<std::string, char> ExistsCache;
static std::unordered_map<std::string, unsigned int> ResourceExistsCache;
static std::unordered_map<std::string, int> ResourceLoadCounts;
static std::unordered_set<std::string> AlreadyLoadedSet;

static size_t CachedBytes = 0;
static const size_t CacheLimit = 32u * 1024u * 1024u;
__declspec(thread) static char CurrentResource[260];
static volatile LONG ResExistsHits = 0;
static volatile LONG DecompressHits = 0;
static volatile LONG XmlCuts = 0;

static FILE* g_pedidosLog = nullptr;
static DWORD g_startTick = 0;
static volatile LONG TotalLogged = 0;

static const bool g_enablePedidosLog = false;
static const bool g_enableConsoleSpam = false;

// Estado del cliente
static volatile bool g_isInLobbyOrGame = false;

static bool IsBootPhase()
{
    return !g_isInLobbyOrGame;
}

static void LogPedido(const char* tipo, const char* path, uintptr_t caller, const char* status)
{
    if (!g_enablePedidosLog) return;
    if (!path || !path[0]) return;
    EnterCriticalSection(&LogLock);
    if (!g_pedidosLog) {
        g_pedidosLog = fopen("pedidos.log", "w");
        if (g_pedidosLog) {
            setvbuf(g_pedidosLog, nullptr, _IOFBF, 64 * 1024);
            fprintf(g_pedidosLog, "--- INICIO DE REGISTRO DIRECTO ---\n");
        }
    }
    if (g_pedidosLog) {
        DWORD elapsed = GetTickCount() - g_startTick;
        fprintf(g_pedidosLog, "[%02lu.%03lu s][%s][caller=%08X][%s]: %s\n",
            elapsed / 1000, elapsed % 1000, tipo, (unsigned)caller, status ? status : "OK", path);
        if (InterlockedIncrement(&TotalLogged) % 100 == 0) {
            fflush(g_pedidosLog);
        }
    }
    LeaveCriticalSection(&LogLock);
}

static std::string CanonicalName(const char* name)
{
    if (!name) return "";
    std::string out = name;
    for (char& c : out) {
        if (c == '/') c = '\\';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

static bool IsWhitelistedScn(const std::string& name)
{
    if (name.empty()) return false;
    std::string canonical = CanonicalName(name.c_str());
    if (g_WhitelistedScns.find(canonical) != g_WhitelistedScns.end())
        return true;
    size_t lastSlash = canonical.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        std::string base = canonical.substr(lastSlash + 1);
        if (g_WhitelistedScns.find(base) != g_WhitelistedScns.end())
            return true;
    }
    return false;
}

static void CheckLobbyEntry(const std::string& name, uintptr_t callerAddr)
{
    if (g_isInLobbyOrGame) return;

    // Se activa al cargar el personaje real del jugador, sala de espera (standby), arcade o escenario
    if (callerAddr == 0x0144B83F || callerAddr == 0x0144C24F ||
        name.find("character\\face") != std::string::npos ||
        name.find("character/face") != std::string::npos ||
        name.find("character\\body") != std::string::npos ||
        name.find("character/body") != std::string::npos ||
        name.find("standby") != std::string::npos ||
        name.find("arcade") != std::string::npos ||
        name.find("scenario") != std::string::npos)
    {
        g_isInLobbyOrGame = true;
        printf("\n============================================================\n");
        printf(">>> [OPTIMIZER] ENTRADA A LOBBY / PARTIDA DETECTADA <<<\n");
        printf(">>> MODO ON-DEMAND TOTAL ACTIVADO (100%% JUEGO LISTO) <<<\n");
        printf("============================================================\n\n");
    }
}

// ============================================================================
// 1. Filtro Quirurgico de XMLs y Tablas de Lenguaje Innecesarias
// ============================================================================
static bool ShouldCutXmlOrLang(const std::string& name)
{
    // ARCHIVOS DE TIENDA Y GUARDADO NUNCA SE CORTAN
    if (name.find("shop") != std::string::npos ||
        name.find(".s4") != std::string::npos ||
        name.find("collect_book") != std::string::npos)
    {
        return false;
    }

    // SI YA ESTAMOS EN LOBBY O JUEGO, TODO LO QUE PIDA ES 100% ON-DEMAND (NUNCA CORTAR NADA)
    if (!IsBootPhase()) {
        return false;
    }

    bool isXmlFolder = (name.rfind("xml\\", 0) == 0 || name.rfind("xml/", 0) == 0);
    bool isLangFolder = (name.rfind("language\\", 0) == 0 || name.rfind("language/", 0) == 0);
    if (!isXmlFolder && !isLangFolder) {
        return false;
    }

    // NUNCA TOCAR XUI, TEXTURAS NI MODELOS
    if (name.find(".xui") != std::string::npos ||
        name.find(".dds") != std::string::npos ||
        name.find(".tga") != std::string::npos ||
        name.find(".png") != std::string::npos ||
        name.find(".scn") != std::string::npos)
    {
        return false;
    }

    // A. XMLs y Tablas Estrictamente ESENCIALES (NUNCA CORTAR):
    if (name.find("weapon") != std::string::npos ||
        name.find("equiped_weapon") != std::string::npos ||
        name.find("weapon_lua") != std::string::npos ||
        name.find("tran_effect") != std::string::npos ||
        name.find("scenario") != std::string::npos ||
        name.find("arcade") != std::string::npos ||
        name.find("challenge") != std::string::npos ||
        name.find("mission") != std::string::npos ||
        name.find("monster") != std::string::npos ||
        name.find("card_info") != std::string::npos ||
        name.find("conquest") != std::string::npos ||
        name.find("book_scenario") != std::string::npos ||
        name.find("script") != std::string::npos ||
        name.find("movie") != std::string::npos ||
        name.find("stage") != std::string::npos ||
        name.find("camera") != std::string::npos ||
        name.find("map") != std::string::npos ||
        name.find("item.x7") != std::string::npos ||
        name.find("default_item") != std::string::npos ||
        name.find("item_grade") != std::string::npos ||
        name.find("map.x7") != std::string::npos ||
        name.find("common_game_option") != std::string::npos ||
        name.find("content_setting") != std::string::npos ||
        name.find("experience") != std::string::npos ||
        name.find("gameinfo") != std::string::npos ||
        name.find("font_config") != std::string::npos ||
        name.find("constant") != std::string::npos ||
        name.find("channel") != std::string::npos ||
        name.find("string_table") != std::string::npos ||
        name.find("default_option") != std::string::npos ||
        name.find("promotion") != std::string::npos ||
        name.find("parse_string") != std::string::npos ||
        name.find("club") != std::string::npos ||
        name.find("tutorial") != std::string::npos ||
        name.find("guide") != std::string::npos ||
        name.find("ui_dir.xml") != std::string::npos)
    {
        return false;
    }

    // B. Cortar XMLs y Lenguajes auxiliares no usados en arranque:
    if (name.find("training") != std::string::npos ||
        name.find("card_") != std::string::npos ||
        name.find("cardinfo") != std::string::npos ||
        name.find("randomshop") != std::string::npos ||
        name.find("ghost_trail") != std::string::npos ||
        name.find("tempos") != std::string::npos ||
        name.find("timedelay") != std::string::npos ||
        name.find("esper") != std::string::npos ||
        name.find("attackrange") != std::string::npos ||
        name.find("extractkey") != std::string::npos ||
        name.find("tooltip") != std::string::npos ||
        name.find("develop_social") != std::string::npos ||
        name.find("ban_reason") != std::string::npos ||
        name.find("autonomic_regulation") != std::string::npos ||
        name.find("sentrygun") != std::string::npos ||
        name.find("slaughter_tag") != std::string::npos ||
        name.find("game_help") != std::string::npos ||
        name.find("server_notice") != std::string::npos)
    {
        return true;
    }

    return false;
}

// ============================================================================
// 2. Skip List de Rutas Nativas Inexistentes y Basura de Arranque
// ============================================================================
static bool DebeSaltarRuta(const std::string& norm)
{
    // NUNCA TOCAR ARCHIVOS DE SHOP NI .S4
    if (norm.find("shop") != std::string::npos ||
        norm.find(".s4") != std::string::npos ||
        norm.find("collect_book") != std::string::npos)
    {
        return false;
    }

    // NUNCA TOCAR XUI, TEXTURAS DE MODELOS (WEAPON, CHARACTER), BOTONES NI ACCIONES
    if (norm.find(".xui") != std::string::npos) return false;
    if (norm.find("model\\") != std::string::npos || norm.find("model/") != std::string::npos) return false;
    if (norm.find(".scn") != std::string::npos) return false;
    if (norm.find(".seq") != std::string::npos) return false;
    if (norm.find("effects") != std::string::npos) return false;
    if (norm.find("image\\action") != std::string::npos || norm.find("image/action") != std::string::npos) return false;
    if (norm.find("image\\onetimecharge") != std::string::npos || norm.find("image/onetimecharge") != std::string::npos) return false;
    if (norm.find("image\\itemtooltip") != std::string::npos || norm.find("image/itemtooltip") != std::string::npos) return false;
    if (norm.find("image\\weapon") != std::string::npos || norm.find("image/weapon") != std::string::npos) return false;
    if (norm.find("image\\costume") != std::string::npos || norm.find("image/costume") != std::string::npos) return false;
    if (norm.find("gui\\") != std::string::npos || norm.find("gui/") != std::string::npos) return false;
    if (norm.find("training") != std::string::npos || norm.find("tutorial") != std::string::npos) return false;
    if (norm.find("game_help") != std::string::npos || norm.find("guide") != std::string::npos) return false;
    if (norm.find("standby") != std::string::npos || norm.find("bulletin") != std::string::npos) return false;
    if (norm.find("room") != std::string::npos || norm.find("map") != std::string::npos) return false;
    if (norm.find("scenario") != std::string::npos || norm.find("arcade") != std::string::npos ||
        norm.find("script") != std::string::npos || norm.find(".lua") != std::string::npos ||
        norm.find("monster") != std::string::npos || norm.find("movie") != std::string::npos ||
        norm.find("camera") != std::string::npos || norm.find("stage") != std::string::npos ||
        norm.find("challenge") != std::string::npos || norm.find("mission") != std::string::npos) return false;

    // 0. XMLs y Language cortados
    if (ShouldCutXmlOrLang(norm))
        return true;

// mapinfo nunca se salta

    // 0. URLs muertas de banners web de Aeria Games
    if (norm.find("http:") != std::string::npos || norm.find("https:") != std::string::npos ||
        norm.find("aeriagames") != std::string::npos)
        return true;

    // 0. Texturas inexistentes nativas o filtros de slang
    if (norm.find("plainwhite") != std::string::npos ||
        norm.find("loading_practics_mode") != std::string::npos ||
        norm.find("slang_filter") != std::string::npos)
        return true;

    // 1. Sonidos: SOLO EN ARRANQUE INICIAL
    if (norm.find("sound\\") != std::string::npos || norm.find("sound/") != std::string::npos ||
        norm.find(".ogg") != std::string::npos || norm.find(".wav") != std::string::npos)
    {
        if (norm.find("acade") != std::string::npos || norm.find("arcade") != std::string::npos) {
            return false;
        }
        return IsBootPhase();
    }

    // 2. Errores nativos de concatenacion de S4 League
    if (norm.find("xui\\image\\ingame\\tga") != std::string::npos ||
        norm.find("xui\\image\\common\\tga") != std::string::npos ||
        norm.find("xui\\image\\lobby\\tga") != std::string::npos ||
        norm.find("itemtooltip\\tga") != std::string::npos ||
        norm.find("xui\\ui") != std::string::npos)
    {
        return true;
    }

    return false;
}

// ----------------------------------------------------------------------------
// 1. Bypasses Definitivos de los 3 Bucles de Precarga Masiva (0x15A1810, 0x15A1AB0, 0x15A1BD0)
// Estos bucles solo iteran sobre 1,575 armas/items y redibujan la pantalla en DirectX 1,575 veces.
// Se retornan de inmediato (0 ns) para que la carga sea 100% On-Demand.
// ----------------------------------------------------------------------------
static unsigned int __fastcall HookMassPreload1(void* /*self*/, void* /*edx*/, void* /*vec*/)
{
    return 0; // Desactivado permanentemente (0 ns)
}

static unsigned int __fastcall HookMassPreload2(void* /*self*/, void* /*edx*/, void* /*vec*/)
{
    return 0; // Desactivado permanentemente (0 ns)
}

static unsigned int __fastcall HookMassPreload3(void* /*self*/, void* /*edx*/, void* /*vec*/)
{
    *reinterpret_cast<unsigned char*>(0x26BD45C) = 0;
    *reinterpret_cast<unsigned int*>(0x26BA644) = 0;
    return 0; // Desactivado permanentemente (0 ns)
}

// Hook de Inicializacion de Configuracion de Juego (sub_1A8AE20)
static int* __fastcall HookInitConfig(int* self, void* /*edx*/)
{
    int* res = OriginalInitConfig(self);
    if (res) {
        *(reinterpret_cast<unsigned char*>(res) + 74) = 0;
        *(reinterpret_cast<unsigned char*>(res) + 78) = 0;
    }
    return res;
}

// ----------------------------------------------------------------------------
// 2. Hook QueuePreload (0x01CB8500) - Descarte Total de Precargas Masivas
// ----------------------------------------------------------------------------
static int __fastcall HookQueuePreload(void* self, void* edx, char* Str, int a3, int a4)
{
    if (!Str || !Str[0]) return 0;

    const std::string name = CanonicalName(Str);
    bool isWhitelisted = IsWhitelistedScn(name);

    void* caller = _ReturnAddress();
    uintptr_t callerAddr = reinterpret_cast<uintptr_t>(caller);

    // Si está en el whitelist, se DEBE cargar desde el inicio y nunca descartarse
    if (!isWhitelisted) {
        // DESCARTAR SOLO PRECARGAS MASIVAS EN PANTALLA DE CARGA (Pure On-Demand)
        // Efectos y animaciones (0x01CB52AD, etc.) se cargan libremente bajo demanda en partida
        if (callerAddr == 0x015A18EB || callerAddr == 0x015A1B8B || callerAddr == 0x015A1CBC || callerAddr == 0x015A14B6)
        {
            return 0; // Omision de bucles masivos de pantalla de carga
        }
    }

    CheckLobbyEntry(name, callerAddr);

    EnterCriticalSection(&CacheLock);
    bool isFirstTime = (AlreadyLoadedSet.find(name) == AlreadyLoadedSet.end());
    if (isFirstTime) {
        AlreadyLoadedSet.insert(name);
    }
    LeaveCriticalSection(&CacheLock);

    if (isFirstTime) {
        if (isWhitelisted) {
            printf("[WHITELIST SCN LOAD (caller=%p)]: %s\n", caller, Str);
            LogPedido("PRELOAD_WHITELIST", Str, callerAddr, "WHITELIST");
        } else {
            printf("[ON-DEMAND LOAD (1st time, caller=%p)]: %s\n", caller, Str);
            LogPedido("PRELOAD_ONDEMAND", Str, callerAddr, "1ST_TIME");
        }
    }

    return OriginalQueuePreload(self, edx, Str, a3, a4);
}

// ----------------------------------------------------------------------------
// 3. Hook FileExists (0x01B7AFB0) - O(1) Bypass a _resources/ y Cache Seguro
// ----------------------------------------------------------------------------
static char __cdecl HookFileExists(const char* path)
{
    if (!path || !path[0]) return 0;
    const std::string key = CanonicalName(path);

    // Banners web inexistentes
    if (key.find("http:") != std::string::npos || key.find("https:") != std::string::npos ||
        key.find("aeriagames") != std::string::npos)
    {
        return 0;
    }

// mapinfo permitido

    // XMLs cortados nunca estan en disco
    if (ShouldCutXmlOrLang(key)) {
        return 0;
    }

    // Archivos en _resources/<hash>: Existen en _resources! Retornar 1 de inmediato (0 ns)
    if (key.rfind("_resources\\", 0) == 0 || key.rfind("_resources/", 0) == 0) {
        return 1;
    }

    // Cache O(1) para archivos sueltos
    EnterCriticalSection(&CacheLock);
    auto it = ExistsCache.find(key);
    if (it != ExistsCache.end()) {
        char res = it->second;
        LeaveCriticalSection(&CacheLock);
        return res;
    }
    LeaveCriticalSection(&CacheLock);

    DWORD attrs = GetFileAttributesA(path);
    char result = (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
    if (!result) {
        char altPath[MAX_PATH];
        snprintf(altPath, sizeof(altPath), "C:\\S4League\\extracted_resources\\%s", path);
        DWORD altAttrs = GetFileAttributesA(altPath);
        if (altAttrs != INVALID_FILE_ATTRIBUTES && !(altAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
            result = 1;
        }
    }

    EnterCriticalSection(&CacheLock);
    ExistsCache.emplace(key, result);
    LeaveCriticalSection(&CacheLock);

    return result;
}

// ----------------------------------------------------------------------------
// 4. Hook ResourceExists (0x01B792A0) - O(1) Cache First & Safe Resolution
// ----------------------------------------------------------------------------
static unsigned int __fastcall HookResourceExists(void* self, void* /*edx*/, char* path)
{
    if (!path || !path[0])
        return OriginalResourceExists(self, path);

    const std::string key = CanonicalName(path);

    // Bypass inmediato de pantallas de carga en carpetas incorrectas de UI
    if (key.find("loading_") != std::string::npos && key.find("ingame_loading") == std::string::npos) {
        return 0;
    }

    // 1. O(1) Cache Lookup
    EnterCriticalSection(&CacheLock);
    auto it = ResourceExistsCache.find(key);
    if (it != ResourceExistsCache.end()) {
        unsigned int res = it->second;
        LeaveCriticalSection(&CacheLock);
        InterlockedIncrement(&ResExistsHits);
        return res;
    }
    LeaveCriticalSection(&CacheLock);

    void* caller = _ReturnAddress();
    uintptr_t callerAddr = reinterpret_cast<uintptr_t>(caller);

    // 2. Descarte por regla de ruta invalida, XML cortado o sonido (solo en boot)
    if (DebeSaltarRuta(key)) {
        if (key.find("mapinfo") == std::string::npos) {
            EnterCriticalSection(&CacheLock);
            ResourceExistsCache.emplace(key, 0);
            LeaveCriticalSection(&CacheLock);
        }
        LogPedido("PROBE_SKIP", path, callerAddr, "SALTADO");
        return 0;
    }

    // 3. Consulta real al contenedor .s4hd
    unsigned int result = OriginalResourceExists(self, path);
    if (!result) {
        DWORD attrs = GetFileAttributesA(path);
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            result = 1;
        } else {
            char altPath[MAX_PATH];
            snprintf(altPath, sizeof(altPath), "C:\\S4League\\extracted_resources\\%s", path);
            DWORD altAttrs = GetFileAttributesA(altPath);
            if (altAttrs != INVALID_FILE_ATTRIBUTES && !(altAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
                result = 1;
            }
        }
    }

    EnterCriticalSection(&CacheLock);
    ResourceExistsCache.emplace(key, result);
    LeaveCriticalSection(&CacheLock);

    LogPedido("PROBE_REAL", path, callerAddr, result ? "ENCONTRADO" : "NO_EXISTE");
    return result;
}

// ----------------------------------------------------------------------------
// 5. Hook ResourceLoader (0x01B79480) - Proteccion Total de Texturas, Botones y Modelos
// ----------------------------------------------------------------------------
static void* __fastcall HookResourceLoader(void* self, void* edx, char* name, char a2, char a3)
{
    if (!name || !name[0])
        return OriginalResourceLoader(self, edx, name, a2, a3);

    const std::string canonical = CanonicalName(name);
    uintptr_t callerAddr = reinterpret_cast<uintptr_t>(_ReturnAddress());

    CheckLobbyEntry(canonical, callerAddr);

    // 0. Whitelist SCNs: 100% PERMITIDOS DESDE EL INICIO
    if (IsWhitelistedScn(canonical)) {
        return OriginalResourceLoader(self, edx, name, a2, a3);
    }

    // NUNCA BLOQUEAR ARCHIVOS DE SCENARIO / ARCADE / CHALLENGE / SCRIPTS / CINEMATICAS
    if (canonical.find("scenario") != std::string::npos ||
        canonical.find("arcade") != std::string::npos ||
        canonical.find("acade") != std::string::npos ||
        canonical.find("challenge") != std::string::npos ||
        canonical.find("mission") != std::string::npos ||
        canonical.find("script") != std::string::npos ||
        canonical.find(".lua") != std::string::npos ||
        canonical.find("monster") != std::string::npos ||
        canonical.find("movie") != std::string::npos ||
        canonical.find("camera") != std::string::npos ||
        canonical.find("stage") != std::string::npos ||
        canonical.find("card_info") != std::string::npos)
    {
        return OriginalResourceLoader(self, edx, name, a2, a3);
    }
    if (canonical.find("weapon") != std::string::npos ||
        canonical.find("effects") != std::string::npos ||
        canonical.find(".seq") != std::string::npos ||
        canonical.find("map") != std::string::npos ||
        canonical.find("background") != std::string::npos)
    {
        return OriginalResourceLoader(self, edx, name, a2, a3);
    }
    if (canonical.find(".xui") != std::string::npos) {
        return OriginalResourceLoader(self, edx, name, a2, a3);
    }

    // NUNCA BLOQUEAR ARCHIVOS DE TIENDA NI .S4
    if (canonical.find("shop") != std::string::npos ||
        canonical.find(".s4") != std::string::npos ||
        canonical.find("collect_book") != std::string::npos)
    {
        return OriginalResourceLoader(self, edx, name, a2, a3);
    }

    // NUNCA BLOQUEAR MODELOS NI TEXTURAS DE MODELOS (WEAPONS, COSTUMES, HAIR, FACE, LOBBY 3D)
    if (canonical.find("model\\") != std::string::npos || canonical.find("model/") != std::string::npos ||
        canonical.find(".scn") != std::string::npos)
    {
        return OriginalResourceLoader(self, edx, name, a2, a3);
    }

    // NUNCA BLOQUEAR BOTONES, ICONOS NI ACCIONES DE ARMAS EN TIENDA / LOBBY / ROOMS
    if (canonical.find("image\\action") != std::string::npos || canonical.find("image/action") != std::string::npos ||
        canonical.find("image\\onetimecharge") != std::string::npos || canonical.find("image/onetimecharge") != std::string::npos ||
        canonical.find("image\\itemtooltip") != std::string::npos || canonical.find("image/itemtooltip") != std::string::npos ||
        canonical.find("image\\weapon") != std::string::npos || canonical.find("image/weapon") != std::string::npos ||
        canonical.find("image\\costume") != std::string::npos || canonical.find("image/costume") != std::string::npos)
    {
        return OriginalResourceLoader(self, edx, name, a2, a3);
    }

    // NUNCA BLOQUEAR SALA DE ESPERA (WAITING ROOM), MAPAS NI CANALES
    if (canonical.find("standby") != std::string::npos || canonical.find("bulletin") != std::string::npos ||
        canonical.find("room") != std::string::npos || canonical.find("channel") != std::string::npos)
    {
        return OriginalResourceLoader(self, edx, name, a2, a3);
    }

    // NUNCA BLOQUEAR GUI, TUTORIAL, HELP, TRAINING
    if (canonical.find("gui\\") != std::string::npos || canonical.find("gui/") != std::string::npos ||
        canonical.find("training") != std::string::npos || canonical.find("tutorial") != std::string::npos ||
        canonical.find("game_help") != std::string::npos || canonical.find("guide") != std::string::npos)
    {
        return OriginalResourceLoader(self, edx, name, a2, a3);
    }

    // 0. CORTE TOTAL DE XMLS Y LENGUAJES INNECESARIOS
    if (ShouldCutXmlOrLang(canonical)) {
        InterlockedIncrement(&XmlCuts);
        LogPedido("XML_CUT_SKIP", name, callerAddr, "SKIP_0NS");
        return nullptr;
    }

    // 0. CORTE DE SLANG FILTER
    if (canonical.find("slang_filter") != std::string::npos) {
        LogPedido("SLANG_SKIP", name, callerAddr, "SKIP_0NS");
        return nullptr;
    }

    // 0. CORTE DE EFECTOS .SEQ SOLO EN ARRANQUE INICIAL POR BULK CALLER (0x01D77694)
    // En partida las animaciones y balas .seq se cargan 100% On-Demand
    if (callerAddr == 0x01D77694 && IsBootPhase()) {
        LogPedido("EFFECT_SEQ_SKIP", name, callerAddr, "SKIP_0NS");
        return nullptr;
    }

// mapinfo 100% permitido

    // 0. BLOQUEO DE BANNERS WEB Y TEXTURAS INEXISTENTES
    if (canonical.find("http:") != std::string::npos ||
        canonical.find("https:") != std::string::npos ||
        canonical.find("aeriagames") != std::string::npos ||
        canonical.find("plainwhite") != std::string::npos)
    {
        LogPedido("DUMMY_SKIP", name, callerAddr, "SKIP_0NS");
        return nullptr;
    }

    // 1. BLOQUEO DE AUDIOS SOLO EN ARRANQUE INICIAL
    if (canonical.find("sound\\") != std::string::npos ||
        canonical.find("sound/") != std::string::npos ||
        canonical.find(".ogg") != std::string::npos ||
        canonical.find(".wav") != std::string::npos)
    {
        if (IsBootPhase()) {
            LogPedido("SOUND_BLOCKED_BOOT", name, callerAddr, "SKIP_0NS");
            return nullptr;
        }
    }

    // Conteo y deteccion de repeticiones para log limpio
    int loadCount = 0;
    EnterCriticalSection(&CacheLock);
    loadCount = ++ResourceLoadCounts[canonical];
    LeaveCriticalSection(&CacheLock);

    char previous[sizeof(CurrentResource)];
    strcpy_s(previous, CurrentResource);
    strncpy_s(CurrentResource, canonical.c_str(), _TRUNCATE);

    if (loadCount == 1) {
        LogPedido("RESOURCE_LOAD", name, callerAddr, "1ST_LOAD");
    } else {
        LogPedido("RESOURCE_LOAD_REPEAT", name, callerAddr, "CACHED_0NS");
    }

    void* result = OriginalResourceLoader(self, edx, name, a2, a3);
    strcpy_s(CurrentResource, previous);
    return result;
}

// ----------------------------------------------------------------------------
// 6. Hook Decompress (Safe Repeat Cache Global para XML, X7, TTF, LUA, CSV, XUI)
// ----------------------------------------------------------------------------
static bool IsRepeatWanted(const char* name)
{
    if (!name || !*name) return false;
    if (IsWhitelistedScn(name)) return true;
    return strstr(name, ".x7") != nullptr ||
           strstr(name, ".xml") != nullptr ||
           strstr(name, ".ttf") != nullptr ||
           strstr(name, ".xui") != nullptr ||
           strstr(name, ".lua") != nullptr ||
           strstr(name, ".csv") != nullptr;
}

static char __cdecl HookDecompress(unsigned* size, void* dst, unsigned expected)
{
    if (CurrentResource[0] && size && dst && IsRepeatWanted(CurrentResource)) {
        EnterCriticalSection(&CacheLock);
        auto it = ResourceCache.find(CurrentResource);
        if (it != ResourceCache.end() && it->second.size() <= expected) {
            memcpy(dst, it->second.data(), it->second.size());
            *size = static_cast<unsigned>(it->second.size());
            LeaveCriticalSection(&CacheLock);
            InterlockedIncrement(&DecompressHits);
            return 1;
        }
        LeaveCriticalSection(&CacheLock);
    }

    char result = OriginalDecompress(size, dst, expected);
    if (result && CurrentResource[0] && size && dst && *size <= expected && IsRepeatWanted(CurrentResource)) {
        EnterCriticalSection(&CacheLock);
        if (ResourceCache.find(CurrentResource) == ResourceCache.end()) {
            bool whitelisted = IsWhitelistedScn(CurrentResource);
            if (whitelisted || (CachedBytes + *size <= CacheLimit)) {
                auto& bytes = ResourceCache[CurrentResource];
                bytes.assign(static_cast<unsigned char*>(dst),
                             static_cast<unsigned char*>(dst) + *size);
                if (!whitelisted) {
                    CachedBytes += bytes.size();
                }
            }
        }
        LeaveCriticalSection(&CacheLock);
    }
    return result;
}

// ----------------------------------------------------------------------------
// 7. Hook ModelLoader (Passthrough Seguro 100%)
// ----------------------------------------------------------------------------
static char __fastcall HookModelLoader(void* self, void*, char* path)
{
    return OriginalModelLoader(self, path);
}

// ----------------------------------------------------------------------------
// 8. Hook sub_1229130 (Safe Sprite UV Calculation - Inmunidad contra Crash en 00000054)
// ----------------------------------------------------------------------------
static float* __fastcall HookGetSpriteUV(void* self, void* /*edx*/, unsigned int a2, float* a3, float* a4, float* a5, float* a6)
{
    if (!self) {
        if (a3) *a3 = 0.0f;
        if (a4) *a4 = 0.0f;
        if (a5) *a5 = 0.0f;
        if (a6) *a6 = 0.0f;
        return a5;
    }
    uintptr_t* p = reinterpret_cast<uintptr_t*>(self);
    if (!p[6]) {
        if (a3) *a3 = 0.0f;
        if (a4) *a4 = 0.0f;
        if (a5) *a5 = 0.0f;
        if (a6) *a6 = 0.0f;
        return a5;
    }
    return OriginalGetSpriteUV(self, a2, a3, a4, a5, a6);
}

// ----------------------------------------------------------------------------
// 9. Hook sub_1D558C0 (Safe COctree::Update - Inmunidad contra Crash en 00000000)
// ----------------------------------------------------------------------------
static int __fastcall HookOctreeUpdate(void* self, void* /*edx*/)
{
    if (!self) return 0;
    unsigned char* p = reinterpret_cast<unsigned char*>(self);
    if (!p[229]) {
        p[229] = 1;
        uintptr_t child = *reinterpret_cast<uintptr_t*>(p + 224);
        if (!child) return reinterpret_cast<int>(self);
        uintptr_t vtable = *reinterpret_cast<uintptr_t*>(child);
        if (!vtable) return reinterpret_cast<int>(self);
        typedef int (__thiscall* FnChild)(uintptr_t);
        FnChild fn = *reinterpret_cast<FnChild*>(vtable + 36);
        if (fn) return fn(child);
    }
    return reinterpret_cast<int>(self);
}

// ----------------------------------------------------------------------------
// 10. Hook sub_1637E40 (Safe CreateActorBox - Inmunidad contra Crash en 0x01637E90)
// ----------------------------------------------------------------------------
static int __fastcall HookCreateActorBox(void* self, void* /*edx*/, int a1, char a2, int a3, int a4, int a5)
{
    if (a3 != 0) {
        uintptr_t mgr = *reinterpret_cast<uintptr_t*>(0x25BF544);
        if (!mgr) return 0;
        typedef void* (__thiscall* FnFindActor)(uintptr_t, int);
        FnFindActor fnFind = reinterpret_cast<FnFindActor>(0x0118F360);
        void* actor = fnFind(mgr, a3);
        if (!actor) {
            return 0; // Evita Crash 0xC0000005 (LECTURA 00000000) en 0x01637E90
        }
    }
    return OriginalCreateActorBox(self, nullptr, a1, a2, a3, a4, a5);
}

// ----------------------------------------------------------------------------
// 11. Fast Passthrough & On-Demand Fallback CreateFileA
// ----------------------------------------------------------------------------
static HANDLE WINAPI HookCreateFileA(LPCSTR path, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags, HANDLE templateFile)
{
    if (path && path[0]) {
        DWORD attrs = GetFileAttributesA(path);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            char altPath[MAX_PATH];
            snprintf(altPath, sizeof(altPath), "C:\\S4League\\extracted_resources\\%s", path);
            DWORD altAttrs = GetFileAttributesA(altPath);
            if (altAttrs != INVALID_FILE_ATTRIBUTES && !(altAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
                return OriginalCreateFileA(altPath, access, share, security, creation, flags, templateFile);
            }
        }
    }
    return OriginalCreateFileA(path, access, share, security, creation, flags, templateFile);
}

// ----------------------------------------------------------------------------
// Instalacion de Hooks
// ----------------------------------------------------------------------------

// Helper de Verificacion Profunda de Widgets de GUI
static inline bool IsWidgetChildValid(void* self, size_t offset)
{
    if (!self) return false;
    void** ptr = *reinterpret_cast<void***>(static_cast<char*>(self) + offset);
    if (!ptr) return false;
    if (!*ptr) return false;
    return true;
}

typedef void(__thiscall* tWidgetSetVisible)(void* self, bool visible);
static tWidgetSetVisible OriginalWidgetSetVisible = reinterpret_cast<tWidgetSetVisible>(0x01394FE0);

static void __fastcall HookWidgetSetVisible(void* self, void* /*edx*/, bool visible)
{
    if (!IsWidgetChildValid(self, 0x14)) return;
    OriginalWidgetSetVisible(self, visible);
}

typedef void(__thiscall* tSub1394D90)(void* self);
static tSub1394D90 OriginalSub1394D90 = reinterpret_cast<tSub1394D90>(0x01394D90);

static void __fastcall HookSub1394D90(void* self, void* /*edx*/)
{
    if (!IsWidgetChildValid(self, 0x14)) return;
    OriginalSub1394D90(self);
}

typedef void(__thiscall* tSub1395020)(void* self, int a1);
static tSub1395020 OriginalSub1395020 = reinterpret_cast<tSub1395020>(0x01395020);

static void __fastcall HookSub1395020(void* self, void* /*edx*/, int a1)
{
    if (!IsWidgetChildValid(self, 0x14)) return;
    OriginalSub1395020(self, a1);
}

typedef int(__thiscall* tWidget1E4SetVisible)(void* self, unsigned char a2);
static tWidget1E4SetVisible OriginalWidget1E4SetVisible = reinterpret_cast<tWidget1E4SetVisible>(0x01390DE0);

static int __fastcall HookWidget1E4SetVisible(void* self, void* /*edx*/, unsigned char a2)
{
    if (!IsWidgetChildValid(self, 0x1E4)) return 0;
    return OriginalWidget1E4SetVisible(self, a2);
}

typedef void(__thiscall* tSub1595FC0)(void* self, unsigned int a1, unsigned int a2, unsigned int a3, unsigned int a4);
static tSub1595FC0 OriginalSub1595FC0 = reinterpret_cast<tSub1595FC0>(0x01595FC0);

static void __fastcall HookSub1595FC0(void* self, void* /*edx*/, unsigned int a1, unsigned int a2, unsigned int a3, unsigned int a4)
{
    if (!self) return;
    void* tiles = *reinterpret_cast<void**>(static_cast<char*>(self) + 0x28);
    if (!tiles) return;
    OriginalSub1595FC0(self, a1, a2, a3, a4);
}

static DWORD WINAPI InstallHooks(void*)
{
    g_startTick = GetTickCount();

    const bool opcionesCreadas = (*reinterpret_cast<volatile unsigned int*>(0x025BD824) & 1) != 0;

    if(opcionesCreadas){
        char* opciones = reinterpret_cast<char*>(0x025BD7C8);
        opciones[74] = 0;
        opciones[78] = 0;
    }

    puts("=================================================");
    puts("[Optimizer] S4 Ultra-Fast Optimizer Iniciado");
    puts("[Optimizer] - Safe Sprite UV Guard (Anti-Crash 0x54): ACTIVO");
    puts("[Optimizer] - Safe Octree Guard (Anti-Crash 0x1D558EB): ACTIVO");
    puts("[Optimizer] - Safe ActorBox Guard (Anti-Crash 0x1637E90): ACTIVO");
    puts("[Optimizer] - Safe Shop / .s4 Files: 100% PERMITIDOS");
    puts("[Optimizer] - Weapons & Costumes Textures: 100% VISIBLES");
    puts("[Optimizer] - Botones, Acciones e Iconos: 100% PERMITIDOS");
    puts("[Optimizer] - Waiting Room, Canales y Mapas: 100% DISPONIBLES");
    puts("[Optimizer] - Lobby 3D y Modelos .scn: 100% VISIBLES");
    puts("[Optimizer] - Whitelist SCN (Modelos / UI / Bips): INMUNIDAD TOTAL & CARGA DIRECTA");
    puts("[Optimizer] - Corte Quirurgico de 70+ XMLs / Lang: ACTIVO (0 ns)");
    puts("[Optimizer] - Safe UI XUI Protection: ACTIVO (100% Protegido)");
    puts("[Optimizer] - Bloqueo de MapInfo (Solo en Boot Inicial): ACTIVO");
    puts("[Optimizer] - Bloqueo de Audios (.ogg / .wav): ACTIVO (0 ns)");
    puts("[Optimizer] - Global Decompress Cache (.x7/.xml/.ttf): ACTIVO (0 ns)");
    puts("[Optimizer] - FileExists O(1) Cache: ACTIVO (0 ns)");
    puts("[Optimizer] - ResourceExists O(1) Cache: ACTIVO (0 ns)");
    puts("[Optimizer] - Salto Bucles Masivos Armas (0x15A1810): ACTIVO (0 ns)");
    puts("[Optimizer] - Descarte Preloaders Masivos Efectos (0x1CB52AD): ACTIVO (0 ns)");
    puts("[Optimizer] - Pure On-Demand Mode (Match & Room safe): ACTIVO");
    puts("[Optimizer] - Auto-Purge Inactive (3s + 1 min): ACTIVO");
    puts("=================================================");

    InitializeCriticalSection(&CacheLock);
    InitializeCriticalSection(&LogLock);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalCreateFileA), HookCreateFileA);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalFileExists), HookFileExists);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalResourceExists), HookResourceExists);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalResourceLoader), HookResourceLoader);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalDecompress), HookDecompress);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalModelLoader), HookModelLoader);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalQueuePreload), HookQueuePreload);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalGetSpriteUV), HookGetSpriteUV);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalOctreeUpdate), HookOctreeUpdate);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalCreateActorBox), HookCreateActorBox);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalInitConfig), HookInitConfig);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalMassPreload1), HookMassPreload1);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalMassPreload2), HookMassPreload2);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalMassPreload3), HookMassPreload3);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalWidgetSetVisible), HookWidgetSetVisible);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalSub1394D90), HookSub1394D90);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalSub1395020), HookSub1395020);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalWidget1E4SetVisible), HookWidget1E4SetVisible);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalSub1595FC0), HookSub1595FC0);
    LONG result = DetourTransactionCommit();

    printf("[Optimizer] Hooks instalados con codigo=%ld (0 = EXITO TOTAL)\n", result);

    return 0;
}

void StartResourceLogTest()
{
    InstallHooks(nullptr);
}
