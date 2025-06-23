/**
 * Compilar: gcc -o tiropa tiropa.c `pkg-config --cflags --libs gtk+-3.0` -lm -lpthread -export-dynamic
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

#define G 9.8
#define PUERTO 4200
#define ANCHO_PERSONAJE 50
#define ALTO_PERSONAJE  40
#define TAM_BOLA        30
#define MAX_ALIAS       32
#define DELTA_T         0.02
#define SLEEP_TIME      20000
#define ANCHO_PANTALLA  1080
#define ESCALA_VISUAL   3.0
#define VELOCIDAD_VISUAL_CONSTANTE 150.0

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

typedef struct {
    double x, y, xo, yo;
    double vo, ang;
    double to;
    char NN[MAX_ALIAS];
} Datos;

typedef struct {
    GMutex mutex;
    double x;
    double y;
    double pos_x;
    double pos_y;
    double vel_x;
    double vel_y;
    double tiempo_ini;
    char remitente[MAX_ALIAS];
} DatosEspejo;

typedef struct {
    int x, y, ancho, alto;
    int visible;
} RECT;

// Variables globales
GtkWidget *window, *draw1;
GtkWidget *entryVel, *entryAng, *entryIP;
GtkWidget *btnDisparar, *btnSalir, *btnAcercaDe;
cairo_surface_t *img_personaje, *img_bola;
GtkAllocation allocation;

char alias[MAX_ALIAS];
int personaje_destruido = 0;
int tiro_en_progreso = 0;
int servidor_activo = 1;

double personaje_xo, personaje_yo;

RECT barras[3];
DatosEspejo *proyectil_local = NULL;

GList *lista_espejos = NULL;
GMutex mutex_espejos;

pthread_mutex_t mutex_ganador = PTHREAD_MUTEX_INITIALIZER;
gboolean ganador = FALSE;
gboolean isGanador = FALSE;
char nombre_ganador[MAX_ALIAS];

/**
 * Detecta colisión entre dos rectángulos usando intersección de áreas
 * @param proj_x Coordenada X del centro del proyectil
 * @param proj_y Coordenada Y del centro del proyectil
 * @param proj_ancho Ancho del proyectil
 * @param proj_alto Alto del proyectil
 * @param obj_x Coordenada X del objeto (esquina superior izquierda)
 * @param obj_y Coordenada Y del objeto (esquina superior izquierda)
 * @param obj_ancho Ancho del objeto
 * @param obj_alto Alto del objeto
 * @return 1 si hay colisión, 0 si no
 */
int colisiona_rectangulos(double proj_x, double proj_y, int proj_ancho, int proj_alto, 
                         double obj_x, double obj_y, int obj_ancho, int obj_alto) {
    double proj_izq = proj_x - proj_ancho/2.0;
    double proj_der = proj_x + proj_ancho/2.0;
    double proj_arr = proj_y - proj_alto/2.0;
    double proj_aba = proj_y + proj_alto/2.0;
    
    double obj_izq = obj_x;
    double obj_der = obj_x + obj_ancho;
    double obj_arr = obj_y;
    double obj_aba = obj_y + obj_alto;
    
    int interseccion_x = (proj_izq < obj_der) && (proj_der > obj_izq);
    int interseccion_y = (proj_arr < obj_aba) && (proj_aba > obj_arr);
    
    return interseccion_x && interseccion_y;
}

/**
 * Función de compatibilidad para colisión con barras
 * @param x Coordenada X del proyectil
 * @param y Coordenada Y del proyectil
 * @param r Rectángulo de la barra
 * @return 1 si hay colisión, 0 si no
 */
int colisiona(double x, double y, RECT r) {
    return colisiona_rectangulos(x, y, TAM_BOLA, TAM_BOLA, r.x, r.y, r.ancho, r.alto);
}

/**
 * Detecta colisión específica entre proyectil y personaje
 * @param proj_x Coordenada X del proyectil
 * @param proj_y Coordenada Y del proyectil
 * @return 1 si hay colisión, 0 si no
 */
int colisiona_con_personaje(double proj_x, double proj_y) {
    return colisiona_rectangulos(proj_x, proj_y, TAM_BOLA, TAM_BOLA, 
                                personaje_xo, personaje_yo, ANCHO_PERSONAJE, ALTO_PERSONAJE);
}

// Prototipos de funciones
void inicializar_barras_y_personaje();
void *animar_tiro(void *arg);
void *animar_espejo(void *arg);
void enviar_datos(const char *ip);
gboolean refrescar(gpointer data);
gboolean on_draw1_draw(GtkDrawingArea *widget, cairo_t *cr);
void *servidor_socket(void *arg);
void *atender_cliente(void *arg);
void finalizar_partida(const char *remitente);
int validar_datos_entrada();

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Uso correcto: ./tiropa <tu_alias>\n");
        return 1;
    }
    
    if (strlen(argv[1]) >= MAX_ALIAS) {
        printf("Error: El alias debe tener menos de %d caracteres\n", MAX_ALIAS);
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

    window       = GTK_WIDGET(gtk_builder_get_object(builder, "window"));
    draw1        = GTK_WIDGET(gtk_builder_get_object(builder, "draw1"));
    entryVel     = GTK_WIDGET(gtk_builder_get_object(builder, "entryVelocidad"));
    entryAng     = GTK_WIDGET(gtk_builder_get_object(builder, "entryAngulo"));
    entryIP      = GTK_WIDGET(gtk_builder_get_object(builder, "entryIP"));
    btnDisparar  = GTK_WIDGET(gtk_builder_get_object(builder, "btnDisparar"));
    btnSalir     = GTK_WIDGET(gtk_builder_get_object(builder, "btnSalir"));
    btnAcercaDe  = GTK_WIDGET(gtk_builder_get_object(builder, "btnAcercaDe"));

    if (!window || !draw1 || !entryVel || !entryAng || !entryIP || !btnDisparar) {
        fprintf(stderr, "Error: No se pudieron cargar todos los widgets de la interfaz\n");
        return 1;
    }

    gtk_builder_connect_signals(builder, NULL);
    gtk_widget_show_all(window);
    gtk_widget_get_allocation(draw1, &allocation);

    g_mutex_init(&mutex_espejos);

    // Cargar imagen del personaje y escalarla al tamaño definido
    img_personaje = cairo_image_surface_create_from_png("personaje.png");
    if (cairo_surface_status(img_personaje) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error al cargar personaje.png\n");
        return 1;
    }

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

    // Cargar imagen de la bola y escalarla a tamaño exacto
    img_bola = cairo_image_surface_create_from_png("comer.png");
    if (cairo_surface_status(img_bola) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error al cargar comer.png\n");
        return 1;
    }

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

    inicializar_barras_y_personaje();
    gtk_widget_queue_draw(draw1);

    // Iniciar hilo del servidor TCP
    pthread_t hiloServidor;
    if (pthread_create(&hiloServidor, NULL, servidor_socket, NULL) != 0) {
        fprintf(stderr, "Error creando hilo servidor\n");
        return 1;
    }

    // Configurar timer de refresco de pantalla
    g_timeout_add(20, refrescar, NULL);

    gtk_main();

    // Limpieza al salir
    servidor_activo = 0;
    pthread_cancel(hiloServidor);
    
    if (img_personaje) cairo_surface_destroy(img_personaje);
    if (img_bola) cairo_surface_destroy(img_bola);
    
    return 0;
}

/**
 * Función de callback para refrescar la pantalla periódicamente
 */
gboolean refrescar(gpointer data) {
    gtk_widget_queue_draw(draw1);
    return TRUE;
}

/**
 * Función de renderizado principal usando Cairo
 */
gboolean on_draw1_draw(GtkDrawingArea *widget, cairo_t *cr) {
    // Limpiar fondo con color blanco
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    // Renderizar barras visibles
    cairo_set_source_rgb(cr, 0.3, 0.3, 1.0);
    for (int i = 0; i < 3; i++) {
        if (barras[i].visible) {
            cairo_rectangle(cr, barras[i].x, barras[i].y, barras[i].ancho, barras[i].alto);
            cairo_fill(cr);
        }
    }

    // Renderizar personaje si no está destruido
    if (!personaje_destruido && img_personaje) {
        cairo_set_source_surface(cr, img_personaje, personaje_xo, personaje_yo);
        cairo_paint(cr);
    }

    // Renderizar proyectil local
    if (proyectil_local != NULL) {
        g_mutex_lock(&proyectil_local->mutex);
        double x = proyectil_local->x;
        double y = proyectil_local->y;
        g_mutex_unlock(&proyectil_local->mutex);
        
        cairo_set_source_surface(cr, img_bola, x - TAM_BOLA/2, y - TAM_BOLA/2);
        cairo_paint(cr);
    }

    // Renderizar proyectiles remotos (espejos)
    g_mutex_lock(&mutex_espejos);
    for (GList *n = lista_espejos; n != NULL; n = n->next) {
        DatosEspejo *d = (DatosEspejo*)n->data;
        g_mutex_lock(&d->mutex);
        double x = d->x;
        double y = d->y;
        g_mutex_unlock(&d->mutex);
        
        cairo_set_source_surface(cr, img_bola, x - TAM_BOLA/2, y - TAM_BOLA/2);
        cairo_paint(cr);
    }
    g_mutex_unlock(&mutex_espejos);

    return FALSE;
}

/**
 * Termina la partida cuando el personaje es destruido
 */
void finalizar_partida(const char *remitente) {
    personaje_destruido = 1;
    
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(window),
        GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_INFO,
        GTK_BUTTONS_OK,
        "¡Tu personaje ha sido destruido por %s!\nLa aplicación se cerrará.", remitente);
    
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    
    gtk_main_quit();
}

/**
 * Envía datos del proyectil al servidor remoto via TCP
 */
void enviar_datos(const char *ip_destino) {
    if (!ip_destino || strlen(ip_destino) == 0) {
        return;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PUERTO);
    
    if (inet_pton(AF_INET, ip_destino, &serv_addr.sin_addr) <= 0) {
        close(sockfd);
        return;
    }

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sockfd);
        return;
    }

    Datos datos;
    if (proyectil_local != NULL) {
        g_mutex_lock(&proyectil_local->mutex);
        datos.x = proyectil_local->x;
        datos.y = proyectil_local->y;
        datos.xo = proyectil_local->pos_x;
        datos.yo = proyectil_local->pos_y;
        datos.vo = proyectil_local->vel_x / cos(atan2(proyectil_local->vel_y, proyectil_local->vel_x));
        datos.ang = atan2(proyectil_local->vel_y, proyectil_local->vel_x);
        datos.to = proyectil_local->tiempo_ini;
        strncpy(datos.NN, alias, MAX_ALIAS - 1);
        datos.NN[MAX_ALIAS - 1] = '\0';
        g_mutex_unlock(&proyectil_local->mutex);
    } else {
        close(sockfd);
        return;
    }
    
    write(sockfd, (char *)&datos, sizeof(Datos));
    close(sockfd);
}

/**
 * Hilo para animar el proyectil local con física parabólica
 */
void *animar_tiro(void *arg) {
    proyectil_local = malloc(sizeof(DatosEspejo));
    if (!proyectil_local) {
        tiro_en_progreso = 0;
        return NULL;
    }
    
    g_mutex_init(&proyectil_local->mutex);
    
    const char *vel_txt = gtk_entry_get_text(GTK_ENTRY(entryVel));
    const char *ang_txt = gtk_entry_get_text(GTK_ENTRY(entryAng));
    
    double velocidad_inicial = atof(vel_txt);
    double angulo_rad = atof(ang_txt) * M_PI / 180.0;
    
    double velocidad_total = velocidad_inicial;
    double factor_normalizacion = VELOCIDAD_VISUAL_CONSTANTE / velocidad_total;
    
    // Inicializar posición y velocidad del proyectil
    g_mutex_lock(&proyectil_local->mutex);
    proyectil_local->pos_x = personaje_xo + ANCHO_PERSONAJE/2;
    proyectil_local->pos_y = personaje_yo + ALTO_PERSONAJE/2;
    proyectil_local->x = proyectil_local->pos_x;
    proyectil_local->y = proyectil_local->pos_y;
    proyectil_local->vel_x = velocidad_inicial * cos(angulo_rad);
    proyectil_local->vel_y = velocidad_inicial * sin(angulo_rad);
    proyectil_local->tiempo_ini = 0.0;
    strncpy(proyectil_local->remitente, alias, MAX_ALIAS - 1);
    proyectil_local->remitente[MAX_ALIAS - 1] = '\0';
    g_mutex_unlock(&proyectil_local->mutex);

    gboolean enviado = FALSE;
    
    // Bucle principal de animación
    while (1) {
        g_mutex_lock(&proyectil_local->mutex);
        
        // Aplicar normalización de velocidad visual
        double vel_x_visual = proyectil_local->vel_x * factor_normalizacion;
        double vel_y_visual = proyectil_local->vel_y * factor_normalizacion;
        
        // Calcular nueva posición usando ecuaciones de movimiento parabólico
        proyectil_local->x = proyectil_local->pos_x + vel_x_visual * proyectil_local->tiempo_ini * ESCALA_VISUAL;
        proyectil_local->y = proyectil_local->pos_y - (vel_y_visual * proyectil_local->tiempo_ini - 
                             0.5 * G * factor_normalizacion * proyectil_local->tiempo_ini * proyectil_local->tiempo_ini) * ESCALA_VISUAL;
        
        proyectil_local->tiempo_ini += DELTA_T;
        
        double x = proyectil_local->x;
        double y = proyectil_local->y;
        g_mutex_unlock(&proyectil_local->mutex);

        // Verificar colisión con el suelo
        if (y >= allocation.height) {
            break;
        }

        // Verificar salida por borde derecho y enviar datos
        if (x >= allocation.width + TAM_BOLA && !enviado) {
            enviar_datos(gtk_entry_get_text(GTK_ENTRY(entryIP)));
            enviado = TRUE;
            break;
        }

        usleep(SLEEP_TIME);
    }

    // Limpieza de recursos
    g_mutex_clear(&proyectil_local->mutex);
    free(proyectil_local);
    proyectil_local = NULL;
    
    tiro_en_progreso = 0;
    
    // Reactivar botón de disparo
    SensibleData *info = g_malloc(sizeof(SensibleData));
    info->widget = btnDisparar;
    info->sensitive = TRUE;
    g_idle_add(set_widget_sensitive, info);
    
    return NULL;
}

/**
 * Hilo para animar proyectiles remotos con efecto espejo
 */
void *animar_espejo(void *arg) {
    DatosEspejo *d = (DatosEspejo*)arg;
    
    // Agregar a lista de proyectiles activos
    g_mutex_lock(&mutex_espejos);
    lista_espejos = g_list_append(lista_espejos, d);
    g_mutex_unlock(&mutex_espejos);

    double velocidad_total = sqrt(d->vel_x * d->vel_x + d->vel_y * d->vel_y);
    double factor_normalizacion = VELOCIDAD_VISUAL_CONSTANTE / velocidad_total;

    double x, y;
    gboolean salir = FALSE;
    
    // Bucle principal de animación del proyectil remoto
    while (TRUE) {
        pthread_mutex_lock(&mutex_ganador);
        if (ganador) {
            pthread_mutex_unlock(&mutex_ganador);
            break;
        }
        pthread_mutex_unlock(&mutex_ganador);
        
        double nt = DELTA_T;
        
        g_mutex_lock(&d->mutex);    
        // Aplicar normalización de velocidad
        double vel_x_visual = d->vel_x * factor_normalizacion;
        double vel_y_visual = d->vel_y * factor_normalizacion;
        
        // Calcular posición con efecto espejo horizontal
        d->x = d->pos_x + vel_x_visual * d->tiempo_ini * ESCALA_VISUAL; 
        d->x = (2 * allocation.width) - d->x;  // Efecto espejo
        d->y = d->pos_y - (vel_y_visual * d->tiempo_ini - 0.5 * G * factor_normalizacion * d->tiempo_ini * d->tiempo_ini) * ESCALA_VISUAL;
        d->tiempo_ini += nt;        
        x = d->x;       
        y = d->y;       
        g_mutex_unlock(&d->mutex);

        // Verificar colisión con el suelo
        if (y >= allocation.height - 12) {
            break;
        }
        
        // Verificar colisión con barras
        for (int i = 0; i < 3; i++) {
            if (!barras[i].visible) continue;
            
            if (colisiona(x, y, barras[i])) {
                barras[i].visible = 0;

                // Verificar si el personaje estaba sobre la barra destruida
                if (!personaje_destruido &&
                    personaje_xo + ANCHO_PERSONAJE > barras[i].x &&
                    personaje_xo < barras[i].x + barras[i].ancho &&
                    personaje_yo + ALTO_PERSONAJE >= barras[i].y) {
                    
                    personaje_yo = allocation.height - ALTO_PERSONAJE;
                }
                
                salir = TRUE;
                break;
            }
        }
        
        if (salir) break;
        
        // Verificar colisión con personaje usando detección rectángulo vs rectángulo
        if (!personaje_destruido && colisiona_con_personaje(x, y)) {
            pthread_mutex_lock(&mutex_ganador);
            ganador = TRUE;
            isGanador = TRUE;
            strncpy(nombre_ganador, d->remitente, MAX_ALIAS);
            pthread_mutex_unlock(&mutex_ganador);
            
            finalizar_partida(d->remitente);
            break;
        }
        
        // Verificar salida por borde izquierdo
        if (x <= 0) {
            break;
        }
        
        usleep(SLEEP_TIME);
    }
    
    // Remover de lista y liberar memoria
    g_mutex_lock(&mutex_espejos);
    lista_espejos = g_list_remove(lista_espejos, d);
    g_mutex_unlock(&mutex_espejos);
    
    g_mutex_clear(&d->mutex);
    free(d);
    
    return NULL;
}

/**
 * Hilo para atender conexiones de clientes TCP
 */
void *atender_cliente(void *arg) {
    int socket_cliente = *(int *)arg;
    free(arg);
    
    // Leer estructura de datos del proyectil
    Datos datos_recibidos;
    if (read(socket_cliente, (char *)&datos_recibidos, sizeof(Datos)) <= 0) {
        close(socket_cliente);
        return NULL;
    }
    
    // Crear estructura para proyectil remoto
    DatosEspejo *de = malloc(sizeof(DatosEspejo));
    if (!de) {
        close(socket_cliente);
        return NULL;
    }
    
    g_mutex_init(&de->mutex);
    
    // Inicializar datos del proyectil remoto
    de->x = 0;
    de->y = 0;
    de->vel_x = datos_recibidos.vo * cos(datos_recibidos.ang);
    de->vel_y = datos_recibidos.vo * sin(datos_recibidos.ang);
    de->pos_x = datos_recibidos.xo;
    de->pos_y = datos_recibidos.yo;
    de->tiempo_ini = datos_recibidos.to;
    strncpy(de->remitente, datos_recibidos.NN, MAX_ALIAS - 1);
    de->remitente[MAX_ALIAS - 1] = '\0';
    
    // Crear hilo para animar el proyectil remoto
    pthread_t hilo_espejo;
    if (pthread_create(&hilo_espejo, NULL, animar_espejo, de) != 0) {
        g_mutex_clear(&de->mutex);
        free(de);
        close(socket_cliente);
        return NULL;
    }
    pthread_detach(hilo_espejo);
    
    close(socket_cliente);
    return NULL;
}

/**
 * Hilo principal del servidor TCP
 */
void *servidor_socket(void *arg) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        return NULL;
    }

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

    // Bucle principal del servidor
    while (servidor_activo) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (servidor_activo) {
                continue;
            }
        }
        
        // Crear hilo para atender cliente
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
 * Inicializa posiciones aleatorias de barras y personaje
 */
void inicializar_barras_y_personaje() {
    gtk_widget_get_allocation(draw1, &allocation);
    srand(time(NULL));

    // Generar barras en la mitad izquierda de la pantalla
    int baseX = rand() % (allocation.width / 2 - 3 * 70);
    for (int i = 0; i < 3; i++) {
        barras[i].ancho = 70;
        barras[i].alto = 50 + rand() % (allocation.height / 2 - 50);
        barras[i].x = baseX + i * 70;
        barras[i].y = allocation.height - barras[i].alto;
        barras[i].visible = 1;
    }

    // Posicionar personaje sobre una barra aleatoria
    int elegido = rand() % 3;
    personaje_xo = barras[elegido].x + (barras[elegido].ancho - ANCHO_PERSONAJE) / 2.0;
    personaje_yo = barras[elegido].y - ALTO_PERSONAJE;
}

/**
 * Valida los datos de entrada del usuario
 */
int validar_datos_entrada() {
    const char *vel_txt = gtk_entry_get_text(GTK_ENTRY(entryVel));
    const char *ang_txt = gtk_entry_get_text(GTK_ENTRY(entryAng));
    const char *ip_txt = gtk_entry_get_text(GTK_ENTRY(entryIP));

    if (!vel_txt || !ang_txt || !ip_txt || 
        strlen(vel_txt) == 0 || strlen(ang_txt) == 0 || strlen(ip_txt) == 0) {
        
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(window),
            GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK,
            "Error: Todos los campos son obligatorios");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return 0;
    }

    double vel = atof(vel_txt);
    double ang = atof(ang_txt);

    if (vel <= 0 || vel > 1000) {
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(window),
            GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK,
            "Error: La velocidad debe estar entre 1 y 1000");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return 0;
    }

    if (ang <= 0 || ang >= 90) {
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(window),
            GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK,
            "Error: El ángulo debe estar entre 1 y 89 grados");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return 0;
    }

    struct sockaddr_in sa;
    if (inet_pton(AF_INET, ip_txt, &(sa.sin_addr)) != 1) {
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(window),
            GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK,
            "Error: La dirección IP no es válida");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return 0;
    }

    return 1;
}

/**
 * Callback para el botón de disparo
 */
void on_btnDisparar_clicked() {
    if (personaje_destruido) {
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(window),
            GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_INFO,
            GTK_BUTTONS_OK,
            "Personaje destruido. Debes reiniciar la aplicación.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    if (tiro_en_progreso) {
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(window),
            GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_WARNING,
            GTK_BUTTONS_OK,
            "Ya hay un tiro en progreso. Espera a que termine.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    if (!validar_datos_entrada()) {
        return;
    }

    tiro_en_progreso = 1;
    gtk_widget_set_sensitive(btnDisparar, FALSE);

    pthread_t hilo;
    if (pthread_create(&hilo, NULL, animar_tiro, NULL) != 0) {
        tiro_en_progreso = 0;
        gtk_widget_set_sensitive(btnDisparar, TRUE);
    } else {
        pthread_detach(hilo);
    }
}

/**
 * Callback para el botón "Acerca de"
 */
void on_btnAcercaDe_clicked() {
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(window),
        GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_INFO,
        GTK_BUTTONS_OK,
        "Tiro Parabólico - Sistemas de Cómputo Paralelo y Distribuido\n"
        "Tercer Parcial 2025\n"
        "Dr. J. Jesús Arellano Pimentel");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/**
 * Callback para el botón de salir
 */
void on_btnSalir_clicked() { 
    servidor_activo = 0;
    gtk_main_quit(); 
}

/**
 * Callback para el cierre de ventana
 */
void on_window_destroy() { 
    servidor_activo = 0;
    gtk_main_quit(); 
}
