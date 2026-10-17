#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <detours.h>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdint>

#define printf(...) ((void)0)
#define puts(...) ((void)0)

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dx9.lib")

// ============================================================================
// S4 League Standalone HD Font Renderer with Arabic & Unicode Support v4.0
// Features:
// - Automatic CP1256 Arabic & UTF-8 decoding (resolves Mojibake "ÇÎãÔ" -> "اخمش")
// - Arabic Cursive Shaping (Presentation Forms-B U+FE80 - U+FEFC) & RTL BiDi
// - Native DirectX 9 Unicode rendering via ID3DXFont::DrawTextW
// - Slender fine strokes (0.44 width ratio, [F11] hotkey)
// - Crisp 1px drop shadow border (+1, +1) ([F8] hotkey)
// - Confirmed perfect size (-3px offset, [F5]/[F6] hotkeys)
// - CLEARTYPE_NATURAL_QUALITY (6) for razor-sharp text
// - Natural 1-space separation for Battle stats ([F3]/[F4] hotkeys)
// ============================================================================

#ifndef CLEARTYPE_NATURAL_QUALITY
#define CLEARTYPE_NATURAL_QUALITY 6
#endif

static volatile bool g_enableHDText       = true;
static volatile bool g_suppressLegacy     = true;   // Suppress blurry old FreeType
static volatile bool g_logEnabled          = true;
static volatile bool g_logChatDebug        = false;
static volatile LONG g_countCaught         = 0;
static volatile LONG g_countRendered       = 0;

static std::string   g_overrideFontFace    = "Verdana"; // True S4 Verdana font
static float         g_fontScaleMult       = 1.0f;     // Native ratio
static int           g_fontSizeOffset      = -3;       // 3px smaller (perfect size)
static float         g_fontWidthRatio      = 0.44f;    // Slender aspect ratio ("mas fina")
static int           g_defaultWeight       = FW_NORMAL;// 400: Sleek, clean
static DWORD         g_fontQuality         = CLEARTYPE_NATURAL_QUALITY; // Highest optical resolution
static bool          g_enableShadow        = true;     // Crisp 1px shadow border (+1, +1)
static int           g_battleMarginRight   = 7;        // Natural 1-space separation (~7px)

static bool activarTextoHd = true;
static bool activarEmoji = true;

std::wstring ConvertToUnicodeAndShape(const char* str, int len = -1);
const char* FuenteParaIdioma(const std::wstring& texto);
int EmojiEn(const std::wstring& texto, size_t posicion);
bool EmojiDisponible(int emoji);
bool EmojiAnimado(int emoji);
float ProporcionEmoji(int emoji);
void DibujarEmoji(IDirect3DDevice9* dispositivo, int emoji, float x, float y, float alto);

static const uint32_t kBaseCliente = 0x00E80000;
static const int kSinEmoji = 0;
static const int kEmojiChat = 1;
static const int kEmojiGlobo = 2;
static const wchar_t kMarcaEmoji = 0xE000;
static const float kEscalaEmojiChat = 1.2f;
static const float kEscalaEmojiGlobo = 2.9f;
static const float kEscalaEmojiAnimado = 2.5f;
static const float kSeparacionEmoji = 6.f;

struct SitioChat {
    uint32_t dibujo;
    uint32_t dueno;
    int tipo;
};

struct EmojiUbicado {
    int emoji;
    float x;
    float y;
    float alto;
};

static const SitioChat kSitiosChat[] = {
    { 0x00E1E872, 0x00E182F0, kEmojiChat },
    { 0x00E21B4F, 0x00E182F0, kEmojiGlobo },
    { 0x00E21B4F, 0x00EE846F, kEmojiChat },
};

static uint32_t retornoDibujo = 0;
static uint32_t marcoDibujo = 0;
static void* destinoDibujo = nullptr;

static uint32_t DuenoDeMarco(uint32_t marco)
{
    __try {
        if(marco <= 0x10000){
            return 0;
        }

        const uint32_t arriba = *(const uint32_t*)marco;

        if(arriba <= 0x10000){
            return 0;
        }

        return *(const uint32_t*)(arriba + 4) - kBaseCliente;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int SitioEmoji(uint32_t retorno, uint32_t marco)
{
    const uint32_t llamada = retorno - kBaseCliente;
    const uint32_t dueno = DuenoDeMarco(marco);

    for(const SitioChat& sitio : kSitiosChat){
        if(sitio.dibujo == llamada && sitio.dueno == dueno){
            return sitio.tipo;
        }
    }

    return kSinEmoji;
}

static std::wstring MarcarEmojis(const std::wstring& texto, std::vector<int>& emojis)
{
    std::wstring marcado;

    for(size_t i = 0; i < texto.size(); i++){
        const int emoji = EmojiEn(texto, i);

        if(emoji < 0 || !EmojiDisponible(emoji)){
            marcado.push_back(texto[i]);
            continue;
        }

        marcado.push_back(kMarcaEmoji);
        emojis.push_back(emoji);
        i++;
    }

    return marcado;
}

// ----------------------------------------------------------------------------
// Queue & Font Cache Structures
// ----------------------------------------------------------------------------
struct HDQueuedText {
    int virtualX;
    int virtualY;
    float z;
    std::wstring textW; // Full Unicode wide string
    int sitioEmoji;
    std::vector<int> emojis;
    std::string face;
    int baseHeight;
    int weight;
    bool italic;
    D3DCOLOR color;
    D3DCOLOR outlineColor;
    bool hasOutline;
    int outlineSize;
};

static std::vector<HDQueuedText> g_HDTextQueue;
static CRITICAL_SECTION g_HDTextLock;

struct HDFontKey {
    std::string face;
    int height;
    int width;
    int weight;
    bool italic;
    DWORD quality;
    bool operator==(const HDFontKey& o) const {
        return height == o.height && width == o.width && weight == o.weight &&
               italic == o.italic && quality == o.quality && face == o.face;
    }
};

struct HDFontKeyHash {
    size_t operator()(const HDFontKey& k) const {
        size_t h = std::hash<std::string>()(k.face);
        h ^= std::hash<int>()(k.height) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(k.width) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(k.weight) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>()(k.italic) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<DWORD>()(k.quality) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

static std::unordered_map<HDFontKey, ID3DXFont*, HDFontKeyHash> g_HDFontCache;
static IDirect3DDevice9* g_pLastD3DDev = nullptr;

static inline IDirect3DDevice9* GetS4D3DDevice() {
    uintptr_t* pRenderDev = *reinterpret_cast<uintptr_t**>(0x025BD0B8);
    if (!pRenderDev) return nullptr;
    return *reinterpret_cast<IDirect3DDevice9**>((char*)pRenderDev + 0x69C);
}

static inline D3DCOLOR ConvertFloat4ToD3D(const float* c, float defaultAlpha = 1.0f) {
    if (!c) return 0xFFFFFFFF;
    float a_val = (c[3] > 0.001f) ? c[3] : defaultAlpha;
    int a = (int)(a_val * 255.0f + 0.5f); if (a < 0) a = 0; else if (a > 255) a = 255;
    int r = (int)(c[0] * 255.0f + 0.5f); if (r < 0) r = 0; else if (r > 255) r = 255;
    int g = (int)(c[1] * 255.0f + 0.5f); if (g < 0) g = 0; else if (g > 255) g = 255;
    int b = (int)(c[2] * 255.0f + 0.5f); if (b < 0) b = 0; else if (b > 255) b = 255;
    if (a == 0 && (r > 0 || g > 0 || b > 0)) a = 255;
    return D3DCOLOR_ARGB(a, r, g, b);
}

static std::string CleanFontFace(const char* rawFace) {
    if (!g_overrideFontFace.empty()) {
        return g_overrideFontFace;
    }
    if (!rawFace || !rawFace[0]) return "Verdana";
    if (_strnicmp(rawFace, "2002", 4) == 0) return "Verdana";
    if (_stricmp(rawFace, "default") == 0) return "Verdana";
    if (strstr(rawFace, "verdana") || strstr(rawFace, "Verdana")) return "Verdana";
    if (strstr(rawFace, "segoe") || strstr(rawFace, "Segoe")) return "Segoe UI";
    if (strstr(rawFace, "tahoma") || strstr(rawFace, "Tahoma")) return "Tahoma";
    if (strstr(rawFace, "arial") || strstr(rawFace, "Arial")) return "Arial";
    return rawFace;
}

static ID3DXFont* GetOrCreateHDFont(IDirect3DDevice9* dev, const std::string& face, int height, int width, int weight, bool italic) {
    if (!dev) return nullptr;
    if (dev != g_pLastD3DDev) {
        for (auto& pair : g_HDFontCache) {
            if (pair.second) pair.second->Release();
        }
        g_HDFontCache.clear();
        g_pLastD3DDev = dev;
    }

    if (height <= 0) height = 11;
    HDFontKey key{ face.empty() ? "Verdana" : face, height, width, weight, italic, g_fontQuality };
    auto it = g_HDFontCache.find(key);
    if (it != g_HDFontCache.end()) return it->second;

    ID3DXFont* pFont = nullptr;
    HRESULT hr = D3DXCreateFontA(
        dev,
        height,
        width,
        weight,
        1,
        italic ? TRUE : FALSE,
        DEFAULT_CHARSET,
        OUT_TT_ONLY_PRECIS,
        g_fontQuality,
        DEFAULT_PITCH | FF_DONTCARE,
        key.face.c_str(),
        &pFont
    );
    if (SUCCEEDED(hr) && pFont) {
        g_HDFontCache[key] = pFont;
        return pFont;
    }
    return nullptr;
}

static void LogChatDebug(const char* src, const char* text, const wchar_t* wtext, const char* fontFace) {
    if (!text) return;
    if (!g_logChatDebug) return;
    FILE* f = fopen("hd_chat_debug.log", "a");
    if (f) {
        fprintf(f, "[%s][Font: %s] RAW: '%s' | WIDE: '%ls'\n", src, fontFace ? fontFace : "null", text, wtext ? wtext : L"null");
        fclose(f);
    }
}

static void QueueTextInternal(void* self, int x, int y, float z, const char* text, const char* srcName, int sitioEmoji) {
    if (!text || !text[0]) return;

    if (g_logEnabled && InterlockedIncrement(&g_countCaught) <= 25) {
        printf("[HD_ARABIC][%s] at (%d, %d) '%s'\n", srcName, x, y, text);
    }

    if (!g_enableHDText || !self) return;

    HDQueuedText item;
    item.virtualX = x;
    item.virtualY = y;
    item.z = z;
    // Decode CP1256 Arabic or UTF-8 and shape for RTL
    item.textW = ConvertToUnicodeAndShape(text);
    item.sitioEmoji = sitioEmoji;

    if(sitioEmoji != kSinEmoji){
        item.textW = MarcarEmojis(item.textW, item.emojis);
    }

    const char* fuenteIdioma = FuenteParaIdioma(item.textW);

    if(fuenteIdioma){
        item.face = fuenteIdioma;
    } else {
        const char* rawFace = reinterpret_cast<const char*>(static_cast<char*>(self) + 0x114);
        item.face = CleanFontFace(rawFace);
    }

    LogChatDebug(srcName, text, item.textW.c_str(), item.face.c_str());

    int baseHeight = *reinterpret_cast<int*>(static_cast<char*>(self) + 0x224);
    if (baseHeight <= 0) baseHeight = 13;
    item.baseHeight = baseHeight;

    bool isBold = *reinterpret_cast<unsigned char*>(static_cast<char*>(self) + 0x214) != 0;
    item.weight = isBold ? FW_BOLD : g_defaultWeight;
    item.italic = *reinterpret_cast<unsigned char*>(static_cast<char*>(self) + 0x215) != 0;

    const float* col = reinterpret_cast<const float*>(static_cast<char*>(self) + 0x228);
    const float* sub = reinterpret_cast<const float*>(static_cast<char*>(self) + 0x238);
    item.color = ConvertFloat4ToD3D(col, 1.0f);
    item.outlineColor = ConvertFloat4ToD3D(sub, 1.0f);

    item.outlineSize = *reinterpret_cast<int*>(static_cast<char*>(self) + 0x248);
    item.hasOutline = ((*reinterpret_cast<unsigned char*>(static_cast<char*>(self) + 0x24C) != 0) ||
                       (*reinterpret_cast<unsigned char*>(static_cast<char*>(self) + 0x24D) != 0));

    EnterCriticalSection(&g_HDTextLock);
    g_HDTextQueue.push_back(std::move(item));
    LeaveCriticalSection(&g_HDTextLock);
}

static float AnchoTexto(ID3DXFont* fuente, const std::wstring& texto)
{
    const std::wstring conTope = texto + L"|";
    RECT medidaTexto = { 0, 0, 0, 0 };
    RECT medidaTope = { 0, 0, 0, 0 };
    fuente->DrawTextW(nullptr, conTope.c_str(), -1, &medidaTexto, DT_CALCRECT | DT_SINGLELINE, 0);
    fuente->DrawTextW(nullptr, L"|", -1, &medidaTope, DT_CALCRECT | DT_SINGLELINE, 0);
    return (float)((medidaTexto.right - medidaTexto.left) - (medidaTope.right - medidaTope.left));
}

static std::wstring UbicarEmojis(ID3DXFont* fuente, const HDQueuedText& item, int pantallaX, int pantallaY, int altoLinea, std::vector<EmojiUbicado>& ubicados)
{
    const float anchoEspacio = (std::max)(1.f, AnchoTexto(fuente, L" "));
    const float escala = item.sitioEmoji == kEmojiGlobo
        ? kEscalaEmojiGlobo
        : kEscalaEmojiChat;
    const float altoBase = altoLinea * escala;
    const float reservado = altoLinea + kSeparacionEmoji;
    const int espacios = (int)ceilf(reservado / anchoEspacio);
    const float ocupado = espacios * anchoEspacio;
    std::wstring visible;
    size_t siguiente = 0;

    for(wchar_t letra : item.textW){
        if(letra != kMarcaEmoji || siguiente >= item.emojis.size()){
            visible.push_back(letra);
            continue;
        }

        const int emoji = item.emojis[siguiente++];
        const float alto = EmojiAnimado(emoji)
            ? altoBase * kEscalaEmojiAnimado
            : altoBase;
        const float ancho = alto * ProporcionEmoji(emoji);
        const float inicio = pantallaX + AnchoTexto(fuente, visible);

        ubicados.push_back({ emoji, inicio + (ocupado - ancho) * 0.5f, pantallaY + (altoLinea - alto) * 0.5f, alto });
        visible.append(espacios, L' ');
    }

    return visible;
}

// ----------------------------------------------------------------------------
// Render Queue with Native DrawTextW (Full Unicode & Arabic Support)
// ----------------------------------------------------------------------------
static void RenderHDTextQueue(void* self) {
    EnterCriticalSection(&g_HDTextLock);
    if (g_HDTextQueue.empty()) {
        LeaveCriticalSection(&g_HDTextLock);
        return;
    }

    IDirect3DDevice9* dev = GetS4D3DDevice();
    if (!dev) {
        g_HDTextQueue.clear();
        LeaveCriticalSection(&g_HDTextLock);
        return;
    }

    D3DVIEWPORT9 vp = { 0 };
    float scale = 1.40625f;
    int vpX = 0, vpY = 0;
    if (SUCCEEDED(dev->GetViewport(&vp)) && vp.Height > 0) {
        scale = (float)vp.Height / 768.0f;
        vpX = (int)vp.X;
        vpY = (int)vp.Y;
    }

    int battleIndex = 0;
    int lastBattleVirtualY = -9999;
    int renderedCount = 0;

    for (const auto& item : g_HDTextQueue) {
        int screenX = (int)(item.virtualX * scale + 0.5f) + vpX;
        int screenY = (int)(item.virtualY * scale + 0.5f) + vpY;
        
        int screenHeight = (int)(item.baseHeight * scale * g_fontScaleMult + 0.5f) + g_fontSizeOffset;
        if (screenHeight < 9) screenHeight = 9;

        int fontWidth = (g_fontWidthRatio > 0.05f) ? (int)(screenHeight * g_fontWidthRatio + 0.5f) : 0;

        // Natural 1-space separation for Battle stats
        bool isBattleStat = (item.textW.find(L"match") != std::wstring::npos ||
                             item.textW.find(L"win") != std::wstring::npos ||
                             item.textW.find(L"Loss") != std::wstring::npos ||
                             item.textW.find(L"defeat") != std::wstring::npos ||
                             (item.textW.find(L"%") != std::wstring::npos && item.textW.find(L"(") != std::wstring::npos));

        if (item.textW.find(L"Battle") != std::wstring::npos) {
            lastBattleVirtualY = item.virtualY;
            battleIndex = 0;
        } else if (isBattleStat && abs(item.virtualY - lastBattleVirtualY) <= 4) {
            battleIndex++;
            screenX += battleIndex * g_battleMarginRight;
        }

        ID3DXFont* font = GetOrCreateHDFont(dev, item.face, screenHeight, fontWidth, item.weight, item.italic);
        if (!font) continue;

        std::vector<EmojiUbicado> emojisUbicados;
        std::wstring textoConEmojis;
        const std::wstring* textoVisible = &item.textW;

        if(!item.emojis.empty()){
            textoConEmojis = UbicarEmojis(font, item, screenX, screenY, screenHeight, emojisUbicados);
            textoVisible = &textoConEmojis;
        }

        // 1. Crisp 1px drop shadow border at (+1, +1)
        if (g_enableShadow) {
            RECT rcShadow;
            SetRect(&rcShadow, screenX + 1, screenY + 1, screenX + 1 + 2048, screenY + 1 + 1024);
            D3DCOLOR shadowCol = D3DCOLOR_ARGB(230, 0, 0, 0);
            font->DrawTextW(nullptr, textoVisible->c_str(), -1, &rcShadow, DT_NOCLIP, shadowCol);
        }

        // 2. Main luminous crisp HD Unicode text
        RECT rc;
        SetRect(&rc, screenX, screenY, screenX + 2048, screenY + 1024);
        int res = font->DrawTextW(nullptr, textoVisible->c_str(), -1, &rc, DT_NOCLIP, item.color);
        if (res > 0) renderedCount++;

        for(const EmojiUbicado& ubicado : emojisUbicados){
            DibujarEmoji(dev, ubicado.emoji, ubicado.x, ubicado.y, ubicado.alto);
        }
    }

    if (g_logEnabled && InterlockedIncrement(&g_countRendered) <= 10) {
        printf("[HD_ARABIC] Rendered %d HD Unicode text items (scale=%.3f, offset=%d px, widthRatio=%.2f)\n", 
               renderedCount, scale, g_fontSizeOffset, g_fontWidthRatio);
    }

    g_HDTextQueue.clear();
    LeaveCriticalSection(&g_HDTextLock);
}

// ----------------------------------------------------------------------------
// S4 Client Hooks (Proven 100% Stable)
// ----------------------------------------------------------------------------
typedef void (__thiscall* tEUDrawTextA)(void* self, int x, int y, float z, const char* text);
static tEUDrawTextA OriginalEUDrawTextA = reinterpret_cast<tEUDrawTextA>(0x01DF0060);

static void __fastcall HookEUDrawTextA(void* self, void* /*edx*/, int x, int y, float z, const char* text) {
    const int sitioEmoji = activarEmoji
        ? SitioEmoji(retornoDibujo, marcoDibujo)
        : kSinEmoji;
    QueueTextInternal(self, x, y, z, text, "EU_DrawTextA", sitioEmoji);
    if (!g_suppressLegacy) {
        OriginalEUDrawTextA(self, x, y, z, text);
    }
}

__declspec(naked) static void EntradaDibujoTexto()
{
    __asm {
        mov eax, dword ptr [esp]
        mov retornoDibujo, eax
        mov marcoDibujo, ebp
        jmp dword ptr [destinoDibujo]
    }
}

typedef void (__thiscall* tEUDrawText2)(void* self, int x, int y, float z, const char* text);
static tEUDrawText2 OriginalEUDrawText2 = reinterpret_cast<tEUDrawText2>(0x01DF03F0);

static void __fastcall HookEUDrawText2(void* self, void* /*edx*/, int x, int y, float z, const char* text) {
    QueueTextInternal(self, x, y, z, text, "EU_DrawText2", kSinEmoji);
    if (!g_suppressLegacy) {
        OriginalEUDrawText2(self, x, y, z, text);
    }
}

typedef void (__thiscall* tKRDrawTextA)(void* self, int x, int y, float z, const char* text);
static tKRDrawTextA OriginalKRDrawTextA = reinterpret_cast<tKRDrawTextA>(0x01CA51B0);

static void __fastcall HookKRDrawTextA(void* self, void* /*edx*/, int x, int y, float z, const char* text) {
    QueueTextInternal(self, x, y, z, text, "KR_DrawTextA", kSinEmoji);
    if (!g_suppressLegacy) {
        OriginalKRDrawTextA(self, x, y, z, text);
    }
}

typedef void (__thiscall* tFontRender)(void* self);
static tFontRender OriginalFontRender = reinterpret_cast<tFontRender>(0x01CBB5E0);

static void __fastcall HookFontRender(void* self, void* /*edx*/) {
    if (g_enableHDText) {
        RenderHDTextQueue(self);
    }

    if (g_suppressLegacy && self) {
        // Clear legacy glyph quads buffer so old blurry font is NEVER drawn
        *reinterpret_cast<int*>(static_cast<char*>(self) + 0x1C250) = 0;
        return;
    }

    OriginalFontRender(self);
}

// ----------------------------------------------------------------------------
// Key Monitor & Hotkeys
// ----------------------------------------------------------------------------
static DWORD WINAPI KeyMonitorThread(void*) {
    while (true) {
        Sleep(100);
        if (GetAsyncKeyState(VK_F3) & 1) {
            g_battleMarginRight -= 1;
            if (g_battleMarginRight < 0) g_battleMarginRight = 0;
            printf("[HD_ARABIC] Battle Spacing: %d px\n", g_battleMarginRight);
        }
        if (GetAsyncKeyState(VK_F4) & 1) {
            g_battleMarginRight += 1;
            if (g_battleMarginRight > 30) g_battleMarginRight = 30;
            printf("[HD_ARABIC] Battle Spacing: %d px\n", g_battleMarginRight);
        }
        if (GetAsyncKeyState(VK_F5) & 1) {
            g_fontSizeOffset -= 1;
            EnterCriticalSection(&g_HDTextLock);
            for (auto& p : g_HDFontCache) { if (p.second) p.second->Release(); }
            g_HDFontCache.clear();
            LeaveCriticalSection(&g_HDTextLock);
            printf("[HD_ARABIC] Font Size Offset: %d px\n", g_fontSizeOffset);
        }
        if (GetAsyncKeyState(VK_F6) & 1) {
            g_fontSizeOffset += 1;
            EnterCriticalSection(&g_HDTextLock);
            for (auto& p : g_HDFontCache) { if (p.second) p.second->Release(); }
            g_HDFontCache.clear();
            LeaveCriticalSection(&g_HDTextLock);
            printf("[HD_ARABIC] Font Size Offset: %d px\n", g_fontSizeOffset);
        }
        if (GetAsyncKeyState(VK_F7) & 1) {
            g_enableHDText = !g_enableHDText;
            printf("[HD_ARABIC] HD Font Renderer: %s\n", g_enableHDText ? "ON" : "OFF");
        }
        if (GetAsyncKeyState(VK_F8) & 1) {
            g_enableShadow = !g_enableShadow;
            printf("[HD_ARABIC] Shadow Border (+1,+1): %s\n", g_enableShadow ? "ENABLED" : "DISABLED");
        }
        if (GetAsyncKeyState(VK_F9) & 1) {
            g_suppressLegacy = !g_suppressLegacy;
            printf("[HD_ARABIC] Suppress Legacy Font: %s\n", g_suppressLegacy ? "SUPPRESSED" : "VISIBLE");
        }
        if (GetAsyncKeyState(VK_F10) & 1) {
            if (g_overrideFontFace == "Verdana") g_overrideFontFace = "Segoe UI";
            else if (g_overrideFontFace == "Segoe UI") g_overrideFontFace = "Tahoma";
            else if (g_overrideFontFace == "Tahoma") g_overrideFontFace = "Arial";
            else g_overrideFontFace = "Verdana";
            EnterCriticalSection(&g_HDTextLock);
            for (auto& p : g_HDFontCache) { if (p.second) p.second->Release(); }
            g_HDFontCache.clear();
            LeaveCriticalSection(&g_HDTextLock);
            printf("[HD_ARABIC] Active Font Face: %s\n", g_overrideFontFace.c_str());
        }
        if (GetAsyncKeyState(VK_F11) & 1) {
            if (g_fontWidthRatio == 0.0f) g_fontWidthRatio = 0.44f;
            else if (g_fontWidthRatio == 0.44f) g_fontWidthRatio = 0.38f;
            else g_fontWidthRatio = 0.0f;
            EnterCriticalSection(&g_HDTextLock);
            for (auto& p : g_HDFontCache) { if (p.second) p.second->Release(); }
            g_HDFontCache.clear();
            LeaveCriticalSection(&g_HDTextLock);
            printf("[HD_ARABIC] Font Width Ratio: %.2f (%s)\n", g_fontWidthRatio, 
                   (g_fontWidthRatio == 0.0f) ? "Standard Width" : (g_fontWidthRatio == 0.44f) ? "Fina (Slender)" : "Ultra-Fina");
        }
    }
    return 0;
}

static DWORD WINAPI InitHDTextArabic(void*) {
    puts("=================================================");
    puts("[HD_ARABIC] S4 League Native HD Arabic Renderer v4.0");
    puts("[HD_ARABIC] - Cursive Arabic Shaping (U+FE80 - U+FEFC)");
    puts("[HD_ARABIC] - Bi-directional Right-to-Left (RTL) ordering");
    puts("[HD_ARABIC] - Native DirectX 9 DrawTextW Unicode Engine");
    puts("[HD_ARABIC] - Slender Fine Letter Strokes (0.44)");
    puts("[HD_ARABIC] - Crisp 1px Shadow Border (+1, +1)");
    puts("=================================================");

    if(*reinterpret_cast<const unsigned char*>(0x01DF0060) == 0xE9){
        puts("[HD_ARABIC] The client's text draw is already hooked, HD text not installed");
        return 0;
    }

    InitializeCriticalSection(&g_HDTextLock);
    destinoDibujo = (void*)&HookEUDrawTextA;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalEUDrawTextA), EntradaDibujoTexto);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalEUDrawText2), HookEUDrawText2);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalKRDrawTextA), HookKRDrawTextA);
    DetourAttach(reinterpret_cast<PVOID*>(&OriginalFontRender), HookFontRender);
    LONG res = DetourTransactionCommit();

    printf("[HD_ARABIC] Hooks installed with status = %ld (0 = SUCCESS)\n", res);

    CreateThread(nullptr, 0, KeyMonitorThread, nullptr, 0, nullptr);
    return 0;
}

void StartHdText() {
    if(!activarTextoHd){
        return;
    }

    if (!LoadLibraryA("d3dx9_43.dll")) {
        return;
    }

    InitHDTextArabic(nullptr);
}
