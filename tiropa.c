/**
 * Compilar: gcc -o tiropa tiropa.c `pkg-config --cflags --libs gtk+-3.0` -lm -lpthread -export-dynamic
 * VERSIÓN IDÉNTICA AL PROYECTO WINDOWS
 */
#include <gtk/gtk.h>
#include <cairo.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>

// Constantes físicas idénticas a Windows
#define FUERZA_GRAVEDAD 9.8
#define DELTA_TIEMPO 0.5
#define PUERTO 4200
#define ANCHO_PERSONAJE 40
#define ALTO_PERSONAJE  50
#define TAM_BOLA        30
#define MAX_ALIAS       32
#define SLEEP_TIME      20000

typedef struct {
    GtkWidget *widget;
    gboolean sensitive;
} SensibleData;

gboolean set_widget_sensitive(gpointer data) {
    SensibleData *info = (SensibleData *)data;
    gtk_widget_set_sensitive(info->widget, info->sensitive);
    g_free(info);
    return FALSE;
}

// Estructura de datos de red idéntica a Windows
typedef struct {
    double x, y, velocidadInicialX, velocidadInicialY;
    double velocidadTotal, anguloRadianes;
    double tiempoTranscurrido;
    char nombreJugador[32];
} DATOS_RED;

// Estructura de proyectil compatible con Windows
typedef struct {
    GMutex mutex;
    double posicionInicialX, posicionInicialY;
    double velocidadMovimientoX, velocidadMovimientoY;
    double tiempoVida;
    double velocidadLanzamiento;
    double anguloDisparo;
    char nombrePropietario[MAX_ALIAS];
    int socketConexion;
    int estaVisible;
} ProyectilJuego;

typedef struct {
    int x1, x2, y1, y2;
    int visible;
} CoordenadasPlataforma;

typedef struct {
    int x, y;
} PosicionPersonaje;

// Variables globales

// Sistema de cola de mensajes para evitar múltiples diálogos
typedef struct {
    char mensaje[128];
    int tipo; // 0=info, 1=victoria
} MensajeRespuesta;

GQueue *cola_mensajes = NULL;
GMutex mutex_mensajes;
gboolean procesando_mensaje = FALSE;
int max_hilos_tcp = 3;  // Limitar hilos TCP simultáneos
int hilos_tcp_activos = 0;
GMutex mutex_hilos_tcp;

GtkWidget *window, *draw1;
GtkWidget *entryVel, *entryAng, *entryIP, *entryNombre;
GtkWidget *btnDisparar, *btnSalir, *btnAcercaDe;
cairo_surface_t *img_personaje, *img_bola;
GtkAllocation allocation;

char alias[MAX_ALIAS];
int personaje_destruido = 0;
int tiro_en_progreso = 0;
int servidor_activo = 1;
int disparo_transmitido = 0;

CoordenadasPlataforma coordenadas_plataformas[3];
PosicionPersonaje posicion_jugador;
int posicion_inicial_x, posicion_inicial_y;
int indice_plataforma_jugador = -1;

ProyectilJuego *proyectil_local = NULL;
GList *lista_proyectiles_remotos = NULL;
GMutex mutex_proyectiles;

DATOS_RED datos_envio, datos_recepcion;

// Control de recursos y limpieza
pthread_t *hilos_activos = NULL;
int num_hilos_activos = 0;
GMutex mutex_hilos;
int aplicacion_cerrando = 0;

/**
 * Detección de colisión precisa usando IntersectRect equivalente
 */
int detectar_colision_rectangulos(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2) {
    int izq1 = x1, der1 = x1 + w1, arr1 = y1, aba1 = y1 + h1;
    int izq2 = x2, der2 = x2 + w2, arr2 = y2, aba2 = y2 + h2;
    
    return (izq1 < der2) && (der1 > izq2) && (arr1 < aba2) && (aba1 > arr2);
}

/**
 * Obtener nombre del jugador desde interfaz
 */
void obtener_nombre_jugador_actual() {
    const char *texto = gtk_entry_get_text(GTK_ENTRY(entryNombre));
    if (strlen(texto) > 0) {
        strncpy(alias, texto, MAX_ALIAS - 1);
        alias[MAX_ALIAS - 1] = '\0';
    } else {
        strcpy(alias, "jugador1");
    }
}

// Función para procesar mensajes de forma serializada
gboolean procesar_mensaje_cola(gpointer data) {
    if (aplicacion_cerrando) return FALSE;
    
    g_mutex_lock(&mutex_mensajes);
    
    if (g_queue_is_empty(cola_mensajes)) {
        procesando_mensaje = FALSE;
        g_mutex_unlock(&mutex_mensajes);
        return FALSE;
    }
    
    MensajeRespuesta *msg = g_queue_pop_head(cola_mensajes);
    g_mutex_unlock(&mutex_mensajes);
    
    if (msg && !aplicacion_cerrando) {
        GtkWidget *dialog;
        if (msg->tipo == 1) {
            dialog = gtk_message_dialog_new(
                GTK_WINDOW(window),
                GTK_DIALOG_DESTROY_WITH_PARENT,
                GTK_MESSAGE_INFO,
                GTK_BUTTONS_OK,
                "¡Eliminaste el objetivo!");
        } else {
            dialog = gtk_message_dialog_new(
                GTK_WINDOW(window),
                GTK_DIALOG_DESTROY_WITH_PARENT,
                GTK_MESSAGE_INFO,
                GTK_BUTTONS_OK,
                "%s", msg->mensaje);
        }
        
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        
        // Reactivar botón SIEMPRE después de mostrar mensaje
        SensibleData *info = g_malloc(sizeof(SensibleData));
        info->widget = btnDisparar;
        info->sensitive = TRUE;
        g_idle_add(set_widget_sensitive, info);
        disparo_transmitido = 0;
        tiro_en_progreso = 0;  // AGREGAR: También resetear tiro_en_progreso
    }
    
    if (msg) free(msg);
    
    // Continuar procesando si hay más mensajes
    g_mutex_lock(&mutex_mensajes);
    if (!g_queue_is_empty(cola_mensajes)) {
        g_timeout_add(100, procesar_mensaje_cola, NULL);
    } else {
        procesando_mensaje = FALSE;
    }
    g_mutex_unlock(&mutex_mensajes);
    
    return FALSE;
}

// Función para agregar mensaje a la cola
void agregar_mensaje_cola(const char* mensaje, int tipo) {
    if (aplicacion_cerrando) return;
    
    MensajeRespuesta *msg = malloc(sizeof(MensajeRespuesta));
    strncpy(msg->mensaje, mensaje, 127);
    msg->mensaje[127] = '\0';
    msg->tipo = tipo;
    
    g_mutex_lock(&mutex_mensajes);
    g_queue_push_tail(cola_mensajes, msg);
    
    // Iniciar procesamiento si no está activo
    if (!procesando_mensaje) {
        procesando_mensaje = TRUE;
        g_idle_add(procesar_mensaje_cola, NULL);
    }
    g_mutex_unlock(&mutex_mensajes);
    
    return;
}

/**
 * Cliente TCP para transmisión con sistema de respuestas
 */
void *hilo_cliente_tcp(void *arg) {
    char *ip_destino = (char*)arg;
    int sockfd = -1;
    
    // Limitar hilos TCP simultáneos
    g_mutex_lock(&mutex_hilos_tcp);
    if (hilos_tcp_activos >= max_hilos_tcp) {
        g_mutex_unlock(&mutex_hilos_tcp);
        free(ip_destino);
        
        // Reactivar botón si no se puede crear hilo
        if (!aplicacion_cerrando) {
            SensibleData *info = g_malloc(sizeof(SensibleData));
            info->widget = btnDisparar;
            info->sensitive = TRUE;
            g_idle_add(set_widget_sensitive, info);
            disparo_transmitido = 0;
        }
        return NULL;
    }
    hilos_tcp_activos++;
    g_mutex_unlock(&mutex_hilos_tcp);
    
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        free(ip_destino);
        goto cleanup_hilo_tcp;
    }

    // Configurar timeout más corto
    struct timeval timeout;
    timeout.tv_sec = 3;  // 3 segundos timeout
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PUERTO);
    
    if (inet_pton(AF_INET, ip_destino, &serv_addr.sin_addr) <= 0) {
        goto cleanup_socket_tcp;
    }

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        goto cleanup_socket_tcp;
    }

    // Enviar datos del proyectil
    if (send(sockfd, (char*)&datos_envio, sizeof(DATOS_RED), 0) < 0) {
        goto cleanup_socket_tcp;
    }

    // Escuchar UNA SOLA respuesta
    char buffer_respuesta[64];
    int bytes_recibidos = recv(sockfd, buffer_respuesta, sizeof(buffer_respuesta) - 1, 0);

    if (bytes_recibidos > 0 && !aplicacion_cerrando) {
        buffer_respuesta[bytes_recibidos] = '\0';
        
        // Agregar a cola en lugar de mostrar directamente
        if (strstr(buffer_respuesta, "Impacto con personaje") != NULL) {
            agregar_mensaje_cola("¡Eliminaste el objetivo!", 1);
        } else {
            agregar_mensaje_cola(buffer_respuesta, 0);
        }
    } else {
        // SIEMPRE reactivar botón, incluso si no hay respuesta o hay error
        if (!aplicacion_cerrando) {
            SensibleData *info = g_malloc(sizeof(SensibleData));
            info->widget = btnDisparar;
            info->sensitive = TRUE;
            g_idle_add(set_widget_sensitive, info);
            disparo_transmitido = 0;
        }
    }

cleanup_socket_tcp:
    if (sockfd >= 0) {
        shutdown(sockfd, SHUT_RDWR);
        close(sockfd);
    }
    
    if (ip_destino) free(ip_destino);
    
cleanup_hilo_tcp:
    // Decrementar contador de hilos TCP
    g_mutex_lock(&mutex_hilos_tcp);
    hilos_tcp_activos--;
    g_mutex_unlock(&mutex_hilos_tcp);
    
    return NULL;
}

/**
 * Enviar datos del proyectil con sistema de respuestas
 */
void enviar_datos_red(const char *ip_destino) {
    if (!ip_destino || strlen(ip_destino) == 0) return;
    
    char *ip_copia = malloc(strlen(ip_destino) + 1);
    strcpy(ip_copia, ip_destino);
    
    pthread_t hilo_cliente;
    pthread_create(&hilo_cliente, NULL, hilo_cliente_tcp, ip_copia);
    pthread_detach(hilo_cliente);
}

/**
 * Animación del proyectil local con física idéntica a Windows
 */
void *animar_tiro(void *arg) {
    proyectil_local = malloc(sizeof(ProyectilJuego));
    if (!proyectil_local) {
        tiro_en_progreso = 0;
        return NULL;
    }
    
    g_mutex_init(&proyectil_local->mutex);
    
    const char *vel_txt = gtk_entry_get_text(GTK_ENTRY(entryVel));
    const char *ang_txt = gtk_entry_get_text(GTK_ENTRY(entryAng));
    
    double velocidad_inicial = atof(vel_txt);
    double angulo_rad = atof(ang_txt) * M_PI / 180.0;
    
    obtener_nombre_jugador_actual();
    
    // Inicializar proyectil con física de Windows
    g_mutex_lock(&proyectil_local->mutex);
    proyectil_local->posicionInicialX = 0.0;
    proyectil_local->posicionInicialY = 0.0;
    proyectil_local->velocidadMovimientoX = velocidad_inicial * cos(angulo_rad);
    proyectil_local->velocidadMovimientoY = velocidad_inicial * sin(angulo_rad);
    proyectil_local->velocidadLanzamiento = velocidad_inicial;
    proyectil_local->tiempoVida = 0.0;
    proyectil_local->anguloDisparo = angulo_rad;
    proyectil_local->estaVisible = 1;
    strncpy(proyectil_local->nombrePropietario, alias, MAX_ALIAS - 1);
    proyectil_local->nombrePropietario[MAX_ALIAS - 1] = '\0';
    g_mutex_unlock(&proyectil_local->mutex);

    disparo_transmitido = 0;
    
    // Bucle de animación con ecuaciones de Windows
    while (1) {
        g_mutex_lock(&proyectil_local->mutex);
        
        // Ecuaciones físicas idénticas a Windows
        proyectil_local->posicionInicialX = proyectil_local->velocidadMovimientoX * proyectil_local->tiempoVida;
        proyectil_local->posicionInicialY = proyectil_local->velocidadMovimientoY * proyectil_local->tiempoVida - 
                                           DELTA_TIEMPO * FUERZA_GRAVEDAD * proyectil_local->tiempoVida * proyectil_local->tiempoVida;
        proyectil_local->tiempoVida += DELTA_TIEMPO;
        
        int coordenada_x = posicion_inicial_x + (int)proyectil_local->posicionInicialX;
        int coordenada_y = posicion_inicial_y - (int)proyectil_local->posicionInicialY;
        
        g_mutex_unlock(&proyectil_local->mutex);

        // Verificar límites de pantalla
        if (coordenada_x < 0 || coordenada_y > allocation.height - TAM_BOLA) {
            // SOLO reactivar botón si NO se transmitió (no salió por la derecha)
            if (!disparo_transmitido) {
                SensibleData *info = g_malloc(sizeof(SensibleData));
                info->widget = btnDisparar;
                info->sensitive = TRUE;
                g_idle_add(set_widget_sensitive, info);
                disparo_transmitido = 0;
            }
            break;
        }
        
        // Transmitir datos al salir por la derecha
        if (coordenada_x > allocation.width && !disparo_transmitido) {
            disparo_transmitido = 1;
            
            // Preparar datos para transmisión (formato Windows)
            datos_envio.x = coordenada_x;
            datos_envio.y = coordenada_y;
            datos_envio.tiempoTranscurrido = proyectil_local->tiempoVida;
            datos_envio.velocidadTotal = proyectil_local->velocidadLanzamiento;
            datos_envio.velocidadInicialX = posicion_inicial_x;
            datos_envio.velocidadInicialY = posicion_inicial_y;
            datos_envio.anguloRadianes = proyectil_local->anguloDisparo;
            strncpy(datos_envio.nombreJugador, alias, 32);
            
            enviar_datos_red(gtk_entry_get_text(GTK_ENTRY(entryIP)));
            break;
        }

        usleep(SLEEP_TIME);
    }

    // Limpieza
    g_mutex_clear(&proyectil_local->mutex);
    free(proyectil_local);
    proyectil_local = NULL;
    tiro_en_progreso = 0;
    
    return NULL;
}

/**
 * Animación de proyectiles remotos con colisiones precisas
 */
void *animar_proyectil_remoto(void *arg) {
    ProyectilJuego *proyectil = (ProyectilJuego*)arg;
    
    if (!proyectil) return NULL;
    
    // Registrar hilo activo
    g_mutex_lock(&mutex_hilos);
    num_hilos_activos++;
    g_mutex_unlock(&mutex_hilos);
    
    g_mutex_lock(&mutex_proyectiles);
    lista_proyectiles_remotos = g_list_append(lista_proyectiles_remotos, proyectil);
    g_mutex_unlock(&mutex_proyectiles);

    int hay_impacto = 0;
    int socket_valido = (proyectil->socketConexion > 0);
    
    while (!aplicacion_cerrando && !hay_impacto) {
        if (!proyectil || !proyectil->estaVisible) break;
        
        g_mutex_lock(&proyectil->mutex);
        
        // Física idéntica a Windows con efecto espejo
        float posicion_pantalla_x = proyectil->posicionInicialX + (proyectil->velocidadMovimientoX * proyectil->tiempoVida);
        float posicion_pantalla_y = proyectil->posicionInicialY - (proyectil->velocidadMovimientoY * proyectil->tiempoVida - 
                                   DELTA_TIEMPO * FUERZA_GRAVEDAD * proyectil->tiempoVida * proyectil->tiempoVida);
        proyectil->tiempoVida += DELTA_TIEMPO;
        
        g_mutex_unlock(&proyectil->mutex);

        // Verificar colisión con suelo
        if (posicion_pantalla_y > allocation.height - TAM_BOLA) {
            if (socket_valido) {
                char mensaje_limite[] = "Proyectil salio de pantalla";
                send(proyectil->socketConexion, mensaje_limite, strlen(mensaje_limite) + 1, MSG_NOSIGNAL);
            }
            break;
        }
        
        // Detección precisa de colisión con personaje
        if (!personaje_destruido && 
            detectar_colision_rectangulos((int)posicion_pantalla_x, (int)posicion_pantalla_y, 30, 30,
                                        posicion_jugador.x, posicion_jugador.y, ANCHO_PERSONAJE, ALTO_PERSONAJE)) {
            
            proyectil->estaVisible = 0;
            hay_impacto = 1;
            
            if (socket_valido) {
                char mensaje_respuesta[] = "Impacto con personaje";
                send(proyectil->socketConexion, mensaje_respuesta, strlen(mensaje_respuesta) + 1, MSG_NOSIGNAL);
            }
            
            if (!aplicacion_cerrando) {
                char mensaje_eliminacion[64];
                sprintf(mensaje_eliminacion, "Fuiste eliminado por %s", proyectil->nombrePropietario);
                
                GtkWidget *dialog = gtk_message_dialog_new(
                    GTK_WINDOW(window),
                    GTK_DIALOG_DESTROY_WITH_PARENT,
                    GTK_MESSAGE_INFO,
                    GTK_BUTTONS_OK,
                    "%s", mensaje_eliminacion);
                
                gtk_dialog_run(GTK_DIALOG(dialog));
                gtk_widget_destroy(dialog);
                
                aplicacion_cerrando = 1;
                servidor_activo = 0;
                gtk_main_quit();
            }
            break;
        }
        
        // Verificar colisiones con plataformas
        if (!hay_impacto) {
            for (int i = 0; i < 3; i++) {
                if (!coordenadas_plataformas[i].visible) continue;
                
                int min_x = (coordenadas_plataformas[i].x1 < coordenadas_plataformas[i].x2) ? 
                           coordenadas_plataformas[i].x1 : coordenadas_plataformas[i].x2;
                int max_x = (coordenadas_plataformas[i].x1 > coordenadas_plataformas[i].x2) ? 
                           coordenadas_plataformas[i].x1 : coordenadas_plataformas[i].x2;
                int min_y = (coordenadas_plataformas[i].y1 < coordenadas_plataformas[i].y2) ? 
                           coordenadas_plataformas[i].y1 : coordenadas_plataformas[i].y2;
                int max_y = (coordenadas_plataformas[i].y1 > coordenadas_plataformas[i].y2) ? 
                           coordenadas_plataformas[i].y1 : coordenadas_plataformas[i].y2;
                
                if (detectar_colision_rectangulos((int)posicion_pantalla_x, (int)posicion_pantalla_y, 30, 30,
                                                min_x, min_y, max_x - min_x, max_y - min_y)) {
                    
                    coordenadas_plataformas[i].visible = 0;
                    proyectil->estaVisible = 0;
                    hay_impacto = 1;
                    
                    if (i == indice_plataforma_jugador) {
                        posicion_jugador.y = allocation.height - ALTO_PERSONAJE;
                        indice_plataforma_jugador = -1;
                        posicion_inicial_y = posicion_jugador.y - 50;
                    }
                    
                    if (socket_valido) {
                        char mensaje_obstaculo[] = "Impacto con obstaculo";
                        send(proyectil->socketConexion, mensaje_obstaculo, strlen(mensaje_obstaculo) + 1, MSG_NOSIGNAL);
                    }
                    break;
                }
            }
        }
        
        if (hay_impacto) break;
        
        // Verificar salida por borde izquierdo
        if (posicion_pantalla_x < 0) {
            if (socket_valido) {
                char mensaje_limite[] = "Proyectil salio de pantalla";
                send(proyectil->socketConexion, mensaje_limite, strlen(mensaje_limite) + 1, MSG_NOSIGNAL);
            }
            break;
        }
        
        usleep(SLEEP_TIME);
    }
    
    // Limpieza segura
    g_mutex_lock(&mutex_proyectiles);
    lista_proyectiles_remotos = g_list_remove(lista_proyectiles_remotos, proyectil);
    g_mutex_unlock(&mutex_proyectiles);
    
    if (proyectil) {
        if (socket_valido) {
            shutdown(proyectil->socketConexion, SHUT_RDWR);
            close(proyectil->socketConexion);
        }
        g_mutex_clear(&proyectil->mutex);
        free(proyectil);
    }
    
    // Desregistrar hilo activo
    g_mutex_lock(&mutex_hilos);
    num_hilos_activos--;
    g_mutex_unlock(&mutex_hilos);
    
    return NULL;
}

/**
 * Atender conexiones de clientes TCP
 */
void *atender_cliente(void *arg) {
    int socket_cliente = *(int *)arg;
    free(arg);
    
    // Leer datos del proyectil
    DATOS_RED datos_recibidos;
    if (recv(socket_cliente, (char *)&datos_recibidos, sizeof(DATOS_RED), 0) <= 0) {
        close(socket_cliente);
        return NULL;
    }
    
    // Crear proyectil remoto
    ProyectilJuego *proyectil_remoto = malloc(sizeof(ProyectilJuego));
    if (!proyectil_remoto) {
        close(socket_cliente);
        return NULL;
    }
    
    g_mutex_init(&proyectil_remoto->mutex);
    
    // Inicializar con datos recibidos (formato Windows)
    proyectil_remoto->posicionInicialX = datos_recibidos.x + ((datos_recibidos.velocidadTotal * cos(datos_recibidos.anguloRadianes)) * datos_recibidos.tiempoTranscurrido);
    proyectil_remoto->posicionInicialY = datos_recibidos.y + ((datos_recibidos.velocidadTotal * sin(datos_recibidos.anguloRadianes)) * datos_recibidos.tiempoTranscurrido - FUERZA_GRAVEDAD * datos_recibidos.tiempoTranscurrido * datos_recibidos.tiempoTranscurrido * DELTA_TIEMPO);
    proyectil_remoto->velocidadMovimientoX = -(datos_recibidos.velocidadTotal * cos(datos_recibidos.anguloRadianes));
    proyectil_remoto->velocidadMovimientoY = (datos_recibidos.velocidadTotal * sin(datos_recibidos.anguloRadianes));
    proyectil_remoto->tiempoVida = datos_recibidos.tiempoTranscurrido;
    proyectil_remoto->socketConexion = socket_cliente;
    proyectil_remoto->estaVisible = 1;
    strncpy(proyectil_remoto->nombrePropietario, datos_recibidos.nombreJugador, MAX_ALIAS - 1);
    proyectil_remoto->nombrePropietario[MAX_ALIAS - 1] = '\0';
    
    // Crear hilo de animación
    pthread_t hilo_animacion;
    if (pthread_create(&hilo_animacion, NULL, animar_proyectil_remoto, proyectil_remoto) != 0) {
        g_mutex_clear(&proyectil_remoto->mutex);
        free(proyectil_remoto);
        close(socket_cliente);
        return NULL;
    }
    pthread_detach(hilo_animacion);
    
    return NULL;
}

/**
 * Servidor TCP principal
 */
void *servidor_socket(void *arg) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return NULL;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PUERTO);
    
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(server_fd);
        return NULL;
    }
    
    if (listen(server_fd, 10) < 0) {
        close(server_fd);
        return NULL;
    }

    while (servidor_activo) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) continue;
        
        int *socket_cliente = malloc(sizeof(int));
        *socket_cliente = client_fd;
        
        pthread_t hilo_cliente;
        if (pthread_create(&hilo_cliente, NULL, atender_cliente, socket_cliente) != 0) {
            free(socket_cliente);
            close(client_fd);
        } else {
            pthread_detach(hilo_cliente);
        }
    }

    close(server_fd);
    return NULL;
}

/**
 * Inicializar coordenadas del juego (estilo Windows)
 */
void inicializar_coordenadas_juego() {
    gtk_widget_get_allocation(draw1, &allocation);
    srand(time(NULL));

    int coordenada_x1 = rand() % (allocation.width / 2 - 3 * 70);
    int coordenada_x2 = coordenada_x1 + 70;
    int plataforma_seleccionada = (rand() % 3) + 1;

    for (int i = 0; i < 3; i++) {
        coordenadas_plataformas[i].x1 = coordenada_x1;
        coordenadas_plataformas[i].x2 = coordenada_x2;
        coordenadas_plataformas[i].y1 = allocation.height;
        coordenadas_plataformas[i].y2 = 300 + rand() % (500 - 300 + 1);
        coordenadas_plataformas[i].visible = 1;

        if ((i + 1) == plataforma_seleccionada) {
            posicion_jugador.x = coordenada_x1 + 15;
            posicion_jugador.y = coordenadas_plataformas[i].y2 - 50;
            indice_plataforma_jugador = i;
        }

        coordenada_x1 += 70;
        coordenada_x2 += 70;
    }

    posicion_inicial_x = posicion_jugador.x + 17;
    posicion_inicial_y = posicion_jugador.y - 50;
}

/**
 * Validación de entrada numérica
 */
int validar_entrada_numerica(const char *texto) {
    if (!texto || strlen(texto) == 0) return 0;
    
    char *endptr;
    double valor = strtod(texto, &endptr);
    return (*endptr == '\0' && valor > 0);
}

/**
 * Validar datos de entrada
 */
int validar_datos_entrada() {
    const char *vel_txt = gtk_entry_get_text(GTK_ENTRY(entryVel));
    const char *ang_txt = gtk_entry_get_text(GTK_ENTRY(entryAng));
    const char *ip_txt = gtk_entry_get_text(GTK_ENTRY(entryIP));

    if (!validar_entrada_numerica(vel_txt)) {
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Ingresa un valor numérico válido para la velocidad.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return 0;
    }

    if (!validar_entrada_numerica(ang_txt)) {
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Ingresa un valor numérico válido para el ángulo.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return 0;
    }

    if (!ip_txt || strlen(ip_txt) == 0) {
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Ingresa una dirección IP válida.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return 0;
    }

    return 1;
}

/**
 * Función de renderizado principal
 */
gboolean on_draw1_draw(GtkDrawingArea *widget, cairo_t *cr) {
    // Fondo blanco
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    // Renderizar plataformas con gradiente (estilo Windows)
    for (int i = 0; i < 3; i++) {
        if (!coordenadas_plataformas[i].visible) continue;

        int min_x = (coordenadas_plataformas[i].x1 < coordenadas_plataformas[i].x2) ? 
                   coordenadas_plataformas[i].x1 : coordenadas_plataformas[i].x2;
        int max_x = (coordenadas_plataformas[i].x1 > coordenadas_plataformas[i].x2) ? 
                   coordenadas_plataformas[i].x1 : coordenadas_plataformas[i].x2;
        int min_y = (coordenadas_plataformas[i].y1 < coordenadas_plataformas[i].y2) ? 
                   coordenadas_plataformas[i].y1 : coordenadas_plataformas[i].y2;
        int max_y = (coordenadas_plataformas[i].y1 > coordenadas_plataformas[i].y2) ? 
                   coordenadas_plataformas[i].y1 : coordenadas_plataformas[i].y2;

        cairo_pattern_t *gradiente = cairo_pattern_create_linear(min_x, min_y, min_x, max_y);
        cairo_pattern_add_color_stop_rgb(gradiente, 0, 0.25, 0.5, 1.0);
        cairo_pattern_add_color_stop_rgb(gradiente, 1, 0.5, 0.0, 1.0);
        
        cairo_set_source(cr, gradiente);
        cairo_rectangle(cr, min_x, min_y, max_x - min_x, max_y - min_y);
        cairo_fill(cr);
        cairo_pattern_destroy(gradiente);
    }

    // Renderizar personaje
    if (!personaje_destruido && img_personaje) {
        cairo_set_source_surface(cr, img_personaje, posicion_jugador.x, posicion_jugador.y);
        cairo_paint(cr);
    }

    // Renderizar proyectil local
    if (proyectil_local != NULL && proyectil_local->estaVisible) {
        g_mutex_lock(&proyectil_local->mutex);
        int coordenada_x = posicion_inicial_x + (int)proyectil_local->posicionInicialX;
        int coordenada_y = posicion_inicial_y - (int)proyectil_local->posicionInicialY;
        g_mutex_unlock(&proyectil_local->mutex);
        
        cairo_set_source_surface(cr, img_bola, coordenada_x - TAM_BOLA/2, coordenada_y - TAM_BOLA/2);
        cairo_paint(cr);
    }

    // Renderizar proyectiles remotos
    g_mutex_lock(&mutex_proyectiles);
    for (GList *n = lista_proyectiles_remotos; n != NULL; n = n->next) {
        ProyectilJuego *p = (ProyectilJuego*)n->data;
        if (p->estaVisible) {
            g_mutex_lock(&p->mutex);
            float pos_x = p->posicionInicialX + (p->velocidadMovimientoX * p->tiempoVida);
            float pos_y = p->posicionInicialY - (p->velocidadMovimientoY * p->tiempoVida - 
                         DELTA_TIEMPO * FUERZA_GRAVEDAD * p->tiempoVida * p->tiempoVida);
            g_mutex_unlock(&p->mutex);
            
            cairo_set_source_surface(cr, img_bola, (int)pos_x - TAM_BOLA/2, (int)pos_y - TAM_BOLA/2);
            cairo_paint(cr);
        }
    }
    g_mutex_unlock(&mutex_proyectiles);

    return FALSE;
}

/**
 * Función de refresco periódico
 */
gboolean refrescar(gpointer data) {
    gtk_widget_queue_draw(draw1);
    return TRUE;
}

void limpiar_recursos_aplicacion() {
    aplicacion_cerrando = 1;
    servidor_activo = 0;
    
    // Esperar a que terminen los hilos activos
    int intentos = 0;
    while (num_hilos_activos > 0 && intentos < 50) {
        usleep(100000);  // 100ms
        intentos++;
    }
    
    // Limpiar proyectiles remotos
    g_mutex_lock(&mutex_proyectiles);
    for (GList *n = lista_proyectiles_remotos; n != NULL; n = n->next) {
        ProyectilJuego *p = (ProyectilJuego*)n->data;
        if (p) {
            if (p->socketConexion > 0) {
                close(p->socketConexion);
            }
            g_mutex_clear(&p->mutex);
            free(p);
        }
    }
    g_list_free(lista_proyectiles_remotos);
    lista_proyectiles_remotos = NULL;
    g_mutex_unlock(&mutex_proyectiles);
    
    // Limpiar proyectil local
    if (proyectil_local) {
        g_mutex_clear(&proyectil_local->mutex);
        free(proyectil_local);
        proyectil_local = NULL;
    }
    
    // Limpiar mutex
    g_mutex_clear(&mutex_proyectiles);
    g_mutex_clear(&mutex_hilos);
    
    // Limpiar imágenes
    if (img_personaje) {
        cairo_surface_destroy(img_personaje);
        img_personaje = NULL;
    }
    if (img_bola) {
        cairo_surface_destroy(img_bola);
        img_bola = NULL;
    }

    // Limpiar cola de mensajes
    g_mutex_lock(&mutex_mensajes);
    while (!g_queue_is_empty(cola_mensajes)) {
        MensajeRespuesta *msg = g_queue_pop_head(cola_mensajes);
        if (msg) free(msg);
    }
    g_queue_free(cola_mensajes);
    g_mutex_unlock(&mutex_mensajes);
    g_mutex_clear(&mutex_mensajes);
    g_mutex_clear(&mutex_hilos_tcp);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Uso correcto: ./tiropa <tu_alias>\n");
        return 1;
    }
    
    strncpy(alias, argv[1], MAX_ALIAS - 1);
    alias[MAX_ALIAS - 1] = '\0';

    GtkBuilder *builder;
    gtk_init(&argc, &argv);

    builder = gtk_builder_new();
    GError *error = NULL;
    if (!gtk_builder_add_from_file(builder, "tiropa.glade", &error)) {
        fprintf(stderr, "Error cargando interfaz: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    // Obtener widgets
    window = GTK_WIDGET(gtk_builder_get_object(builder, "window"));
    draw1 = GTK_WIDGET(gtk_builder_get_object(builder, "draw1"));
    entryVel = GTK_WIDGET(gtk_builder_get_object(builder, "entryVelocidad"));
    entryAng = GTK_WIDGET(gtk_builder_get_object(builder, "entryAngulo"));
    entryIP = GTK_WIDGET(gtk_builder_get_object(builder, "entryIP"));
    btnDisparar = GTK_WIDGET(gtk_builder_get_object(builder, "btnDisparar"));
    btnSalir = GTK_WIDGET(gtk_builder_get_object(builder, "btnSalir"));
    btnAcercaDe = GTK_WIDGET(gtk_builder_get_object(builder, "btnAcercaDe"));

    // Crear campo de nombre en la interfaz
    entryNombre = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entryNombre), alias);
    gtk_widget_show(entryNombre);

    gtk_builder_connect_signals(builder, NULL);
    gtk_widget_show_all(window);
    gtk_widget_get_allocation(draw1, &allocation);

    g_mutex_init(&mutex_proyectiles);
    g_mutex_init(&mutex_hilos);
    cola_mensajes = g_queue_new();
    g_mutex_init(&mutex_mensajes);
    g_mutex_init(&mutex_hilos_tcp);

    // Cargar imágenes escaladas
    img_personaje = cairo_image_surface_create_from_png("personaje.png");
    img_bola = cairo_image_surface_create_from_png("comer.png");

    if (cairo_surface_status(img_personaje) != CAIRO_STATUS_SUCCESS ||
        cairo_surface_status(img_bola) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error al cargar imágenes\n");
        return 1;
    }

    // Escalar imágenes a tamaños exactos
    int ow = cairo_image_surface_get_width(img_personaje);
    int oh = cairo_image_surface_get_height(img_personaje);
    
    if (ow > 0 && oh > 0) {
        double scale = (double)ALTO_PERSONAJE / oh;
        int new_w = (int)(ow * scale);
        int new_h = (int)(oh * scale);

        cairo_surface_t *scaled_surface = cairo_surface_create_similar(img_personaje,
                              CAIRO_CONTENT_COLOR_ALPHA, new_w, new_h);
        cairo_t *cr = cairo_create(scaled_surface);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, img_personaje, 0, 0);
        cairo_paint(cr);
        cairo_destroy(cr);
        
        cairo_surface_destroy(img_personaje);
        img_personaje = scaled_surface;
    }

    // Escalar proyectil a 30x30 exacto
    int bola_w = cairo_image_surface_get_width(img_bola);
    int bola_h = cairo_image_surface_get_height(img_bola);

    if (bola_w > 0 && bola_h > 0) {
        cairo_surface_t *scaled_bola = cairo_surface_create_similar(img_bola,
                              CAIRO_CONTENT_COLOR_ALPHA, TAM_BOLA, TAM_BOLA);
        cairo_t *cr_bola = cairo_create(scaled_bola);
        
        double scale_x = (double)TAM_BOLA / bola_w;
        double scale_y = (double)TAM_BOLA / bola_h;
        
        cairo_scale(cr_bola, scale_x, scale_y);
        cairo_set_source_surface(cr_bola, img_bola, 0, 0);
        cairo_paint(cr_bola);
        cairo_destroy(cr_bola);
        
        cairo_surface_destroy(img_bola);
        img_bola = scaled_bola;
    }

    inicializar_coordenadas_juego();

    // Iniciar servidor TCP
    pthread_t hilo_servidor;
    if (pthread_create(&hilo_servidor, NULL, servidor_socket, NULL) != 0) {
        fprintf(stderr, "Error creando hilo servidor\n");
        return 1;
    }

    // Timer de refresco
    g_timeout_add(20, refrescar, NULL);

    gtk_main();

    // Limpieza
    limpiar_recursos_aplicacion();
    pthread_cancel(hilo_servidor);
    
    return 0;
}

/**
 * Callback del botón disparar
 */
void on_btnDisparar_clicked() {
    if (personaje_destruido) {
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "Personaje destruido. Debes reiniciar la aplicación.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    if (tiro_en_progreso) {
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
            "Ya hay un tiro en progreso. Espera a que termine.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    if (!validar_datos_entrada()) return;

    tiro_en_progreso = 1;
    gtk_widget_set_sensitive(btnDisparar, FALSE);  // Bloquear hasta recibir respuesta

    pthread_t hilo;
    if (pthread_create(&hilo, NULL, animar_tiro, NULL) != 0) {
        tiro_en_progreso = 0;
        gtk_widget_set_sensitive(btnDisparar, TRUE);
    } else {
        pthread_detach(hilo);
    }
}

void on_btnAcercaDe_clicked() {
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "Tiro Parabólico - Sistemas de Cómputo Paralelo y Distribuido\n"
        "Tercer Parcial 2025\n"
        "Dr. J. Jesús Arellano Pimentel");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

void on_btnSalir_clicked() { 
    limpiar_recursos_aplicacion();
    gtk_main_quit(); 
}

void on_window_destroy() { 
    limpiar_recursos_aplicacion();
    gtk_main_quit(); 
}
