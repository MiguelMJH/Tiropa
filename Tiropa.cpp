// Tiropa.cpp : Juego de tiro parabólico multijugador con soporte para múltiples proyectiles simultáneos
#include "framework.h"
#include "Tiropa.h"
#include <windows.h>
#include "resource.h"
#include <time.h>
#include <math.h>
#include <commctrl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Mswsock.lib")
#pragma comment(lib, "AdvApi32.lib")
#pragma comment(lib, "Msimg32.lib")

// Constantes físicas y de sistema
#define MAX_CADENA 100
#define ID_TEMPORIZADOR 1
#define PI 3.14159265
#define FUERZA_GRAVEDAD 9.8
#define DELTA_TIEMPO 0.3
#define PUERTO_RED "4200"
#define SLEEP_TIME 20  // Tiempo de refresco en milisegundos
#define MAX_PROYECTILES_SIMULTANEOS 50  // Máximo de proyectiles remotos simultáneos
#define MAX_CONEXIONES_SIMULTANEAS 20   // Máximo de conexiones TCP simultáneas

// Estructura para coordenadas de plataformas del juego
struct CoordenadasPlataforma {
    int x1, x2, y1, y2;
};

// Estructura para posición del personaje jugador
struct PosicionPersonaje {
    int x, y;
};

// Estructura de datos para transmisión de red entre clientes
struct DatosTransmision {
    double x, y, velocidadInicialX, velocidadInicialY;
    double velocidadTotal, anguloRadianes;
    double tiempoTranscurrido;
    char nombreJugador[32] = "jugador1";
    DWORD timestampCreacion;  // Timestamp para identificar proyectiles únicos
};
typedef struct DatosTransmision DATOS_RED;

// Estructura para cálculos físicos internos del proyectil
struct CalculosFisicos {
    double velocidadX = 0.0, velocidadY = 0.0;
    double velocidadTotal = 0.0;
    double anguloRadianes = 0.0;
    double tiempo = 0.0;
    double posicionX, posicionY;
};
typedef struct CalculosFisicos FISICA_INTERNA;

// Estructura principal del proyectil con lista enlazada mejorada
typedef struct ProyectilJuego {
    double posicionInicialX, posicionInicialY;
    double velocidadMovimientoX, velocidadMovimientoY;
    double tiempoVida;
    double velocidadLanzamiento;
    double anguloDisparo;
    char nombrePropietario[32] = "jugador1";
    SOCKET socketConexion;
    BOOL estaVisible;
    BOOL esLocal;  // Distinguir entre proyectiles locales y remotos
    DWORD idUnico;  // ID único para evitar duplicados
    DWORD timestampCreacion;  // Timestamp de creación
    struct ProyectilJuego* siguiente;
    struct ProyectilJuego* anterior;
} ProyectilJuego;

// Variables globales de la aplicación
HINSTANCE instanciaApp;
WCHAR tituloVentana[MAX_CADENA];
WCHAR claseVentana[MAX_CADENA];

// Variables de proyectiles y sincronización mejoradas
ProyectilJuego* proyectilLocal = NULL;
ProyectilJuego* listaProyectilesRemotos = NULL;
HANDLE mutexSincronizacion;
HANDLE mutexContadores;  // Mutex adicional para contadores
BOOL temporizadorActivo = FALSE;
BOOL disparoTransmitido = FALSE;

// Contadores para gestión de recursos
int contadorProyectilesRemotos = 0;
int contadorConexionesActivas = 0;
DWORD contadorIDsUnicos = 1;

// Controles de la interfaz de usuario
HWND campoNombre, campoAngulo, campoVelocidad, campoDireccionIP, botonDisparar;

// Recursos gráficos (bitmaps)
HBITMAP imagenProyectil, imagenPersonaje;
BITMAP informacionBitmap;

// Estado actual del juego
CoordenadasPlataforma coordenadasPlataformas[3];
bool plataformaActiva[3] = { true, true, true };
PosicionPersonaje posicionJugador;
FISICA_INTERNA calculosLocales;
DATOS_RED datosEnvio, datosRecepcion;

// Variables de posicionamiento y dimensiones
int posicionInicialX, posicionInicialY;
int anchoVentana, altoVentana, anchoProyectil, altoProyectil;
HWND ventanaPrincipal;
int indicePlataformaJugador = -1;

// Variables de comunicación de red
SOCKET socketServidor = INVALID_SOCKET;
BOOL servidorEnFuncionamiento = TRUE;
BOOL aplicacionTerminando = FALSE;
char nombreJugadorActual[32] = "jugador1";

// Función para generar ID único para proyectiles
DWORD GenerarIDUnico() {
    WaitForSingleObject(mutexContadores, INFINITE);
    DWORD id = contadorIDsUnicos++;
    ReleaseMutex(mutexContadores);
    return id;
}

// Función para verificar si un proyectil ya existe (evitar duplicados)
BOOL ProyectilYaExiste(DWORD idUnico, DWORD timestamp) {
    ProyectilJuego* actual = listaProyectilesRemotos;
    while (actual != NULL) {
        if (actual->idUnico == idUnico && actual->timestampCreacion == timestamp) {
            return TRUE;
        }
        actual = actual->siguiente;
    }
    return FALSE;
}

// Función para limpiar proyectiles remotos inactivos
void LimpiarProyectilesInactivos() {
    WaitForSingleObject(mutexSincronizacion, INFINITE);

    ProyectilJuego* actual = listaProyectilesRemotos;
    while (actual != NULL) {
        ProyectilJuego* siguiente = actual->siguiente;

        if (!actual->estaVisible) {
            // Eliminar de la lista
            if (actual->anterior) {
                actual->anterior->siguiente = actual->siguiente;
            }
            else {
                listaProyectilesRemotos = actual->siguiente;
            }

            if (actual->siguiente) {
                actual->siguiente->anterior = actual->anterior;
            }

            // Cerrar socket si está abierto
            if (actual->socketConexion != INVALID_SOCKET) {
                shutdown(actual->socketConexion, SD_BOTH);
                closesocket(actual->socketConexion);
            }

            free(actual);
            contadorProyectilesRemotos--;
        }

        actual = siguiente;
    }

    ReleaseMutex(mutexSincronizacion);
}

// Función para terminar la aplicación de forma segura
void TerminarJuegoCompleto() {
    aplicacionTerminando = TRUE;
    servidorEnFuncionamiento = FALSE;

    if (ventanaPrincipal) KillTimer(ventanaPrincipal, ID_TEMPORIZADOR);

    // Limpiar todos los proyectiles remotos
    LimpiarProyectilesInactivos();

    if (socketServidor != INVALID_SOCKET) {
        shutdown(socketServidor, SD_BOTH);
        closesocket(socketServidor);
        socketServidor = INVALID_SOCKET;
    }

    if (mutexSincronizacion) {
        CloseHandle(mutexSincronizacion);
        mutexSincronizacion = NULL;
    }

    if (mutexContadores) {
        CloseHandle(mutexContadores);
        mutexContadores = NULL;
    }

    WSACleanup();
    TerminateProcess(GetCurrentProcess(), 0);
}

// Prototipos de funciones principales
ATOM RegistrarClaseVentana(HINSTANCE instancia);
BOOL InicializarInstancia(HINSTANCE, int);
LRESULT CALLBACK ProcedimientoVentana(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK AcercaDe(HWND, UINT, WPARAM, LPARAM);

DWORD WINAPI HiloServidorTCP(LPVOID);
DWORD WINAPI ProcesarConexionCliente(LPVOID);
DWORD WINAPI EscucharRespuestasServidor(LPVOID);
int TransmitirDatosRed(HWND, char*);

int GenerarNumeroAleatorio();
int GenerarPosicionBase();
int SeleccionarPlataformaAleatoria();
void EjecutarDisparo(HWND, HWND, HWND);
void DibujarPersonajeJuego(HDC);
void RenderizarProyectilLocal(HWND, HDC, HWND);
void RenderizarProyectilesRemotos(HWND, HDC);
BOOL ValidarEntradaNumerica(const wchar_t*);

// Obtener nombre del jugador desde el campo de texto
void ObtenerNombreJugadorActual() {
    TCHAR bufferTexto[32];
    GetWindowText(campoNombre, bufferTexto, 32);
    WideCharToMultiByte(CP_UTF8, 0, bufferTexto, -1, nombreJugadorActual, 32, NULL, NULL);

    if (strlen(nombreJugadorActual) == 0) {
        strcpy_s(nombreJugadorActual, sizeof(nombreJugadorActual), "jugador1");
    }
}

// Generador de números aleatorios para posicionamiento de plataformas
int GenerarNumeroAleatorio() {
    return 300 + rand() % (500 - 300 + 1);
}

// Generador de posición base para plataformas
int GenerarPosicionBase() {
    return 0 + rand() % (240 - 0 + 1);
}

// Selector aleatorio de plataforma para el jugador
int SeleccionarPlataformaAleatoria() {
    return (rand() % 3) + 1;
}

// Validador de entrada numérica en campos de texto
BOOL ValidarEntradaNumerica(const wchar_t* textoEntrada) {
    BOOL tienePuntoDecimal = FALSE;
    BOOL tieneDigitos = FALSE;

    while (iswspace(*textoEntrada)) textoEntrada++;
    if (*textoEntrada == L'+' || *textoEntrada == L'-') textoEntrada++;

    while (*textoEntrada) {
        if (iswdigit(*textoEntrada)) {
            tieneDigitos = TRUE;
        }
        else if (*textoEntrada == L'.') {
            if (tienePuntoDecimal) return FALSE;
            tienePuntoDecimal = TRUE;
        }
        else if (iswspace(*textoEntrada)) {
            break;
        }
        else {
            return FALSE;
        }
        textoEntrada++;
    }
    return tieneDigitos;
}

// Cliente TCP para transmisión de datos del proyectil
int TransmitirDatosRed(HWND campoIP, char* nombreUsuario) {
    WSADATA datosWinsock;
    SOCKET socketConexion = INVALID_SOCKET;
    struct addrinfo* resultadoDNS = NULL, * punteroAddr = NULL, configuracionAddr;
    int resultadoOperacion;

    TCHAR textoIP[16];
    char direccionIP[16];
    int longitudTexto = 0;

    GetWindowText(campoIP, textoIP, 16);
    longitudTexto = GetWindowTextLength(campoIP);
    wcstombs(direccionIP, textoIP, longitudTexto);
    direccionIP[longitudTexto] = '\0';

    resultadoOperacion = WSAStartup(MAKEWORD(2, 2), &datosWinsock);
    if (resultadoOperacion != 0) return 1;

    ZeroMemory(&configuracionAddr, sizeof(configuracionAddr));
    configuracionAddr.ai_family = AF_UNSPEC;
    configuracionAddr.ai_socktype = SOCK_STREAM;
    configuracionAddr.ai_protocol = IPPROTO_TCP;

    resultadoOperacion = getaddrinfo(direccionIP, PUERTO_RED, &configuracionAddr, &resultadoDNS);
    if (resultadoOperacion != 0) {
        WSACleanup();
        return 1;
    }

    for (punteroAddr = resultadoDNS; punteroAddr != NULL; punteroAddr = punteroAddr->ai_next) {
        socketConexion = socket(punteroAddr->ai_family, punteroAddr->ai_socktype, punteroAddr->ai_protocol);
        if (socketConexion == INVALID_SOCKET) {
            WSACleanup();
            return 1;
        }

        resultadoOperacion = connect(socketConexion, punteroAddr->ai_addr, (int)punteroAddr->ai_addrlen);
        if (resultadoOperacion == SOCKET_ERROR) {
            closesocket(socketConexion);
            socketConexion = INVALID_SOCKET;
            continue;
        }
        break;
    }

    freeaddrinfo(resultadoDNS);

    if (socketConexion == INVALID_SOCKET) {
        WSACleanup();
        return 1;
    }

    // Agregar timestamp único al proyectil
    datosEnvio.timestampCreacion = GetTickCount();

    resultadoOperacion = send(socketConexion, (char*)&datosEnvio, sizeof(DATOS_RED), 0);
    if (resultadoOperacion == SOCKET_ERROR) {
        closesocket(socketConexion);
        WSACleanup();
        return 1;
    }

    // Crear hilo para recibir respuestas del servidor
    SOCKET* punteroSocket = (SOCKET*)malloc(sizeof(SOCKET));
    if (punteroSocket == NULL) {
        closesocket(socketConexion);
        return 1;
    }
    *punteroSocket = socketConexion;

    HANDLE hiloRespuesta = CreateThread(NULL, 0, EscucharRespuestasServidor, punteroSocket, 0, NULL);
    if (hiloRespuesta == NULL) {
        closesocket(socketConexion);
        free(punteroSocket);
        return 1;
    }

    return 0;
}

// Hilo para recibir respuestas del servidor remoto
DWORD WINAPI EscucharRespuestasServidor(LPVOID parametroSocket) {
    SOCKET socketConexion = *((SOCKET*)parametroSocket);
    int bytesRecibidos;
    char bufferRespuesta[64];

    do {
        bytesRecibidos = recv(socketConexion, bufferRespuesta, (64 * sizeof(char)), 0);
        if (bytesRecibidos > 0) {
            bufferRespuesta[bytesRecibidos - 1] = '\0';

            wchar_t textoUnicode[64];
            mbstowcs(textoUnicode, bufferRespuesta, 64);

            if (strstr(bufferRespuesta, "Impacto con personaje") != NULL) {
                MessageBox(NULL, L"¡Eliminaste al objetivo!", L"¡Victoria!", MB_OK | MB_ICONEXCLAMATION);
            }
            else {
                MessageBox(NULL, textoUnicode, L"Resultado del disparo", MB_OK | MB_ICONINFORMATION);
            }

            EnableWindow(botonDisparar, TRUE);
            disparoTransmitido = FALSE;
        }
        else if (bytesRecibidos == 0) {
            break;
        }
        else {
            break;
        }
    } while (bytesRecibidos > 0);

    shutdown(socketConexion, SD_SEND);
    closesocket(socketConexion);
    free(parametroSocket);
    return 0;
}

// Servidor TCP principal para recibir proyectiles remotos (mejorado para múltiples conexiones)
DWORD WINAPI HiloServidorTCP(LPVOID parametrosDatos) {
    WSADATA datosWinsock;
    int resultadoOperacion;
    SOCKET socketEscucha = INVALID_SOCKET;
    SOCKET socketCliente = INVALID_SOCKET;
    struct addrinfo* resultadoConfig = NULL;
    struct addrinfo configuracionServidor;

    resultadoOperacion = WSAStartup(MAKEWORD(2, 2), &datosWinsock);
    if (resultadoOperacion != 0) return 1;

    ZeroMemory(&configuracionServidor, sizeof(configuracionServidor));
    configuracionServidor.ai_family = AF_INET;
    configuracionServidor.ai_socktype = SOCK_STREAM;
    configuracionServidor.ai_protocol = IPPROTO_TCP;
    configuracionServidor.ai_flags = AI_PASSIVE;

    resultadoOperacion = getaddrinfo(NULL, PUERTO_RED, &configuracionServidor, &resultadoConfig);
    if (resultadoOperacion != 0) {
        WSACleanup();
        return 1;
    }

    socketEscucha = socket(resultadoConfig->ai_family, resultadoConfig->ai_socktype, resultadoConfig->ai_protocol);
    if (socketEscucha == INVALID_SOCKET) {
        freeaddrinfo(resultadoConfig);
        WSACleanup();
        return 1;
    }

    // Configurar socket para reutilización de dirección
    int opcionReutilizar = 1;
    setsockopt(socketEscucha, SOL_SOCKET, SO_REUSEADDR, (char*)&opcionReutilizar, sizeof(opcionReutilizar));

    resultadoOperacion = bind(socketEscucha, resultadoConfig->ai_addr, (int)resultadoConfig->ai_addrlen);
    if (resultadoOperacion == SOCKET_ERROR) {
        freeaddrinfo(resultadoConfig);
        closesocket(socketEscucha);
        WSACleanup();
        return 1;
    }

    freeaddrinfo(resultadoConfig);

    // Aumentar la cola de conexiones pendientes
    resultadoOperacion = listen(socketEscucha, MAX_CONEXIONES_SIMULTANEAS);
    if (resultadoOperacion == SOCKET_ERROR) {
        closesocket(socketEscucha);
        WSACleanup();
        return 1;
    }

    while (TRUE && !aplicacionTerminando) {
        socketCliente = accept(socketEscucha, NULL, NULL);
        if (socketCliente == INVALID_SOCKET) {
            if (!aplicacionTerminando) {
                continue;  // Continuar en lugar de terminar por un error
            }
            break;
        }

        // Verificar límite de conexiones simultáneas
        WaitForSingleObject(mutexContadores, INFINITE);
        if (contadorConexionesActivas >= MAX_CONEXIONES_SIMULTANEAS) {
            ReleaseMutex(mutexContadores);
            closesocket(socketCliente);
            continue;
        }
        contadorConexionesActivas++;
        ReleaseMutex(mutexContadores);

        SOCKET* punteroCliente = (SOCKET*)malloc(sizeof(SOCKET));
        if (punteroCliente == NULL) {
            closesocket(socketCliente);
            WaitForSingleObject(mutexContadores, INFINITE);
            contadorConexionesActivas--;
            ReleaseMutex(mutexContadores);
            continue;
        }
        *punteroCliente = socketCliente;

        HANDLE hiloCliente = CreateThread(NULL, 0, ProcesarConexionCliente, punteroCliente, 0, NULL);
        if (hiloCliente == NULL) {
            closesocket(socketCliente);
            free(punteroCliente);
            WaitForSingleObject(mutexContadores, INFINITE);
            contadorConexionesActivas--;
            ReleaseMutex(mutexContadores);
            continue;
        }

        CloseHandle(hiloCliente);
    }

    closesocket(socketCliente);
    closesocket(socketEscucha);
    WSACleanup();
    return 1;
}

// Procesamiento de conexiones de clientes remotos (mejorado para múltiples proyectiles)
DWORD WINAPI ProcesarConexionCliente(LPVOID parametroSocket) {
    SOCKET socketCliente = *((SOCKET*)parametroSocket);
    int bytesRecibidos;
    DATOS_RED datosRecibidosLocal;

    do {
        bytesRecibidos = recv(socketCliente, (char*)&datosRecibidosLocal, sizeof(DATOS_RED), 0);

        if (bytesRecibidos > 0) {
            // Verificar límite de proyectiles simultáneos
            WaitForSingleObject(mutexContadores, INFINITE);
            if (contadorProyectilesRemotos >= MAX_PROYECTILES_SIMULTANEOS) {
                ReleaseMutex(mutexContadores);
                char mensajeLimite[] = "Servidor saturado - demasiados proyectiles";
                send(socketCliente, mensajeLimite, strlen(mensajeLimite) + 1, 0);
                break;
            }
            ReleaseMutex(mutexContadores);

            WaitForSingleObject(mutexSincronizacion, INFINITE);

            // Verificar si el proyectil ya existe (evitar duplicados)
            DWORD idUnico = GenerarIDUnico();
            if (!ProyectilYaExiste(idUnico, datosRecibidosLocal.timestampCreacion)) {
                // Crear nuevo proyectil remoto con datos recibidos
                ProyectilJuego* nuevoProyectil = (ProyectilJuego*)malloc(sizeof(ProyectilJuego));
                if (nuevoProyectil != NULL) {
                    nuevoProyectil->posicionInicialX = datosRecibidosLocal.x + ((datosRecibidosLocal.velocidadTotal * cos(datosRecibidosLocal.anguloRadianes)) * datosRecibidosLocal.tiempoTranscurrido);
                    nuevoProyectil->posicionInicialY = datosRecibidosLocal.y + ((datosRecibidosLocal.velocidadTotal * sin(datosRecibidosLocal.anguloRadianes)) * datosRecibidosLocal.tiempoTranscurrido - FUERZA_GRAVEDAD * datosRecibidosLocal.tiempoTranscurrido * datosRecibidosLocal.tiempoTranscurrido * DELTA_TIEMPO);
                    nuevoProyectil->velocidadMovimientoX = -(datosRecibidosLocal.velocidadTotal * cos(datosRecibidosLocal.anguloRadianes));
                    nuevoProyectil->velocidadMovimientoY = (datosRecibidosLocal.velocidadTotal * sin(datosRecibidosLocal.anguloRadianes));
                    nuevoProyectil->tiempoVida = datosRecibidosLocal.tiempoTranscurrido;
                    nuevoProyectil->socketConexion = socketCliente;
                    nuevoProyectil->estaVisible = TRUE;
                    nuevoProyectil->esLocal = FALSE;
                    nuevoProyectil->idUnico = idUnico;
                    nuevoProyectil->timestampCreacion = datosRecibidosLocal.timestampCreacion;
                    nuevoProyectil->siguiente = listaProyectilesRemotos;
                    nuevoProyectil->anterior = NULL;

                    strncpy_s(nuevoProyectil->nombrePropietario, sizeof(nuevoProyectil->nombrePropietario), datosRecibidosLocal.nombreJugador, _TRUNCATE);

                    if (listaProyectilesRemotos) listaProyectilesRemotos->anterior = nuevoProyectil;
                    listaProyectilesRemotos = nuevoProyectil;

                    contadorProyectilesRemotos++;

                    if (!temporizadorActivo) {
                        SetTimer(ventanaPrincipal, ID_TEMPORIZADOR, SLEEP_TIME, NULL);
                        temporizadorActivo = TRUE;
                    }
                }
            }

            ReleaseMutex(mutexSincronizacion);
        }
        else if (bytesRecibidos == 0) {
            break;
        }
        else {
            break;
        }

    } while (bytesRecibidos > 0);

    shutdown(socketCliente, SD_SEND);
    closesocket(socketCliente);
    free(parametroSocket);

    // Decrementar contador de conexiones activas
    WaitForSingleObject(mutexContadores, INFINITE);
    contadorConexionesActivas--;
    ReleaseMutex(mutexContadores);

    return 0;
}

// Función principal de disparo con validación de datos
void EjecutarDisparo(HWND ventana, HWND campoAng, HWND campoVel) {
    wchar_t bufferEntrada[256];

    GetWindowText(campoVel, bufferEntrada, sizeof(bufferEntrada) / sizeof(wchar_t));
    if (!ValidarEntradaNumerica(bufferEntrada)) {
        MessageBox(ventana, L"Ingresa un valor numérico válido para la velocidad.", L"Entrada inválida", MB_ICONERROR);
        return;
    }
    calculosLocales.velocidadTotal = _wtof(bufferEntrada);

    GetWindowText(campoAng, bufferEntrada, sizeof(bufferEntrada) / sizeof(wchar_t));
    if (!ValidarEntradaNumerica(bufferEntrada)) {
        MessageBox(ventana, L"Ingresa un valor numérico válido para el ángulo.", L"Entrada inválida", MB_ICONERROR);
        return;
    }
    calculosLocales.anguloRadianes = _wtof(bufferEntrada);

    ObtenerNombreJugadorActual();

    // Convertir ángulo a radianes y calcular componentes de velocidad
    calculosLocales.anguloRadianes = calculosLocales.anguloRadianes * PI / 180.0;
    calculosLocales.velocidadX = calculosLocales.velocidadTotal * cos(calculosLocales.anguloRadianes);
    calculosLocales.velocidadY = calculosLocales.velocidadTotal * sin(calculosLocales.anguloRadianes);

    WaitForSingleObject(mutexSincronizacion, INFINITE);

    // Crear nuevo proyectil local
    ProyectilJuego* nuevoDisparo = (ProyectilJuego*)malloc(sizeof(ProyectilJuego));
    nuevoDisparo->posicionInicialX = 0.0;
    nuevoDisparo->posicionInicialY = 0.0;
    nuevoDisparo->velocidadMovimientoX = calculosLocales.velocidadX;
    nuevoDisparo->velocidadMovimientoY = calculosLocales.velocidadY;
    nuevoDisparo->velocidadLanzamiento = calculosLocales.velocidadTotal;
    nuevoDisparo->tiempoVida = 0.0;
    nuevoDisparo->anguloDisparo = calculosLocales.anguloRadianes;
    nuevoDisparo->estaVisible = TRUE;
    nuevoDisparo->esLocal = TRUE;
    nuevoDisparo->idUnico = GenerarIDUnico();
    nuevoDisparo->timestampCreacion = GetTickCount();
    nuevoDisparo->siguiente = proyectilLocal;
    nuevoDisparo->anterior = NULL;

    strncpy_s(nuevoDisparo->nombrePropietario, sizeof(nuevoDisparo->nombrePropietario), nombreJugadorActual, _TRUNCATE);

    if (proyectilLocal) proyectilLocal->anterior = nuevoDisparo;
    proyectilLocal = nuevoDisparo;

    disparoTransmitido = FALSE;

    ReleaseMutex(mutexSincronizacion);

    EnableWindow(botonDisparar, FALSE);
    SetTimer(ventanaPrincipal, ID_TEMPORIZADOR, SLEEP_TIME, NULL);
    temporizadorActivo = TRUE;
}

// Renderizado del proyectil local con física parabólica
void RenderizarProyectilLocal(HWND ventana, HDC contextoGrafico, HWND botonControl) {
    if (proyectilLocal == NULL || !proyectilLocal->estaVisible) return;

    // Calcular posición usando ecuaciones de movimiento parabólico
    proyectilLocal->posicionInicialX = proyectilLocal->velocidadMovimientoX * proyectilLocal->tiempoVida;
    proyectilLocal->posicionInicialY = proyectilLocal->velocidadMovimientoY * proyectilLocal->tiempoVida - DELTA_TIEMPO * FUERZA_GRAVEDAD * proyectilLocal->tiempoVida * proyectilLocal->tiempoVida;
    proyectilLocal->tiempoVida += DELTA_TIEMPO;

    int coordenadaX = posicionInicialX + (int)proyectilLocal->posicionInicialX;
    int coordenadaY = posicionInicialY - (int)proyectilLocal->posicionInicialY;

    // Renderizar proyectil si está visible
    if (proyectilLocal->estaVisible) {
        HDC contextoMemoria = CreateCompatibleDC(contextoGrafico);
        HBITMAP bitmapAnterior = (HBITMAP)SelectObject(contextoMemoria, imagenProyectil);
        GetObject(imagenProyectil, sizeof(informacionBitmap), &informacionBitmap);

        anchoProyectil = 30;
        altoProyectil = 30;

        SetStretchBltMode(contextoGrafico, HALFTONE);
        StretchBlt(contextoGrafico, coordenadaX, coordenadaY, anchoProyectil, altoProyectil,
            contextoMemoria, 0, 0, informacionBitmap.bmWidth, informacionBitmap.bmHeight, SRCCOPY);

        SelectObject(contextoMemoria, bitmapAnterior);
        DeleteDC(contextoMemoria);
    }

    // Verificar límites de pantalla
    if (coordenadaX < 0 || coordenadaY > 720 - altoProyectil) {
        if (proyectilLocal == NULL && listaProyectilesRemotos == NULL) {
            KillTimer(ventana, ID_TEMPORIZADOR);
            temporizadorActivo = FALSE;
        }

        EnableWindow(botonControl, TRUE);

        ProyectilJuego* proyectilEliminar = proyectilLocal;
        proyectilLocal = proyectilLocal->siguiente;
        if (proyectilEliminar->anterior) proyectilEliminar->anterior->siguiente = proyectilEliminar->siguiente;
        if (proyectilEliminar->siguiente) proyectilEliminar->siguiente->anterior = proyectilEliminar->anterior;
        if (proyectilEliminar == proyectilLocal) proyectilLocal = proyectilEliminar->siguiente;
        free(proyectilEliminar);
    }
    else if (coordenadaX > 1080 && !disparoTransmitido) {
        if (proyectilLocal == NULL && listaProyectilesRemotos == NULL) {
            KillTimer(ventana, ID_TEMPORIZADOR);
            temporizadorActivo = FALSE;
        }
        disparoTransmitido = TRUE;

        // Preparar datos para transmisión de red
        datosEnvio.x = coordenadaX;
        datosEnvio.y = coordenadaY;
        datosEnvio.tiempoTranscurrido = proyectilLocal->tiempoVida;
        datosEnvio.velocidadTotal = proyectilLocal->velocidadLanzamiento;
        datosEnvio.velocidadInicialX = posicionInicialX;
        datosEnvio.velocidadInicialY = posicionInicialY;
        datosEnvio.anguloRadianes = proyectilLocal->anguloDisparo;
        datosEnvio.timestampCreacion = proyectilLocal->timestampCreacion;

        strncpy_s(datosEnvio.nombreJugador, sizeof(datosEnvio.nombreJugador), nombreJugadorActual, _TRUNCATE);

        TransmitirDatosRed(campoDireccionIP, proyectilLocal->nombrePropietario);

        ProyectilJuego* proyectilEliminar = proyectilLocal;
        proyectilLocal = proyectilLocal->siguiente;
        if (proyectilEliminar->anterior) proyectilEliminar->anterior->siguiente = proyectilEliminar->siguiente;
        if (proyectilEliminar->siguiente) proyectilEliminar->siguiente->anterior = proyectilEliminar->anterior;
        if (proyectilEliminar == proyectilLocal) proyectilLocal = proyectilEliminar->siguiente;
        free(proyectilEliminar);
    }
}

// Renderizado y detección de colisiones de proyectiles remotos (optimizado para múltiples proyectiles)
void RenderizarProyectilesRemotos(HWND ventana, HDC contextoGrafico) {
    HBITMAP bitmapAnterior;
    ProyectilJuego* proyectilActual;
    int proyectilesRenderizados = 0;

    WaitForSingleObject(mutexSincronizacion, INFINITE);

    proyectilActual = listaProyectilesRemotos;

    while (proyectilActual != NULL && proyectilesRenderizados < MAX_PROYECTILES_SIMULTANEOS) {
        if (!proyectilActual->estaVisible) {
            proyectilActual = proyectilActual->siguiente;
            continue;
        }

        // Calcular posición con física parabólica
        float posicionPantallaX = proyectilActual->posicionInicialX + (proyectilActual->velocidadMovimientoX * proyectilActual->tiempoVida);
        float posicionPantallaY = proyectilActual->posicionInicialY - (proyectilActual->velocidadMovimientoY * proyectilActual->tiempoVida - DELTA_TIEMPO * FUERZA_GRAVEDAD * proyectilActual->tiempoVida * proyectilActual->tiempoVida);
        proyectilActual->tiempoVida += DELTA_TIEMPO;

        // Renderizar proyectil remoto
        HDC contextoMemoria = CreateCompatibleDC(contextoGrafico);
        bitmapAnterior = (HBITMAP)SelectObject(contextoMemoria, imagenProyectil);
        GetObject(imagenProyectil, sizeof(informacionBitmap), &informacionBitmap);

        anchoProyectil = 30;
        altoProyectil = 30;

        SetStretchBltMode(contextoGrafico, HALFTONE);
        StretchBlt(contextoGrafico, (int)posicionPantallaX, (int)posicionPantallaY, anchoProyectil, altoProyectil,
            contextoMemoria, 0, 0, informacionBitmap.bmWidth, informacionBitmap.bmHeight, SRCCOPY);

        SelectObject(contextoMemoria, bitmapAnterior);
        DeleteDC(contextoMemoria);

        proyectilesRenderizados++;
        bool hayImpacto = false;

        // Detección precisa de colisión con personaje usando IntersectRect
        RECT rectanguloProyectil = {
            (int)posicionPantallaX, (int)posicionPantallaY,
            (int)posicionPantallaX + 30, (int)posicionPantallaY + 30
        };

        RECT rectanguloPersonaje = {
            posicionJugador.x, posicionJugador.y,
            posicionJugador.x + 40, posicionJugador.y + 50
        };

        RECT areaInterseccion;

        if (IntersectRect(&areaInterseccion, &rectanguloProyectil, &rectanguloPersonaje)) {
            hayImpacto = true;
            proyectilActual->estaVisible = FALSE;

            // Enviar confirmación de impacto al atacante
            char mensajeRespuesta[] = "Impacto con personaje";
            if (proyectilActual->socketConexion != INVALID_SOCKET) {
                send(proyectilActual->socketConexion, mensajeRespuesta, strlen(mensajeRespuesta) + 1, 0);
            }

            // Mostrar mensaje de eliminación
            char mensajeEliminacion[64];
            sprintf_s(mensajeEliminacion, sizeof(mensajeEliminacion), "Fuiste eliminado por %s", proyectilActual->nombrePropietario);

            wchar_t mensajeUnicode[64];
            MultiByteToWideChar(CP_UTF8, 0, mensajeEliminacion, -1, mensajeUnicode, 64);

            MessageBox(ventana, mensajeUnicode, L"¡Eliminado!", MB_OK | MB_ICONEXCLAMATION);

            // Eliminar proyectil de la lista
            ProyectilJuego* proyectilEliminar = proyectilActual;
            proyectilActual = proyectilActual->siguiente;
            if (proyectilEliminar->anterior) proyectilEliminar->anterior->siguiente = proyectilEliminar->siguiente;
            if (proyectilEliminar->siguiente) proyectilEliminar->siguiente->anterior = proyectilEliminar->anterior;
            if (proyectilEliminar == listaProyectilesRemotos) listaProyectilesRemotos = proyectilEliminar->siguiente;

            if (proyectilEliminar->socketConexion != INVALID_SOCKET) {
                closesocket(proyectilEliminar->socketConexion);
            }
            free(proyectilEliminar);
            contadorProyectilesRemotos--;

            ReleaseMutex(mutexSincronizacion);

            TerminarJuegoCompleto();
            return;
        }

        // Verificar colisiones con plataformas
        if (!hayImpacto) {
            for (int i = 0; i < 3; i++) {
                if (!plataformaActiva[i]) continue;

                RECT rectanguloPlataforma = {
                    min(coordenadasPlataformas[i].x1, coordenadasPlataformas[i].x2),
                    min(coordenadasPlataformas[i].y1, coordenadasPlataformas[i].y2),
                    max(coordenadasPlataformas[i].x1, coordenadasPlataformas[i].x2),
                    max(coordenadasPlataformas[i].y1, coordenadasPlataformas[i].y2)
                };

                RECT interseccionPlataforma;

                if (IntersectRect(&interseccionPlataforma, &rectanguloProyectil, &rectanguloPlataforma)) {
                    plataformaActiva[i] = false;
                    hayImpacto = true;
                    proyectilActual->estaVisible = FALSE;

                    if (i == indicePlataformaJugador) {
                        posicionJugador.y = 670;
                        indicePlataformaJugador = -1;
                        posicionInicialY = posicionJugador.y - 50;
                    }

                    InvalidateRect(ventana, NULL, TRUE);

                    char mensajeObstaculo[] = "Impacto con obstaculo";
                    if (proyectilActual->socketConexion != INVALID_SOCKET) {
                        send(proyectilActual->socketConexion, mensajeObstaculo, strlen(mensajeObstaculo) + 1, 0);
                    }
                    break;
                }
            }
        }

        // Eliminar proyectil si hay impacto
        if (hayImpacto) {
            ProyectilJuego* proyectilEliminar = proyectilActual;
            proyectilActual = proyectilActual->siguiente;
            if (proyectilEliminar->anterior) proyectilEliminar->anterior->siguiente = proyectilEliminar->siguiente;
            if (proyectilEliminar->siguiente) proyectilEliminar->siguiente->anterior = proyectilEliminar->anterior;
            if (proyectilEliminar == listaProyectilesRemotos) listaProyectilesRemotos = proyectilEliminar->siguiente;

            if (proyectilEliminar->socketConexion != INVALID_SOCKET) {
                closesocket(proyectilEliminar->socketConexion);
            }
            free(proyectilEliminar);
            contadorProyectilesRemotos--;
            continue;
        }

        // Verificar límites de pantalla
        if (posicionPantallaX < 0 || posicionPantallaY > 720 - altoProyectil) {
            InvalidateRect(ventana, NULL, TRUE);

            char mensajeLimite[] = "Proyectil salio de pantalla";
            if (proyectilActual->socketConexion != INVALID_SOCKET) {
                send(proyectilActual->socketConexion, mensajeLimite, strlen(mensajeLimite) + 1, 0);
            }

            ProyectilJuego* proyectilEliminar = proyectilActual;
            proyectilActual = proyectilActual->siguiente;
            if (proyectilEliminar->anterior) proyectilEliminar->anterior->siguiente = proyectilEliminar->siguiente;
            if (proyectilEliminar->siguiente) proyectilEliminar->siguiente->anterior = proyectilEliminar->anterior;
            if (proyectilEliminar == listaProyectilesRemotos) listaProyectilesRemotos = proyectilEliminar->siguiente;

            if (proyectilEliminar->socketConexion != INVALID_SOCKET) {
                closesocket(proyectilEliminar->socketConexion);
            }
            free(proyectilEliminar);
            contadorProyectilesRemotos--;
            continue;
        }

        proyectilActual = proyectilActual->siguiente;
    }

    // Limpiar proyectiles inactivos periódicamente
    static DWORD ultimaLimpieza = 0;
    DWORD tiempoActual = GetTickCount();
    if (tiempoActual - ultimaLimpieza > 5000) {  // Limpiar cada 5 segundos
        ultimaLimpieza = tiempoActual;
        // La limpieza se hará después de liberar el mutex
    }

    if (proyectilLocal == NULL && listaProyectilesRemotos == NULL) {
        KillTimer(ventana, ID_TEMPORIZADOR);
        temporizadorActivo = FALSE;
    }

    ReleaseMutex(mutexSincronizacion);

    // Limpiar proyectiles inactivos si es necesario
    if (tiempoActual - ultimaLimpieza == 0) {
        LimpiarProyectilesInactivos();
    }
}

// Renderizado del personaje del jugador
void DibujarPersonajeJuego(HDC contextoGrafico) {
    HDC contextoMemoria = CreateCompatibleDC(contextoGrafico);
    HBITMAP bitmapAnterior = (HBITMAP)SelectObject(contextoMemoria, imagenPersonaje);
    GetObject(imagenPersonaje, sizeof(informacionBitmap), &informacionBitmap);

    int anchoPersonaje = 40;
    int altoPersonaje = 50;

    SetStretchBltMode(contextoGrafico, HALFTONE);
    StretchBlt(contextoGrafico, posicionJugador.x, posicionJugador.y, anchoPersonaje, altoPersonaje,
        contextoMemoria, 0, 0, informacionBitmap.bmWidth, informacionBitmap.bmHeight, SRCCOPY);

    SelectObject(contextoMemoria, bitmapAnterior);
    DeleteDC(contextoMemoria);
}

// Punto de entrada principal de la aplicación
int APIENTRY wWinMain(_In_ HINSTANCE instancia, _In_opt_ HINSTANCE instanciaAnterior,
    _In_ LPWSTR lineaComandos, _In_ int modoMostrar) {
    UNREFERENCED_PARAMETER(instanciaAnterior);
    UNREFERENCED_PARAMETER(lineaComandos);

    srand((unsigned int)time(NULL));

    LoadStringW(instancia, IDS_APP_TITLE, tituloVentana, MAX_CADENA);
    LoadStringW(instancia, IDC_TIROPA, claseVentana, MAX_CADENA);
    RegistrarClaseVentana(instancia);

    if (!InicializarInstancia(instancia, modoMostrar)) {
        return FALSE;
    }

    HACCEL tablaAceleradores = LoadAccelerators(instancia, MAKEINTRESOURCE(IDC_TIROPA));
    MSG mensaje;

    while (GetMessage(&mensaje, nullptr, 0, 0)) {
        if (!TranslateAccelerator(mensaje.hwnd, tablaAceleradores, &mensaje)) {
            TranslateMessage(&mensaje);
            DispatchMessage(&mensaje);
        }
    }

    return (int)mensaje.wParam;
}

// Registro de la clase de ventana
ATOM RegistrarClaseVentana(HINSTANCE instancia) {
    WNDCLASSEXW configuracionClase;
    configuracionClase.cbSize = sizeof(WNDCLASSEX);
    configuracionClase.style = CS_HREDRAW | CS_VREDRAW;
    configuracionClase.lpfnWndProc = ProcedimientoVentana;
    configuracionClase.cbClsExtra = 0;
    configuracionClase.cbWndExtra = 0;
    configuracionClase.hInstance = instancia;
    configuracionClase.hIcon = LoadIcon(instancia, MAKEINTRESOURCE(IDI_TIROPA));
    configuracionClase.hCursor = LoadCursor(nullptr, IDC_ARROW);
    configuracionClase.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    configuracionClase.lpszMenuName = MAKEINTRESOURCEW(IDC_TIROPA);
    configuracionClase.lpszClassName = claseVentana;
    configuracionClase.hIconSm = LoadIcon(configuracionClase.hInstance, MAKEINTRESOURCE(IDI_SMALL));
    return RegisterClassExW(&configuracionClase);
}

// Inicialización de la instancia de la aplicación
BOOL InicializarInstancia(HINSTANCE instancia, int modoMostrar) {
    instanciaApp = instancia;

    RECT rectanguloVentana = { 0, 0, 1080, 720 };
    AdjustWindowRect(&rectanguloVentana, WS_OVERLAPPEDWINDOW, TRUE);

    anchoVentana = rectanguloVentana.right - rectanguloVentana.left;
    altoVentana = rectanguloVentana.bottom - rectanguloVentana.top;

    ventanaPrincipal = CreateWindowW(claseVentana, tituloVentana, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, anchoVentana, altoVentana, nullptr, nullptr, instancia, nullptr);

    if (!ventanaPrincipal) {
        return FALSE;
    }

    ShowWindow(ventanaPrincipal, modoMostrar);
    UpdateWindow(ventanaPrincipal);
    return TRUE;
}

// Procedimiento principal de la ventana (manejo de mensajes)
LRESULT CALLBACK ProcedimientoVentana(HWND ventana, UINT mensaje, WPARAM wParam, LPARAM lParam) {
    static HANDLE hiloServidor;
    static DWORD idHiloServidor;

    switch (mensaje) {
    case WM_TIMER:
        if (wParam == ID_TEMPORIZADOR) {
            InvalidateRect(ventana, NULL, FALSE);
        }
        break;

    case WM_CREATE:
    {
        int indice = 0;
        int coordenadaX1 = GenerarPosicionBase();
        int coordenadaX2 = coordenadaX1 + 70;

        // Crear interfaz de usuario en línea horizontal
        int posicionY = 15;
        int inicioX = 10;

        CreateWindowW(L"STATIC", L"Nombre:", WS_CHILD | WS_VISIBLE,
            inicioX, posicionY, 50, 20, ventana, NULL, instanciaApp, NULL);
        campoNombre = CreateWindowW(L"EDIT", L"jugador1", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            inicioX + 55, posicionY - 2, 80, 25, ventana, (HMENU)ID_CAMPO_NOMBRE_JUGADOR, instanciaApp, NULL);

        inicioX += 150;
        CreateWindowW(L"STATIC", L"Vel:", WS_CHILD | WS_VISIBLE,
            inicioX, posicionY, 30, 20, ventana, NULL, instanciaApp, NULL);
        campoVelocidad = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            inicioX + 35, posicionY - 2, 60, 25, ventana, (HMENU)ID_CAMPO_VELOCIDAD, instanciaApp, NULL);

        inicioX += 110;
        CreateWindowW(L"STATIC", L"Ang:", WS_CHILD | WS_VISIBLE,
            inicioX, posicionY, 30, 20, ventana, NULL, instanciaApp, NULL);
        campoAngulo = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            inicioX + 35, posicionY - 2, 60, 25, ventana, (HMENU)ID_CAMPO_ANGULO, instanciaApp, NULL);

        inicioX += 110;
        CreateWindowW(L"STATIC", L"IP:", WS_CHILD | WS_VISIBLE,
            inicioX, posicionY, 20, 20, ventana, NULL, instanciaApp, NULL);
        campoDireccionIP = CreateWindowW(L"EDIT", L"127.0.0.1", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            inicioX + 25, posicionY - 2, 100, 25, ventana, (HMENU)ID_CAMPO_DIRECCION_IP, instanciaApp, NULL);

        inicioX += 140;
        botonDisparar = CreateWindowW(L"BUTTON", L"DISPARAR", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
            inicioX, posicionY - 5, 90, 35, ventana, (HMENU)ID_BOTON_DISPARAR, instanciaApp, NULL);

        // Cargar recursos gráficos
        imagenProyectil = LoadBitmap(GetModuleHandle(NULL), MAKEINTRESOURCE(IDB_COMER));
        imagenPersonaje = LoadBitmap(GetModuleHandle(NULL), MAKEINTRESOURCE(IDB_PERSONAJE));

        if (imagenProyectil == NULL || imagenPersonaje == NULL) {
            MessageBox(ventana, L"Error al cargar las imágenes del juego", L"Error", MB_OK | MB_ICONERROR);
        }

        // Inicializar mutexes de sincronización
        mutexSincronizacion = CreateMutex(NULL, FALSE, NULL);
        mutexContadores = CreateMutex(NULL, FALSE, NULL);
        if (mutexSincronizacion == NULL || mutexContadores == NULL) {
            MessageBox(ventana, L"Error al crear los mutex de sincronización", L"Error", MB_OK | MB_ICONERROR);
        }

        int plataformaSeleccionada = SeleccionarPlataformaAleatoria();

        // Configurar coordenadas de plataformas y posición del jugador
        for (indice = 0; indice < 3; indice++) {
            coordenadasPlataformas[indice].x1 = coordenadaX1;
            coordenadasPlataformas[indice].x2 = coordenadaX2;
            coordenadasPlataformas[indice].y1 = 720;
            coordenadasPlataformas[indice].y2 = GenerarNumeroAleatorio();

            if ((indice + 1) == plataformaSeleccionada) {
                posicionJugador.x = coordenadaX1 + 15;
                posicionJugador.y = coordenadasPlataformas[indice].y2 - 50;
                indicePlataformaJugador = indice;
            }

            coordenadaX1 = coordenadaX1 + 70;
            coordenadaX2 = coordenadaX2 + 70;
        }

        posicionInicialX = posicionJugador.x + 17;
        posicionInicialY = posicionJugador.y - 50;

        // Iniciar servidor TCP en hilo separado
        hiloServidor = CreateThread(NULL, 0, HiloServidorTCP, (LPVOID)&datosRecepcion, 0, &idHiloServidor);
        if (hiloServidor == NULL) {
            MessageBox(ventana, L"Error al crear el hilo del servidor", L"Error", MB_OK | MB_ICONERROR);
        }
    }
    break;

    case WM_COMMAND:
    {
        int identificadorComando = LOWORD(wParam);
        switch (identificadorComando) {
        case ID_BOTON_DISPARAR:
            EjecutarDisparo(ventana, campoAngulo, campoVelocidad);
            break;
        case IDM_ABOUT:
            DialogBox(instanciaApp, MAKEINTRESOURCE(IDD_ABOUTBOX), ventana, AcercaDe);
            break;
        case IDM_EXIT:
            TerminarJuegoCompleto();
            break;
        default:
            return DefWindowProc(ventana, mensaje, wParam, lParam);
        }
    }
    break;

    case WM_PAINT:
    {
        PAINTSTRUCT estructuraPintado;
        HDC contextoDispositivo = BeginPaint(ventana, &estructuraPintado);

        RECT rectanguloCliente;
        GetClientRect(ventana, &rectanguloCliente);
        int anchoAreaCliente = rectanguloCliente.right;
        int altoAreaCliente = rectanguloCliente.bottom;

        // Crear buffer de doble renderizado
        HDC contextoMemoria = CreateCompatibleDC(contextoDispositivo);
        HBITMAP bitmapMemoria = CreateCompatibleBitmap(contextoDispositivo, anchoAreaCliente, altoAreaCliente);
        HBITMAP bitmapAnterior = (HBITMAP)SelectObject(contextoMemoria, bitmapMemoria);

        FillRect(contextoMemoria, &rectanguloCliente, (HBRUSH)(COLOR_WINDOW + 1));

        // Renderizar plataformas con gradiente vertical
        for (int i = 0; i < 3; i++) {
            if (!plataformaActiva[i]) continue;

            RECT rectanguloPlataforma = {
                coordenadasPlataformas[i].x1, coordenadasPlataformas[i].y2,
                coordenadasPlataformas[i].x2, coordenadasPlataformas[i].y1
            };

            TRIVERTEX verticesGradiente[2];
            verticesGradiente[0].x = rectanguloPlataforma.left;
            verticesGradiente[0].y = rectanguloPlataforma.top;
            verticesGradiente[0].Red = 0x4000;
            verticesGradiente[0].Green = 0x8000;
            verticesGradiente[0].Blue = 0xff00;
            verticesGradiente[0].Alpha = 0x0000;

            verticesGradiente[1].x = rectanguloPlataforma.right;
            verticesGradiente[1].y = rectanguloPlataforma.bottom;
            verticesGradiente[1].Red = 0x8000;
            verticesGradiente[1].Green = 0x0000;
            verticesGradiente[1].Blue = 0xff00;
            verticesGradiente[1].Alpha = 0x0000;

            GRADIENT_RECT rectanguloGradiente;
            rectanguloGradiente.UpperLeft = 0;
            rectanguloGradiente.LowerRight = 1;

            GradientFill(contextoMemoria, verticesGradiente, 2, &rectanguloGradiente, 1, GRADIENT_FILL_RECT_V);
        }

        DibujarPersonajeJuego(contextoMemoria);
        RenderizarProyectilLocal(ventana, contextoMemoria, botonDisparar);
        RenderizarProyectilesRemotos(ventana, contextoMemoria);

        // Mostrar información de estado en la esquina superior derecha
        //char infoEstado[100];
        //sprintf_s(infoEstado, sizeof(infoEstado), "Proyectiles: %d | Conexiones: %d", contadorProyectilesRemotos, contadorConexionesActivas);

        //wchar_t infoUnicode[100];
        //MultiByteToWideChar(CP_UTF8, 0, infoEstado, -1, infoUnicode, 100);

        //SetTextColor(contextoMemoria, RGB(255, 0, 0));
        //SetBkMode(contextoMemoria, TRANSPARENT);
        //TextOut(contextoMemoria, anchoAreaCliente - 250, 50, infoUnicode, wcslen(infoUnicode));

        // Copiar buffer a pantalla
        BitBlt(contextoDispositivo, 0, 0, anchoAreaCliente, altoAreaCliente, contextoMemoria, 0, 0, SRCCOPY);

        SelectObject(contextoMemoria, bitmapAnterior);
        DeleteObject(bitmapMemoria);
        DeleteDC(contextoMemoria);

        EndPaint(ventana, &estructuraPintado);
    }
    break;

    case WM_DESTROY:
        TerminarJuegoCompleto();
        break;

    default:
        return DefWindowProc(ventana, mensaje, wParam, lParam);
    }
    return 0;
}

// Diálogo "Acerca de"
INT_PTR CALLBACK AcercaDe(HWND dialogo, UINT mensaje, WPARAM wParam, LPARAM lParam) {
    UNREFERENCED_PARAMETER(lParam);
    switch (mensaje) {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialogo, LOWORD(wParam));
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                EndDialog(dialogo, LOWORD(wParam));
                return (INT_PTR)TRUE;
            }
            break;
        }
        return (INT_PTR)FALSE;
    }
}
