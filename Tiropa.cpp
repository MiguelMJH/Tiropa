// Tiropa.cpp : Defines the entry point for the application.

#include "framework.h"
#include "Tiropa.h"
#include <windows.h>
#include "resource.h"
#include <time.h>
#include <math.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")



#define MAX_LOADSTRING 100
#define DELTA_T 0.06f
#define G 9.81f // Aceleración de la gravedad

SOCKET serverSocket;
BOOL serverActivo = TRUE;
HWND hWndGlobal = NULL;  // este se usa para mandar mensajes desde el hilo del servidor


// Global Variables:
HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name


// Variables globals para imagenes
HBITMAP hBmpPersonaje = NULL;
HBITMAP hBmpComer = NULL;

//Variables globales para personaje
int personaje_x = 0;
int personaje_y = 0;
int personaje_xo = 0;
int personaje_yo = 0;
#define ANCHO_PERSONAJE 50
#define ALTO_PERSONAJE  40

// Tamaño del proyectil
HBITMAP hBmpBola = NULL;
#define TAM_BOLA 30


//Variable para las barras
#define NUM_BARRAS 3
struct Barra {
    int x, y;
    int ancho, alto;
    bool visible;
};
Barra barras[NUM_BARRAS];

// Estructura para almacenar datos del disparo
#define MAX_ALIAS 32
typedef struct {
    double x, y, xo, yo;
    double vo, ang;
    double to;
    char NN[MAX_ALIAS];
} Datos;
Datos d;

// Estructura para el proyectil
typedef struct {
    double x, y;
    double xo, yo;
    double vo;
    double ang;
    double tiempo;
    BOOL activo;
} Proyectil;

Proyectil proyectil_local;
BOOL enviado = FALSE;  // bandera global para controlar si ya se envió el proyectil

// UI Elements
HWND hEditVelocidad;
HWND hEditAngulo;
HWND hEditIP;
HWND hBtnDisparar;
HWND hBtnSalir;
HWND hBtnAcercaDe;

// Forward declarations of functions included in this code module:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

/////////////////////////////////FUNCIONES//////////////////////////////////
void DibujarBitmapEscalado(HDC hdcDestino, HBITMAP hBitmap, int x, int y, int ancho, int alto) {
    if (!hBitmap) return;

    BITMAP bmp;
    GetObject(hBitmap, sizeof(BITMAP), &bmp);

    HDC hdcMem = CreateCompatibleDC(hdcDestino);
    SelectObject(hdcMem, hBitmap);

    // Escala al tamaño especificado
    StretchBlt(hdcDestino, x, y, ancho, alto, hdcMem, 0, 0, bmp.bmWidth, bmp.bmHeight, SRCCOPY);

    DeleteDC(hdcMem);
}
void DibujarBitmap(HDC hdcDestino, HBITMAP hBitmap, int x, int y) {
    if (!hBitmap) return;

    BITMAP bmp;
    GetObject(hBitmap, sizeof(BITMAP), &bmp);

    HDC hdcMem = CreateCompatibleDC(hdcDestino);
    SelectObject(hdcMem, hBitmap);

    BitBlt(hdcDestino, x, y, bmp.bmWidth, bmp.bmHeight, hdcMem, 0, 0, SRCCOPY);

    DeleteDC(hdcMem);
}
void InicializarBarrasAleatorias(HWND hWnd) {
    RECT cliente;
    GetClientRect(hWnd, &cliente);
    int ancho_ventana = cliente.right;
    int alto_ventana = cliente.bottom;

    //srand((unsigned int)time(NULL));
    srand((unsigned int)GetTickCount());

    const int anchoBarra = 70;
    const int numBarras = NUM_BARRAS;
    const int anchoTotal = anchoBarra * numBarras;

    // La mitad izquierda va de x = 0 hasta x = ancho_ventana / 2
    // El bloque completo debe caber dentro de ese espacio
    int maxBaseX = (ancho_ventana / 2) - anchoTotal;

    // Elegimos una posición de inicio completamente aleatoria válida
    int baseX = rand() % (maxBaseX + 1);

    for (int i = 0; i < numBarras; i++) {
        barras[i].ancho = anchoBarra;
        barras[i].alto = 50 + rand() % (alto_ventana / 2 - 50);  // altura entre 50 y la mitad de la ventana
        barras[i].x = baseX + i * anchoBarra;
        barras[i].y = alto_ventana - barras[i].alto;
        barras[i].visible = TRUE;
    }
}
void PosicionarPersonajeSobreBarraAleatoria(HWND hWnd) {
    if (!hBmpPersonaje) return;

    BITMAP bmp;
    GetObject(hBmpPersonaje, sizeof(BITMAP), &bmp);

    int anchoPersonaje = bmp.bmWidth;
    int altoPersonaje = bmp.bmHeight;

    // Elegir una barra al azar
    int barraSeleccionada = rand() % NUM_BARRAS;

    // Centrado horizontal sobre la barra
    personaje_x = barras[barraSeleccionada].x +
        (barras[barraSeleccionada].ancho - anchoPersonaje) / 2;

    // Posicionado justo encima de la barra
    personaje_y = barras[barraSeleccionada].y - altoPersonaje - 1;

    // Seguridad: evita que el personaje esté fuera del área visible
    if (personaje_y < 0) personaje_y = 0;
}
void EnviarDatos(const char* ipDestino, Datos datos) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return;

    // ?? Timeout de 2 segundos para envío y recepción
    struct timeval timeout;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    // Dirección del servidor
    sockaddr_in servidor;
    servidor.sin_family = AF_INET;
    servidor.sin_port = htons(4200);

    // Validar IP (inet_pton regresa 1 si es válida)
    if (inet_pton(AF_INET, ipDestino, &servidor.sin_addr) != 1) {
        closesocket(s);
        return;
    }

    // Intentar conectar (bloquea máximo 2 segundos por timeout)
    if (connect(s, (sockaddr*)&servidor, sizeof(servidor)) == SOCKET_ERROR) {
        closesocket(s);
        return;
    }

    // Enviar datos
    send(s, (char*)&datos, sizeof(Datos), 0);
    closesocket(s);
}
DWORD WINAPI ServidorThread(LPVOID param) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in local;
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(4200);

    bind(serverSocket, (struct sockaddr*)&local, sizeof(local));
    listen(serverSocket, 5);  // Acepta hasta 5 conexiones en espera

    while (true) {
        struct sockaddr_in cliente;
        int len = sizeof(cliente);
        SOCKET clienteSocket = accept(serverSocket, (struct sockaddr*)&cliente, &len);
        if (clienteSocket == INVALID_SOCKET) continue;

        Datos datos;
        int bytesRecibidos = recv(clienteSocket, (char*)&datos, sizeof(Datos), 0);
        if (recv(clienteSocket, (char*)&d, sizeof(Datos), 0) > 0) {
            proyectil_local.xo = d.x;
            proyectil_local.yo = d.y;
            proyectil_local.vo = d.vo;
            proyectil_local.ang = d.ang;
            proyectil_local.tiempo = d.to;
            proyectil_local.x = d.x;
            proyectil_local.y = d.y;
            proyectil_local.activo = TRUE;

            // Activa el temporizador desde otro hilo (usar PostMessage para seguridad)
            PostMessage(hWndGlobal, WM_USER + 1, 0, 0);  // Lanzar temporizador desde hilo principal
        }

        closesocket(clienteSocket);

        if (bytesRecibidos == sizeof(Datos)) {
            // Iniciar proyectil con los datos recibidos
            proyectil_local.xo = datos.x;
            proyectil_local.yo = datos.y;
            proyectil_local.x = datos.x;
            proyectil_local.y = datos.y;
            proyectil_local.vo = datos.vo;
            proyectil_local.ang = datos.ang;
            proyectil_local.tiempo = 0.0;
            proyectil_local.activo = TRUE;

            SetTimer((HWND)param, 1, 5, NULL);  // Animar proyectil recibido
        }
    }

    closesocket(serverSocket);
    WSACleanup();
    return 0;
}
////////////////////////////////////////////////////////////////////////////////////

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_TIROPA, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    if (!InitInstance(hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_TIROPA));

    MSG msg;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MessageBox(NULL, L"Error inicializando Winsock", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MessageBox(NULL, L"Error inicializando Winsock", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    WSACleanup();
    return (int)msg.wParam;
}


ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_TIROPA));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_TIROPA);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;

    HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, 1080, 720, nullptr, nullptr, hInstance, nullptr);

    if (!hWnd)
    {
        return FALSE;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    hWndGlobal = hWnd;

    return TRUE;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_USER + 1:
        SetTimer(hWnd, 1, 5, NULL);  // Relanza el timer para el proyectil remoto
        break;
    case WM_TIMER:

        if (proyectil_local.activo) {
            // Verificar si toca el suelo o sale por la derecha ANTES de avanzar
            if (proyectil_local.y >= 720 || proyectil_local.x > 1080) {
                proyectil_local.activo = FALSE;
                KillTimer(hWnd, 1);
                enviado = FALSE;  // para permitir nuevos envíos
                EnableWindow(hBtnDisparar, TRUE); // reactiva el botón
                InvalidateRect(hWnd, NULL, TRUE); // redibuja sin proyectil
                break; // salir inmediatamente para no dibujar fuera de pantalla
            }

            // Avanza posición del proyectil
            proyectil_local.tiempo += DELTA_T;
            proyectil_local.x = proyectil_local.xo + proyectil_local.vo * cos(proyectil_local.ang) * proyectil_local.tiempo;
            proyectil_local.y = proyectil_local.yo - (proyectil_local.vo * sin(proyectil_local.ang) * proyectil_local.tiempo - 0.5 * G * proyectil_local.tiempo * proyectil_local.tiempo);

            // Enviar si sale por la derecha
            if (!enviado && proyectil_local.x > 1080) {
                Datos d;
                d.x = proyectil_local.x;
                d.y = proyectil_local.y;
                d.xo = proyectil_local.xo;
                d.yo = proyectil_local.yo;
                d.vo = proyectil_local.vo;
                d.ang = proyectil_local.ang;
                d.to = proyectil_local.tiempo;
                strcpy_s(d.NN, "jugador1");

                TCHAR bufferIP[32];
                GetWindowText(hEditIP, bufferIP, 32);
                char ipDestino[32];
                size_t converted = 0;
                wcstombs_s(&converted, ipDestino, sizeof(ipDestino), bufferIP, _TRUNCATE);

                EnviarDatos(ipDestino, d);
                enviado = TRUE;
            }

            InvalidateRect(hWnd, NULL, TRUE);
        }
        break;
    case WM_CREATE:
        hEditVelocidad = CreateWindowW(L"EDIT", L"Velocidad", WS_CHILD | WS_VISIBLE | WS_BORDER,
            20, 20, 100, 25, hWnd, NULL, hInst, NULL);

        hEditAngulo = CreateWindowW(L"EDIT", L"Angulo", WS_CHILD | WS_VISIBLE | WS_BORDER,
            140, 20, 100, 25, hWnd, NULL, hInst, NULL);

        hEditIP = CreateWindowW(L"EDIT", L"127.0.0.1", WS_CHILD | WS_VISIBLE | WS_BORDER,
            260, 20, 120, 25, hWnd, NULL, hInst, NULL);

        hBtnDisparar = CreateWindowW(L"BUTTON", L"Disparar", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
            400, 20, 100, 30, hWnd, (HMENU)1, hInst, NULL);

        hBtnAcercaDe = CreateWindowW(L"BUTTON", L"Acerca de", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            510, 20, 100, 30, hWnd, (HMENU)2, hInst, NULL);

        hBtnSalir = CreateWindowW(L"BUTTON", L"Salir", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            620, 20, 100, 30, hWnd, (HMENU)3, hInst, NULL);


		// Cargar imágenes
        hBmpPersonaje = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_PERSONAJE));
        hBmpComer = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_COMER));

        if (!hBmpPersonaje || !hBmpComer) {
            MessageBox(hWnd, L"Falló la carga de imágenes", L"Error", MB_OK | MB_ICONERROR);
        }

        // Inicializar las barras
        InicializarBarrasAleatorias(hWnd);

		// Posicionar el personaje sobre una barra aleatoria
        PosicionarPersonajeSobreBarraAleatoria(hWnd);

		// Inicializar el proyectil
        hBmpBola = hBmpComer; // usar la misma imagen para el proyectil

        InvalidateRect(hWnd, NULL, TRUE); // Fuerza el repintado de toda la ventana

        CreateThread(NULL, 0, ServidorThread, hWnd, 0, NULL);
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case 1: {
            TCHAR bufferVel[16], bufferAng[16], bufferIP[32];
            GetWindowText(hEditVelocidad, bufferVel, 16);
            GetWindowText(hEditAngulo, bufferAng, 16);
            GetWindowText(hEditIP, bufferIP, 32);

            if (wcslen(bufferVel) == 0 || wcslen(bufferAng) == 0 || wcslen(bufferIP) == 0) {
                MessageBox(hWnd, L"Debe rellenar todas las casillas.", L"Campos vacíos", MB_OK | MB_ICONWARNING);
                break;
            }

            double vel = _wtof(bufferVel);
            double angDeg = _wtof(bufferAng);

            // Validación de ángulo
            if (angDeg <= 0 || angDeg >= 90) {
                MessageBox(hWnd, L"El ángulo debe estar entre 1 y 89 grados.", L"Ángulo inválido", MB_OK | MB_ICONERROR);
                break;
            }

            // Validación básica de IP (muy simple: verifica que tenga puntos)
            if (wcsstr(bufferIP, L".") == NULL) {
                MessageBox(hWnd, L"Dirección IP inválida.", L"IP incorrecta", MB_OK | MB_ICONERROR);
                break;
            }

            double angRad = angDeg * 3.1416 / 180.0;

            BITMAP bmpPersonaje;
            GetObject(hBmpPersonaje, sizeof(BITMAP), &bmpPersonaje);
            int anchoRealPersonaje = bmpPersonaje.bmWidth;
            int altoRealPersonaje = bmpPersonaje.bmHeight;

            proyectil_local.vo = vel;
            proyectil_local.ang = angRad;
            proyectil_local.tiempo = 0.0;

            proyectil_local.xo = personaje_x + anchoRealPersonaje;
            proyectil_local.yo = personaje_y + altoRealPersonaje / 2;
            proyectil_local.x = proyectil_local.xo;
            proyectil_local.y = proyectil_local.yo;
            proyectil_local.activo = TRUE;

            EnableWindow(hBtnDisparar, FALSE); // Desactiva el botón
            SetTimer(hWnd, 1, 5, NULL);
            break;
        }
        case 2:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            break;
        case 3:
            DestroyWindow(hWnd);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        // Dibujar las barras visibles
        HBRUSH hBarraBrush = CreateSolidBrush(RGB(0, 77, 255)); // azul
        HPEN hPen = CreatePen(PS_SOLID, 2, RGB(0, 0, 0));        // borde negro
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);

        for (int i = 0; i < NUM_BARRAS; i++) {
            if (!barras[i].visible) continue;

            RECT rect = {
                barras[i].x,
                barras[i].y,
                barras[i].x + barras[i].ancho,
                barras[i].y + barras[i].alto
            };

            // Relleno
            FillRect(hdc, &rect, hBarraBrush);

            // Contorno
            MoveToEx(hdc, rect.left, rect.top, NULL);
            LineTo(hdc, rect.right, rect.top);
            LineTo(hdc, rect.right, rect.bottom);
            LineTo(hdc, rect.left, rect.bottom);
            LineTo(hdc, rect.left, rect.top);
        }

        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);
        DeleteObject(hBarraBrush);

        //DibujarBitmapEscalado(hdc, hBmpPersonaje, 100, 100, 50, 40);
        //DibujarBitmapEscalado(hdc, hBmpComer, 200, 150, 30, 30);
        
        
        // Dibujar personaje (tamaño real o escalado)
        if (hBmpPersonaje) {
            DibujarBitmap(hdc, hBmpPersonaje, personaje_x, personaje_y); // si quieres tamaño real
            // o usa DibujarBitmapEscalado(...) si estás escalando
        }

		// Dibujar proyectil
        if (proyectil_local.activo && hBmpBola != NULL &&
            proyectil_local.x >= -TAM_BOLA && proyectil_local.x <= 1080 + TAM_BOLA &&
            proyectil_local.y >= -TAM_BOLA && proyectil_local.y <= 720 + TAM_BOLA) {

            DibujarBitmap(hdc, hBmpBola,
                (int)(proyectil_local.x - TAM_BOLA / 2),
                (int)(proyectil_local.y - TAM_BOLA / 2));
        }

        EndPaint(hWnd, &ps);
    }
    break;

    case WM_DESTROY:
        serverActivo = FALSE;
        closesocket(serverSocket);  // fuerza salida de accept()
        PostQuitMessage(0);
        if (hBmpPersonaje) DeleteObject(hBmpPersonaje);
        if (hBmpComer) DeleteObject(hBmpComer);
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



