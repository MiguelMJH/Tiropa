// Tiropa.cpp : Defines the entry point for the application.
// VERSIÓN SINCRONIZADA CON LINUX - TCP + Mismos tiempos

#include "framework.h"
#include "Tiropa.h"
#include <windows.h>
#include "resource.h"
#include <time.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <commctrl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ws2_32.lib")

// Constants - SINCRONIZADOS CON LINUX
#define MAX_LOADSTRING 100
#define G 9.8f                      // ✅ Igual que Linux
#define PUERTO_SERVIDOR 4200        // ✅ Igual que Linux
#define ANCHO_PERSONAJE 50          // ✅ Igual que Linux
#define ALTO_PERSONAJE 40           // ✅ Igual que Linux
#define TAM_BOLA 30                 // ✅ Igual que Linux
#define MAX_ALIAS 32                // ✅ Igual que Linux
#define MAX_PROYECTILES 10
#define ANCHO_JUEGO 1080            // ✅ Igual que Linux
#define ALTO_JUEGO 720              // ✅ Igual que Linux
#define ALTO_CONTROLES 40
#define ANCHO_VENTANA (ANCHO_JUEGO + 20)
#define ALTO_VENTANA (ALTO_JUEGO + ALTO_CONTROLES + 60)
#define TIMER_INTERVAL 20           // ✅ CAMBIADO: 20ms igual que Linux
#define TIMEOUT_SEGUNDOS 3
#define ESCALA_VISUAL 3.0           // ✅ Igual que Linux
#define VELOCIDAD_VISUAL_CONSTANTE 150.0  // ✅ Igual que Linux
#define DELTA_T 0.02                // ✅ AGREGADO: Igual que Linux
#define SLEEP_TIME 20               // ✅ CAMBIADO: 20ms igual que Linux

// Structures - IDÉNTICAS A LINUX
typedef struct {
    double x, y, xo, yo;
    double vo, ang;
    double to;
    char NN[MAX_ALIAS];
} Datos;

typedef struct {
    CRITICAL_SECTION mutex;
    double x, y;
    double pos_x, pos_y;
    double vel_x, vel_y;
    double tiempo_ini;
    char remitente[MAX_ALIAS];
    BOOL activo;
    BOOL es_local;
} DatosProyectil;

typedef struct {
    int x, y, ancho, alto;
    BOOL visible;
} BarraJuego;

// Global Variables
HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];

SOCKET serverSocket = INVALID_SOCKET;
BOOL serverActivo = TRUE;
BOOL aplicacionCerrando = FALSE;
HWND hWndGlobal = NULL;
char alias_jugador[MAX_ALIAS] = "jugador1";

// Game area - exactly 1080x720 starting after controls
RECT gameArea = { 10, ALTO_CONTROLES + 10, ANCHO_JUEGO + 10, ALTO_JUEGO + ALTO_CONTROLES + 10 };

// Image resources
HBITMAP hBmpPersonaje = NULL;
HBITMAP hBmpBola = NULL;

// Character and game elements
int personaje_x = 0;
int personaje_y = 0;
int personaje_barra_actual = -1;
BarraJuego barras[3];
BOOL personaje_destruido = FALSE;
BOOL tiro_en_progreso = FALSE;

// Projectiles management
DatosProyectil proyectiles[MAX_PROYECTILES];
CRITICAL_SECTION mutex_proyectiles;
int num_proyectiles = 0;

// UI Elements
HWND hEditVelocidad = NULL;
HWND hEditAngulo = NULL;
HWND hEditIP = NULL;
HWND hEditNombre = NULL;
HWND hBtnDisparar = NULL;

// Double buffering
HDC hMemDC = NULL;
HBITMAP hMemBitmap = NULL;
HBITMAP hOldBitmap = NULL;

// Thread handles for proper cleanup
HANDLE hServidorThread = NULL;

// Function prototypes
ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

// Game functions
void InicializarJuego(HWND hWnd);
void InicializarBarrasAleatorias();
void PosicionarPersonajeSobreBarraAleatoria();
void DibujarJuego(HDC hdc);
void DibujarBitmap(HDC hdcDestino, HBITMAP hBitmap, int x, int y);
void ActualizarProyectiles();
void VerificarGravedadPersonaje();
void LimpiarProyectiles();
BOOL ValidarDatosEntrada();

// Network functions (TCP - CAMBIADO DE UDP A TCP)
void EnviarDatosTCP(const char* ipDestino, const Datos* datos);
DWORD WINAPI ServidorTCPThread(LPVOID param);
DWORD WINAPI ClienteTCPThread(LPVOID param);
DWORD WINAPI AtenderClienteThread(LPVOID param);
void ProcesarDatosRecibidos(const Datos* datos);

// Projectile functions
int CrearProyectil(double x, double y, double vo, double ang, BOOL es_local, const char* remitente);
void EliminarProyectil(int index);
void ActualizarProyectil(int index);

// Utility functions
void LimpiarRecursos();
void CerrarAplicacion();
BOOL Colisiona(double x, double y, const BarraJuego* barra);
void FinalizarPartida(const char* remitente);

// Implementation
void InicializarJuego(HWND hWnd) {
    // Initialize critical sections
    InitializeCriticalSection(&mutex_proyectiles);

    // Initialize projectiles array
    for (int i = 0; i < MAX_PROYECTILES; i++) {
        proyectiles[i].activo = FALSE;
        InitializeCriticalSection(&proyectiles[i].mutex);
    }

    // Load images
    hBmpPersonaje = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_PERSONAJE));
    hBmpBola = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_COMER));

    if (!hBmpPersonaje || !hBmpBola) {
        MessageBoxA(hWnd, "Error cargando imagenes del juego", "Error", MB_OK | MB_ICONERROR);
    }

    // Initialize game elements
    InicializarBarrasAleatorias();
    PosicionarPersonajeSobreBarraAleatoria();

    // Setup double buffering for exact game area
    HDC hdc = GetDC(hWnd);
    hMemDC = CreateCompatibleDC(hdc);
    hMemBitmap = CreateCompatibleBitmap(hdc, ANCHO_JUEGO, ALTO_JUEGO);
    hOldBitmap = (HBITMAP)SelectObject(hMemDC, hMemBitmap);
    ReleaseDC(hWnd, hdc);

    // Start TCP server thread
    hServidorThread = CreateThread(NULL, 0, ServidorTCPThread, hWnd, 0, NULL);

    // Start game timer - SINCRONIZADO CON LINUX (20ms)
    SetTimer(hWnd, 1, TIMER_INTERVAL, NULL);
}

void InicializarBarrasAleatorias() {
    srand((unsigned int)GetTickCount());

    const int anchoBarra = 70;
    const int anchoTotal = anchoBarra * 3;
    int maxBaseX = (ANCHO_JUEGO / 2) - anchoTotal;

    if (maxBaseX < 0) maxBaseX = 0;
    int baseX = maxBaseX > 0 ? rand() % (maxBaseX + 1) : 0;

    for (int i = 0; i < 3; i++) {
        barras[i].ancho = anchoBarra;
        barras[i].alto = 50 + (ALTO_JUEGO > 100 ? rand() % (ALTO_JUEGO / 2 - 50) : 50);
        barras[i].x = gameArea.left + baseX + i * anchoBarra;
        barras[i].y = gameArea.bottom - barras[i].alto;
        barras[i].visible = TRUE;
    }
}

void PosicionarPersonajeSobreBarraAleatoria() {
    if (!hBmpPersonaje) return;

    int barraSeleccionada = rand() % 3;

    // Centrar horizontalmente en la barra
    personaje_x = barras[barraSeleccionada].x + (barras[barraSeleccionada].ancho - ANCHO_PERSONAJE) / 2;

    // Posicionar COMPLETAMENTE encima de la barra con separación
    personaje_y = barras[barraSeleccionada].y - ALTO_PERSONAJE - 11;

    // Verificar que no se salga del área de juego por arriba
    if (personaje_y < gameArea.top) {
        personaje_y = gameArea.top;
    }

    // Establecer la barra actual
    personaje_barra_actual = barraSeleccionada;

    // Debug: Verificar posiciones
    char debug[256];
    sprintf_s(debug, sizeof(debug),
        "Personaje en barra %d: x=%d, y=%d",
        barraSeleccionada, personaje_x, personaje_y);
    OutputDebugStringA(debug);
}

void VerificarGravedadPersonaje() {
    if (personaje_destruido) return;

    // Si el personaje está asignado a una barra específica
    if (personaje_barra_actual >= 0 && personaje_barra_actual < 3) {
        // Verificar si esa barra fue destruida
        if (!barras[personaje_barra_actual].visible) {
            // Posicionar el personaje en el suelo
            personaje_y = gameArea.bottom - ALTO_PERSONAJE - 20;
            personaje_barra_actual = -1;  // Ahora está en el suelo

            // Debug
            OutputDebugStringA("Personaje cayó al suelo por destrucción de barra");
        }
    }
}

void DibujarBitmap(HDC hdcDestino, HBITMAP hBitmap, int x, int y) {
    if (!hBitmap) return;

    BITMAP bmp;
    if (!GetObject(hBitmap, sizeof(BITMAP), &bmp)) return;

    HDC hdcMem = CreateCompatibleDC(hdcDestino);
    if (!hdcMem) return;

    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hBitmap);
    BitBlt(hdcDestino, x, y, bmp.bmWidth, bmp.bmHeight, hdcMem, 0, 0, SRCCOPY);
    SelectObject(hdcMem, hOldBmp);
    DeleteDC(hdcMem);
}

void DibujarJuego(HDC hdc) {
    // Clear background with WHITE
    RECT rect = { 0, 0, ANCHO_JUEGO, ALTO_JUEGO };
    HBRUSH hBrushFondo = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdc, &rect, hBrushFondo);
    DeleteObject(hBrushFondo);

    // Draw bars
    HBRUSH hBarBrush = CreateSolidBrush(RGB(76, 76, 255)); // ✅ Color similar a Linux (0.3, 0.3, 1.0)
    for (int i = 0; i < 3; i++) {
        if (!barras[i].visible) continue;

        RECT barRect = {
            barras[i].x - gameArea.left,
            barras[i].y - gameArea.top,
            barras[i].x - gameArea.left + barras[i].ancho,
            barras[i].y - gameArea.top + barras[i].alto
        };
        FillRect(hdc, &barRect, hBarBrush);
    }
    DeleteObject(hBarBrush);

    // Draw character
    if (!personaje_destruido && hBmpPersonaje) {
        DibujarBitmap(hdc, hBmpPersonaje,
            personaje_x - gameArea.left,
            personaje_y - gameArea.top);
    }

    // Draw projectiles
    EnterCriticalSection(&mutex_proyectiles);
    for (int i = 0; i < MAX_PROYECTILES; i++) {
        if (!proyectiles[i].activo) continue;

        EnterCriticalSection(&proyectiles[i].mutex);
        double x = proyectiles[i].x - gameArea.left;
        double y = proyectiles[i].y - gameArea.top;
        LeaveCriticalSection(&proyectiles[i].mutex);

        if (x >= -TAM_BOLA && x <= ANCHO_JUEGO + TAM_BOLA &&
            y >= -TAM_BOLA && y <= ALTO_JUEGO + TAM_BOLA) {

            DibujarBitmap(hdc, hBmpBola,
                (int)(x - TAM_BOLA / 2),
                (int)(y - TAM_BOLA / 2));
        }
    }
    LeaveCriticalSection(&mutex_proyectiles);
}

int CrearProyectil(double x, double y, double vo, double ang, BOOL es_local, const char* remitente) {
    EnterCriticalSection(&mutex_proyectiles);

    int index = -1;
    for (int i = 0; i < MAX_PROYECTILES; i++) {
        if (!proyectiles[i].activo) {
            index = i;
            break;
        }
    }

    if (index == -1) {
        LeaveCriticalSection(&mutex_proyectiles);
        return -1;
    }

    DatosProyectil* p = &proyectiles[index];

    EnterCriticalSection(&p->mutex);
    p->pos_x = x;
    p->pos_y = y;
    p->x = x;
    p->y = y;
    p->vel_x = vo * cos(ang);
    p->vel_y = vo * sin(ang);
    p->tiempo_ini = 0.0;
    p->activo = TRUE;
    p->es_local = es_local;

    if (remitente) {
        strncpy_s(p->remitente, sizeof(p->remitente), remitente, _TRUNCATE);
    }
    else {
        strcpy_s(p->remitente, sizeof(p->remitente), alias_jugador);
    }
    LeaveCriticalSection(&p->mutex);

    num_proyectiles++;
    LeaveCriticalSection(&mutex_proyectiles);

    return index;
}

void EliminarProyectil(int index) {
    if (index < 0 || index >= MAX_PROYECTILES) return;

    EnterCriticalSection(&mutex_proyectiles);
    if (proyectiles[index].activo) {
        proyectiles[index].activo = FALSE;
        num_proyectiles--;
    }
    LeaveCriticalSection(&mutex_proyectiles);
}

void ActualizarProyectil(int index) {
    if (index < 0 || index >= MAX_PROYECTILES) return;

    DatosProyectil* p = &proyectiles[index];
    if (!p->activo) return;

    EnterCriticalSection(&p->mutex);

    // ✅ SINCRONIZADO CON LINUX: Misma normalización de velocidad
    double velocidad_total = sqrt(p->vel_x * p->vel_x + p->vel_y * p->vel_y);
    double factor_normalizacion = VELOCIDAD_VISUAL_CONSTANTE / velocidad_total;

    double vel_x_visual = p->vel_x * factor_normalizacion;
    double vel_y_visual = p->vel_y * factor_normalizacion;

    if (p->es_local) {
        // Local projectile physics
        p->x = p->pos_x + vel_x_visual * p->tiempo_ini * ESCALA_VISUAL;
        p->y = p->pos_y - (vel_y_visual * p->tiempo_ini -
            0.5 * G * factor_normalizacion * p->tiempo_ini * p->tiempo_ini) * ESCALA_VISUAL;
    }
    else {
        // Mirror effect for remote projectiles
        p->x = p->pos_x + vel_x_visual * p->tiempo_ini * ESCALA_VISUAL;
        p->x = (2 * gameArea.right) - p->x;  // Horizontal mirror
        p->y = p->pos_y - (vel_y_visual * p->tiempo_ini -
            0.5 * G * factor_normalizacion * p->tiempo_ini * p->tiempo_ini) * ESCALA_VISUAL;
    }

    // ✅ SINCRONIZADO CON LINUX: Mismo incremento de tiempo
    p->tiempo_ini += DELTA_T;  // 0.02 igual que Linux

    double x = p->x;
    double y = p->y;

    LeaveCriticalSection(&p->mutex);

    // Check boundaries
    if (y >= gameArea.bottom ||
        (p->es_local && x > gameArea.right + TAM_BOLA) ||
        (!p->es_local && x < gameArea.left - TAM_BOLA)) {

        if (p->es_local && x > gameArea.right + TAM_BOLA) {
            // Send projectile data when it exits the screen via TCP
            Datos* datos = (Datos*)malloc(sizeof(Datos));
            if (datos) {
                datos->x = x;
                datos->y = y;
                datos->xo = p->pos_x;
                datos->yo = p->pos_y;
                datos->vo = sqrt(p->vel_x * p->vel_x + p->vel_y * p->vel_y);
                datos->ang = atan2(p->vel_y, p->vel_x);
                datos->to = p->tiempo_ini;
                strcpy_s(datos->NN, sizeof(datos->NN), alias_jugador);

                CreateThread(NULL, 0, ClienteTCPThread, datos, 0, NULL);
            }
        }

        EliminarProyectil(index);
        return;
    }

    // Solo proyectiles REMOTOS verifican colisiones con barras
    if (!p->es_local) {
        // Check collisions with bars
        for (int i = 0; i < 3; i++) {
            if (!barras[i].visible) continue;

            if (Colisiona(x, y, &barras[i])) {
                // Destruir barra
                barras[i].visible = FALSE;

                // Eliminar el proyectil
                EliminarProyectil(index);
                return;
            }
        }

        // Check collision with character
        if (!personaje_destruido) {
            if (x >= personaje_x && x <= personaje_x + ANCHO_PERSONAJE &&
                y >= personaje_y && y <= personaje_y + ALTO_PERSONAJE) {

                // Eliminar el proyectil ANTES de finalizar la partida
                EliminarProyectil(index);

                // Finalizar partida
                FinalizarPartida(p->remitente);
                return;
            }
        }
    }
}

void ActualizarProyectiles() {
    if (aplicacionCerrando) return;

    VerificarGravedadPersonaje();

    for (int i = 0; i < MAX_PROYECTILES; i++) {
        if (proyectiles[i].activo) {
            ActualizarProyectil(i);
        }
    }

    // Check if we can enable the fire button again
    BOOL hayProyectilLocal = FALSE;
    EnterCriticalSection(&mutex_proyectiles);
    for (int i = 0; i < MAX_PROYECTILES; i++) {
        if (proyectiles[i].activo && proyectiles[i].es_local) {
            hayProyectilLocal = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&mutex_proyectiles);

    if (!hayProyectilLocal && tiro_en_progreso) {
        tiro_en_progreso = FALSE;
        if (hBtnDisparar && IsWindow(hBtnDisparar)) {
            EnableWindow(hBtnDisparar, TRUE);
        }
    }
}

BOOL Colisiona(double x, double y, const BarraJuego* barra) {
    return (x >= barra->x && x <= barra->x + barra->ancho &&
        y >= barra->y && y <= barra->y + barra->alto);
}

void FinalizarPartida(const char* remitente) {
    personaje_destruido = TRUE;

    char mensaje[256];
    sprintf_s(mensaje, sizeof(mensaje),
        "¡Tu personaje ha sido destruido por %s!\nLa aplicación se cerrará.", remitente);

    MessageBoxA(hWndGlobal, mensaje, "Fin del juego", MB_OK | MB_ICONINFORMATION);

    CerrarAplicacion();
    ExitProcess(0);
}

void CerrarAplicacion() {
    aplicacionCerrando = TRUE;
    serverActivo = FALSE;

    if (hWndGlobal && IsWindow(hWndGlobal)) {
        KillTimer(hWndGlobal, 1);
    }

    if (serverSocket != INVALID_SOCKET) {
        shutdown(serverSocket, SD_BOTH);
        closesocket(serverSocket);
        serverSocket = INVALID_SOCKET;
    }

    if (hServidorThread) {
        DWORD waitResult = WaitForSingleObject(hServidorThread, 1000);
        if (waitResult == WAIT_TIMEOUT) {
            TerminateThread(hServidorThread, 0);
        }
        CloseHandle(hServidorThread);
        hServidorThread = NULL;
    }

    LimpiarRecursos();

    if (hWndGlobal && IsWindow(hWndGlobal)) {
        DestroyWindow(hWndGlobal);
    }

    PostQuitMessage(0);
    Sleep(100);
    ExitProcess(0);
}

// ✅ CAMBIADO DE UDP A TCP - FUNCIONES DE RED SINCRONIZADAS CON LINUX
void EnviarDatosTCP(const char* ipDestino, const Datos* datos) {
    if (!ipDestino || strlen(ipDestino) == 0 || aplicacionCerrando) return;

    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);  // ✅ CAMBIADO A TCP
    if (s == INVALID_SOCKET) return;

    DWORD timeout = TIMEOUT_SEGUNDOS * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    sockaddr_in servidor = { 0 };
    servidor.sin_family = AF_INET;
    servidor.sin_port = htons(PUERTO_SERVIDOR);

    if (inet_pton(AF_INET, ipDestino, &servidor.sin_addr) == 1) {
        if (connect(s, (sockaddr*)&servidor, sizeof(servidor)) == 0) {
            send(s, (const char*)datos, sizeof(Datos), 0);  // ✅ TCP send

            // Recibir respuesta
            char respuesta[32];
            recv(s, respuesta, sizeof(respuesta), 0);
        }
    }

    closesocket(s);
}

DWORD WINAPI ClienteTCPThread(LPVOID param) {
    Datos* datos = (Datos*)param;

    if (!aplicacionCerrando && hEditIP && IsWindow(hEditIP)) {
        TCHAR bufferIP[32];
        GetWindowText(hEditIP, bufferIP, 32);
        char ipDestino[32];
        size_t converted = 0;
        wcstombs_s(&converted, ipDestino, sizeof(ipDestino), bufferIP, _TRUNCATE);

        EnviarDatosTCP(ipDestino, datos);
    }

    if (datos) {
        free(datos);
    }
    return 0;
}

DWORD WINAPI AtenderClienteThread(LPVOID param) {
    SOCKET clientSocket = (SOCKET)(uintptr_t)param;

    Datos datos = { 0 };
    int bytesRecibidos = recv(clientSocket, (char*)&datos, sizeof(Datos), 0);

    if (bytesRecibidos == sizeof(Datos) && !aplicacionCerrando) {
        ProcesarDatosRecibidos(&datos);

        // Enviar respuesta
        char respuesta[32] = "Proyectil recibido";
        send(clientSocket, respuesta, sizeof(respuesta), 0);
    }

    closesocket(clientSocket);
    return 0;
}

DWORD WINAPI ServidorTCPThread(LPVOID param) {
    HWND hWnd = (HWND)param;

    serverSocket = socket(AF_INET, SOCK_STREAM, 0);  // ✅ CAMBIADO A TCP
    if (serverSocket == INVALID_SOCKET) return 0;

    BOOL reuseAddr = TRUE;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuseAddr, sizeof(reuseAddr));

    sockaddr_in local = { 0 };
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(PUERTO_SERVIDOR);

    if (bind(serverSocket, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        closesocket(serverSocket);
        serverSocket = INVALID_SOCKET;
        return 0;
    }

    if (listen(serverSocket, 10) == SOCKET_ERROR) {  // ✅ TCP listen
        closesocket(serverSocket);
        serverSocket = INVALID_SOCKET;
        return 0;
    }

    while (serverActivo && !aplicacionCerrando) {
        sockaddr_in cliente = { 0 };
        int len = sizeof(cliente);

        SOCKET clientSocket = accept(serverSocket, (sockaddr*)&cliente, &len);  // ✅ TCP accept

        if (clientSocket != INVALID_SOCKET && !aplicacionCerrando) {
            // Crear hilo para atender cliente
            CreateThread(NULL, 0, AtenderClienteThread, (LPVOID)(uintptr_t)clientSocket, 0, NULL);
        }
        else if (clientSocket == INVALID_SOCKET) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                Sleep(50);
                continue;
            }
            else {
                break;
            }
        }
    }

    if (serverSocket != INVALID_SOCKET) {
        closesocket(serverSocket);
        serverSocket = INVALID_SOCKET;
    }

    return 0;
}

void ProcesarDatosRecibidos(const Datos* datos) {
    if (!aplicacionCerrando) {
        CrearProyectil(datos->xo, datos->yo, datos->vo, datos->ang, FALSE, datos->NN);
    }
}

BOOL ValidarDatosEntrada() {
    TCHAR bufferVel[16], bufferAng[16], bufferIP[32];
    GetWindowText(hEditVelocidad, bufferVel, 16);
    GetWindowText(hEditAngulo, bufferAng, 16);
    GetWindowText(hEditIP, bufferIP, 32);

    if (wcslen(bufferVel) == 0 || wcslen(bufferAng) == 0 || wcslen(bufferIP) == 0) {
        MessageBox(hWndGlobal, L"Debe rellenar todas las casillas.", L"Campos vacíos", MB_OK | MB_ICONWARNING);
        return FALSE;
    }

    double vel = _wtof(bufferVel);
    double angDeg = _wtof(bufferAng);

    if (vel <= 0 || vel > 1000) {
        MessageBox(hWndGlobal, L"La velocidad debe estar entre 1 y 1000.", L"Velocidad inválida", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    // ✅ SINCRONIZADO CON LINUX: 1-89 grados
    if (angDeg <= 0 || angDeg >= 90) {
        MessageBox(hWndGlobal, L"El ángulo debe estar entre 1 y 89 grados.", L"Ángulo inválido", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    if (wcsstr(bufferIP, L".") == NULL) {
        MessageBox(hWndGlobal, L"Dirección IP inválida.", L"IP incorrecta", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    return TRUE;
}

void LimpiarProyectiles() {
    EnterCriticalSection(&mutex_proyectiles);
    for (int i = 0; i < MAX_PROYECTILES; i++) {
        proyectiles[i].activo = FALSE;
    }
    num_proyectiles = 0;
    LeaveCriticalSection(&mutex_proyectiles);
}

void LimpiarRecursos() {
    LimpiarProyectiles();

    for (int i = 0; i < MAX_PROYECTILES; i++) {
        DeleteCriticalSection(&proyectiles[i].mutex);
    }
    DeleteCriticalSection(&mutex_proyectiles);

    if (hBmpPersonaje) {
        DeleteObject(hBmpPersonaje);
        hBmpPersonaje = NULL;
    }
    if (hBmpBola) {
        DeleteObject(hBmpBola);
        hBmpBola = NULL;
    }

    if (hMemDC) {
        SelectObject(hMemDC, hOldBitmap);
        DeleteObject(hMemBitmap);
        DeleteDC(hMemDC);
        hMemDC = NULL;
    }
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MessageBox(NULL, L"Error inicializando Winsock", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_TIROPA, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    if (!InitInstance(hInstance, nCmdShow)) {
        WSACleanup();
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_TIROPA));
    MSG msg;

    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    WSACleanup();
    return (int)msg.wParam;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex = { 0 };

    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_TIROPA));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_TIROPA);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;

    HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, ANCHO_VENTANA, ALTO_VENTANA, nullptr, nullptr, hInstance, nullptr);

    if (!hWnd) {
        return FALSE;
    }

    hWndGlobal = hWnd;
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    return TRUE;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_TIMER:
        if (wParam == 1 && !aplicacionCerrando) {
            ActualizarProyectiles();
            InvalidateRect(hWnd, &gameArea, FALSE);
        }
        break;

    case WM_CREATE:
        CreateWindowW(L"STATIC", L"Nombre:", WS_CHILD | WS_VISIBLE,
            20, 15, 60, 20, hWnd, NULL, hInst, NULL);
        hEditNombre = CreateWindowW(L"EDIT", L"jugador1", WS_CHILD | WS_VISIBLE | WS_BORDER,
            85, 13, 100, 25, hWnd, NULL, hInst, NULL);

        CreateWindowW(L"STATIC", L"Velocidad:", WS_CHILD | WS_VISIBLE,
            200, 15, 80, 20, hWnd, NULL, hInst, NULL);
        hEditVelocidad = CreateWindowW(L"EDIT", L"50", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
            285, 13, 60, 25, hWnd, NULL, hInst, NULL);

        CreateWindowW(L"STATIC", L"Ángulo:", WS_CHILD | WS_VISIBLE,
            360, 15, 60, 20, hWnd, NULL, hInst, NULL);
        hEditAngulo = CreateWindowW(L"EDIT", L"45", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
            420, 13, 60, 25, hWnd, NULL, hInst, NULL);

        CreateWindowW(L"STATIC", L"IP:", WS_CHILD | WS_VISIBLE,
            495, 15, 30, 20, hWnd, NULL, hInst, NULL);
        hEditIP = CreateWindowW(L"EDIT", L"127.0.0.1", WS_CHILD | WS_VISIBLE | WS_BORDER,
            525, 13, 120, 25, hWnd, NULL, hInst, NULL);

        hBtnDisparar = CreateWindowW(L"BUTTON", L"Disparar", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
            660, 13, 80, 30, hWnd, (HMENU)1, hInst, NULL);

        InicializarJuego(hWnd);
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case 1: // Disparar button
        {
            if (personaje_destruido) {
                MessageBox(hWnd, L"Personaje destruido. Reinicia el juego.", L"No se puede disparar", MB_OK | MB_ICONINFORMATION);
                break;
            }

            if (tiro_en_progreso) {
                MessageBox(hWnd, L"Ya hay un tiro en progreso. Espera a que termine.", L"Tiro en progreso", MB_OK | MB_ICONWARNING);
                break;
            }

            if (!ValidarDatosEntrada()) {
                break;
            }

            TCHAR bufferNombre[MAX_ALIAS];
            GetWindowText(hEditNombre, bufferNombre, MAX_ALIAS);
            WideCharToMultiByte(CP_UTF8, 0, bufferNombre, -1, alias_jugador, MAX_ALIAS, NULL, NULL);

            TCHAR bufferVel[16], bufferAng[16];
            GetWindowText(hEditVelocidad, bufferVel, 16);
            GetWindowText(hEditAngulo, bufferAng, 16);

            double vel = _wtof(bufferVel);
            double angRad = _wtof(bufferAng) * M_PI / 180.0;

            CrearProyectil(personaje_x + ANCHO_PERSONAJE / 2, personaje_y + ALTO_PERSONAJE / 2,
                vel, angRad, TRUE, alias_jugador);

            tiro_en_progreso = TRUE;
            EnableWindow(hBtnDisparar, FALSE);
            break;
        }
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        if (ps.rcPaint.left < gameArea.right && ps.rcPaint.right > gameArea.left &&
            ps.rcPaint.top < gameArea.bottom && ps.rcPaint.bottom > gameArea.top) {

            DibujarJuego(hMemDC);
            BitBlt(hdc, gameArea.left, gameArea.top, ANCHO_JUEGO, ALTO_JUEGO,
                hMemDC, 0, 0, SRCCOPY);
        }

        EndPaint(hWnd, &ps);
    }
    break;

    case WM_CLOSE:
        CerrarAplicacion();
        break;

    case WM_DESTROY:
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
