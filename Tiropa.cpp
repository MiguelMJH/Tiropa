// Tiropa.cpp : Juego de tiro parabólico multijugador
// Sistema de colisiones mejorado: rectángulo vs rectángulo

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

// Constantes del juego
#define MAX_LOADSTRING 100
#define G 9.8f                          // Aceleración gravitacional
#define PUERTO_SERVIDOR 4200            // Puerto TCP para comunicación
#define ANCHO_PERSONAJE 50              // Dimensiones del personaje
#define ALTO_PERSONAJE 40
#define TAM_BOLA 30                     // Tamaño del proyectil
#define MAX_ALIAS 32                    // Longitud máxima del nombre
#define MAX_PROYECTILES 10              // Máximo proyectiles simultáneos
#define ANCHO_JUEGO 1080                // Área de juego
#define ALTO_JUEGO 720
#define ALTO_CONTROLES 40               // Espacio para controles UI
#define ANCHO_VENTANA (ANCHO_JUEGO + 20)
#define ALTO_VENTANA (ALTO_JUEGO + ALTO_CONTROLES + 60)
#define TIMER_INTERVAL 20               // Intervalo del timer (50 FPS)
#define TIMEOUT_SEGUNDOS 3              // Timeout de red
#define ESCALA_VISUAL 3.0               // Factor de escala visual
#define VELOCIDAD_VISUAL_CONSTANTE 150.0 // Velocidad visual normalizada
#define DELTA_T 0.02                    // Delta de tiempo para física

// Estructura de datos de red - compatible con otros proyectos
typedef struct {
    double x, y, xo, yo;                // Posición actual y origen
    double vo, ang;                     // Velocidad inicial y ángulo
    double to;                          // Tiempo inicial
    char NN[MAX_ALIAS];                 // Nombre del jugador
} Datos;

// Estructura interna del proyectil
typedef struct {
    CRITICAL_SECTION mutex;             // Sincronización de hilos
    double x, y;                        // Posición actual
    double pos_x, pos_y;                // Posición inicial
    double vel_x, vel_y;                // Velocidades componentes
    double tiempo_ini;                  // Tiempo transcurrido
    char remitente[MAX_ALIAS];          // Quien disparó
    BOOL activo;                        // Estado del proyectil
    BOOL es_local;                      // Proyectil propio o remoto
} DatosProyectil;

// Estructura de las barras del juego
typedef struct {
    int x, y, ancho, alto;              // Dimensiones y posición
    BOOL visible;                       // Estado de visibilidad
} BarraJuego;

// Variables globales del sistema
HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];

// Variables de red
SOCKET serverSocket = INVALID_SOCKET;
BOOL serverActivo = TRUE;
BOOL aplicacionCerrando = FALSE;
HWND hWndGlobal = NULL;
char alias_jugador[MAX_ALIAS] = "jugador1";

// Área de juego
RECT gameArea = { 10, ALTO_CONTROLES + 10, ANCHO_JUEGO + 10, ALTO_JUEGO + ALTO_CONTROLES + 10 };

// Recursos gráficos
HBITMAP hBmpPersonaje = NULL;
HBITMAP hBmpBola = NULL;

// Estado del personaje
int personaje_x = 0;
int personaje_y = 0;
int personaje_barra_actual = -1;        // Índice de barra donde está (-1 = suelo)
BarraJuego barras[3];                   // Tres barras del juego
BOOL personaje_destruido = FALSE;
BOOL tiro_en_progreso = FALSE;

// Sistema de proyectiles
DatosProyectil proyectiles[MAX_PROYECTILES];
CRITICAL_SECTION mutex_proyectiles;
int num_proyectiles = 0;

// Controles de interfaz
HWND hEditVelocidad = NULL;
HWND hEditAngulo = NULL;
HWND hEditIP = NULL;
HWND hEditNombre = NULL;
HWND hBtnDisparar = NULL;

// Double buffering
HDC hMemDC = NULL;
HBITMAP hMemBitmap = NULL;
HBITMAP hOldBitmap = NULL;

// Hilo del servidor
HANDLE hServidorThread = NULL;

// Prototipos de funciones
ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

// Funciones del juego
void InicializarJuego(HWND hWnd);
void InicializarBarrasAleatorias();
void PosicionarPersonajeSobreBarraAleatoria();
void DibujarJuego(HDC hdc);
void DibujarBitmap(HDC hdcDestino, HBITMAP hBitmap, int x, int y);
void ActualizarProyectiles();
void VerificarGravedadPersonaje();
void LimpiarProyectiles();
BOOL ValidarDatosEntrada();

// Funciones de red TCP
void EnviarDatosTCP(const char* ipDestino, const Datos* datos);
DWORD WINAPI ServidorTCPThread(LPVOID param);
DWORD WINAPI ClienteTCPThread(LPVOID param);
DWORD WINAPI AtenderClienteThread(LPVOID param);
void ProcesarDatosRecibidos(const Datos* datos);

// Funciones de proyectiles
int CrearProyectil(double x, double y, double vo, double ang, BOOL es_local, const char* remitente);
void EliminarProyectil(int index);
void ActualizarProyectil(int index);

// Funciones de utilidad
void LimpiarRecursos();
void CerrarAplicacion();
BOOL ColisionaConBarra(double x, double y, const BarraJuego* barra);
BOOL ColisionaRectangulos(double x1, double y1, double w1, double h1,
    double x2, double y2, double w2, double h2);
BOOL ColisionaConPersonaje(double proj_x, double proj_y);
void FinalizarPartida(const char* remitente);

// Implementación de funciones

// Inicializa el sistema de juego
void InicializarJuego(HWND hWnd) {
    InitializeCriticalSection(&mutex_proyectiles);

    // Inicializar array de proyectiles
    for (int i = 0; i < MAX_PROYECTILES; i++) {
        proyectiles[i].activo = FALSE;
        InitializeCriticalSection(&proyectiles[i].mutex);
    }

    // Cargar recursos gráficos
    hBmpPersonaje = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_PERSONAJE));
    hBmpBola = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_COMER));

    if (!hBmpPersonaje || !hBmpBola) {
        MessageBoxA(hWnd, "Error cargando imagenes del juego", "Error", MB_OK | MB_ICONERROR);
    }

    // Configurar elementos del juego
    InicializarBarrasAleatorias();
    PosicionarPersonajeSobreBarraAleatoria();

    // Configurar double buffering
    HDC hdc = GetDC(hWnd);
    hMemDC = CreateCompatibleDC(hdc);
    hMemBitmap = CreateCompatibleBitmap(hdc, ANCHO_JUEGO, ALTO_JUEGO);
    hOldBitmap = (HBITMAP)SelectObject(hMemDC, hMemBitmap);
    ReleaseDC(hWnd, hdc);

    // Iniciar servidor TCP
    hServidorThread = CreateThread(NULL, 0, ServidorTCPThread, hWnd, 0, NULL);

    // Iniciar timer del juego
    SetTimer(hWnd, 1, TIMER_INTERVAL, NULL);
}

// Genera posiciones aleatorias para las barras
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

// Coloca el personaje sobre una barra aleatoria
void PosicionarPersonajeSobreBarraAleatoria() {
    if (!hBmpPersonaje) return;

    int barraSeleccionada = rand() % 3;

    // Centrar horizontalmente en la barra
    personaje_x = barras[barraSeleccionada].x + (barras[barraSeleccionada].ancho - ANCHO_PERSONAJE) / 2;

    // Posicionar encima de la barra con separación
    personaje_y = barras[barraSeleccionada].y - ALTO_PERSONAJE - 10;

    // Verificar límites del área de juego
    if (personaje_y < gameArea.top) {
        personaje_y = gameArea.top;
    }

    personaje_barra_actual = barraSeleccionada;
}

// Verifica si el personaje debe caer por destrucción de barra
void VerificarGravedadPersonaje() {
    if (personaje_destruido) return;

    // Si está en una barra específica
    if (personaje_barra_actual >= 0 && personaje_barra_actual < 3) {
        // Verificar si la barra fue destruida
        if (!barras[personaje_barra_actual].visible) {
            // Hacer caer al suelo
            personaje_y = gameArea.bottom - ALTO_PERSONAJE - 20;
            personaje_barra_actual = -1;
        }
    }
}

// Dibuja un bitmap en el contexto especificado
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

// Renderiza todos los elementos del juego
void DibujarJuego(HDC hdc) {
    // Limpiar fondo
    RECT rect = { 0, 0, ANCHO_JUEGO, ALTO_JUEGO };
    HBRUSH hBrushFondo = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdc, &rect, hBrushFondo);
    DeleteObject(hBrushFondo);

    // Dibujar barras
    HBRUSH hBarBrush = CreateSolidBrush(RGB(76, 76, 255));
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

    // Dibujar personaje
    if (!personaje_destruido && hBmpPersonaje) {
        DibujarBitmap(hdc, hBmpPersonaje,
            personaje_x - gameArea.left,
            personaje_y - gameArea.top);
    }

    // Dibujar proyectiles
    EnterCriticalSection(&mutex_proyectiles);
    for (int i = 0; i < MAX_PROYECTILES; i++) {
        if (!proyectiles[i].activo) continue;

        EnterCriticalSection(&proyectiles[i].mutex);
        double x = proyectiles[i].x - gameArea.left;
        double y = proyectiles[i].y - gameArea.top;
        LeaveCriticalSection(&proyectiles[i].mutex);

        // Solo dibujar si está en área visible
        if (x >= -TAM_BOLA && x <= ANCHO_JUEGO + TAM_BOLA &&
            y >= -TAM_BOLA && y <= ALTO_JUEGO + TAM_BOLA) {

            DibujarBitmap(hdc, hBmpBola,
                (int)(x - TAM_BOLA / 2),
                (int)(y - TAM_BOLA / 2));
        }
    }
    LeaveCriticalSection(&mutex_proyectiles);
}

// Crea un nuevo proyectil en el sistema
int CrearProyectil(double x, double y, double vo, double ang, BOOL es_local, const char* remitente) {
    EnterCriticalSection(&mutex_proyectiles);

    // Buscar slot libre
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

    // Inicializar proyectil
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

// Elimina un proyectil del sistema
void EliminarProyectil(int index) {
    if (index < 0 || index >= MAX_PROYECTILES) return;

    EnterCriticalSection(&mutex_proyectiles);
    if (proyectiles[index].activo) {
        proyectiles[index].activo = FALSE;
        num_proyectiles--;
    }
    LeaveCriticalSection(&mutex_proyectiles);
}

// Detección de colisión rectángulo vs rectángulo
BOOL ColisionaRectangulos(double x1, double y1, double w1, double h1,
    double x2, double y2, double w2, double h2) {
    return !(x1 + w1 <= x2 || x2 + w2 <= x1 || y1 + h1 <= y2 || y2 + h2 <= y1);
}

// Verifica colisión del proyectil con una barra
BOOL ColisionaConBarra(double x, double y, const BarraJuego* barra) {
    return ColisionaRectangulos(
        x - TAM_BOLA / 2, y - TAM_BOLA / 2, TAM_BOLA, TAM_BOLA,  // Proyectil
        barra->x, barra->y, barra->ancho, barra->alto        // Barra
    );
}

// Verifica colisión del proyectil con el personaje
BOOL ColisionaConPersonaje(double proj_x, double proj_y) {
    return ColisionaRectangulos(
        proj_x - TAM_BOLA / 2, proj_y - TAM_BOLA / 2, TAM_BOLA, TAM_BOLA,  // Proyectil
        personaje_x, personaje_y, ANCHO_PERSONAJE, ALTO_PERSONAJE      // Personaje
    );
}

// Actualiza la física y colisiones de un proyectil
void ActualizarProyectil(int index) {
    if (index < 0 || index >= MAX_PROYECTILES) return;

    DatosProyectil* p = &proyectiles[index];
    if (!p->activo) return;

    EnterCriticalSection(&p->mutex);

    // Calcular velocidad normalizada para movimiento visual constante
    double velocidad_total = sqrt(p->vel_x * p->vel_x + p->vel_y * p->vel_y);
    double factor_normalizacion = VELOCIDAD_VISUAL_CONSTANTE / velocidad_total;

    double vel_x_visual = p->vel_x * factor_normalizacion;
    double vel_y_visual = p->vel_y * factor_normalizacion;

    // Calcular nueva posición según física parabólica
    if (p->es_local) {
        // Proyectil local - movimiento normal
        p->x = p->pos_x + vel_x_visual * p->tiempo_ini * ESCALA_VISUAL;
        p->y = p->pos_y - (vel_y_visual * p->tiempo_ini -
            0.5 * G * factor_normalizacion * p->tiempo_ini * p->tiempo_ini) * ESCALA_VISUAL;
    }
    else {
        // Proyectil remoto - efecto espejo horizontal
        p->x = p->pos_x + vel_x_visual * p->tiempo_ini * ESCALA_VISUAL;
        p->x = (2 * gameArea.right) - p->x;
        p->y = p->pos_y - (vel_y_visual * p->tiempo_ini -
            0.5 * G * factor_normalizacion * p->tiempo_ini * p->tiempo_ini) * ESCALA_VISUAL;
    }

    p->tiempo_ini += DELTA_T;

    double x = p->x;
    double y = p->y;

    LeaveCriticalSection(&p->mutex);

    // Verificar límites del área de juego
    if (y >= gameArea.bottom ||
        (p->es_local && x > gameArea.right + TAM_BOLA) ||
        (!p->es_local && x < gameArea.left - TAM_BOLA)) {

        // Si es proyectil local que sale por la derecha, enviarlo por red
        if (p->es_local && x > gameArea.right + TAM_BOLA) {
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

    // Solo proyectiles remotos verifican colisiones
    if (!p->es_local) {
        // Verificar colisión con barras
        for (int i = 0; i < 3; i++) {
            if (!barras[i].visible) continue;

            if (ColisionaConBarra(x, y, &barras[i])) {
                barras[i].visible = FALSE;
                EliminarProyectil(index);
                return;
            }
        }

        // Verificar colisión con personaje - MEJORADA
        if (!personaje_destruido) {
            if (ColisionaConPersonaje(x, y)) {
                EliminarProyectil(index);
                FinalizarPartida(p->remitente);
                return;
            }
        }
    }
}

// Actualiza todos los proyectiles activos
void ActualizarProyectiles() {
    if (aplicacionCerrando) return;

    VerificarGravedadPersonaje();

    // Actualizar cada proyectil
    for (int i = 0; i < MAX_PROYECTILES; i++) {
        if (proyectiles[i].activo) {
            ActualizarProyectil(i);
        }
    }

    // Verificar si se puede habilitar el botón de disparo
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

// Finaliza la partida cuando el personaje es destruido
void FinalizarPartida(const char* remitente) {
    personaje_destruido = TRUE;

    char mensaje[256];
    sprintf_s(mensaje, sizeof(mensaje),
        "¡Tu personaje ha sido destruido por %s!\nLa aplicación se cerrará.", remitente);

    MessageBoxA(hWndGlobal, mensaje, "Fin del juego", MB_OK | MB_ICONINFORMATION);

    CerrarAplicacion();
    ExitProcess(0);
}

// Cierra la aplicación de forma segura
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

// Envía datos por TCP - protocolo limpio
void EnviarDatosTCP(const char* ipDestino, const Datos* datos) {
    if (!ipDestino || strlen(ipDestino) == 0 || aplicacionCerrando) return;

    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return;

    DWORD timeout = TIMEOUT_SEGUNDOS * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    sockaddr_in servidor = { 0 };
    servidor.sin_family = AF_INET;
    servidor.sin_port = htons(PUERTO_SERVIDOR);

    if (inet_pton(AF_INET, ipDestino, &servidor.sin_addr) == 1) {
        if (connect(s, (sockaddr*)&servidor, sizeof(servidor)) == 0) {
            // Solo enviar estructura Datos
            send(s, (const char*)datos, sizeof(Datos), 0);
        }
    }

    closesocket(s);
}

// Hilo cliente TCP
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

// Atiende conexiones de clientes TCP
DWORD WINAPI AtenderClienteThread(LPVOID param) {
    SOCKET clientSocket = (SOCKET)(uintptr_t)param;

    Datos datos = { 0 };
    int bytesRecibidos = recv(clientSocket, (char*)&datos, sizeof(Datos), 0);

    if (bytesRecibidos == sizeof(Datos) && !aplicacionCerrando) {
        ProcesarDatosRecibidos(&datos);
    }

    closesocket(clientSocket);
    return 0;
}

// Hilo servidor TCP principal
DWORD WINAPI ServidorTCPThread(LPVOID param) {
    HWND hWnd = (HWND)param;

    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
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

    if (listen(serverSocket, 10) == SOCKET_ERROR) {
        closesocket(serverSocket);
        serverSocket = INVALID_SOCKET;
        return 0;
    }

    while (serverActivo && !aplicacionCerrando) {
        sockaddr_in cliente = { 0 };
        int len = sizeof(cliente);

        SOCKET clientSocket = accept(serverSocket, (sockaddr*)&cliente, &len);

        if (clientSocket != INVALID_SOCKET && !aplicacionCerrando) {
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

// Procesa datos recibidos de la red
void ProcesarDatosRecibidos(const Datos* datos) {
    if (!aplicacionCerrando) {
        CrearProyectil(datos->xo, datos->yo, datos->vo, datos->ang, FALSE, datos->NN);
    }
}

// Valida los datos de entrada del usuario
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

// Limpia todos los proyectiles activos
void LimpiarProyectiles() {
    EnterCriticalSection(&mutex_proyectiles);
    for (int i = 0; i < MAX_PROYECTILES; i++) {
        proyectiles[i].activo = FALSE;
    }
    num_proyectiles = 0;
    LeaveCriticalSection(&mutex_proyectiles);
}

// Libera todos los recursos del sistema
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

// Punto de entrada principal de la aplicación
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

// Registra la clase de ventana
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

// Inicializa la instancia de la aplicación
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

// Procedimiento de ventana principal
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
        // Crear controles de interfaz
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
        case 1: // Botón disparar
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

            // Obtener datos del usuario
            TCHAR bufferNombre[MAX_ALIAS];
            GetWindowText(hEditNombre, bufferNombre, MAX_ALIAS);
            WideCharToMultiByte(CP_UTF8, 0, bufferNombre, -1, alias_jugador, MAX_ALIAS, NULL, NULL);

            TCHAR bufferVel[16], bufferAng[16];
            GetWindowText(hEditVelocidad, bufferVel, 16);
            GetWindowText(hEditAngulo, bufferAng, 16);

            double vel = _wtof(bufferVel);
            double angRad = _wtof(bufferAng) * M_PI / 180.0;

            // Crear proyectil desde el centro del personaje
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

        // Solo redibujar si es necesario
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

// Diálogo "Acerca de"
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
