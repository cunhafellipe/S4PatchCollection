#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <mmsystem.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <d3d9.h>
#include <detours.h>

#pragma comment(lib, "detours.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "d3d9.lib")
#pragma intrinsic(_ReturnAddress)

typedef LONG(NTAPI* NtDelayExecutionFn)(BOOLEAN, PLARGE_INTEGER);
typedef LONG(NTAPI* NtQueryTimerResolutionFn)(PULONG, PULONG, PULONG);
typedef LONG(NTAPI* NtSetTimerResolutionFn)(ULONG, BOOLEAN, PULONG);

typedef int(__thiscall* TickFn)(void*);
typedef int(__thiscall* FrameEstadosFn)(void*);
typedef int(__thiscall* ActualizarTiempoFn)(double*);
typedef unsigned int(__cdecl* RelojJuegoFn)();
typedef void(__thiscall* FisicaActorFn)(void*, unsigned int);
typedef int(__thiscall* MoverActorFn)(void*, float, float, float);
typedef int(__thiscall* SpreadArmaFn)(void*, unsigned int, char);
typedef int(__thiscall* CamaraFn)(void*, unsigned int, unsigned int);
typedef unsigned int*(__thiscall* SumarProtegidoFn)(unsigned int*, float);
typedef void*(__thiscall* VueloActualizarFn)(void*, unsigned int);
typedef int(__thiscall* LeerParametroFn)(void*, int, float*);

static const uintptr_t kTickPartida = 0x0131F360;
static const uintptr_t kFrameEstados = 0x012928B0;
static const uintptr_t kLlamadaFrameEstados = 0x00F1DE00;
static const uintptr_t kRender = 0x025BD0B8;
static const uintptr_t kRetrasoActores = 0x0256B85C;
static const uintptr_t kSaltoLimitePartida = 0x0131F380;
static const uintptr_t kSaltoLimiteLobby = 0x01321960;
static const uintptr_t kLlamadaTiempoPartida = 0x0131F3A5;
static const uintptr_t kActualizarTiempo = 0x01B36E80;
static const uintptr_t kRelojJuego = 0x01B36D90;
static const uintptr_t kFisicaActor = 0x00FAE600;
static const uintptr_t kMoverActor = 0x00FA3960;
static const uintptr_t kRetornoMovimientoAire = 0x00FAED67;
static const uintptr_t kLlamadaGravedad = 0x00FAEC26;
static const uintptr_t kSumarProtegido = 0x00FB6810;
static const uintptr_t kVueloActualizar = 0x0165CA90;
static const uintptr_t kLlamadaFrenadoVuelo = 0x0165D095;
static const uintptr_t kLeerParametro = 0x00FB6B40;
static const uintptr_t kSpreadArma = 0x0101A210;
static const uintptr_t kCamara = 0x01202AD0;
static const uintptr_t kPielColision = 0x0256B1B8;
static const uintptr_t kSitioEpsilonA = 0x01252024;
static const uintptr_t kSitioEpsilonB = 0x0125202C;
static const uintptr_t kSitioPielLibre = 0x01253183;
static const uintptr_t kJuego = 0x025BD770;

static const double kMsFrameOriginal = 1000.0 / 60.0;
static const float kSegundosFrameOriginal = 1.f / 60.f;
static const float kPielOriginal = 0.015f;
static const float kMovimientoMaximo = 180.f;
static const float kFactorVentanaRapida = 20.f;
static const float kVelocidadGolpe = -49999.f;

int max_framerate = 300;
int field_of_view = 90;
int center_field_of_view = 100;
int sprint_field_of_view = 110;
static volatile int fisicaActiva = 1;
static volatile int registroActivo = 0;
static volatile int bajaLatencia = 1;
static volatile int mostrarMs = 0;
static volatile int retrasoActores = 70;
static bool retrasoValido = false;

static char rutaConfig[MAX_PATH] = {};
static char rutaLog[MAX_PATH] = {};
static char rutaTraza[MAX_PATH] = {};

static NtDelayExecutionFn ntDelayExecution = nullptr;
static LARGE_INTEGER frecuenciaQpc = {};
static LARGE_INTEGER inicioQpc = {};
static double siguienteFrameMs = -1.0;

static float epsilonColision = kPielOriginal;
static float pielLibre = kPielOriginal;
static float escalaFrame = 1.f;

static TickFn tickPartida = (TickFn)kTickPartida;
static FrameEstadosFn frameEstados = (FrameEstadosFn)kFrameEstados;
static ActualizarTiempoFn actualizarTiempo = (ActualizarTiempoFn)kActualizarTiempo;
static RelojJuegoFn relojJuego = (RelojJuegoFn)kRelojJuego;
static FisicaActorFn fisicaActor = (FisicaActorFn)kFisicaActor;
static MoverActorFn moverActor = (MoverActorFn)kMoverActor;
static SpreadArmaFn spreadArma = (SpreadArmaFn)kSpreadArma;
static CamaraFn camara = (CamaraFn)kCamara;
static SumarProtegidoFn sumarProtegido = (SumarProtegidoFn)kSumarProtegido;
static VueloActualizarFn vueloActualizar = (VueloActualizarFn)kVueloActualizar;
static LeerParametroFn leerParametro = (LeerParametroFn)kLeerParametro;
static unsigned int pasoVueloMs = 16;

struct RegistroActor {
    void* actor;
    float vyFinal;
    bool valido;
    DWORD usado;
};

struct FisicaEnCurso {
    void* actor;
    float vyEntrada;
    float pasoSegundos;
    float factorMovimiento;
    bool velocidadPuesta;
    float incrementoGravedad;
    bool hayGravedad;
    bool recortada;
    float correccion;
    float movimiento[3];
    bool seMovio;
};

struct NombreClase {
    uintptr_t tabla;
    char nombre[48];
};

static RegistroActor registros[64] = {};
static DWORD contadorUso = 0;
static FisicaEnCurso fisicaActual = {};

static NombreClase nombresClase[64] = {};
static int nombresGuardados = 0;
static char bufferTraza[1 << 18] = {};
static size_t usoTraza = 0;
static DWORD ultimoVolcado = 0;
static int trazaAbierta = 0;
static int fpsTrazado = -1;
static int fisicaTrazada = -1;
static int otrosMovimientos = 0;
static float largoOtros = 0.f;

static double inicioUltimoFrame = 0.0;
static double ultimoReporte = 0.0;
static double sumaFrame = 0.0;
static double sumaEsperaGpu = 0.0;
static double sumaEsperaLimite = 0.0;
static double sumaLatencia = 0.0;
static int framesMedidos = 0;
static int latenciasMedidas = 0;
static bool consolaAbierta = false;

static float Limitar(float valor, float minimo, float maximo)
{
    return valor < minimo
        ? minimo
        : valor > maximo
            ? maximo
            : valor;
}

static float LeerProtegido(const uint8_t* campo)
{
    const uint32_t palabra = *(const uint32_t*)campo;
    const uint32_t llave = *(const uint32_t*)(campo + 8);
    const uint32_t bits = palabra ^ llave;
    float valor = 0.f;
    memcpy(&valor, &bits, 4);
    return valor;
}

static void EscribirProtegido(uint8_t* campo, float valor)
{
    uint32_t* palabras = (uint32_t*)campo;
    uint32_t bits = 0;
    memcpy(&bits, &valor, 4);
    palabras[0] = bits ^ palabras[2];
    palabras[1] = ~palabras[0];
}

static void Registrar(const char* linea)
{
    if(!registroActivo){
        return;
    }

    FILE* archivo = fopen(rutaLog, "a");

    if(!archivo){
        return;
    }

    fprintf(archivo, "%s\n", linea);
    fclose(archivo);
}

static bool Escribir(uintptr_t direccion, const void* datos, size_t cantidad)
{
    DWORD proteccion = 0;

    if(!VirtualProtect((void*)direccion, cantidad, PAGE_EXECUTE_READWRITE, &proteccion)){
        return false;
    }

    memcpy((void*)direccion, datos, cantidad);
    VirtualProtect((void*)direccion, cantidad, proteccion, &proteccion);
    FlushInstructionCache(GetCurrentProcess(), (void*)direccion, cantidad);
    return true;
}

static bool BytesSon(uintptr_t direccion, const uint8_t* esperados, size_t cantidad)
{
    return memcmp((const void*)direccion, esperados, cantidad) == 0;
}

static bool LlamaA(uintptr_t llamada, uintptr_t destino)
{
    const uint8_t* bytes = (const uint8_t*)llamada;
    const uintptr_t destinoReal = llamada + 5 + *(const int32_t*)(bytes + 1);
    return bytes[0] == 0xE8 && destinoReal == destino;
}

static int LeerEntero(const char* texto, const char* clave, int porDefecto)
{
    char buscado[64] = {};
    snprintf(buscado, sizeof(buscado), "\"%s\"", clave);
    const char* encontrado = strstr(texto, buscado);

    if(!encontrado){
        return porDefecto;
    }

    const char* dosPuntos = strchr(encontrado + strlen(buscado), ':');

    if(!dosPuntos){
        return porDefecto;
    }

    return atoi(dosPuntos + 1);
}

static void LeerConfig()
{
    FILE* archivo = fopen(rutaConfig, "rb");

    if(!archivo){
        return;
    }

    char texto[2048] = {};
    fread(texto, 1, sizeof(texto) - 1, archivo);
    fclose(archivo);

    const int fps = LeerEntero(texto, "max_framerate", max_framerate);
    const int fov = LeerEntero(texto, "field_of_view", field_of_view);
    const int desfaseCorrer = LeerEntero(texto, "sprint_offset", 20);

    max_framerate = fps < 0
        ? 0
        : fps > 1000
            ? 1000
            : fps;
    field_of_view = fov;
    center_field_of_view = LeerEntero(texto, "center_field_of_view", fov + 10);
    sprint_field_of_view = LeerEntero(texto, "sprint_field_of_view", fov + desfaseCorrer);
    fisicaActiva = LeerEntero(texto, "physics_fix", 1) != 0;
    registroActivo = LeerEntero(texto, "trace", 0) != 0;
    bajaLatencia = LeerEntero(texto, "low_latency", 1) != 0;
    mostrarMs = LeerEntero(texto, "show_ms", 0) != 0;

    const int retraso = LeerEntero(texto, "actor_delay", 70);
    retrasoActores = retraso < 0
        ? 0
        : retraso > 200
            ? 200
            : retraso;
}

static void AplicarRetrasoActores()
{
    if(!retrasoValido){
        return;
    }

    *(volatile uint32_t*)kRetrasoActores = (uint32_t)retrasoActores;
}

static void PrepararRutas()
{
    char carpeta[MAX_PATH] = {};
    HMODULE modulo = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&PrepararRutas, &modulo);
    GetModuleFileNameA(modulo, carpeta, MAX_PATH);
    char* barra = strrchr(carpeta, '\\');

    if(barra){
        barra[1] = '\0';
    }

    snprintf(rutaConfig, sizeof(rutaConfig), "%sconf.json", carpeta);
    snprintf(rutaLog, sizeof(rutaLog), "%sfps_unlock.log", carpeta);
    snprintf(rutaTraza, sizeof(rutaTraza), "%sfps_trace.log", carpeta);
}

static void SubirResolucionTimer()
{
    timeBeginPeriod(1);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");

    if(!ntdll){
        return;
    }

    ntDelayExecution = (NtDelayExecutionFn)GetProcAddress(ntdll, "NtDelayExecution");
    const NtQueryTimerResolutionFn consultar = (NtQueryTimerResolutionFn)GetProcAddress(ntdll, "NtQueryTimerResolution");
    const NtSetTimerResolutionFn poner = (NtSetTimerResolutionFn)GetProcAddress(ntdll, "NtSetTimerResolution");

    if(!consultar || !poner){
        return;
    }

    ULONG minima = 0;
    ULONG maxima = 0;
    ULONG actual = 0;
    consultar(&minima, &maxima, &actual);
    poner(maxima, TRUE, &actual);
}

static void Dormir(double ms)
{
    if(!ntDelayExecution || ms <= 0.0){
        return;
    }

    LARGE_INTEGER espera = {};
    espera.QuadPart = -(LONGLONG)(ms * 10000.0);
    ntDelayExecution(FALSE, &espera);
}

static void EsperarFrame()
{
    const int limite = max_framerate;

    if(limite <= 0){
        siguienteFrameMs = -1.0;
        return;
    }

    const double duracionMs = 1000.0 / limite;
    double ahoraMs = (double)relojJuego();
    const bool sinReferencia = siguienteFrameMs < 0.0
        || ahoraMs - siguienteFrameMs > 100.0
        || siguienteFrameMs - ahoraMs > 1000.0;

    if(sinReferencia){
        siguienteFrameMs = ahoraMs + duracionMs;
        return;
    }

    while(ahoraMs < siguienteFrameMs){
        const double faltaMs = siguienteFrameMs - ahoraMs;

        if(faltaMs > 2.0){
            Dormir(faltaMs - 1.5);
        } else {
            YieldProcessor();
        }

        ahoraMs = (double)relojJuego();
    }

    const bool atrasado = ahoraMs - siguienteFrameMs > duracionMs;
    siguienteFrameMs = atrasado
        ? ahoraMs + duracionMs
        : siguienteFrameMs + duracionMs;
}

static double MsDesdeInicio()
{
    LARGE_INTEGER ahora = {};
    QueryPerformanceCounter(&ahora);
    return (double)(ahora.QuadPart - inicioQpc.QuadPart) * 1000.0 / (double)frecuenciaQpc.QuadPart;
}

static const uint8_t* ActorLocal()
{
    const uint8_t* juego = *(const uint8_t* const*)kJuego;

    if(!juego){
        return nullptr;
    }

    return *(const uint8_t* const*)(juego + 0x124);
}

static const char* NombreRtti(const void* objeto)
{
    __try {
        const uintptr_t tabla = *(const uintptr_t*)objeto;
        const uintptr_t localizador = *(const uintptr_t*)(tabla - 4);
        const uintptr_t descriptor = *(const uintptr_t*)(localizador + 12);
        return (const char*)(descriptor + 8);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static const char* NombreEstado(const uint8_t* actor)
{
    const void* estado = *(const void* const*)(actor + 0x1C);

    if(!estado){
        return "-";
    }

    const uintptr_t tabla = *(const uintptr_t*)estado;

    for(int i = 0; i < nombresGuardados; i++){
        if(nombresClase[i].tabla == tabla){
            return nombresClase[i].nombre;
        }
    }

    const char* crudo = NombreRtti(estado);

    if(!crudo || nombresGuardados >= 64){
        return "?";
    }

    NombreClase& nuevo = nombresClase[nombresGuardados++];
    nuevo.tabla = tabla;
    const char* limpio = strncmp(crudo, ".?AV", 4) == 0
        ? crudo + 4
        : crudo;
    size_t largo = 0;

    while(limpio[largo] && limpio[largo] != '@' && largo < sizeof(nuevo.nombre) - 1){
        nuevo.nombre[largo] = limpio[largo];
        largo++;
    }

    nuevo.nombre[largo] = '\0';
    return nuevo.nombre;
}

static void VolcarTraza()
{
    if(usoTraza == 0){
        return;
    }

    FILE* archivo = fopen(rutaTraza, "ab");

    if(archivo){
        fwrite(bufferTraza, 1, usoTraza, archivo);
        fclose(archivo);
    }

    usoTraza = 0;
    ultimoVolcado = GetTickCount();
}

static void AnotarTraza(const char* formato, ...)
{
    if(usoTraza > sizeof(bufferTraza) - 512){
        VolcarTraza();
    }

    va_list argumentos;
    va_start(argumentos, formato);
    const int escrito = vsnprintf(bufferTraza + usoTraza, sizeof(bufferTraza) - usoTraza, formato, argumentos);
    va_end(argumentos);

    if(escrito > 0){
        usoTraza += (size_t)escrito;
    }

    if(GetTickCount() - ultimoVolcado > 500){
        VolcarTraza();
    }
}

static void RevisarTraza()
{
    if(!registroActivo){
        if(trazaAbierta){
            VolcarTraza();
            trazaAbierta = 0;
        }

        return;
    }

    if(!trazaAbierta){
        FILE* archivo = fopen(rutaTraza, "wb");

        if(archivo){
            fclose(archivo);
        }

        trazaAbierta = 1;
        fpsTrazado = -1;
        fisicaTrazada = -1;
    }

    if(fpsTrazado != max_framerate || fisicaTrazada != fisicaActiva){
        fpsTrazado = max_framerate;
        fisicaTrazada = fisicaActiva;
        AnotarTraza("# fps %d physics_fix %d\n", fpsTrazado, fisicaTrazada);
    }
}

static int __fastcall TickPartida(void* partida, void*)
{
    RevisarTraza();
    return tickPartida(partida);
}

static IDirect3DDevice9* DispositivoJuego()
{
    const uint8_t* render = *(const uint8_t* const*)kRender;

    if(!render){
        return nullptr;
    }

    return *(IDirect3DDevice9* const*)(render + 0x69C);
}

static bool EsperarGpu()
{
    IDirect3DDevice9* dispositivo = DispositivoJuego();

    if(!dispositivo){
        return false;
    }

    IDirect3DQuery9* consulta = nullptr;

    if(FAILED(dispositivo->CreateQuery(D3DQUERYTYPE_EVENT, &consulta)) || !consulta){
        return false;
    }

    consulta->Issue(D3DISSUE_END);
    HRESULT estado = consulta->GetData(nullptr, 0, D3DGETDATA_FLUSH);

    while(estado == S_FALSE){
        YieldProcessor();
        estado = consulta->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    }

    consulta->Release();
    return estado == S_OK;
}

static void AbrirConsola()
{
    if(consolaAbierta){
        return;
    }

    consolaAbierta = true;

    if(!GetConsoleWindow()){
        AllocConsole();
    }

    FILE* salida = nullptr;
    freopen_s(&salida, "CONOUT$", "w", stdout);
}

static void AnotarTiempos(double antesGpu, double despuesGpu, bool esperoGpu, double inicioFrame)
{
    const double anterior = inicioUltimoFrame;
    inicioUltimoFrame = inicioFrame;

    if(!mostrarMs || anterior <= 0.0){
        return;
    }

    sumaFrame += inicioFrame - anterior;
    sumaEsperaGpu += despuesGpu - antesGpu;
    sumaEsperaLimite += inicioFrame - despuesGpu;
    framesMedidos++;

    if(esperoGpu){
        sumaLatencia += despuesGpu - anterior;
        latenciasMedidas++;
    }

    if(inicioFrame - ultimoReporte < 1000.0){
        return;
    }

    AbrirConsola();
    const double frameMedio = sumaFrame / framesMedidos;
    const double esperaGpu = sumaEsperaGpu / framesMedidos;
    const double esperaLimite = sumaEsperaLimite / framesMedidos;

    if(latenciasMedidas){
        printf("[fps] %.0f fps | frame %.2f ms | input->gpu %.2f ms | gpu wait %.2f ms | limiter wait %.2f ms\n",
            1000.0 / frameMedio, frameMedio, sumaLatencia / latenciasMedidas, esperaGpu, esperaLimite);
    } else {
        printf("[fps] %.0f fps | frame %.2f ms | limiter wait %.2f ms | low_latency off\n",
            1000.0 / frameMedio, frameMedio, esperaLimite);
    }

    ultimoReporte = inicioFrame;
    sumaFrame = 0.0;
    sumaEsperaGpu = 0.0;
    sumaEsperaLimite = 0.0;
    sumaLatencia = 0.0;
    framesMedidos = 0;
    latenciasMedidas = 0;
}

static int __fastcall FrameEstados(void* estados, void*)
{
    const double antesGpu = MsDesdeInicio();
    const bool esperoGpu = bajaLatencia && EsperarGpu();
    const double despuesGpu = MsDesdeInicio();

    EsperarFrame();
    AnotarTiempos(antesGpu, despuesGpu, esperoGpu, MsDesdeInicio());
    return frameEstados(estados);
}

static int __fastcall TiempoPartida(double* reloj, void*)
{
    const int resultado = actualizarTiempo(reloj);
    const double pasoMs = floor(reloj[2]);

    if(!fisicaActiva){
        escalaFrame = 1.f;
        epsilonColision = kPielOriginal;
        pielLibre = kPielOriginal;
        return resultado;
    }

    if(pasoMs <= 0.0 || pasoMs > 250.0){
        return resultado;
    }

    escalaFrame = (float)(pasoMs / kMsFrameOriginal);
    epsilonColision = kPielOriginal * Limitar(escalaFrame, 0.05f, 1.f);
    pielLibre = kPielOriginal * Limitar(escalaFrame, 0.05f, 4.f);
    return resultado;
}

static RegistroActor* RegistroDe(void* actor)
{
    RegistroActor* masViejo = &registros[0];

    for(RegistroActor& registro : registros){
        if(registro.actor == actor){
            registro.usado = ++contadorUso;
            return &registro;
        }

        if(registro.usado < masViejo->usado){
            masViejo = &registro;
        }
    }

    masViejo->actor = actor;
    masViejo->valido = false;
    masViejo->usado = ++contadorUso;
    return masViejo;
}

static void AnotarFisica(const uint8_t* actor, unsigned int pasoMs, float vySalida)
{
    const float* posicion = (const float*)(actor + 0x2FC);
    const int enAire = *(const int*)(actor + 0x114);

    AnotarTraza("%10.1f dt=%u air=%d %s vy0=%.2f g=%.3f vy1=%.2f cl=%d rs=%d k=%.0f c=%.4f mv=%.3f,%.3f,%.3f p=%.1f,%.1f,%.1f o=%d:%.3f\n",
        MsDesdeInicio(),
        pasoMs,
        enAire,
        NombreEstado(actor),
        fisicaActual.vyEntrada,
        fisicaActual.incrementoGravedad,
        vySalida,
        fisicaActual.recortada ? 1 : 0,
        fisicaActual.velocidadPuesta ? 1 : 0,
        fisicaActual.factorMovimiento,
        fisicaActual.correccion,
        fisicaActual.movimiento[0],
        fisicaActual.movimiento[1],
        fisicaActual.movimiento[2],
        posicion[0],
        posicion[1],
        posicion[2],
        otrosMovimientos,
        largoOtros);

    otrosMovimientos = 0;
    largoOtros = 0.f;
}

static void __fastcall FisicaActor(uint8_t* actor, void*, unsigned int pasoMs)
{
    const RegistroActor* registro = RegistroDe(actor);
    const float vyEntrada = LeerProtegido(actor + 0xFC);
    const bool velocidadPuesta = registro->valido && registro->vyFinal != vyEntrada;
    const uint32_t limiteVentana = *(const uint32_t*)(actor + 0x238);
    const uint32_t tiempoVentana = *(const uint32_t*)(actor + 0x23C);
    const float factorMovimiento = limiteVentana >= tiempoVentana
        ? kFactorVentanaRapida
        : 1.f;

    const FisicaEnCurso anterior = fisicaActual;
    fisicaActual = FisicaEnCurso{};
    fisicaActual.actor = actor;
    fisicaActual.vyEntrada = vyEntrada;
    fisicaActual.pasoSegundos = Limitar(pasoMs * 0.001f, 0.f, 0.1f);
    fisicaActual.factorMovimiento = factorMovimiento;
    fisicaActual.velocidadPuesta = velocidadPuesta;

    fisicaActor(actor, pasoMs);

    const float vySalida = LeerProtegido(actor + 0xFC);
    RegistroActor* registroFinal = RegistroDe(actor);
    registroFinal->vyFinal = vySalida;
    registroFinal->valido = true;

    if(trazaAbierta && actor == ActorLocal()){
        AnotarFisica(actor, pasoMs, vySalida);
    }

    fisicaActual = anterior;
}

static unsigned int* __fastcall SumarGravedad(unsigned int* vy, void*, float incremento)
{
    fisicaActual.incrementoGravedad = incremento;
    fisicaActual.hayGravedad = true;
    return sumarProtegido(vy, incremento);
}

static float CorreccionGravedad(const uint8_t* actor)
{
    if(!fisicaActual.hayGravedad){
        return 0.f;
    }

    const float vyEsperada = fisicaActual.vyEntrada + fisicaActual.incrementoGravedad;
    const float vyNueva = LeerProtegido(actor + 0xFC);
    const float tolerancia = 0.001f + 0.01f * fabsf(fisicaActual.incrementoGravedad);
    fisicaActual.recortada = fabsf(vyNueva - vyEsperada) > tolerancia;

    if(fisicaActual.recortada){
        return 0.f;
    }

    const float diferenciaPaso = kSegundosFrameOriginal - fisicaActual.pasoSegundos;
    const float peso = fisicaActual.velocidadPuesta
        ? 1.f
        : 0.5f;

    return fisicaActual.factorMovimiento * fisicaActual.incrementoGravedad * peso * diferenciaPaso;
}

static void AnotarOtroMovimiento(const uint8_t* actor, float dx, float dy, float dz)
{
    if(!trazaAbierta || actor != ActorLocal()){
        return;
    }

    otrosMovimientos += 1;
    largoOtros += sqrtf(dx * dx + dy * dy + dz * dz);
}

static int MoverAire(uint8_t* actor, float dx, float dy, float dz, float correccion)
{
    fisicaActual.correccion = correccion;
    fisicaActual.movimiento[0] = dx;
    fisicaActual.movimiento[1] = dy;
    fisicaActual.movimiento[2] = dz;
    fisicaActual.seMovio = true;
    return moverActor(actor, dx, dy, dz);
}

static int __fastcall MoverActor(uint8_t* actor, void*, float dx, float dy, float dz)
{
    const bool esMovimientoAire = _ReturnAddress() == (void*)kRetornoMovimientoAire && fisicaActual.actor == actor;

    if(!esMovimientoAire){
        AnotarOtroMovimiento(actor, dx, dy, dz);
        return moverActor(actor, dx, dy, dz);
    }

    if(!fisicaActiva){
        return MoverAire(actor, dx, dy, dz, 0.f);
    }

    const bool golpeDeUnFrame = fisicaActual.velocidadPuesta && fisicaActual.vyEntrada <= kVelocidadGolpe;

    if(golpeDeUnFrame){
        const float vyGolpe = LeerProtegido(actor + 0xFC);
        const float caidaOriginal = vyGolpe * kSegundosFrameOriginal * fisicaActual.factorMovimiento;
        return MoverAire(actor, dx, caidaOriginal, dz, caidaOriginal - dy);
    }

    const float correccion = CorreccionGravedad(actor);
    dy += correccion;

    const bool soloCaida = dy < 0.f && dx == 0.f && dz == 0.f;
    const float escalaActor = fisicaActual.pasoSegundos / kSegundosFrameOriginal;
    const float tope = kMovimientoMaximo * Limitar(escalaActor, 0.05f, 1.f);
    const float largo = sqrtf(dx * dx + dy * dy + dz * dz);

    if(soloCaida || largo <= tope){
        return MoverAire(actor, dx, dy, dz, correccion);
    }

    const float recorte = tope / largo;
    return MoverAire(actor, dx * recorte, dy * recorte, dz * recorte, correccion);
}

static void* __fastcall VueloActualizar(void* vuelo, void*, unsigned int pasoMs)
{
    pasoVueloMs = pasoMs;
    return vueloActualizar(vuelo, pasoMs);
}

static int __fastcall LeerFrenadoVuelo(void* parametros, void*, int clave, float* salida)
{
    const int resultado = leerParametro(parametros, clave, salida);
    const float frenado = *salida;

    if(!fisicaActiva || frenado <= 0.f || frenado >= 1.f){
        return resultado;
    }

    const float pasoMs = (float)pasoVueloMs;
    const float msOriginal = (float)kMsFrameOriginal;
    *salida = frenado * pasoMs / (frenado * pasoMs + msOriginal * (1.f - frenado));
    return resultado;
}

static bool DisparoContinuo()
{
    const uint8_t* juego = *(const uint8_t* const*)kJuego;

    if(!juego){
        return false;
    }

    const uint8_t* modo = *(const uint8_t* const*)(juego + 0x250);

    if(!modo){
        return false;
    }

    return modo[157] && modo[156] && *(const int*)(modo + 160) == 3;
}

static int __fastcall SpreadArma(uint8_t* arma, void*, unsigned int pasoMs, char sinDisparo)
{
    if(!fisicaActiva || pasoMs == 0 || sinDisparo || DisparoContinuo()){
        return spreadArma(arma, pasoMs, sinDisparo);
    }

    uint8_t aumentoInterior[12] = {};
    uint8_t aumentoExterior[12] = {};
    memcpy(aumentoInterior, arma + 0x17C, 12);
    memcpy(aumentoExterior, arma + 0x1B8, 12);

    const float escala = (float)(kMsFrameOriginal / pasoMs);
    EscribirProtegido(arma + 0x17C, LeerProtegido(arma + 0x17C) * escala);
    EscribirProtegido(arma + 0x1B8, LeerProtegido(arma + 0x1B8) * escala);

    const int resultado = spreadArma(arma, pasoMs, sinDisparo);

    memcpy(arma + 0x17C, aumentoInterior, 12);
    memcpy(arma + 0x1B8, aumentoExterior, 12);
    return resultado;
}

static float FovPara(float fovJuego)
{
    const float normal = (float)field_of_view;
    const float centro = (float)center_field_of_view;
    const float correr = (float)sprint_field_of_view;

    return fovJuego == 60.f && normal > 0.f
        ? normal
        : fovJuego == 66.f && centro > 0.f
            ? centro
            : fovJuego == 80.f && correr > 0.f
                ? correr
                : fovJuego;
}

static int __fastcall Camara(uint8_t* camaraJuego, void*, unsigned int a, unsigned int b)
{
    float* fovObjetivo = (float*)(camaraJuego + 0x158);
    const float fovOriginal = *fovObjetivo;
    const float fovPuesto = FovPara(fovOriginal);
    *fovObjetivo = fovPuesto;

    const int resultado = camara(camaraJuego, a, b);

    if(*fovObjetivo == fovPuesto){
        *fovObjetivo = fovOriginal;
    }

    return resultado;
}

static bool BuildCorrecto()
{
    const uint8_t prologoTickPartida[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x6C };
    const uint8_t prologoConSeh[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68 };
    const uint8_t prologoSpread[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x30, 0x01, 0x00, 0x00 };
    const uint8_t prologoReloj[] = { 0x55, 0x8B, 0xEC, 0xE8 };
    const uint8_t saltoLimite[] = { 0x74, 0x1D };
    const uint8_t cargaEpsilon[] = { 0xF3, 0x0F, 0x10, 0x05 };
    const uint8_t multiplicaEpsilon[] = { 0xF3, 0x0F, 0x59, 0x05 };
    const uint8_t restaPiel[] = { 0xF3, 0x0F, 0x5C, 0x05 };
    const uint8_t llamadaMover[] = { 0x8B, 0x42, 0x70, 0xFF, 0xD0 };

    const bool prologosBien = BytesSon(kTickPartida, prologoTickPartida, sizeof(prologoTickPartida))
        && BytesSon(kFisicaActor, prologoConSeh, sizeof(prologoConSeh))
        && BytesSon(kMoverActor, prologoConSeh, sizeof(prologoConSeh))
        && BytesSon(kCamara, prologoConSeh, sizeof(prologoConSeh))
        && BytesSon(kSpreadArma, prologoSpread, sizeof(prologoSpread))
        && BytesSon(kRelojJuego, prologoReloj, sizeof(prologoReloj))
        && BytesSon(kVueloActualizar, prologoConSeh, sizeof(prologoConSeh));

    const bool limitesBien = BytesSon(kSaltoLimitePartida, saltoLimite, sizeof(saltoLimite))
        && BytesSon(kSaltoLimiteLobby, saltoLimite, sizeof(saltoLimite));

    const bool colisionBien = BytesSon(kSitioEpsilonA - 4, cargaEpsilon, sizeof(cargaEpsilon))
        && BytesSon(kSitioEpsilonB - 4, multiplicaEpsilon, sizeof(multiplicaEpsilon))
        && BytesSon(kSitioPielLibre - 4, restaPiel, sizeof(restaPiel))
        && *(const uint32_t*)kSitioEpsilonA == (uint32_t)kPielColision
        && *(const uint32_t*)kSitioEpsilonB == (uint32_t)kPielColision
        && *(const uint32_t*)kSitioPielLibre == (uint32_t)kPielColision;

    const bool llamadasBien = LlamaA(kLlamadaTiempoPartida, kActualizarTiempo)
        && LlamaA(kLlamadaFrameEstados, kFrameEstados)
        && LlamaA(kLlamadaGravedad, kSumarProtegido)
        && LlamaA(kLlamadaFrenadoVuelo, kLeerParametro)
        && BytesSon(kRetornoMovimientoAire - 5, llamadaMover, sizeof(llamadaMover));

    return prologosBien && limitesBien && colisionBien && llamadasBien;
}

static bool QuitarLimiteJuego()
{
    const uint8_t saltoSiempre = 0xEB;
    const bool partida = Escribir(kSaltoLimitePartida, &saltoSiempre, 1);
    const bool lobby = Escribir(kSaltoLimiteLobby, &saltoSiempre, 1);
    return partida && lobby;
}

static bool RedirigirLlamada(uintptr_t llamada, const void* destino)
{
    const int32_t nuevoRelativo = (int32_t)((uintptr_t)destino - (llamada + 5));
    return Escribir(llamada + 1, &nuevoRelativo, 4);
}

static bool RedirigirColision()
{
    const uint32_t epsilon = (uint32_t)(uintptr_t)&epsilonColision;
    const uint32_t piel = (uint32_t)(uintptr_t)&pielLibre;
    const bool sitioA = Escribir(kSitioEpsilonA, &epsilon, 4);
    const bool sitioB = Escribir(kSitioEpsilonB, &epsilon, 4);
    const bool sitioPiel = Escribir(kSitioPielLibre, &piel, 4);
    return sitioA && sitioB && sitioPiel;
}

static LONG InstalarHooks()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)tickPartida, (PVOID)TickPartida);
    DetourAttach(&(PVOID&)fisicaActor, (PVOID)FisicaActor);
    DetourAttach(&(PVOID&)moverActor, (PVOID)MoverActor);
    DetourAttach(&(PVOID&)spreadArma, (PVOID)SpreadArma);
    DetourAttach(&(PVOID&)camara, (PVOID)Camara);
    DetourAttach(&(PVOID&)vueloActualizar, (PVOID)VueloActualizar);
    return DetourTransactionCommit();
}

static DWORD WINAPI RecargarConfig(LPVOID)
{
    for(;;){
        Sleep(2000);
        LeerConfig();
        AplicarRetrasoActores();
    }

    return 0;
}

static DWORD WINAPI Instalar(LPVOID)
{
    PrepararRutas();
    LeerConfig();

    if(registroActivo){
        FILE* logNuevo = fopen(rutaLog, "w");

        if(logNuevo){
            fclose(logNuevo);
        }
    }
    QueryPerformanceFrequency(&frecuenciaQpc);
    QueryPerformanceCounter(&inicioQpc);
    SubirResolucionTimer();

    if(!BuildCorrecto()){
        Registrar("S4Client.exe does not match the expected build, nothing was patched");
        return 0;
    }

    retrasoValido = *(const uint32_t*)kRetrasoActores == 70;
    AplicarRetrasoActores();

    const bool sinLimite = QuitarLimiteJuego();
    const bool tiempo = RedirigirLlamada(kLlamadaTiempoPartida, (const void*)&TiempoPartida);
    const bool gravedad = RedirigirLlamada(kLlamadaGravedad, (const void*)&SumarGravedad);
    const bool vuelo = RedirigirLlamada(kLlamadaFrenadoVuelo, (const void*)&LeerFrenadoVuelo);
    const bool frame = RedirigirLlamada(kLlamadaFrameEstados, (const void*)&FrameEstados);
    const bool colision = RedirigirColision();
    const LONG hooks = InstalarHooks();

    char linea[320] = {};
    snprintf(linea, sizeof(linea), "unlock %s | frame start %s | frame time %s | gravity %s | fly %s | collision %s | hooks %ld | fps %d | fov %d/%d/%d | physics_fix %d | trace %d | low_latency %d | actor_delay %d%s",
        sinLimite ? "ok" : "fail",
        frame ? "ok" : "fail",
        tiempo ? "ok" : "fail",
        gravedad ? "ok" : "fail",
        vuelo ? "ok" : "fail",
        colision ? "ok" : "fail",
        hooks,
        max_framerate,
        field_of_view,
        center_field_of_view,
        sprint_field_of_view,
        fisicaActiva,
        registroActivo,
        bajaLatencia,
        retrasoActores,
        retrasoValido ? "" : " (not applied)");
    Registrar(linea);

    CreateThread(nullptr, 0, RecargarConfig, nullptr, 0, nullptr);
    return 0;
}

void ApplyFrameRate()
{
    siguienteFrameMs = -1.0;
}

void StartGameEnhancements()
{
    Instalar(nullptr);
}
