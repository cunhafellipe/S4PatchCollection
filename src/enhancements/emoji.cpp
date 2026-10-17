#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <wincodec.h>
#include <cstdint>
#include <cstring>
#include <string>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "uuid.lib")

static const int kMaxCuadros = 64;

struct DefinicionEmoji {
    wchar_t primera;
    wchar_t segunda;
    const char* archivo;
};

struct ImagenEmoji {
    IDirect3DTexture9* texturas[kMaxCuadros];
    uint32_t* pixeles[kMaxCuadros];
    UINT esperas[kMaxCuadros];
    int cuadros;
    UINT duracion;
    UINT ancho;
    UINT alto;
    bool intentada;
};

struct VerticeEmoji {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};

static const DefinicionEmoji kEmojis[] = {
    { L':', L')', "1f600.png" },
    { L':', L'D', "1f600.png" },
    { L':', L'(', "1f622.png" },
    { L':', L'P', "1f61b.png" },
    { L':', L'o', "1f633.png" },
    { L';', L')', "1f60d.png" },
    { L'<', L'3', "2764.png" },
    { L':', L'c', "1f431.png" },
    { L':', L'g', "1f436.png" },
    { L':', L'm', "1f42d.png" },
    { L':', L'z', "1f355.png" },
    { L':', L'k', "1f382.png" },
    { L':', L'e', "1f389.png" },
    { L':', L't', "1f34a.png" },
    { L':', L'i', "1f4a1.png" },
    { L':', L'x', "1f525.png" },
    { L':', L'v', "1f697.png" },
    { L':', L'w', "1f31f.png" },
    { L':', L'n', "1f638.png" },
    { L':', L'1', "../gifs/pokemon.gif" },
    { L':', L'2', "../gifs/what's-up-sprigatito.gif" },
    { L':', L'3', "../gifs/beer-beer-bottle.gif" },
};

static const int kTotalEmojis = sizeof(kEmojis) / sizeof(kEmojis[0]);

static ImagenEmoji imagenes[kTotalEmojis] = {};
static IDirect3DDevice9* dispositivoTexturas = nullptr;

static bool EmojiValido(int emoji)
{
    return emoji >= 0 && emoji < kTotalEmojis;
}

static bool MismaLetra(wchar_t buscada, wchar_t letra)
{
    if(buscada == letra){
        return true;
    }

    const bool esMayuscula = buscada >= L'A' && buscada <= L'Z';
    return esMayuscula && letra == (wchar_t)(buscada - L'A' + L'a');
}

static void RutaEmoji(int emoji, wchar_t* salida)
{
    char ruta[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, ruta, MAX_PATH);
    char* barra = strrchr(ruta, '\\');

    if(barra){
        barra[1] = '\0';
    }

    strcat_s(ruta, "emoji\\");
    strcat_s(ruta, kEmojis[emoji].archivo);
    MultiByteToWideChar(CP_ACP, 0, ruta, -1, salida, MAX_PATH);
}

static UINT LeerMetadato(IWICMetadataQueryReader* lector, const wchar_t* nombre, UINT porDefecto)
{
    PROPVARIANT valor;
    PropVariantInit(&valor);
    UINT resultado = porDefecto;

    if(SUCCEEDED(lector->GetMetadataByName(nombre, &valor)) && valor.vt == VT_UI2){
        resultado = valor.uiVal;
    }

    PropVariantClear(&valor);
    return resultado;
}

static void ComponerCuadro(IWICImagingFactory* fabrica, IWICBitmapFrameDecode* cuadro, ImagenEmoji& imagen, uint32_t* lienzo,
    UINT anchoCuadro, UINT altoCuadro, UINT izquierda, UINT arriba, UINT espera)
{
    uint32_t* temporal = (uint32_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, anchoCuadro * altoCuadro * 4);
    IWICFormatConverter* conversor = nullptr;
    const bool convertido = temporal
        && SUCCEEDED(fabrica->CreateFormatConverter(&conversor))
        && conversor
        && SUCCEEDED(conversor->Initialize(cuadro, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))
        && SUCCEEDED(conversor->CopyPixels(nullptr, anchoCuadro * 4, anchoCuadro * altoCuadro * 4, (BYTE*)temporal));

    if(convertido){
        for(UINT y = 0; y < altoCuadro && arriba + y < imagen.alto; y++){
            for(UINT x = 0; x < anchoCuadro && izquierda + x < imagen.ancho; x++){
                const uint32_t pixel = temporal[(size_t)y * anchoCuadro + x];

                if(pixel >> 24){
                    lienzo[(size_t)(arriba + y) * imagen.ancho + izquierda + x] = pixel;
                }
            }
        }

        const UINT bytes = imagen.ancho * imagen.alto * 4;
        uint32_t* copia = (uint32_t*)HeapAlloc(GetProcessHeap(), 0, bytes);

        if(copia){
            memcpy(copia, lienzo, bytes);
            imagen.pixeles[imagen.cuadros] = copia;
            imagen.esperas[imagen.cuadros] = espera < 2
                ? 100
                : espera * 10;
            imagen.duracion += imagen.esperas[imagen.cuadros];
            imagen.cuadros++;
        }
    }

    if(temporal){
        HeapFree(GetProcessHeap(), 0, temporal);
    }

    if(conversor){
        conversor->Release();
    }
}

static void CargarEmoji(int emoji)
{
    ImagenEmoji& imagen = imagenes[emoji];

    if(imagen.intentada){
        return;
    }

    imagen.intentada = true;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IWICImagingFactory* fabrica = nullptr;
    const HRESULT fabricaCreada = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&fabrica);

    if(FAILED(fabricaCreada) || !fabrica){
        return;
    }

    wchar_t ruta[MAX_PATH] = {};
    RutaEmoji(emoji, ruta);
    IWICBitmapDecoder* decodificador = nullptr;
    const HRESULT abierto = fabrica->CreateDecoderFromFilename(ruta, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decodificador);

    if(FAILED(abierto) || !decodificador){
        fabrica->Release();
        return;
    }

    UINT total = 1;
    decodificador->GetFrameCount(&total);
    total = total > (UINT)kMaxCuadros
        ? (UINT)kMaxCuadros
        : total;
    uint32_t* lienzo = nullptr;

    for(UINT n = 0; n < total; n++){
        IWICBitmapFrameDecode* cuadro = nullptr;

        if(FAILED(decodificador->GetFrame(n, &cuadro)) || !cuadro){
            break;
        }

        UINT anchoCuadro = 0;
        UINT altoCuadro = 0;
        cuadro->GetSize(&anchoCuadro, &altoCuadro);
        UINT espera = 10;
        UINT izquierda = 0;
        UINT arriba = 0;
        IWICMetadataQueryReader* lector = nullptr;

        if(SUCCEEDED(cuadro->GetMetadataQueryReader(&lector)) && lector){
            espera = LeerMetadato(lector, L"/grctlext/Delay", 10);
            izquierda = LeerMetadato(lector, L"/imgdesc/Left", 0);
            arriba = LeerMetadato(lector, L"/imgdesc/Top", 0);
            lector->Release();
        }

        if(n == 0){
            imagen.ancho = anchoCuadro;
            imagen.alto = altoCuadro;
            lienzo = (uint32_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, anchoCuadro * altoCuadro * 4);
        }

        if(!lienzo){
            cuadro->Release();
            break;
        }

        ComponerCuadro(fabrica, cuadro, imagen, lienzo, anchoCuadro, altoCuadro, izquierda, arriba, espera);
        cuadro->Release();
    }

    if(lienzo){
        HeapFree(GetProcessHeap(), 0, lienzo);
    }

    decodificador->Release();
    fabrica->Release();
}

static void SoltarTexturas()
{
    for(ImagenEmoji& imagen : imagenes){
        for(int c = 0; c < kMaxCuadros; c++){
            if(imagen.texturas[c]){
                imagen.texturas[c]->Release();
                imagen.texturas[c] = nullptr;
            }
        }
    }
}

static int CuadroActual(const ImagenEmoji& imagen)
{
    if(imagen.cuadros <= 1 || !imagen.duracion){
        return 0;
    }

    UINT momento = GetTickCount() % imagen.duracion;

    for(int c = 0; c < imagen.cuadros; c++){
        if(momento < imagen.esperas[c]){
            return c;
        }

        momento -= imagen.esperas[c];
    }

    return 0;
}

static IDirect3DTexture9* TexturaEmoji(IDirect3DDevice9* dispositivo, int emoji)
{
    CargarEmoji(emoji);
    ImagenEmoji& imagen = imagenes[emoji];

    if(!imagen.cuadros){
        return nullptr;
    }

    if(dispositivo != dispositivoTexturas){
        SoltarTexturas();
        dispositivoTexturas = dispositivo;
    }

    const int actual = CuadroActual(imagen);

    if(imagen.texturas[actual]){
        return imagen.texturas[actual];
    }

    if(!imagen.pixeles[actual]){
        return nullptr;
    }

    IDirect3DTexture9* textura = nullptr;
    const HRESULT creada = dispositivo->CreateTexture(imagen.ancho, imagen.alto, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &textura, nullptr);

    if(FAILED(creada) || !textura){
        return nullptr;
    }

    D3DLOCKED_RECT bloqueo = {};

    if(FAILED(textura->LockRect(0, &bloqueo, nullptr, 0))){
        textura->Release();
        return nullptr;
    }

    for(UINT y = 0; y < imagen.alto; y++){
        memcpy((uint8_t*)bloqueo.pBits + (size_t)y * bloqueo.Pitch, imagen.pixeles[actual] + (size_t)y * imagen.ancho, imagen.ancho * 4);
    }

    textura->UnlockRect(0);
    imagen.texturas[actual] = textura;
    return textura;
}

int EmojiEn(const std::wstring& texto, size_t posicion)
{
    if(posicion + 1 >= texto.size()){
        return -1;
    }

    for(int i = 0; i < kTotalEmojis; i++){
        const bool coincide = texto[posicion] == kEmojis[i].primera && MismaLetra(kEmojis[i].segunda, texto[posicion + 1]);

        if(coincide){
            return i;
        }
    }

    return -1;
}

bool EmojiDisponible(int emoji)
{
    if(!EmojiValido(emoji)){
        return false;
    }

    CargarEmoji(emoji);
    return imagenes[emoji].cuadros > 0;
}

bool EmojiAnimado(int emoji)
{
    return EmojiDisponible(emoji) && imagenes[emoji].cuadros > 1;
}

float ProporcionEmoji(int emoji)
{
    if(!EmojiDisponible(emoji) || !imagenes[emoji].alto){
        return 1.f;
    }

    return (float)imagenes[emoji].ancho / (float)imagenes[emoji].alto;
}

void DibujarEmoji(IDirect3DDevice9* dispositivo, int emoji, float x, float y, float alto)
{
    if(!dispositivo || !EmojiValido(emoji)){
        return;
    }

    IDirect3DTexture9* textura = TexturaEmoji(dispositivo, emoji);

    if(!textura){
        return;
    }

    const float ancho = alto * ProporcionEmoji(emoji);
    DWORD formatoAntes = 0;
    DWORD profundidadAntes = 0;
    DWORD mezclaAntes = 0;
    DWORD origenAntes = 0;
    DWORD destinoAntes = 0;
    DWORD luzAntes = 0;
    DWORD caraAntes = 0;
    DWORD colorAntes = 0;
    DWORD colorArgAntes = 0;
    DWORD alfaAntes = 0;
    DWORD alfaArgAntes = 0;
    IDirect3DBaseTexture9* texturaAntes = nullptr;

    dispositivo->GetFVF(&formatoAntes);
    dispositivo->GetTexture(0, &texturaAntes);
    dispositivo->GetRenderState(D3DRS_ZENABLE, &profundidadAntes);
    dispositivo->GetRenderState(D3DRS_ALPHABLENDENABLE, &mezclaAntes);
    dispositivo->GetRenderState(D3DRS_SRCBLEND, &origenAntes);
    dispositivo->GetRenderState(D3DRS_DESTBLEND, &destinoAntes);
    dispositivo->GetRenderState(D3DRS_LIGHTING, &luzAntes);
    dispositivo->GetRenderState(D3DRS_CULLMODE, &caraAntes);
    dispositivo->GetTextureStageState(0, D3DTSS_COLOROP, &colorAntes);
    dispositivo->GetTextureStageState(0, D3DTSS_COLORARG1, &colorArgAntes);
    dispositivo->GetTextureStageState(0, D3DTSS_ALPHAOP, &alfaAntes);
    dispositivo->GetTextureStageState(0, D3DTSS_ALPHAARG1, &alfaArgAntes);

    dispositivo->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dispositivo->SetRenderState(D3DRS_ZENABLE, FALSE);
    dispositivo->SetRenderState(D3DRS_LIGHTING, FALSE);
    dispositivo->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dispositivo->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dispositivo->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    dispositivo->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dispositivo->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dispositivo->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dispositivo->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dispositivo->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);

    const VerticeEmoji vertices[4] = {
        { x, y, 0.f, 1.f, 0xFFFFFFFF, 0.f, 0.f },
        { x + ancho, y, 0.f, 1.f, 0xFFFFFFFF, 1.f, 0.f },
        { x, y + alto, 0.f, 1.f, 0xFFFFFFFF, 0.f, 1.f },
        { x + ancho, y + alto, 0.f, 1.f, 0xFFFFFFFF, 1.f, 1.f },
    };
    dispositivo->SetTexture(0, textura);
    dispositivo->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(VerticeEmoji));

    dispositivo->SetTexture(0, texturaAntes);

    if(texturaAntes){
        texturaAntes->Release();
    }

    dispositivo->SetTextureStageState(0, D3DTSS_COLOROP, colorAntes);
    dispositivo->SetTextureStageState(0, D3DTSS_COLORARG1, colorArgAntes);
    dispositivo->SetTextureStageState(0, D3DTSS_ALPHAOP, alfaAntes);
    dispositivo->SetTextureStageState(0, D3DTSS_ALPHAARG1, alfaArgAntes);
    dispositivo->SetRenderState(D3DRS_ZENABLE, profundidadAntes);
    dispositivo->SetRenderState(D3DRS_ALPHABLENDENABLE, mezclaAntes);
    dispositivo->SetRenderState(D3DRS_SRCBLEND, origenAntes);
    dispositivo->SetRenderState(D3DRS_DESTBLEND, destinoAntes);
    dispositivo->SetRenderState(D3DRS_LIGHTING, luzAntes);
    dispositivo->SetRenderState(D3DRS_CULLMODE, caraAntes);
    dispositivo->SetFVF(formatoAntes);
}
