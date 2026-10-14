/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Donkey Kong
  *
  * MECANICAS INCLUIDAS:
  * - Escenario procedural
  * - 2 jugadores por USART3 desde ESP32
  * - Bloqueo inicial hasta que DK libera el primer barril
  * - Barriles marrones y azules
  * - Barril de aceite fijo
  * - Barril azul + aceite = nueva flama
  * - Maximo de flamas simultaneas
  * - Flamas con IA por decisiones temporizadas
  * - Objetivo J1/J2 persistente
  * - Persecucion horizontal
  * - Patrullaje
  * - Uso de escaleras completas
  * - Evita reutilizar inmediatamente la misma escalera
  * - Si el jugador esquiva la flama, esta NO gira instantaneamente:
  *   mantiene su decision hasta el siguiente ciclo de IA
  * - La flama sale saltando fisicamente desde el barril de aceite
  * - Render por regiones finales para reducir/eliminar parpadeo de barriles
  * - 2 martillos con duración de 8.5 segundos
  * - golpe arriba/frente que destruye barriles y flamas
  * - estados de sprite preparados para caminar y saltar con martillo
  * - sprites RGB565 para martillos y enemigos de fuego
  * - musica del martillo enviada a la segunda STM32 por USART3_TX
  *
  * NOTA:
  * Los jugadores y barriles mantienen su representacion original.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"                                                       // HAL, periféricos y pines definidos por CubeMX.

/* USER CODE BEGIN Includes */
#include "ili9341.h"                                                    // Funciones para controlar la TFT ILI9341.
#include "game_sprites.h"                                               // Sprites principales de Mario, Luigi y Peach.
#include "retro_sprites.h"                                              // Sprites de barriles, flamas, muerte, puntos y efectos.
#include "sd_store.h"                                                   // Funciones de microSD, menús y puntuaciones.
#include <stdint.h>                                                     // Tipos enteros de tamaño fijo: uint8_t, int16_t, uint32_t, etc.
#include <string.h>                                                     // Funciones de memoria usadas por memset() y memcmp().
/* USER CODE END Includes */


/* ============================================================
 * MAPA GENERAL DEL CÓDIGO
 * Ruta general: datos -> constantes -> variables -> funciones -> física -> IA -> UART -> main -> periféricos.
 * ============================================================ */


/* USER CODE BEGIN PTD */

/* ============================================================
 * 1. TIPOS DE DATOS Y ESTADOS
 *
 * Se crean las plantillas que describen cajas de colisión, plataformas, escaleras, jugadores, barriles, flamas y martillos.
 * Los enum indican los estados posibles; todavía no ejecutan movimiento ni física.
 * ============================================================ */

typedef struct {                                                        // Caja rectangular usada para detectar colisiones.
    int16_t x, y, w, h;                                                 // Posición (x,y) y tamaño (w,h).
} AABB;                                                                 // Tipo de caja usado por las funciones de colisión.

typedef struct {                                                        // Datos de una viga inclinada del escenario.
    int16_t x1, y1, x2, y2;                                             // Punto inicial (x1,y1) y punto final (x2,y2); si y1 != y2, la viga queda inclinada.
} Plataforma;                                                           // Tipo usado para guardar cada viga del nivel.

typedef struct {                                                        // Datos de una escalera del escenario.
    int16_t x, y1, y2;                                                  // x fija su posición horizontal y y1,y2 indican hasta dónde llega verticalmente.
} Escalera;                                                             // Tipo usado para guardar cada escalera del nivel.

typedef enum {                                                          // Apariencias posibles del jugador.
    SPRITE_JUGADOR_IDLE = 0,                                            // Sin martillo: quieto sobre una plataforma.
    SPRITE_JUGADOR_CAMINANDO,                                           // Sin martillo: caminando a izquierda o derecha.
    SPRITE_JUGADOR_SALTANDO,                                            // Sin martillo: en el aire, subiendo o cayendo.
    SPRITE_JUGADOR_ESCALERA,                                            // Sin martillo: subiendo o bajando una escalera.

    SPRITE_MARTILLO_IDLE_ARRIBA,                                        // Con martillo: quieto y martillo levantado.
    SPRITE_MARTILLO_IDLE_FRENTE,                                        // Con martillo: quieto y martillo al frente.
    SPRITE_MARTILLO_CAMINANDO_ARRIBA,                                   // Con martillo: caminando y martillo levantado.
    SPRITE_MARTILLO_CAMINANDO_FRENTE,                                   // Con martillo: caminando y martillo al frente.
    SPRITE_MARTILLO_SALTANDO_ARRIBA,                                    // Con martillo: en el aire y martillo levantado.
    SPRITE_MARTILLO_SALTANDO_FRENTE                                     // Con martillo: en el aire y martillo al frente.
} EstadoSpriteJugador;                                                  // Estado visual usado para elegir el sprite del jugador.

typedef struct {                                                        // Datos de Mario o Luigi durante la partida.
    int16_t x, y, w, h;                                                 // Posición (x,y) y tamaño (w,h).
    int16_t vy;                                                         // Velocidad vertical; negativa sube y positiva baja.
    char direccion;                                                     // Dirección recibida: L, R, U, D o N.
    char ultimoComandoRX;                                               // Último comando recibido; evita repetir eventos como el salto.
    uint8_t saltoSolicitado;                                            // 1 cuando hay un salto pendiente.
    uint8_t enSuelo;                                                    // 1 cuando el jugador está apoyado en una plataforma.
    int8_t escaleraActual;                                              // Escalera actual; -1 significa ninguna.
    int8_t escaleraSalida;                                              // Última escalera abandonada; evita reentrar mientras se mantiene U/D.
    int8_t plataformaActual;                                            // Plataforma actual; -1 significa ninguna.
    uint16_t color;                                                     // Color normal o rojo temporal cuando recibe daño.
    uint32_t ultimoPaquete;                                             // Tick en que llegó el último comando del ESP32; permite aplicar timeout de seguridad.

    /* Dirección visual: 0 = izquierda, 1 = derecha. */
    uint8_t mirandoDerecha;                                             // 0 mira a la izquierda; 1 mira a la derecha.

    /* Mecánica del martillo. */
    uint8_t martilloActivo;                                             // 1 mientras el jugador tiene martillo.
    uint8_t faseMartillo;                                               // 0 = martillo arriba; 1 = martillo al frente.
    int8_t martilloOrigen;                                              // Índice del martillo recogido; -1 significa ninguno.
    uint32_t martilloHasta;                                             // Tick en que termina el martillo.
    uint32_t ultimoGolpeMartillo;                                       // Tick usado para alternar martillo arriba/frente.
    uint32_t golpePausaHasta;                                           // Tick en que termina la pausa tras romper un enemigo.
    int16_t golpeImpactoX, golpeImpactoY;                               // golpeImpactoX/Y guardan dónde debe aparecer el efecto visual del impacto.

    /* Estado preparado para los sprites finales. */
    EstadoSpriteJugador estadoSprite;                                   // Sprite que corresponde al estado actual del jugador.
    uint8_t frameAnimacion;                                             // Frame actual de la animación.
    uint32_t ultimoFrameAnimacion;                                      // Tick de la última actualización de animación.
    uint8_t vidas;                                                      // Vidas restantes del jugador.
    uint8_t muriendo;                                                   // 1 mientras se reproduce la animación de muerte.
    uint32_t muerteDesde;                                               // Tick en que comenzó la animación de muerte.
    uint8_t muerteCuadro;                                               // Cuadro actual de la animación de muerte.
    uint32_t puntos;                                                    // Puntuación acumulada del jugador.
    uint32_t invulnerableHasta;                                         // Hasta este tick no puede recibir otro golpe.
    uint32_t letreroHasta;                                              // Tick hasta el que permanece visible el popup de puntos.
    uint8_t letreroVisible;                                             // 1 mientras se muestra +100, +200 o +300.
    uint16_t letreroValor;                                              // Puntos mostrados: 100, 200 o 300.
    uint8_t letreroFijo;                                                // 1 fija el popup en el punto donde se dieron los puntos.
    int16_t letreroX, letreroY;                                         // letreroX/Y guardan la posición fija del popup de puntos.
} Jugador;                                                              // Plantilla usada por jugador1 y jugador2.

typedef enum {                                                          // Estados que tiene DK durante el lanzamiento.
    DK_IDLE = 0,                                                        // DK está esperando el momento del siguiente lanzamiento.
    DK_LANZANDO                                                         // DK está reproduciendo la secuencia de frames de lanzamiento.
} EstadoDK;                                                             // Estado actual de DK.

typedef enum {                                                          // Tipos de barril del juego.
    BARRIL_MARRON = 0,                                                  // Barril normal que recorre el escenario.
    BARRIL_AZUL                                                         // Barril especial capaz de activar el aceite y generar una flama.
} TipoBarril;                                                           // Tipo que distingue barriles marrones y azules.

typedef enum {                                                          // Estados de movimiento de un barril.
    BARRIL_INACTIVO = 0,                                                // Slot libre: el barril no existe en pantalla.
    BARRIL_RODANDO,                                                     // Barril apoyado sobre una viga y desplazándose horizontalmente.
    BARRIL_CAYENDO,                                                     // Barril en caída libre entre plataformas.
    BARRIL_REBOTANDO,                                                   // Barril acaba de tocar una plataforma y ejecuta un pequeño rebote.
    BARRIL_ESCALERA,                                                    // Barril descendiendo verticalmente por una escalera.
    BARRIL_DIRECTO,                                                     // Barril especial que cae atravesando algunos contactos y persigue horizontalmente.
    BARRIL_A_ACEITE                                                     // Primer barril azul: vuelo directo hacia el aceite.
} EstadoBarril;                                                         // Estado que decide la física del barril.

typedef struct {                                                        // Datos y física de cada barril.
    int16_t x, y, w, h;                                                 // Posición (x,y) y tamaño (w,h).
    int16_t vy;                                                         // Velocidad vertical; negativa sube y positiva baja.
    int8_t direccion;                                                   // Sentido horizontal: -1 = izquierda, +1 = derecha; 0 se usa en casos especiales.
    int8_t plataformaActual;                                            // Plataforma actual; -1 significa ninguna.
    int8_t escaleraActual;                                              // Escalera actual; -1 significa ninguna.
    int8_t ultimaEscaleraEvaluada;                                      // Última escalera evaluada por el barril; evita repetir la decisión.
    uint8_t activo;                                                     // 1 si este barril está activo; 0 si el slot está libre.
    uint8_t contactosIgnorar;                                           // Plataformas que un barril directo debe atravesar.
    uint8_t vueloAceitePaso;                                            // Avance del primer barril azul hacia el aceite.
    TipoBarril tipo;                                                    // Marrón o azul.
    EstadoBarril estado;                                                // Movimiento actual del barril.
    uint16_t color;                                                     // Color asociado al tipo de barril.
    uint8_t saltado[2];                                                 // Evita dar dos veces los +100 a J1 o J2 por el mismo barril.
} Barril;                                                               // Plantilla usada por el arreglo de barriles.


/* ============================================================
 * ESTADOS DEL BARRIL DE ACEITE
 *
 * Se definen los dos estados del aceite: apagado y encendido.
 * El aceite cambia a encendido cuando el primer barril azul llega a él.
 * ============================================================ */
typedef enum {                                                          // Estados del barril de aceite.
    ACEITE_APAGADO = 0,                                                 // Barril de aceite aún sin llama.
    ACEITE_ENCENDIDO                                                    // Barril de aceite encendido después del primer barril azul.
} EstadoBarrilAceite;                                                   // Indica si el aceite está apagado o encendido.


/* ============================================================
 * ESTADOS DE LAS FLAMAS
 *
 * Se definen los estados que controlan el comportamiento de cada flama.
 * Una flama puede nacer, salir del aceite, patrullar, perseguir, usar una escalera o recuperarse.
 * ============================================================ */
typedef enum {                                                          // Estados de movimiento e IA de una flama.
    FLAMA_INACTIVA = 0,                                                 // Slot libre del arreglo de flamas.
    FLAMA_NACIENDO,                                                     // Flama creada pero todavía oculta dentro del barril de aceite.
    FLAMA_SALIENDO_ACEITE,                                              // Flama realizando el salto físico para salir del aceite.
    FLAMA_PATRULLANDO,                                                  // Flama avanza sin corregir continuamente hacia un jugador.
    FLAMA_PERSIGUIENDO,                                                 // Flama intenta acercarse al jugador objetivo.
    FLAMA_ESCALERA,                                                     // Flama desplazándose verticalmente por una escalera.
    FLAMA_RECUPERANDO                                                   // Estado temporal tras aterrizar, tocar un borde o terminar una escalera.
} EstadoFlama;                                                          // Estado que decide el comportamiento de la flama.

typedef struct {                                                        // Datos, movimiento e IA de cada flama.
    int16_t x, y, w, h;                                                 // Posición (x,y) y tamaño (w,h).

    /* Velocidad usada únicamente durante el salto desde el aceite */
    int16_t vx;                                                         // Velocidad horizontal al salir del aceite.
    int16_t vy;                                                         // Velocidad vertical; negativa sube y positiva baja.

    /* -1 izquierda, +1 derecha */
    int8_t direccion;                                                   // Sentido horizontal: -1 = izquierda, +1 = derecha; 0 se usa en casos especiales.

    /* -1 subir, +1 bajar */
    int8_t direccionVertical;                                           // -1 sube; +1 baja por una escalera.

    int8_t plataformaActual;                                            // Plataforma actual; -1 significa ninguna.
    int8_t plataformaDestino;                                           // Plataforma a la que la flama quiere llegar.
    int8_t escaleraActual;                                              // Escalera actual; -1 significa ninguna.
    int8_t ultimaEscalera;                                              // Última escalera usada por la flama.
    int8_t escaleraObjetivo;                                            // Escalera hacia la que se dirige la flama.

    uint8_t activo;                                                     // 1 si esta flama está activa; 0 si el slot está libre.
    uint8_t jugadorObjetivo;                                            // 1 = J1; 2 = J2.

    EstadoFlama estado;                                                 // Comportamiento actual de la flama.
    uint16_t color;                                                     // Color base de la flama.

    uint32_t tiempoEstado;                                              // Tick en que comenzó el estado actual.
    uint32_t proximaDecision;                                           // Tick en que la IA puede volver a decidir.
    uint32_t bloqueoEscaleraHasta;                                      // Hasta este tick no reutiliza la última escalera.
} Fireball;                                                             // Plantilla usada por el arreglo de flamas.


/* ============================================================
 * MARTILLOS - DATOS Y ESTADOS
 *
 * Se definen los estados y datos que necesita cada martillo: posición, plataforma, jugador dueño y disponibilidad.
 * La lógica para recogerlo, usarlo y golpear enemigos aparece más adelante.
 * ============================================================ */
typedef enum {                                                          // Estados de un martillo.
    MARTILLO_DISPONIBLE = 0,                                            // Pickup visible y disponible para ser recogido.
    MARTILLO_TOMADO,                                                    // Pickup ya pertenece a uno de los jugadores.
    MARTILLO_AGOTADO                                                    // Power-up consumido; el pickup no debe reaparecer en esta ronda.
} EstadoMartillo;                                                       // Indica si el martillo está disponible, tomado o agotado.

typedef struct {                                                        // Datos de cada martillo del escenario.
    int16_t x, y, w, h;                                                 // Posición (x,y) y tamaño (w,h).
    int8_t plataforma;                                                  // Plataforma donde está colocado el martillo.
    uint8_t jugador;                                                    // 0 = ninguno; 1 = J1; 2 = J2.
    EstadoMartillo estado;                                              // Disponible, tomado o agotado.
    uint16_t color;                                                     // Color base del martillo.
} Martillo;                                                             // Plantilla usada por los dos martillos.

typedef enum {                                                          // Música activa durante la partida.
    MUSICA_NORMAL = 0,                                                  // Debe sonar el tema normal del nivel.
    MUSICA_MARTILLO                                                     // Debe sonar la música especial mientras existe un martillo activo.
} EstadoMusicaJuego;                                                    // Indica si suena música normal o de martillo.

/* USER CODE END PTD */


/* USER CODE BEGIN PD */

/* ============================================================
 * 2. CONSTANTES Y AJUSTES DEL JUEGO
 *
 * Se fijan tamaños, velocidades, tiempos, colores, límites y posiciones que usa toda la partida.
 * Estos valores permiten ajustar el juego sin cambiar la lógica de las funciones.
 * ============================================================ */

/* Pantalla */
#define SCREEN_W                         320                            // Ancho lógico de la TFT en píxeles.
#define SCREEN_H                         240                            // Alto lógico de la TFT en píxeles.

/* Escenario */
#define VIGA_GROSOR                        4                            // Grosor visual de cada viga/plataforma.
#define VIGA_SEGMENTO                      8                            // Longitud de cada tramo usado para dibujar el patrón diagonal de la viga.
#define ESCALERA_W                         8                            // Ancho lógico de las escaleras.

/* Colores RGB565 */
#define COLOR_FONDO                    0x0000                           // Color RGB565 del fondo negro.
#define COLOR_VIGA                     0xF800                           // Color principal de las vigas.
#define COLOR_VIGA_CLARO               0xF81F                           // Tono claro usado para dar volumen a las vigas.
#define COLOR_VIGA_OSCURO              0x7800                           // Tono oscuro usado para sombrear las vigas.
#define COLOR_ESCALERA                 0x07FF                           // Color RGB565 de las escaleras.

#define COLOR_J1                       0xFFFF                           // Color base asociado al jugador 1.
#define COLOR_J2                       0xFFE0                           // Color base asociado al jugador 2.
#define COLOR_GOLPE                    0xF800                           // Color temporal usado para indicar que un jugador recibió daño.

#define COLOR_BARRIL_MARRON            0xA145                           // Color lógico del barril marrón.
#define COLOR_BARRIL_AZUL              0x001F                           // Color lógico del barril azul.

/* Aceite */
#define ACEITE_X                           50                           // Coordenada X fija del barril de aceite.
#define ACEITE_W                           14                           // Ancho del barril de aceite.
#define ACEITE_H                           18                           // Alto del barril de aceite.
#define COLOR_ACEITE                   0x001F
#define COLOR_ACEITE_BORDE             0xFFFF

#define ACEITE_FUEGO_H                       7                          // Altura extra reservada para la llama sobre el aceite encendido.
#define COLOR_ACEITE_FUEGO               0xFD20
#define COLOR_ACEITE_FUEGO_CENTRO        0xFFE0

/* Flamas */
#define MAX_FIREBALLS                       4                           // Límite de flamas simultáneas para controlar dificultad y memoria.
#define FIREBALL_W                         10                           // Ancho del sprite/caja de una flama.
#define FIREBALL_H                         12                           // Alto del sprite/caja de una flama.
#define COLOR_FIREBALL                 0xFD20

#define VELOCIDAD_FIREBALL                  1                           // Desplazamiento horizontal por paso de una flama.
#define VELOCIDAD_ESCALERA_FIREBALL         1                           // Desplazamiento vertical por paso al usar una escalera.
#define MOVIMIENTO_FIREBALL_MS             40                           // Periodo mínimo entre pasos de movimiento de las flamas.

#define FIREBALL_NACER_MS                 300                           // Tiempo que una nueva flama permanece oculta dentro del aceite.

#define FIREBALL_SALIDA_VX                  1                           // Velocidad horizontal de salida desde el aceite.
#define FIREBALL_SALIDA_VY                 -4                           // Velocidad vertical inicial negativa que impulsa la flama hacia arriba.
#define FIREBALL_GRAVEDAD_SALIDA             1                          // Aceleración vertical aplicada durante la salida parabólica.
#define FIREBALL_MAX_CAIDA_SALIDA            3                          // Límite de velocidad de caída durante el salto desde el aceite.

#define FIREBALL_DECISION_MS              600                           // Tiempo entre decisiones de IA para evitar cambios instantáneos de dirección.
#define FIREBALL_RECUPERAR_MS             350                           // Tiempo de recuperación antes de volver a tomar una decisión.
#define FIREBALL_BLOQUEO_ESCALERA_MS      500                           // Tiempo durante el cual la misma escalera queda bloqueada después de usarla.
#define FIREBALL_TOL_ESCALERA                3                          // Tolerancia horizontal para considerar que la flama llegó al centro de una escalera.

/* Probabilidades de IA */
#define FIREBALL_PROB_PERSEGUIR             70                          // Probabilidad porcentual de perseguir al jugador cuando está en el mismo nivel.
#define FIREBALL_PROB_ESCALERA              60                          // Probabilidad porcentual de decidir usar una escalera disponible.
#define FIREBALL_PROB_CAMBIAR_OBJETIVO      25                          // Probabilidad de reconsiderar qué jugador es el objetivo.
#define FIREBALL_PROB_GIRAR_ALEATORIO       10                          // Probabilidad de invertir dirección durante patrullaje.

/* Buffer compacto de fondo */
#define BG_NEGRO                            0                           // Índice compacto usado en la paleta del buffer de fondo.
#define BG_VIGA                             1                           // Índice compacto usado en la paleta del buffer de fondo.
#define BG_VIGA_CLARO                       2                           // Índice compacto usado en la paleta del buffer de fondo.
#define BG_VIGA_OSCURO                      3                           // Índice compacto usado en la paleta del buffer de fondo.
#define BG_ESCALERA                         4                           // Índice compacto usado en la paleta del buffer de fondo.

#define BG_BUFFER_SIZE ((SCREEN_W * SCREEN_H + 1) / 2)                  // Cantidad de bytes del fondo comprimido: se almacenan dos índices de 4 bits por byte.

/* DK */
#define DK_X                               70                           // Posición X fija de Donkey Kong.
#define DK_Y                               38                           // Posición Y fija de Donkey Kong.
#define DK_W                               45                           // Ancho de cada frame de Donkey Kong.
#define DK_H                               35                           // Alto de cada frame de Donkey Kong.
#define DK_FRAMES                           8                           // Cantidad total de frames en la spritesheet de DK.
#define DK_FRAME_IDLE                       0                           // Índice del frame de reposo.
#define DK_FRAME_LIBERA_BARRIL              7                           // Frame exacto en el que la animación crea/libera un barril.
#define DK_TRANSPARENTE                 0x0000                          // Valor RGB565 tratado como transparente en la spritesheet.
#define DK_TIEMPO_FRAME                   260                           // Duración de cada frame de la animación de DK.
#define PILA_BARRILES_CANTIDAD              3                           // Cantidad de barriles decorativos apilados junto a DK.

/* Jugadores */
#define JUGADOR_W                          12                           // Ancho de la caja/sprite del jugador.
#define JUGADOR_H                          12                           // Alto de la caja/sprite del jugador.
#define VELOCIDAD_X                         2                           // Velocidad horizontal normal en suelo.
#define VELOCIDAD_X_SALTO                   3                           // Velocidad horizontal mientras el jugador está en el aire.
#define VELOCIDAD_ESCALERA                  1                           // Velocidad vertical del jugador en escaleras.
#define MOVIMIENTO_JUGADOR_MS              16                           // Periodo entre pasos de desplazamiento horizontal/vertical del jugador.
#define TOLERANCIA_EXTREMO_ESCALERA         6                           // Margen en píxeles para detectar entrada/salida de escalera.

#define GRAVEDAD                            1                           // Incremento de velocidad vertical por actualización física.
#define MAX_CAIDA                           5                           // Velocidad máxima de caída del jugador.
#define VELOCIDAD_SALTO                    -6                           // Velocidad vertical inicial negativa del salto.
#define TOLERANCIA_SUELO                    4                           // Margen para decidir si los pies están sobre una plataforma.
#define FISICA_MS                          16                           // Periodo base del ciclo de física.
#define CONTROL_TIMEOUT_MS                250                           // Si no llegan comandos dentro de este tiempo, se fuerza dirección neutral.
#define VIDAS_INICIALES                     3                           // Número de vidas de cada jugador al iniciar.
#define MUERTE_DURACION_MS                1400U                         // Duración total del estado/animación de muerte.
#define MUERTE_FRAME_MS                   220U                          // Duración de cada cuadro de la animación de muerte.
#define GOLPE_PAUSA_MS                     140U                         // Pausa breve al destruir un enemigo con martillo.
#define RESCATE_DURACION_MS              1200U                          // Tiempo que se mantiene la escena de rescate antes del final.
#define FIN_MINIMA_MS                    2600U                          // Tiempo mínimo que debe permanecer visible la pantalla final.
#define CORAZON_X (PEACH_X + (PEACH_W - CORAZON_W) / 2)                 // Coordenada calculada para centrar el corazón sobre Peach.
#define CORAZON_Y (PEACH_Y - CORAZON_H - 3)                             // Coordenada calculada para centrar el corazón sobre Peach.
#define INVULNERABLE_MS                  1200U                          // Tiempo de invulnerabilidad después de reaparecer o recibir daño.
#define PUNTOS_SALTO_BARRIL               100U                          // Puntos otorgados por saltar correctamente un barril.
#define PUNTOS_RESCATE                    500U                          // Puntos extra por rescatar a Peach.
#define PUNTOS_POPUP_MS                   700U                          // Duración del letrero flotante de puntos.
#define PEACH_W                             15                          // Dimensión o posición usada para el sprite de Peach.
#define PEACH_H                             20                          // Dimensión o posición usada para el sprite de Peach.
#define PEACH_X                            146                          // Dimensión o posición usada para el sprite de Peach.
#define PEACH_Y                             (49 - PEACH_H)              // Dimensión o posición usada para el sprite de Peach.
#define HELP_X                             190                          // Posición del sprite/indicador de ayuda.
#define HELP_Y                              26                          // Posición del sprite/indicador de ayuda.

/* Martillos */
#define MAX_MARTILLOS                       2                           // Cantidad de pickups de martillo presentes en el nivel.

#define MARTILLO_W                          8                           // Ancho del martillo.
#define MARTILLO_H                         12                           // Alto del martillo.

#define MARTILLO_SEPARACION_PLATAFORMA     13                           // Separación vertical del pickup respecto de la plataforma para obligar a saltar.

#define MARTILLO_DURACION_MS             8500                           // Duración del power-up de martillo: 8.5 segundos.
#define MARTILLO_GOLPE_MS                 180                           // Periodo con el que alterna la fase del martillo arriba/frente.

#define MARTILLO_GOLPE_ARRIBA_H            13                           // Altura de la caja de ataque sobre el jugador.
#define MARTILLO_GOLPE_FRENTE_W            13                           // Ancho de la caja de ataque frontal.
#define MARTILLO_GOLPE_FRENTE_H            10                           // Alto de la caja de ataque frontal.
#define MARTILLO_RENDER_PADDING             18                          // Margen extra redibujado alrededor del jugador para cubrir el martillo.

/* Colores provisionales del pickup */
#define COLOR_MARTILLO_CABEZA           0xF800
#define COLOR_MARTILLO_CABEZA_CENTRO    0xFFFF
#define COLOR_MARTILLO_MANGO            0xA145

/* Se conserva para el martillo que lleva el jugador */
#define COLOR_MARTILLO                  0xFFE0
#define COLOR_MARTILLO_ACTIVO           0xFD20

/* Barriles */
#define MAX_BARRILES                        8                           // Cantidad máxima de slots reservados para barriles.
#define MAX_BARRILES_SIMULTANEOS            5                           // Límite de barriles que DK puede mantener activos al mismo tiempo.
#define BARRIL_W                            8                           // Ancho del barril.
#define BARRIL_H                            8                           // Alto del barril.

#define VELOCIDAD_BARRIL                    1                           // Desplazamiento horizontal del barril por paso.
#define MOVIMIENTO_BARRIL_MS               30                           // Periodo entre pasos de movimiento de barril.
#define VELOCIDAD_ESCALERA_BARRIL           1                           // Velocidad de descenso del barril por una escalera.
#define VELOCIDAD_DIRECTA_X                 1                           // Velocidad horizontal de los barriles lanzados directamente.

#define GRAVEDAD_BARRIL                     1                           // Aceleración vertical aplicada a barriles en el aire.
#define MAX_CAIDA_BARRIL                    4                           // Velocidad vertical máxima de barriles normales.
#define MAX_CAIDA_DIRECTO                   3                           // Velocidad vertical máxima de un barril en modo directo.
#define VELOCIDAD_REBOTE                    -2                          // Velocidad vertical negativa que inicia el rebote.
#define DIFICULTAD_BARRILES                 2                           // Parámetro usado para modificar probabilidades e intervalo de lanzamiento.

#define BARRIL_SALIDA_X (DK_X + DK_W - 5)                               // Coordenada inicial donde DK libera el barril.
#define BARRIL_SALIDA_Y (DK_Y + 20)                                     // Coordenada inicial donde DK libera el barril.

/* UART */
#define UART_RX_BUFFER_SIZE                64                           // Tamaño del buffer circular de recepción USART3.

/* USER CODE END PD */


/* Private variables ---------------------------------------------------------*/
SPI_HandleTypeDef hspi1;                                                // hspi1: Manejador HAL de SPI1, utilizado por las rutinas de microSD.
UART_HandleTypeDef huart3;                                              // huart3: Manejador HAL de USART3; recibe controles del ESP32 por PC11 y transmite audio por PC10.


/* USER CODE BEGIN PV */

/* ============================================================
 * 3. VARIABLES Y OBJETOS DE LA PARTIDA
 *
 * Se crean los datos que sí existen durante la ejecución: J1, J2, barriles, flamas, martillos, DK, temporizadores y estados de pantalla.
 * También se reservan buffers para el fondo, la microSD y la comunicación UART.
 * ============================================================ */

/* Tres barriles apilados a la izquierda de DK, apoyados en el piso 1. */
static const int16_t pilaBarrilesX[PILA_BARRILES_CANTIDAD] = {54, 54, 54}; // pilaBarrilesX: Coordenadas X de los tres barriles decorativos apilados junto a DK.
static const int16_t pilaBarrilesY[PILA_BARRILES_CANTIDAD] = {65, 57, 49}; // pilaBarrilesY: Coordenadas Y de los tres barriles decorativos apilados junto a DK.

/* Plataformas */
static const Plataforma plataformas[] = {                               // plataformas: Tabla inmutable con los extremos de todas las vigas del nivel.
    {141,  49, 221,  49},
    { 51,  73, 222,  78},
    { 63, 108, 246, 102},
    { 55, 131, 230, 138},
    { 71, 168, 246, 163},
    { 55, 191, 262, 199},
    { 47, 226, 285, 221}
};
#define NUM_PLATAFORMAS (sizeof(plataformas) / sizeof(plataformas[0]))

/* Escaleras */
static const Escalera escaleras[] = {                                   // escaleras: Tabla inmutable con la posición y extremos de todas las escaleras transitables.
    /* Piso de Peach: una escalera justo a su derecha. */
    {166,  49,  76},
    { 90,  74, 107},
    {212,  78, 103},
    { 70, 108, 132},
    {188, 104, 136},
    {100, 133, 167},
    {222, 138, 164},
    {148, 166, 195},
    { 80, 192, 225},
    {250, 199, 222}
};
#define NUM_ESCALERAS (sizeof(escaleras) / sizeof(escaleras[0]))

/* A la izquierda de Peach, fuera de la viga y separada de DK. No transitable. */
static const Escalera escaleraDecorativaPeach = {132, 14, 49};          // escaleraDecorativaPeach: Escalera dibujada junto a Peach que no participa en la lógica de tránsito.

/* Fondo */
static uint8_t fondoBuffer[BG_BUFFER_SIZE];                             // fondoBuffer: Buffer comprimido del escenario estático; usa 4 bits por píxel para ahorrar RAM.
static const uint16_t fondoPaleta[] = {                                 // fondoPaleta: Paleta que traduce los índices del fondo compacto a colores RGB565.
    COLOR_FONDO,
    COLOR_VIGA,
    COLOR_VIGA_CLARO,
    COLOR_VIGA_OSCURO,
    COLOR_ESCALERA
};

/* DK */
extern const uint16_t dkongAnim[];                                          // Accede a la posición correspondiente del arreglo dkongAnim.

EstadoDK estadoDK = DK_IDLE;                                            // estadoDK: Estado actual de la máquina de estados de Donkey Kong.
uint8_t dkFrame = DK_FRAME_IDLE;                                        // dkFrame: Frame actual de la animación de DK.
uint32_t dkUltimoCambio = 0;                                            // dkUltimoCambio: Tick del último cambio de frame de DK.
uint32_t dkProximoLanzamiento = 0;                                      // dkProximoLanzamiento: Tick programado para iniciar el próximo lanzamiento.
uint8_t dkBarrilLiberado = 0;                                           // dkBarrilLiberado: Evita crear más de un barril dentro de una misma animación de lanzamiento.

TipoBarril dkTipoPendiente = BARRIL_MARRON;                             // dkTipoPendiente: Tipo de barril seleccionado para el próximo lanzamiento.
uint8_t dkDirectoPendiente = 0;                                         // dkDirectoPendiente: Marca si el próximo barril usará el modo de lanzamiento directo.
uint8_t dkContactosPendientes = 0;                                      // dkContactosPendientes: Cantidad de contactos/plataformas que el próximo barril directo deberá ignorar.
uint8_t primerAzulLanzado = 0;                                          // primerAzulLanzado: Registra si ya se lanzó el primer barril azul obligatorio de la ronda.

/* Bloqueo inicial */
uint8_t permitirMovimientoJugadores = 0;                                // permitirMovimientoJugadores: Bloquea los controles hasta que DK libere el primer barril.
uint8_t primerBarrilLiberado = 0;                                       // primerBarrilLiberado: Marca si ya ocurrió el evento que habilita el movimiento.

/* Jugadores */
Jugador jugador1;                                                       // jugador1: Estado completo del jugador 1 (Mario).
Jugador jugador2;                                                       // jugador2: Estado completo del jugador 2 (Luigi).

/* Martillos */
Martillo martillos[MAX_MARTILLOS];                                      // martillos: Arreglo con los dos pickups de martillo.
EstadoMusicaJuego musicaActual =                                            // Actualiza musicaActual.
    MUSICA_NORMAL;                                                      // musicaActual: Estado lógico de la música que debería estar reproduciéndose.

/* Barriles */
Barril barriles[MAX_BARRILES];                                          // barriles: Pool fijo de barriles; evita asignación dinámica de memoria.

/* Flamas */
Fireball fireballs[MAX_FIREBALLS];                                      // fireballs: Pool fijo de flamas activas/inactivas.
uint8_t fireballsActivos = 0;                                           // fireballsActivos: Contador de flamas actualmente activas.
uint8_t cuadroFlamaActual = 0;                                          // cuadroFlamaActual: Frame global de animación usado por las flamas.


/* ============================================================
 * BARRIL DE ACEITE - ESTADO GLOBAL
 *
 * Se guarda si el aceite está apagado o encendido y si su zona necesita redibujarse.
 * Este estado se comparte durante toda la partida.
 * ============================================================ */
EstadoBarrilAceite estadoAceite =                                           // Actualiza estadoAceite.
    ACEITE_APAGADO;                                                     // estadoAceite: Estado apagado/encendido del barril de aceite.

/*
 * Bandera para redibujar toda la zona visual del aceite cuando
 * cambia de apagado a encendido.
 */
uint8_t aceiteNecesitaRedibujar =                                           // Calcula y guarda aceiteNecesitaRedibujar.
    0;                                                                  // Solicita redibujar la zona del aceite cuando cambia de estado.


/* Relojes */
uint32_t ultimoTickFisica = 0;                                          // ultimoTickFisica: Último tick en que se ejecutó el bloque base de física.
uint32_t ultimoMovimientoJugador = 0;                                   // ultimoMovimientoJugador: Último tick en que se permitió un paso de movimiento de jugador.
uint32_t ultimoMovimientoBarril = 0;                                    // ultimoMovimientoBarril: Último tick en que se permitió un paso de barril.
uint32_t ultimoMovimientoFireball = 0;                                  // ultimoMovimientoFireball: Último tick en que se permitió un paso de flama.

/* Golpes */
uint32_t golpeJ1Hasta = 0;                                              // golpeJ1Hasta: Fin del efecto visual temporal de daño de J1.
uint32_t golpeJ2Hasta = 0;                                              // golpeJ2Hasta: Fin del efecto visual temporal de daño de J2.

typedef enum { JUEGO_MENU, JUEGO_ACTIVO, JUEGO_RESCATE, JUEGO_FIN } EstadoPantalla;// Pantallas principales: menú, partida, rescate y final.
static EstadoPantalla pantallaActual = JUEGO_MENU;                      // pantallaActual: Estado actual de la interfaz: menú, juego, rescate o final.
static uint8_t comenzarSolicitado = 0, ganador = 0;                     // comenzarSolicitado: Bandera puesta por el botón de salto para iniciar o continuar desde una pantalla.
static uint8_t avisoFinalVisible = 0;                                   // avisoFinalVisible: Estado de parpadeo del mensaje de la pantalla final.
static uint8_t ayudaVisible = 0, hudSucio = 0;                          // ayudaVisible: Estado de parpadeo del sprite de ayuda.
static uint32_t finDesde = 0;                                           // finDesde: Tick en que empezó la pantalla final.
static uint32_t rescateDesde = 0;                                       // rescateDesde: Tick en que empezó la secuencia de rescate.
static uint8_t corazonVisible = 0, finPorRescate = 0;                   // corazonVisible: Controla el parpadeo del corazón durante el rescate.
static uint32_t ultimoEstadoAudio = 0;                                  // ultimoEstadoAudio: Último tick en que se reenvió el estado de música a la STM32 de audio.
static uint32_t recordMario = 0, recordLuigi = 0;                       // recordMario: Récord persistente de Mario cargado/guardado en microSD.
static uint8_t sdDisponible = 0;                                        // Marca si la microSD pudo inicializarse correctamente.
static uint8_t sdPuntajesOk = 0;                                        // Marca si el archivo de puntuaciones pudo cargarse correctamente.
static uint32_t ultimoIntentoSD = 0;                                    // Guarda el tick del último intento de recuperar la microSD.
/* Una fila RGB565: la imagen reside en la microSD, no en el firmware. */
static uint8_t filaMenuBytes[SCREEN_W * 2U];                            // filaMenuBytes: Buffer de una fila leída de la microSD en formato de bytes.
static uint16_t filaMenuPixeles[SCREEN_W];                              // filaMenuPixeles: Buffer de una fila convertida a palabras RGB565 para enviarla a la TFT.

/* RNG */
uint32_t rngState = 0xA5C31F27U;                                        // rngState: Estado interno de 32 bits del generador pseudoaleatorio xorshift.

/* UART */
volatile uint8_t uartRxBuffer[UART_RX_BUFFER_SIZE];                     // uartRxBuffer: Buffer circular donde la ISR deposita los bytes recibidos desde el ESP32.
volatile uint8_t uartRxHead = 0;                                        // uartRxHead: Índice de escritura del buffer circular UART.
volatile uint8_t uartRxTail = 0;                                        // uartRxTail: Índice de lectura del buffer circular UART.
volatile uint32_t uartOverflow = 0;                                     // uartOverflow: Contador de bytes perdidos cuando el buffer circular se llena.
uint8_t jugadorPendiente = 0;                                           // Guarda si el próximo comando pertenece a J1 o J2.

/* USER CODE END PV */


/* ============================================================
 * CONFIGURACIÓN BÁSICA DEL STM32 - PROTOTIPOS
 *
 * Se declaran las funciones que configuran reloj, GPIO, SPI1 y USART3.
 * Sus implementaciones aparecen al final del archivo.
 * ============================================================ */
/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);                                               // Configura el reloj principal del STM32.
static void MX_GPIO_Init(void);                                                     // Configura los GPIO usados por la TFT y la microSD.
static void MX_SPI1_Init(void);                                                     // Configura SPI1 para comunicarse con la microSD.
static void MX_USART3_UART_Init(void);                                              // Configura USART3: RX desde ESP32 y TX hacia la STM32 de audio.


/* USER CODE BEGIN PFP */

/* ============================================================
 * 4. PROTOTIPOS DE FUNCIONES
 *
 * Se anuncian las funciones que aparecen más adelante para que el compilador conozca sus nombres, parámetros y valores de retorno.
 * Esta sección funciona como un índice rápido de las funciones principales del juego.
 * ============================================================ */

int AbsInt(int valor);                                                      // Devuelve el valor absoluto de un entero.
uint32_t RNG_Siguiente(void);                                                    // Genera el siguiente número pseudoaleatorio.
uint8_t RNG_Porcentaje(uint8_t porcentaje);                                     // Devuelve 1 con la probabilidad porcentual indicada.

void FondoSetIndice(int x, int y, uint8_t indice);                           // Guarda un píxel del fondo usando un índice de 4 bits.
uint8_t FondoGetIndice(int x, int y);                                           // Lee el índice de color de un píxel del fondo.
uint16_t FondoGetColor(int x, int y);                                            // Convierte el índice almacenado en un color RGB565.
void FondoLineaIndice(int x0, int y0, int x1, int y1, uint8_t indice);       // Dibuja una línea dentro del buffer lógico del fondo.
void FondoHLineIndice(int x, int y, int w, uint8_t indice);                  // Dibuja una línea horizontal en el buffer del fondo.
void FondoVLineIndice(int x, int y, int h, uint8_t indice);                  // Dibuja una línea vertical en el buffer del fondo.
void GenerarFondoBuffer(void);                                               // Construye en RAM el fondo estático de vigas y escaleras.
void RestaurarFondoRect(int x, int y, int w, int h);                         // Restaura una región usando únicamente el fondo estático.

uint16_t ObtenerColorFinalPixel(int x, int y);                                   // Calcula qué color final debe verse en un píxel tras superponer todas las capas.
void RenderizarRegionFinal(int x, int y, int w, int h);                      // Redibuja una región pequeña ya compuesta, evitando borrar y luego volver a dibujar.
void RenderizarUnionMovimiento(                                             // Inicia la lógica de RenderizarUnionMovimiento.
    int oldX, int oldY,
    int newX, int newY,
    int w, int h
);

uint8_t ColisionAABB(AABB a, AABB b);                                           // Comprueba si dos cajas rectangulares se tocan.
uint8_t RectangulosIntersectan(                                                 // Devuelve 1 cuando los dos rectángulos comparten área.
    int x1, int y1, int w1, int h1,
    int x2, int y2, int w2, int h2
);

void DibujarLineaSimple(int x0, int y0, int x1, int y1, uint16_t color);     // Dibuja una línea directamente en la TFT.
int ObtenerYPlataforma(const Plataforma *p, int x);                         // Calcula la altura Y de una viga inclinada para una coordenada X.
void DibujarPlataforma(const Plataforma *p);                                 // Dibuja una viga completa.
void DibujarEscalera(const Escalera *e);                                     // Dibuja una escalera completa.
void DibujarEscenario(void);                                                 // Dibuja todas las vigas y escaleras del nivel.

int AceiteY(void);                                                          // Calcula la coordenada Y donde debe apoyarse el barril de aceite.
AABB CajaAceite(void);                                                       // Construye la caja de colisión del barril de aceite.

uint8_t ObtenerColorAceitePixel(                                                // Indica si un píxel pertenece al aceite y devuelve su color.
    int x,                                                                  // Recibe coordenada X.
    int y,                                                                  // Recibe coordenada Y.
    uint16_t *color
);

void DibujarAceite(void);                                                    // Dibuja el barril de aceite y su llama si está encendido.
void EncenderAceite(void);                                                   // Cambia el aceite de apagado a encendido.
uint8_t BarrilTocaAceite(const Barril *b);                                      // Comprueba si un barril azul está tocando el aceite.

int BuscarPlataformaSoporte(const Jugador *j);                              // Busca la plataforma que actualmente sostiene al jugador.
int BuscarPlataformaCaida(const Jugador *j, int oldBottom, int newBottom);  // Busca la plataforma que el jugador cruza mientras cae.
int BuscarEscaleraCercana(const Jugador *j);                                // Busca una escalera válida cerca del jugador.
int LimitarXDentroPlataforma(const Jugador *j, int nuevaX);                 // Evita que el jugador salga horizontalmente de su plataforma.
void InicializarJugadores(void);                                             // Coloca a ambos jugadores en sus posiciones iniciales y reinicia sus estados.
void ReaparecerJugador(Jugador *j);                                          // Devuelve un jugador con vidas al punto de inicio tras morir.
void DibujarJugador(const Jugador *j);                                       // Redibuja la región ocupada por un jugador.
void DibujarJugadores(void);                                                 // Dibuja a los dos jugadores.
void ActualizarJugador(Jugador *j, uint8_t pasoMovimiento);                  // Actualiza controles, salto, escaleras, movimiento horizontal y gravedad.

/* Martillos */
void InicializarMartillos(void);                                             // Coloca y reinicia los dos martillos del nivel.
void DibujarMartilloEscenario(const Martillo *m);                            // Dibuja un martillo disponible en el escenario.
void DibujarMartillos(void);                                                 // Dibuja todos los martillos disponibles.
AABB CajaMartilloEscenario(const Martillo *m);                               // Devuelve la caja de colisión de un martillo del escenario.
AABB ObtenerCajaMartilloAtaque(const Jugador *j);                            // Calcula la zona que puede golpear el martillo del jugador.
uint8_t ObtenerColorMartilloEscenarioPixel(int x, int y, uint16_t *color);      // Devuelve el color del martillo disponible para un píxel concreto.
uint8_t ObtenerColorMartilloJugadorPixel(const Jugador *j, int x, int y, uint16_t *color); // Devuelve el color del martillo que lleva el jugador para un píxel concreto.
void ActivarMartilloJugador(Jugador *j, uint8_t numeroJugador, int indiceMartillo); // Entrega el martillo al jugador e inicia sus 8.5 segundos de duración.
void FinalizarMartilloJugador(Jugador *j);                                   // Desactiva el martillo cuando se agota o el jugador lo pierde.
void ActualizarMartilloJugador(Jugador *j);                                  // Controla duración y alternancia arriba/frente del martillo.
void ActualizarEstadoSpriteJugador(Jugador *j);                              // Elige qué estado visual corresponde al jugador.
void ComprobarRecogidaMartillos(void);                                       // Detecta si un jugador toca un martillo disponible.
void ComprobarGolpesMartillo(void);                                          // Detecta golpes del martillo contra barriles y flamas.
void ActualizarMusicaMartillo(void);                                         // Decide si debe mantenerse la música normal o la del martillo.
void Audio_ReiniciarMusicaMartillo(void);                                    // Ordena a la STM32 de audio reiniciar la música del martillo.
void Audio_ReproducirMusicaNormal(void);                                     // Ordena a la STM32 de audio volver a la música normal.

void MarcarGolpeJugador(Jugador *j, uint32_t *golpeHasta);                   // Resta una vida e inicia la animación de muerte del jugador.
void ActualizarGolpes(void);                                                 // Termina el efecto visual rojo después de recibir daño.

void DibujarFrameDK(uint8_t frame);                                          // Dibuja un frame específico de la animación de Donkey Kong.
void IniciarLanzamientoDK(void);                                             // Inicia una nueva animación de lanzamiento si hay espacio para otro barril.
void SeleccionarSiguienteBarrilDK(void);                                     // Decide qué tipo de barril lanzará Donkey Kong.
uint32_t CalcularIntervaloLanzamiento(void);                                     // Calcula cuánto esperará DK antes del siguiente lanzamiento.
void ActualizarDK(void);                                                     // Avanza la animación de DK y libera el barril en el frame correcto.

void InicializarBarriles(void);                                              // Marca todos los slots de barriles como libres.
uint8_t ContarBarrilesActivos(void);                                            // Cuenta cuántos barriles existen actualmente.
void CrearBarril(TipoBarril tipo, uint8_t directo, uint8_t contactosIgnorar); // Busca un slot libre y crea un nuevo barril.
void DesactivarBarril(Barril *b);                                            // Elimina lógicamente un barril y libera su slot.
void DibujarBarril(const Barril *b);                                         // Redibuja la región ocupada por un barril.
void DibujarBarriles(void);                                                  // Dibuja todos los barriles activos.
int DireccionBarrilPorPlataforma(int plataforma);                           // Devuelve hacia qué lado debe rodar un barril según la plataforma.
int BuscarPlataformaBarrilCaida(const Barril *b, int oldBottom, int newBottom); // Busca qué plataforma cruza un barril mientras cae.
int BuscarPlataformaDestinoEscalera(int escalera);                          // Busca la plataforma inferior conectada a una escalera.
int BuscarEscaleraParaBarril(const Barril *b);                              // Busca si el barril está pasando sobre una escalera válida.
Jugador *JugadorObjetivoBarril(const Barril *b);
uint8_t DebeBajarEscalera(Barril *b, int escalera);                             // Decide probabilísticamente si el barril debe bajar por una escalera.
void IniciarBajadaEscalera(Barril *b, int escalera);                         // Coloca al barril en una escalera e inicia su descenso.
void IniciarReboteBarril(Barril *b, int plataforma);                         // Coloca el barril sobre una plataforma e inicia su rebote.
void BarrilLlegaAceite(Barril *b);                                           // Enciende el aceite y crea una flama cuando llega un barril azul.
void ActualizarBarril(Barril *b, uint8_t pasoMovimiento);                    // Ejecuta la física correspondiente al estado actual de un barril.
void ActualizarBarriles(uint8_t pasoMovimiento);                             // Actualiza todos los barriles activos.
void ComprobarColisionesBarriles(void);                                      // Detecta daño y saltos puntuables entre jugadores y barriles.

void InicializarFireballs(void);                                             // Marca todos los slots de flamas como libres.
void CrearFireball(void);                                                    // Crea una flama dentro del aceite si queda un slot libre.
void DesactivarFireball(Fireball *f);                                        // Elimina una flama y libera su slot.
void DibujarFireball(const Fireball *f);                                     // Dibuja una flama activa.
void DibujarFireballs(void);                                                 // Dibuja todas las flamas activas.

Jugador *ObtenerJugadorObjetivoFireball(const Fireball *f);
uint8_t ElegirJugadorObjetivoFireball(const Fireball *f);                       // Elige como objetivo al jugador más cercano.

int BuscarPlataformaExtremoEscalera(int escalera, uint8_t superior);        // Busca la plataforma conectada a un extremo de una escalera.
int BuscarMejorEscaleraFireball(                                            // Busca la escalera más útil para acercar la flama al jugador objetivo.
    const Fireball *f,
    const Jugador *objetivo,
    int8_t *direccionVertical,
    int8_t *plataformaDestino
);

void IniciarEscaleraFireball(                                                // Pone a la flama en una escalera e inicia su movimiento vertical.
    Fireball *f,
    int escalera,                                                           // Recibe índice de escalera.
    int8_t direccionVertical,                                               // Recibe sentido de subida o bajada.
    int8_t plataformaDestino                                                // Recibe plataforma a la que debe llegar.
);

void TomarDecisionFireball(Fireball *f);                                     // Hace que la IA decida entre perseguir, patrullar o buscar una escalera.
void ActualizarFireball(Fireball *f, uint8_t pasoMovimiento);                // Ejecuta el comportamiento de una flama según su estado actual.
void ActualizarFireballs(uint8_t pasoMovimiento);                            // Actualiza todas las flamas activas.
void ComprobarColisionesFireballs(void);                                     // Detecta daño cuando una flama toca a un jugador.

void UART3_IniciarRecepcion(void);                                           // Activa la recepción de USART3 por interrupciones.
uint8_t UART3_LeerByte(uint8_t *dato);                                  // Saca un byte pendiente del buffer circular de USART3.
void ProcesarByteESP32(uint8_t dato);                                        // Interpreta la pareja jugador+comando enviada por el ESP32.
void AplicarComando(Jugador *j, char comando);                               // Aplica L/R/U/D/N/J al jugador correspondiente.
void IniciarPartida(void);                                                   // Reinicia todas las mecánicas y dibuja una partida nueva.
void EntrarMenu(void);                                                       // Muestra el menú inicial y activa su música.
void TerminarPartida(uint8_t quien);                                         // Guarda puntuaciones y muestra la pantalla final.
void IniciarRescate(uint8_t quien, uint32_t ahora);                          // Inicia la secuencia de victoria al alcanzar a Peach.
void DibujarMarcador(void);                                                  // Actualiza vidas y puntuación de ambos jugadores.

/* USER CODE END PFP */


/* USER CODE BEGIN 0 */

/* ============================================================
 * 5. LÓGICA Y FUNCIONES DEL JUEGO
 *
 * A partir de este punto se implementa el comportamiento real: dibujo, física, colisiones, jugadores, enemigos, IA, UART y pantallas.
 * Cada sección modifica los datos definidos en los bloques anteriores.
 * ============================================================ */

/* Sprite de 8x12, en la STM32 que ejecuta las colisiones y dibuja la TFT.
 * Los espacios son transparentes. R: rojo, W: brillo, B: mango cafe. */
/* ============================================================
 * SPRITE DEL MARTILLO Y ENVÍO DE AUDIO
 *
 * Se describe el martillo con caracteres y después se convierten esos caracteres a colores RGB565.
 * También se prepara una trama corta por USART3 para enviar eventos de música y sonido a la segunda STM32.
 * ============================================================ */

static const char *const spriteMartillo[MARTILLO_H] = {                     // Actualiza spriteMartillo.
    " RRRRRR ", "RRWWWWRR", "RRWWWWRR", " RRRRRR ",                         // Continúa el cálculo de spriteMartillo.
    "   BB   ", "   BB   ", "   BB   ", "   BB   ",                         // Continúa el cálculo de spriteMartillo.
    "   BB   ", "   BB   ", "   BB   ", "   BB   "                          // Continúa el cálculo de spriteMartillo.
};                                                                          // Continúa el cálculo de spriteMartillo.

static uint16_t ColorPixelMartillo(char pixel)                          // Convierte R/W/B del sprite del martillo a RGB565.
{
    if (pixel == 'R') return COLOR_MARTILLO_CABEZA;                         // Comprueba si se cumplen las condiciones necesarias para continuar.
    if (pixel == 'W') return COLOR_MARTILLO_CABEZA_CENTRO;                  // Comprueba si se cumplen las condiciones necesarias para continuar.
    if (pixel == 'B') return COLOR_MARTILLO_MANGO;                          // Comprueba si se cumplen las condiciones necesarias para continuar.
    return 0;                                                               // Devuelve 0 para indicar que no se encontró o no se cumple la condición buscada.
}


static void Audio_Enviar(uint8_t comando)                               // Envía a la STM32 de audio una trama: cabecera 0xA5 + comando + comprobación.
{
    uint8_t trama[3] = {0xA5U, comando, (uint8_t)(comando ^ 0xFFU)};        // Calcula y guarda trama enviada al STM32 de audio.
    (void)HAL_UART_Transmit(&huart3, trama, sizeof(trama), 10U);        // Envía la trama por USART3.
}

/* ============================================================
 * UTILIDADES
 *
 * Se agrupan funciones pequeñas que apoyan al resto del juego.
 * Incluyen valor absoluto y números pseudoaleatorios usados por barriles y flamas.
 * ============================================================ */

int AbsInt(int valor)                                                   // Calcula el valor absoluto de un entero.
{
    return (valor < 0) ? -valor : valor;                                    // Devuelve una de las opciones según la condición que se evalúa a continuación.
}

uint32_t RNG_Siguiente(void)                                            // Genera un número pseudoaleatorio con xorshift32 para decisiones de barriles y flamas.
{
    rngState ^= rngState << 13;                                             // Actualiza rngState mediante una operación bit a bit.
    rngState ^= rngState >> 17;                                             // Actualiza rngState mediante una operación bit a bit.
    rngState ^= rngState << 5;                                              // Actualiza rngState mediante una operación bit a bit.
    return rngState;                                                        // Devuelve rngState como resultado de la función.
}

uint8_t RNG_Porcentaje(uint8_t porcentaje)                              // Devuelve 1 si el número aleatorio cae dentro del porcentaje pedido; si no, devuelve 0.
{
    return (RNG_Siguiente() % 100U) < porcentaje;                           // Devuelve una de las opciones según la condición que se evalúa a continuación.
}


/* ============================================================
 * BUFFER DE FONDO
 *
 * Se guarda una copia compacta del escenario estático en RAM.
 * El buffer permite recuperar rápidamente el fondo detrás de objetos que se mueven y ayuda a evitar parpadeos.
 * ============================================================ */

void FondoSetIndice(int x, int y, uint8_t indice)                       // Guarda un píxel del fondo en 4 bits; así dos píxeles caben en un solo byte de RAM.
{
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;           // Comprueba si se cumplen las condiciones necesarias para continuar.

    uint32_t pixel = (uint32_t)y * SCREEN_W + (uint32_t)x;                  // Calcula y guarda posición lineal del píxel.
    uint32_t bytePos = pixel >> 1;                                          // Calcula y guarda byte del buffer que contiene el píxel.

    indice &= 0x0F;                                                         // Actualiza índice de color mediante una operación bit a bit.

    if (pixel & 1U) {                                                       // Comprueba si se cumplen las condiciones necesarias para continuar.
        fondoBuffer[bytePos] =                                              // Actualiza fondoBuffer.
            (fondoBuffer[bytePos] & 0x0F) |                                 // Accede a la posición correspondiente del arreglo (fondoBuffer.
            (indice << 4);                                                  // Evalúa esta comparación como parte de la condición actual.
    } else {                                                                // Continúa el cálculo de fondoBuffer.
        fondoBuffer[bytePos] =                                              // Actualiza fondoBuffer.
            (fondoBuffer[bytePos] & 0xF0) |                                 // Accede a la posición correspondiente del arreglo (fondoBuffer.
            indice;                                                         // Continúa el cálculo de fondoBuffer.
    }
}

uint8_t FondoGetIndice(int x, int y)                                    // Recupera los 4 bits que representan el color de este píxel del fondo.
{
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H)                   // Comprueba si se cumplen las condiciones necesarias para continuar.
        return BG_NEGRO;                                                    // Devuelve BG_NEGRO como resultado de la función.

    uint32_t pixel = (uint32_t)y * SCREEN_W + (uint32_t)x;                  // Calcula y guarda posición lineal del píxel.
    uint8_t valor = fondoBuffer[pixel >> 1];                                // Calcula y guarda valor.

    if (pixel & 1U)                                                         // Comprueba si se cumplen las condiciones necesarias para continuar.
        return (valor >> 4) & 0x0F;                                         // Devuelve una de las opciones según la condición que se evalúa a continuación.

    return valor & 0x0F;                                                    // Devuelve x0F como resultado de la función.
}

uint16_t FondoGetColor(int x, int y)                                    // Convierte el índice del fondo a su color RGB565.
{
    uint8_t indice = FondoGetIndice(x, y);                                  // Calcula y guarda índice de color.

    if (indice > BG_ESCALERA)                                               // Comprueba si se cumplen las condiciones necesarias para continuar.
        indice = BG_NEGRO;                                                  // Calcula y guarda índice de color.

    return fondoPaleta[indice];                                             // Devuelve fondoPaleta como resultado de la función.
}

void FondoLineaIndice(int x0, int y0, int x1, int y1, uint8_t indice)   // Traza una línea en el buffer del fondo con Bresenham, sin dibujar todavía en la TFT.
{
    int dx = AbsInt(x1 - x0);                                               // Calcula y guarda diferencia horizontal.
    int sx = (x0 < x1) ? 1 : -1;                                            // Calcula y guarda sentido horizontal de la línea.
    int dy = -AbsInt(y1 - y0);                                              // Calcula y guarda diferencia vertical.
    int sy = (y0 < y1) ? 1 : -1;                                            // Calcula y guarda sentido vertical de la línea.
    int err = dx + dy;                                                      // Calcula y guarda error de Bresenham.

    while (1) {                                                             // Repite este bloque mientras la condición indicada siga activa.
        FondoSetIndice(x0, y0, indice);                                 // Guarda en el buffer compacto el índice de color correspondiente a este píxel.

        if (x0 == x1 && y0 == y1)                                           // Comprueba si se cumplen las condiciones necesarias para continuar.
            break;                                                          // Sale de este caso o bucle y continúa después del bloque.

        int e2 = 2 * err;                                                   // Calcula y guarda doble del error de Bresenham.

        if (e2 >= dy) {                                                     // Comprueba si se cumplen las condiciones necesarias para continuar.
            err += dy;                                                      // Aumenta error de Bresenham con este valor.
            x0 += sx;                                                       // Aumenta x0 con este valor.
        }

        if (e2 <= dx) {                                                     // Comprueba si se cumplen las condiciones necesarias para continuar.
            err += dx;                                                      // Aumenta error de Bresenham con este valor.
            y0 += sy;                                                       // Aumenta y0 con este valor.
        }
    }
}

void FondoHLineIndice(int x, int y, int w, uint8_t indice)              // Escribe una línea horizontal de índices de color dentro del buffer del fondo.
{
    for (int i = 0; i < w; i++)                                             // Recorre todas las posiciones necesarias de este bloque.
        FondoSetIndice(x + i, y, indice);                               // Guarda en el buffer compacto el índice de color correspondiente a este píxel.
}

void FondoVLineIndice(int x, int y, int h, uint8_t indice)              // Escribe una línea vertical de índices de color dentro del buffer del fondo.
{
    for (int i = 0; i < h; i++)                                             // Recorre todas las posiciones necesarias de este bloque.
        FondoSetIndice(x, y + i, indice);                               // Guarda en el buffer compacto el índice de color correspondiente a este píxel.
}


/* ============================================================
 * PLATAFORMAS Y ESCALERAS DEL ESCENARIO
 *
 * Se calcula la altura de las vigas inclinadas y se construyen vigas y escaleras dentro del buffer de fondo.
 * Estas formas también sirven como referencia para la física de jugadores y enemigos.
 * ============================================================ */

int ObtenerYPlataforma(const Plataforma *p, int x)                      // Calcula la altura de una viga inclinada en la posición X indicada.
{
    if (x <= p->x1) return p->y1;                                           // Comprueba una condición de la plataforma antes de continuar.
    if (x >= p->x2) return p->y2;                                           // Comprueba una condición de la plataforma antes de continuar.

    int dx = p->x2 - p->x1;                                                 // Calcula y guarda diferencia horizontal.
    int dy = p->y2 - p->y1;                                                 // Calcula y guarda diferencia vertical.

    return p->y1 +                                                          // Devuelve p->y1 + como resultado de la función.
           ((int32_t)dy * (x - p->x1)) /                                    // Evalúa esta comparación como parte de la condición actual.
           dx;                                                              // Divide entre el ancho horizontal para obtener la interpolación de la viga.
}

void GenerarFondoBuffer(void)                                           // Precalcula vigas y escaleras en RAM para poder restaurar zonas rápidamente.
{
    memset(fondoBuffer, 0, sizeof(fondoBuffer));                        // Ejecuta memset().

    for (uint32_t i = 0; i < NUM_PLATAFORMAS; i++) {                        // Recorre todas las plataformas del escenario.
        const Plataforma *p = &plataformas[i];                              // Calcula y guarda p.

        FondoLineaIndice(p->x1, p->y1,                                      // Evalúa esta comparación como parte de la condición actual.
                         p->x2, p->y2,                                      // Evalúa esta comparación como parte de la condición actual.
                         BG_VIGA_CLARO);                                    // Indica Y inicial.

        FondoLineaIndice(p->x1, p->y1 + VIGA_GROSOR,                        // Evalúa esta comparación como parte de la condición actual.
                         p->x2, p->y2 + VIGA_GROSOR,                        // Evalúa esta comparación como parte de la condición actual.
                         BG_VIGA);                                          // Indica Y inicial.

        FondoLineaIndice(p->x1, p->y1 + 2,                                  // Evalúa esta comparación como parte de la condición actual.
                         p->x2, p->y2 + 2,                                  // Evalúa esta comparación como parte de la condición actual.
                         BG_VIGA_OSCURO);                                   // Indica Y inicial.

        int x = p->x1;                                                      // Actualiza la posición horizontal del objeto.
        uint8_t invertir = 0;                                               // Reinicia invertir a cero.

        while (x < p->x2) {                                                 // Repite este bloque mientras la condición indicada siga activa.
            int siguienteX = x + VIGA_SEGMENTO;                             // Calcula y guarda siguienteX.
            if (siguienteX > p->x2)                                         // Comprueba una condición de la plataforma antes de continuar.
                siguienteX = p->x2;                                         // Actualiza siguienteX.

            int yA = ObtenerYPlataforma(p, x);                          // Calcula la altura exacta de la viga inclinada en esta coordenada X.
            int yB = ObtenerYPlataforma(p, siguienteX);                 // Calcula la altura exacta de la viga inclinada en esta coordenada X.

            if (!invertir) {                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
                FondoLineaIndice(                                           // Traza una línea en el buffer del fondo con Bresenham, sin dibujar todavía en la TFT.
                    x, yA + 1,                                              // Indica X inicial.
                    siguienteX, yB + VIGA_GROSOR - 1,                       // Indica Y inicial.
                    BG_VIGA_CLARO                                           // Indica X final.
                );                                                          // Finaliza la llamada a FondoLineaIndice().
            } else {
                FondoLineaIndice(                                           // Traza una línea en el buffer del fondo con Bresenham, sin dibujar todavía en la TFT.
                    x, yA + VIGA_GROSOR - 1,                                // Indica X inicial.
                    siguienteX, yB + 1,                                     // Indica Y inicial.
                    BG_VIGA_CLARO                                           // Indica X final.
                );                                                          // Finaliza la llamada a FondoLineaIndice().
            }

            invertir = !invertir;                                           // Actualiza invertir.
            x = siguienteX;                                                 // Actualiza la posición horizontal del objeto.
        }
    }

    for (uint32_t i = 0; i <= NUM_ESCALERAS; i++) {                         // Recorre todas las escaleras del escenario.
        const Escalera *e = (i == NUM_ESCALERAS) ?                          // Calcula y guarda e.
            &escaleraDecorativaPeach : &escaleras[i];                       // Accede a la posición correspondiente del arreglo escaleras.

        int inicio = e->y1;                                                 // Calcula y guarda inicio vertical.
        int fin = e->y2;                                                    // Calcula y guarda final vertical.

        if (fin < inicio) {                                                 // Comprueba si se cumplen las condiciones necesarias para continuar.
            int temp = inicio;                                              // Calcula y guarda temp.
            inicio = fin;                                                   // Calcula y guarda inicio vertical.
            fin = temp;                                                     // Calcula y guarda final vertical.
        }

        int altura = fin - inicio + 1;                                      // Calcula y guarda altura.

        FondoVLineIndice(                                                   // Escribe una línea vertical de índices de color dentro del buffer del fondo.
            e->x, inicio, altura,                                           // Evalúa esta comparación como parte de la condición actual.
            BG_ESCALERA                                                     // Indica coordenada Y.
        );                                                                  // Finaliza la llamada a FondoVLineIndice().

        FondoVLineIndice(                                                   // Escribe una línea vertical de índices de color dentro del buffer del fondo.
            e->x + ESCALERA_W,                                              // Evalúa esta comparación como parte de la condición actual.
            inicio, altura,                                                 // Indica coordenada Y.
            BG_ESCALERA                                                     // Indica alto.
        );                                                                  // Finaliza la llamada a FondoVLineIndice().

        for (int y = inicio + 3; y < fin; y += 5) {                         // Recorre todas las posiciones necesarias de este bloque.
            FondoHLineIndice(                                               // Escribe una línea horizontal de índices de color dentro del buffer del fondo.
                e->x,                                                       // Evalúa esta comparación como parte de la condición actual.
                y,                                                          // Indica coordenada Y.
                ESCALERA_W + 1,                                             // Indica ancho.
                BG_ESCALERA                                                 // Indica índice de color.
            );                                                              // Finaliza la llamada a FondoHLineIndice().
        }
    }
}

void RestaurarFondoRect(int x, int y, int w, int h)                     // Vuelve a dibujar solo el fondo estático de una región rectangular.
{
    if (w <= 0 || h <= 0) return;                                           // Comprueba si se cumplen las condiciones necesarias para continuar.

    if (x < 0) {                                                            // Comprueba si se cumplen las condiciones necesarias para continuar.
        w += x;                                                             // Aumenta ancho con este valor.
        x = 0;                                                              // Actualiza la posición horizontal del objeto.
    }

    if (y < 0) {                                                            // Comprueba si se cumplen las condiciones necesarias para continuar.
        h += y;                                                             // Aumenta alto con este valor.
        y = 0;                                                              // Actualiza la posición vertical del objeto.
    }

    if (x + w > SCREEN_W)                                                   // Comprueba si se cumplen las condiciones necesarias para continuar.
        w = SCREEN_W - x;                                               // Ancho de la caja o sprite.

    if (y + h > SCREEN_H)                                                   // Comprueba si se cumplen las condiciones necesarias para continuar.
        h = SCREEN_H - y;                                               // Alto de la caja o sprite.

    if (w <= 0 || h <= 0)                                                   // Comprueba si se cumplen las condiciones necesarias para continuar.
        return;                                                             // Termina la función en este punto.

    for (int py = y; py < y + h; py++) {                                    // Recorre todas las posiciones necesarias de este bloque.
        int inicioRun = x;                                                  // Calcula y guarda inicioRun.
        uint16_t colorRun = FondoGetColor(x, py);                       // Consulta el color estático del escenario en esta coordenada.

        for (int px = x + 1; px < x + w; px++) {                            // Recorre todas las posiciones necesarias de este bloque.
            uint16_t nuevoColor = FondoGetColor(px, py);                // Consulta el color estático del escenario en esta coordenada.

            if (nuevoColor != colorRun) {                                   // Comprueba si se cumplen las condiciones necesarias para continuar.
                FillRect(                                                   // Ejecuta FillRect().
                    inicioRun,                                              // Indica coordenada X inicial.
                    py,                                                     // Indica coordenada Y inicial.
                    px - inicioRun,                                         // Indica ancho del rectángulo.
                    1,                                                      // Indica alto del rectángulo.
                    colorRun                                                // Indica color RGB565.
                );                                                          // Finaliza la llamada a FillRect().

                inicioRun = px;                                             // Actualiza inicioRun.
                colorRun = nuevoColor;                                      // Actualiza colorRun.
            }
        }

        FillRect(                                                           // Ejecuta FillRect().
            inicioRun,                                                      // Indica coordenada X inicial.
            py,                                                             // Indica coordenada Y inicial.
            x + w - inicioRun,                                              // Indica ancho del rectángulo.
            1,                                                              // Indica alto del rectángulo.
            colorRun                                                        // Indica color RGB565.
        );                                                                  // Finaliza la llamada a FillRect().
    }
}


/* ============================================================
 * COLISIONES BÁSICAS
 *
 * Se comprueba si dos cajas rectangulares se superponen.
 * Esta prueba se reutiliza para jugadores, barriles, flamas, martillos y aceite.
 * ============================================================ */

uint8_t ColisionAABB(AABB a, AABB b)                                    // Devuelve 1 si las dos cajas de colisión se tocan; si no, devuelve 0.
{
    return (                                                                // Devuelve una de las opciones según la condición que se evalúa a continuación.
        a.x + a.w >= b.x &&                                                 // Añade esta condición a la comprobación actual.
        a.x <= b.x + b.w &&                                                 // Añade esta condición a la comprobación actual.
        a.y + a.h >= b.y &&                                                 // Añade esta condición a la comprobación actual.
        a.y <= b.y + b.h                                                    // Evalúa esta comparación como parte de la condición actual.
    );
}

uint8_t RectangulosIntersectan(                                         // Devuelve 1 cuando los dos rectángulos comparten área.
    int x1, int y1, int w1, int h1,
    int x2, int y2, int w2, int h2
)
{
    return (                                                                // Devuelve una de las opciones según la condición que se evalúa a continuación.
        x1 < x2 + w2 &&                                                     // Añade esta condición a la comprobación actual.
        x1 + w1 > x2 &&                                                     // Añade esta condición a la comprobación actual.
        y1 < y2 + h2 &&                                                     // Añade esta condición a la comprobación actual.
        y1 + h1 > y2                                                        // Evalúa esta comparación como parte de la condición actual.
    );
}


/* ============================================================
 * DIBUJO DEL ESCENARIO
 *
 * Se dibujan en la TFT las vigas y escaleras que forman el nivel.
 * El escenario visual usa la misma geometría que después utiliza la física.
 * ============================================================ */

void DibujarLineaSimple(int x0, int y0, int x1, int y1, uint16_t color) // Dibuja una línea directamente en la TFT píxel por píxel con Bresenham.
{
    int dx = AbsInt(x1 - x0);                                               // Calcula y guarda diferencia horizontal.
    int sx = (x0 < x1) ? 1 : -1;                                            // Calcula y guarda sentido horizontal de la línea.
    int dy = -AbsInt(y1 - y0);                                              // Calcula y guarda diferencia vertical.
    int sy = (y0 < y1) ? 1 : -1;                                            // Calcula y guarda sentido vertical de la línea.
    int err = dx + dy;                                                      // Calcula y guarda error de Bresenham.

    while (1) {                                                             // Repite este bloque mientras la condición indicada siga activa.
        if (x0 >= 0 && x0 < SCREEN_W &&                                     // Comprueba si se cumplen las condiciones necesarias para continuar.
            y0 >= 0 && y0 < SCREEN_H) {                                     // Comprueba que la coordenada permanezca dentro del alto de la pantalla.
            FillRect(x0, y0, 1, 1, color);                                  // Ejecuta FillRect().
        }

        if (x0 == x1 && y0 == y1)                                           // Comprueba si se cumplen las condiciones necesarias para continuar.
            break;                                                          // Sale de este caso o bucle y continúa después del bloque.

        int e2 = 2 * err;                                                   // Calcula y guarda doble del error de Bresenham.

        if (e2 >= dy) {                                                     // Comprueba si se cumplen las condiciones necesarias para continuar.
            err += dy;                                                      // Aumenta error de Bresenham con este valor.
            x0 += sx;                                                       // Aumenta x0 con este valor.
        }

        if (e2 <= dx) {                                                     // Comprueba si se cumplen las condiciones necesarias para continuar.
            err += dx;                                                      // Aumenta error de Bresenham con este valor.
            y0 += sy;                                                       // Aumenta y0 con este valor.
        }
    }
}

void DibujarPlataforma(const Plataforma *p)                             // Dibuja visualmente una viga inclinada, incluyendo bordes y diagonales para imitar la estética del arcade.
{
    DibujarLineaSimple(                                                     // Dibuja una línea directamente en la TFT píxel por píxel con Bresenham.
        p->x1, p->y1,                                                       // Evalúa esta comparación como parte de la condición actual.
        p->x2, p->y2,                                                       // Evalúa esta comparación como parte de la condición actual.
        COLOR_VIGA_CLARO                                                    // Indica X final.
    );                                                                      // Finaliza la llamada a DibujarLineaSimple().

    DibujarLineaSimple(                                                     // Dibuja una línea directamente en la TFT píxel por píxel con Bresenham.
        p->x1, p->y1 + VIGA_GROSOR,                                         // Evalúa esta comparación como parte de la condición actual.
        p->x2, p->y2 + VIGA_GROSOR,                                         // Evalúa esta comparación como parte de la condición actual.
        COLOR_VIGA                                                          // Indica X final.
    );                                                                      // Finaliza la llamada a DibujarLineaSimple().

    DibujarLineaSimple(                                                     // Dibuja una línea directamente en la TFT píxel por píxel con Bresenham.
        p->x1, p->y1 + 2,                                                   // Evalúa esta comparación como parte de la condición actual.
        p->x2, p->y2 + 2,                                                   // Evalúa esta comparación como parte de la condición actual.
        COLOR_VIGA_OSCURO                                                   // Indica X final.
    );                                                                      // Finaliza la llamada a DibujarLineaSimple().

    int x = p->x1;                                                          // Actualiza la posición horizontal del objeto.
    uint8_t invertir = 0;                                                   // Reinicia invertir a cero.

    while (x < p->x2) {                                                     // Repite este bloque mientras la condición indicada siga activa.
        int siguienteX = x + VIGA_SEGMENTO;                                 // Calcula y guarda siguienteX.

        if (siguienteX > p->x2)                                             // Comprueba una condición de la plataforma antes de continuar.
            siguienteX = p->x2;                                             // Actualiza siguienteX.

        int yA = ObtenerYPlataforma(p, x);                              // Calcula la altura exacta de la viga inclinada en esta coordenada X.
        int yB = ObtenerYPlataforma(p, siguienteX);                     // Calcula la altura exacta de la viga inclinada en esta coordenada X.

        if (!invertir) {                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
            DibujarLineaSimple(                                             // Dibuja una línea directamente en la TFT píxel por píxel con Bresenham.
                x, yA + 1,                                                  // Indica X inicial.
                siguienteX,                                                 // Indica Y inicial.
                yB + VIGA_GROSOR - 1,                                       // Indica X final.
                COLOR_VIGA_CLARO                                            // Indica Y final.
            );                                                              // Finaliza la llamada a DibujarLineaSimple().
        } else {
            DibujarLineaSimple(                                             // Dibuja una línea directamente en la TFT píxel por píxel con Bresenham.
                x, yA + VIGA_GROSOR - 1,                                    // Indica X inicial.
                siguienteX,                                                 // Indica Y inicial.
                yB + 1,                                                     // Indica X final.
                COLOR_VIGA_CLARO                                            // Indica Y final.
            );                                                              // Finaliza la llamada a DibujarLineaSimple().
        }

        invertir = !invertir;                                               // Actualiza invertir.
        x = siguienteX;                                                     // Actualiza la posición horizontal del objeto.
    }
}

void DibujarEscalera(const Escalera *e)                                 // Dibuja los dos laterales y los peldaños de una escalera en la TFT.
{
    int inicio = e->y1;                                                     // Calcula y guarda inicio vertical.
    int fin = e->y2;                                                        // Calcula y guarda final vertical.

    if (fin < inicio) {                                                     // Comprueba si se cumplen las condiciones necesarias para continuar.
        int temp = inicio;                                                  // Calcula y guarda temp.
        inicio = fin;                                                       // Calcula y guarda inicio vertical.
        fin = temp;                                                         // Calcula y guarda final vertical.
    }

    V_line(                                                                 // Ejecuta V_line().
        e->x,                                                               // Evalúa esta comparación como parte de la condición actual.
        inicio,                                                             // Indica coordenada Y inicial.
        fin - inicio + 1,                                                   // Indica alto de la línea vertical.
        COLOR_ESCALERA                                                      // Indica color RGB565.
    );                                                                      // Finaliza la llamada a V_line().

    V_line(                                                                 // Ejecuta V_line().
        e->x + ESCALERA_W,                                                  // Evalúa esta comparación como parte de la condición actual.
        inicio,                                                             // Indica coordenada Y inicial.
        fin - inicio + 1,                                                   // Indica alto de la línea vertical.
        COLOR_ESCALERA                                                      // Indica color RGB565.
    );                                                                      // Finaliza la llamada a V_line().

    for (int y = inicio + 3; y < fin; y += 5) {                             // Recorre todas las posiciones necesarias de este bloque.
        H_line(                                                             // Ejecuta H_line().
            e->x,                                                           // Evalúa esta comparación como parte de la condición actual.
            y,                                                              // Indica coordenada Y.
            ESCALERA_W + 1,                                                 // Indica ancho de la línea horizontal.
            COLOR_ESCALERA                                                  // Indica color RGB565.
        );                                                                  // Finaliza la llamada a H_line().
    }
}

void DibujarEscenario(void)                                             // Recorre todas las plataformas y escaleras y las dibuja al iniciar una partida.
{
    for (uint32_t i = 0; i < NUM_PLATAFORMAS; i++)                          // Recorre todas las plataformas del escenario.
        DibujarPlataforma(&plataformas[i]);                             // Dibuja visualmente una viga inclinada, incluyendo bordes y diagonales para imitar la estética del arcade.

    for (uint32_t i = 0; i < NUM_ESCALERAS; i++)                            // Recorre todas las escaleras del escenario.
        DibujarEscalera(&escaleras[i]);                                 // Dibuja los dos laterales y los peldaños de una escalera en la TFT.
    DibujarEscalera(&escaleraDecorativaPeach);                          // Dibuja los dos laterales y los peldaños de una escalera en la TFT.
}


/* ============================================================
 * BARRIL DE ACEITE - DIBUJO Y COLISIÓN
 *
 * Se calcula dónde aparece el aceite, cómo se dibuja y qué zona ocupa para las colisiones.
 * También se detecta cuándo un barril azul llega al aceite y lo enciende.
 * ============================================================ */

int AceiteY(void)                                                       // Calcula la posición vertical del barril de aceite.
{
    int plataformaFinal = (int)NUM_PLATAFORMAS - 1;                         // Calcula y guarda plataformaFinal.
    int centroX = ACEITE_X + ACEITE_W / 2;                              // Calcula el centro para comparar posiciones.

    int py = ObtenerYPlataforma(                                            // Calcula y guarda altura Y de la plataforma.
        &plataformas[plataformaFinal],                                      // Accede a la posición correspondiente del arreglo plataformas.
        centroX                                                             // Continúa el cálculo de altura Y de la plataforma.
    );                                                                  // Calcula la altura exacta de la viga inclinada en esta coordenada X.

    return py - ACEITE_H;                                                   // Devuelve ACEITE_H como resultado de la función.
}                                                                           // Continúa el cálculo de altura Y de la plataforma.

AABB CajaAceite(void)                                                   // Devuelve la caja AABB del barril de aceite para poder comprobar colisiones.
{
    AABB caja = {                                                           // Calcula y guarda caja.
        ACEITE_X,                                                           // Continúa el cálculo de caja.
        AceiteY(),                                                          // Continúa el cálculo de caja.
        ACEITE_W,                                                           // Continúa el cálculo de caja.
        ACEITE_H                                                            // Continúa el cálculo de caja.
    };                                                                      // Continúa el cálculo de caja.

    return caja;                                                            // Devuelve caja como resultado de la función.
}

uint8_t ObtenerColorAceitePixel(                                        // Devuelve el color del aceite o su llama para este píxel.
    int x,                                                                  // Recibe coordenada X.
    int y,                                                                  // Recibe coordenada Y.
    uint16_t *color
)
{
    int aceiteY =                                                           // Calcula y guarda aceiteY.
        AceiteY();                                                          // Continúa el cálculo de aceiteY.


    /* --------------------------------------------------------
     * CUERPO DEL BARRIL DE ACEITE
     * -------------------------------------------------------- */
    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        x >= ACEITE_X &&                                                    // Comprueba que X haya alcanzado el borde izquierdo de barril de aceite.
        x < ACEITE_X + ACEITE_W &&                                          // Comprueba que X no pase el borde derecho de barril de aceite.
        y >= aceiteY &&                                                     // Añade esta condición a la comprobación actual.
        y < aceiteY + ACEITE_H                                              // Evalúa esta comparación como parte de la condición actual.
    ) {
        *color =                                                            // Guarda el color que debe usar el píxel.
            COLOR_ACEITE;                                                   // Continúa el cálculo de color final del píxel.

        if (                                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            y == aceiteY ||                                                 // Añade esta condición a la comprobación actual.
            y == aceiteY + ACEITE_H / 2 ||                                  // Añade esta condición a la comprobación actual.
            y == aceiteY + ACEITE_H - 1                                     // Evalúa esta comparación como parte de la condición actual.
        ) {
            *color =                                                        // Guarda el color que debe usar el píxel.
                COLOR_ACEITE_BORDE;                                         // Continúa el cálculo de color final del píxel.
        }

        return 1;                                                           // Devuelve 1 para indicar que sí se encontró o sí se cumple la condición buscada.
    }


    /* --------------------------------------------------------
     * ESTADO ENCENDIDO
     * -------------------------------------------------------- */
    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        estadoAceite ==                                                     // Evalúa esta comparación como parte de la condición actual.
        ACEITE_ENCENDIDO                                                // Barril de aceite encendido después del primer barril azul.
    ) {
        if (                                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            x >= ACEITE_X + 3 &&                                            // Añade esta condición a la comprobación actual.
            x < ACEITE_X + ACEITE_W - 3 &&                                  // Añade esta condición a la comprobación actual.
            y >= aceiteY - 4 &&                                             // Añade esta condición a la comprobación actual.
            y < aceiteY                                                     // Evalúa esta comparación como parte de la condición actual.
        ) {
            *color =                                                        // Guarda el color que debe usar el píxel.
                COLOR_ACEITE_FUEGO;                                         // Continúa el cálculo de color final del píxel.

            return 1;                                                       // Devuelve 1 para indicar que sí se encontró o sí se cumple la condición buscada.
        }

        if (                                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            x >= ACEITE_X + 5 &&                                            // Añade esta condición a la comprobación actual.
            x < ACEITE_X + ACEITE_W - 5 &&                                  // Añade esta condición a la comprobación actual.
            y >= aceiteY - ACEITE_FUEGO_H &&                                // Añade esta condición a la comprobación actual.
            y < aceiteY - 2                                                 // Evalúa esta comparación como parte de la condición actual.
        ) {
            *color =                                                        // Guarda el color que debe usar el píxel.
                COLOR_ACEITE_FUEGO_CENTRO;                                  // Continúa el cálculo de color final del píxel.

            return 1;                                                       // Devuelve 1 para indicar que sí se encontró o sí se cumple la condición buscada.
        }
    }


    return 0;                                                               // Devuelve 0 para indicar que no se encontró o no se cumple la condición buscada.
}


void DibujarAceite(void)                                                // Dibuja el aceite y su llama píxel por píxel.
{
    int yInicio =                                                           // Calcula y guarda yInicio.
        AceiteY() -                                                         // Continúa el cálculo de yInicio.
        ACEITE_FUEGO_H;                                                     // Continúa el cálculo de yInicio.

    int yFin =                                                              // Calcula y guarda yFin.
        AceiteY() +                                                         // Continúa el cálculo de yFin.
        ACEITE_H;                                                           // Continúa el cálculo de yFin.


    for (                                                                   // Recorre todas las posiciones necesarias de este bloque.
        int y = yInicio;                                                    // Actualiza la posición vertical del objeto.
        y < yFin;                                                           // Evalúa esta comparación como parte de la condición actual.
        y++                                                                 // Incrementa posición vertical en una unidad.
    ) {
        for (                                                               // Recorre todas las posiciones necesarias de este bloque.
            int x = ACEITE_X;                                               // Actualiza la posición horizontal del objeto.
            x < ACEITE_X + ACEITE_W;                                        // Evalúa esta comparación como parte de la condición actual.
            x++                                                             // Incrementa posición horizontal en una unidad.
        ) {
            uint16_t color;                                                 // Reserva color final del píxel para usarla en este bloque.

            if (                                                            // Comprueba si se cumplen las condiciones necesarias para continuar.
                ObtenerColorAceitePixel(                                    // Ejecuta ObtenerColorAceitePixel().
                    x,                                                      // Indica coordenada X del píxel.
                    y,                                                      // Indica coordenada Y del píxel.
                    &color                                                  // Indica variable donde se devuelve el color del aceite.
                )
            ) {
                FillRect(                                                   // Ejecuta FillRect().
                    x,                                                      // Indica coordenada X inicial.
                    y,                                                      // Indica coordenada Y inicial.
                    1,                                                      // Indica ancho del rectángulo.
                    1,                                                      // Indica alto del rectángulo.
                    color                                                   // Indica color RGB565.
                );                                                          // Finaliza la llamada a FillRect().
            }
        }
    }
}


void EncenderAceite(void)                                               // Cambia el aceite de apagado a encendido una sola vez y marca su zona como pendiente de redibujar.
{

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        estadoAceite ==                                                     // Evalúa esta comparación como parte de la condición actual.
        ACEITE_APAGADO                                                  // Barril de aceite aún sin llama.
    ) {
        estadoAceite =                                                      // Actualiza estadoAceite.
            ACEITE_ENCENDIDO;                                               // Continúa el cálculo de estadoAceite.

        aceiteNecesitaRedibujar =                                           // Actualiza aceiteNecesitaRedibujar.
            1;                                                              // Continúa el cálculo de aceiteNecesitaRedibujar.
    }
}

uint8_t BarrilTocaAceite(const Barril *b)                               // Comprueba si un barril azul activo intersecta la caja del barril de aceite.
{
    if (!b->activo || b->tipo != BARRIL_AZUL)                               // Comprueba si se cumplen las condiciones necesarias para continuar.
        return 0;                                                           // Devuelve 0 para indicar que no se encontró o no se cumple la condición buscada.

    AABB cajaBarril = {                                                     // Calcula y guarda cajaBarril.
        b->x,                                                               // Evalúa esta comparación como parte de la condición actual.
        b->y,                                                               // Evalúa esta comparación como parte de la condición actual.
        b->w,                                                               // Evalúa esta comparación como parte de la condición actual.
        b->h                                                                // Evalúa esta comparación como parte de la condición actual.
    };                                                                      // Continúa el cálculo de cajaBarril.

    return ColisionAABB(                                                    // Devuelve ColisionAABB( como resultado de la función.
        cajaBarril,
        CajaAceite()                                                        // Devuelve la caja AABB del barril de aceite para poder comprobar colisiones.
    );
}



/* ============================================================
 * RENDER POR REGIONES - CONTROL DEL PARPADEO
 *
 * Se calcula cómo debe quedar una zona completa antes de enviarla a la TFT.
 * Solo se redibujan las regiones que cambian, evitando mostrar primero el fondo y después los sprites.
 * ============================================================ */

static uint16_t ColorSpriteJugador(const Jugador *j, int x, int y)      // Devuelve el píxel del jugador según estado y orientación.
{
    int localX = j->mirandoDerecha ? x - j->x : JUGADOR_W - 1 - (x - j->x); // Calcula y guarda coordenada X local dentro del sprite.
    int localY = y - j->y;                                                  // Calcula y guarda coordenada Y local dentro del sprite.
    if (j->muriendo || !j->vidas) {                                         // Comprueba una condición del jugador 2 antes de continuar.
        const uint16_t *muerte = (j == &jugador2) ? muerteLuigi12 : muerteMario12; // Calcula y guarda muerte.
        uint32_t cuadro = j->muerteCuadro;                                  // Calcula y guarda cuadro.
        return muerte[(cuadro * JUGADOR_H + localY) * JUGADOR_W + localX];  // Devuelve muerte como resultado de la función.
    }
    if (j->escaleraActual >= 0) {                                           // Comprueba una condición del jugador 2 antes de continuar.
        const uint16_t *espalda = (j == &jugador2) ? escaleraLuigi12 : escaleraMario12; // Calcula y guarda espalda.
        return espalda[localY * JUGADOR_W + localX];                        // Devuelve espalda como resultado de la función.
    }
    const uint16_t *hoja = (j == &jugador2) ? spriteLuigi : spriteMario;    // Calcula y guarda hoja.
    int cuadro = (!j->enSuelo || j->escaleraActual >= 0) ? 0 :              // Calcula y guarda cuadro.
        ((j->direccion == 'L' || j->direccion == 'R') ? ((j->x / 3) & 1) : 1); // Comprueba si el comando actual indica movimiento hacia la izquierda.
    uint16_t pixel = hoja[(cuadro * JUGADOR_H + localY) * JUGADOR_W + localX]; // Calcula y guarda posición lineal del píxel.
    if (pixel && j->color == COLOR_GOLPE) pixel = COLOR_GOLPE;              // Comprueba si se cumplen las condiciones necesarias para continuar.
    return pixel;                                                           // Devuelve posición lineal del píxel como resultado de la función.
}

uint16_t ObtenerColorFinalPixel(int x, int y)                           // Es el compositor principal del render:.
{
    uint16_t color =                                                        // Guarda el color que debe usar el píxel.
        FondoGetColor(x, y);                                                // Continúa el cálculo de color final del píxel.

    if (x >= PEACH_X && x < PEACH_X + PEACH_W &&                            // Comprueba si se cumplen las condiciones necesarias para continuar.
        y >= PEACH_Y && y < PEACH_Y + PEACH_H) {                            // Evalúa esta comparación como parte de la condición actual.
        uint16_t pixel = spritePeach[(y - PEACH_Y) * PEACH_W + x - PEACH_X]; // Calcula y guarda posición lineal del píxel.
        if (pixel) color = pixel;                                           // Comprueba si se cumplen las condiciones necesarias para continuar.
    }
    if (pantallaActual == JUEGO_RESCATE && corazonVisible &&                // Comprueba si se cumplen las condiciones necesarias para continuar.
        x >= CORAZON_X && x < CORAZON_X + CORAZON_W &&                      // Añade esta condición a la comprobación actual.
        y >= CORAZON_Y && y < CORAZON_Y + CORAZON_H) {                      // Evalúa esta comparación como parte de la condición actual.
        uint16_t pixel = corazonSprite[(y - CORAZON_Y) * CORAZON_W + x - CORAZON_X]; // Calcula y guarda posición lineal del píxel.
        if (pixel) color = pixel;                                           // Comprueba si se cumplen las condiciones necesarias para continuar.
    }
    if (ayudaVisible && x >= HELP_X && x < HELP_X + HELP_W &&               // Comprueba si se cumplen las condiciones necesarias para continuar.
        y >= HELP_Y && y < HELP_Y + HELP_H) {                               // Evalúa esta comparación como parte de la condición actual.
        uint16_t pixel = ayudaSprite[(y - HELP_Y) * HELP_W + x - HELP_X];   // Calcula y guarda posición lineal del píxel.
        if (pixel) color = pixel;                                           // Comprueba si se cumplen las condiciones necesarias para continuar.
    }


    /* --------------------------------------------------------
     * ACEITE
     * -------------------------------------------------------- */
    uint16_t colorAceite;                                                   // Reserva colorAceite para usarla en este bloque.

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        ObtenerColorAceitePixel(                                            // Ejecuta ObtenerColorAceitePixel().
            x,                                                              // Indica coordenada X del píxel.
            y,                                                              // Indica coordenada Y del píxel.
            &colorAceite                                                    // Indica variable donde se devuelve el color del aceite.
        )
    ) {
        color =                                                             // Guarda el color que debe usar el píxel.
            colorAceite;                                                    // Continúa el cálculo de color final del píxel.
    }


    /* --------------------------------------------------------
     * MARTILLOS DISPONIBLES
     *
     * Comprueba si el píxel pertenece a un martillo disponible en el escenario.
     * Si coincide, coloca el color del pickup sobre las capas anteriores.
     * -------------------------------------------------------- */
    uint16_t colorMartillo;                                                 // Reserva colorMartillo para usarla en este bloque.

    if (                                                                    // Inicia la comprobación correspondiente a martillos disponibles.
        ObtenerColorMartilloEscenarioPixel(                                 // Comprueba si una coordenada pertenece a un martillo disponible y devuelve su color.
            x,                                                              // Indica coordenada X del píxel.
            y,                                                              // Indica coordenada Y del píxel.
            &colorMartillo                                                  // Indica variable donde se devuelve el color del martillo.
        )
    ) {
        color =                                                             // Guarda el color que debe usar el píxel.
            colorMartillo;                                                  // Continúa el cálculo de color final del píxel.
    }

    /* Pila de barriles junto a DK, con el mismo sprite de los rodantes. */
    for (int i = 0; i < PILA_BARRILES_CANTIDAD; i++) {                      // Recorre todas las posiciones necesarias de este bloque.
        int bx = pilaBarrilesX[i], by = pilaBarrilesY[i];                   // Calcula y guarda coordenada X del centro del barril.
        if (x >= bx && x < bx + BARRIL_W &&                                 // Inicia la comprobación correspondiente a martillos disponibles.
            y >= by && y < by + BARRIL_H) {                                 // Evalúa esta comparación como parte de la condición actual.
            uint16_t pixel = spriteBarriles[((i & 1) * BARRIL_H +           // Calcula y guarda posición lineal del píxel.
                                             y - by) * BARRIL_W + x - bx];  // Continúa el cálculo de posición lineal del píxel.
            if (pixel) color = pixel;                                       // Inicia la comprobación correspondiente a martillos disponibles.
        }
    }


    /* --------------------------------------------------------
     * DONKEY KONG
     *
     * Comprueba si el píxel pertenece al área de DK.
     * Después busca ese píxel dentro del frame actual de la spritesheet y lo dibuja si no es transparente.
     * -------------------------------------------------------- */
    if (                                                                    // Inicia la comprobación correspondiente a donkey kong.
        x >= DK_X &&                                                        // Comprueba que X haya alcanzado el borde izquierdo de DK.
        x < DK_X + DK_W &&                                                  // Comprueba que X no pase el borde derecho de DK.
        y >= DK_Y &&                                                        // Comprueba que Y haya alcanzado el borde superior de DK.
        y < DK_Y + DK_H                                                     // Comprueba que Y no pase el borde inferior de DK.
    ) {
        int localX =                                                        // Calcula y guarda coordenada X local dentro del sprite.
            x - DK_X;                                                       // Convierte la X de pantalla en una X local dentro del sprite de DK.

        int localY =                                                        // Calcula y guarda coordenada Y local dentro del sprite.
            y - DK_Y;                                                       // Convierte la Y de pantalla en una Y local dentro del sprite de DK.

        int anchoSpritesheet =                                              // Calcula y guarda ancho total de la spritesheet.
            DK_W * DK_FRAMES;                                               // Multiplica el ancho de un frame por la cantidad total de frames de DK.

        uint32_t origen =                                                   // Calcula y guarda índice del píxel dentro de la spritesheet.
            (uint32_t)localY *                                              // Selecciona la fila correspondiente dentro de la spritesheet de DK.
            (uint32_t)anchoSpritesheet +                                    // Convierte esa fila en un desplazamiento dentro del arreglo de la spritesheet.
            (uint32_t)dkFrame *                                             // Selecciona el frame de animación que DK muestra actualmente.
            (uint32_t)DK_W +                                                // Multiplica el número de frame por el ancho de cada frame.
            (uint32_t)localX;                                               // Añade la columna exacta dentro del frame seleccionado.

        uint16_t colorDK =                                                  // Calcula y guarda color del píxel actual de DK.
            dkongAnim[origen];                                              // Lee el color RGB565 del píxel calculado en la spritesheet de DK.

        if (                                                                // Inicia la comprobación correspondiente a donkey kong.
            colorDK !=                                                      // Comienza a comparar el píxel de DK con el color transparente.
            DK_TRANSPARENTE                                                 // Usa el color reservado como transparencia de la spritesheet de DK.
        ) {
            color =                                                         // Guarda el color que debe usar el píxel.
                colorDK;                                                    // Copia el píxel visible de DK al color final de la pantalla.
        }
    }


    /* --------------------------------------------------------
     * FLAMAS
     *
     * Recorre las flamas activas y comprueba si alguna ocupa este píxel.
     * Si coincide, superpone el píxel del sprite de la flama sobre las capas anteriores.
     * -------------------------------------------------------- */
    for (                                                                   // Recorre todos los slots de flamas.
        int i = 0;                                                          // Reinicia i a cero.
        i < MAX_FIREBALLS;                                                  // Mantiene el recorrido dentro de todos los slots de flamas.
        i++                                                                 // Incrementa i en una unidad.
    ) {
        const Fireball *f =                                                 // Calcula y guarda f.
            &fireballs[i];                                                  // Accede a la posición correspondiente del arreglo fireballs.

        if (                                                                // Inicia la comprobación correspondiente a flamas.
            !f->activo ||                                                   // Comprueba que la flama esté inactivo.
            f->estado == FLAMA_NACIENDO                                     // Comprueba si el estado es FLAMA_NACIENDO: Flama creada pero todavía oculta dentro del barril de aceite.
        )
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.

        if (                                                                // Inicia la comprobación correspondiente a flamas.
            x >= f->x &&                                                    // Comprueba que X haya alcanzado el borde izquierdo de flama.
            x < f->x + f->w &&                                              // Comprueba que X no pase el borde derecho de flama.
            y >= f->y &&                                                    // Comprueba que Y haya alcanzado el borde superior de flama.
            y < f->y + f->h                                                 // Comprueba que Y no pase el borde inferior de flama.
        ) {
            int cuadro = cuadroFlamaActual;                                 // Calcula y guarda cuadro.
            uint16_t pixel = flamaNueva[(cuadro * FIREBALL_H + y - f->y) *  // Calcula y guarda posición lineal del píxel.
                                          FIREBALL_W + x - f->x];           // Evalúa esta comparación como parte de la condición actual.
            if (pixel) color = pixel;                                       // Inicia la comprobación correspondiente a flamas.
        }                                                                   // Continúa el cálculo de posición lineal del píxel.
    }                                                                       // Continúa el cálculo de posición lineal del píxel.


    /* --------------------------------------------------------
     * BARRILES
     *
     * Recorre los barriles activos y comprueba si alguno ocupa este píxel.
     * Si coincide, superpone el píxel del barril sobre las capas anteriores.
     * -------------------------------------------------------- */
    for (                                                                   // Recorre todos los slots de barriles.
        int i = 0;                                                          // Reinicia i a cero.
        i < MAX_BARRILES;                                                   // Mantiene el recorrido dentro de todos los slots de barriles.
        i++                                                                 // Incrementa i en una unidad.
    ) {
        const Barril *b =                                                   // Calcula y guarda b.
            &barriles[i];                                                   // Accede a la posición correspondiente del arreglo barriles.

        if (!b->activo)                                                     // Inicia la comprobación correspondiente a barriles.
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.

        if (                                                                // Inicia la comprobación correspondiente a barriles.
            x >= b->x &&                                                    // Comprueba que X haya alcanzado el borde izquierdo de barril.
            x < b->x + b->w &&                                              // Comprueba que X no pase el borde derecho de barril.
            y >= b->y &&                                                    // Comprueba que Y haya alcanzado el borde superior de barril.
            y < b->y + b->h                                                 // Comprueba que Y no pase el borde inferior de barril.
        ) {
            int frame = (b->x / 4) & 1;                                     // Calcula y guarda frame de animación.
            uint16_t pixel = spriteBarriles[(((int)b->tipo * 2 + frame) *   // Calcula y guarda posición lineal del píxel.
                BARRIL_H + (y - b->y)) * BARRIL_W + (x - b->x)];            // Evalúa esta comparación como parte de la condición actual.
            if (pixel) color = pixel;                                       // Inicia la comprobación correspondiente a barriles.
        }                                                                   // Continúa el cálculo de posición lineal del píxel.
    }                                                                       // Continúa el cálculo de posición lineal del píxel.


    /* --------------------------------------------------------
     * MARTILLOS EN MANO
     *
     * Comprueba los martillos que llevan J1 y J2.
     * Si el píxel pertenece al martillo, lo superpone según su fase arriba o al frente.
     * -------------------------------------------------------- */
    if (                                                                    // Inicia la comprobación correspondiente a martillos en mano.
        ObtenerColorMartilloJugadorPixel(                                   // Continúa el cálculo de posición lineal del píxel.
            &jugador1,                                                      // Indica jugador que puede llevar el martillo.
            x,                                                              // Indica coordenada X del píxel.
            y,                                                              // Indica coordenada Y del píxel.
            &colorMartillo                                                  // Indica variable donde se devuelve el color del martillo.
        )                                                                   // Continúa el cálculo de posición lineal del píxel.
    ) {                                                                     // Continúa el cálculo de posición lineal del píxel.
        color =                                                             // Guarda el color que debe usar el píxel.
            colorMartillo;                                                  // Continúa el cálculo de color final del píxel.
    }

    if (                                                                    // Inicia la comprobación correspondiente a martillos en mano.
        ObtenerColorMartilloJugadorPixel(                                   // Ejecuta ObtenerColorMartilloJugadorPixel().
            &jugador2,                                                      // Indica jugador que puede llevar el martillo.
            x,                                                              // Indica coordenada X del píxel.
            y,                                                              // Indica coordenada Y del píxel.
            &colorMartillo                                                  // Indica variable donde se devuelve el color del martillo.
        )
    ) {
        color =                                                             // Guarda el color que debe usar el píxel.
            colorMartillo;                                                  // Continúa el cálculo de color final del píxel.
    }


    /* --------------------------------------------------------
     * JUGADOR 1
     *
     * Comprueba si el píxel pertenece a J1 y obtiene el color correcto de su sprite.
     * El estado, frame, orientación y daño determinan qué píxel se muestra.
     * -------------------------------------------------------- */
    if (                                                                    // Inicia la comprobación correspondiente a jugador 1.
        x >= jugador1.x &&                                                  // Comprueba que X haya alcanzado el borde izquierdo de J1.
        x < jugador1.x + jugador1.w &&                                      // Comprueba que X no pase el borde derecho de J1.
        y >= jugador1.y &&                                                  // Comprueba que Y haya alcanzado el borde superior de J1.
        y < jugador1.y + jugador1.h                                         // Comprueba que Y no pase el borde inferior de J1.
    ) {
        uint16_t pixel = ColorSpriteJugador(&jugador1, x, y);               // Calcula y guarda posición lineal del píxel.
        if (pixel) color = pixel;                                           // Inicia la comprobación correspondiente a jugador 1.
    }


    /* --------------------------------------------------------
     * JUGADOR 2
     *
     * Comprueba si el píxel pertenece a J2 y obtiene el color correcto de su sprite.
     * El estado, frame, orientación y daño determinan qué píxel se muestra.
     * -------------------------------------------------------- */
    if (                                                                    // Inicia la comprobación correspondiente a jugador 2.
        x >= jugador2.x &&                                                  // Comprueba que X haya alcanzado el borde izquierdo de J2.
        x < jugador2.x + jugador2.w &&                                      // Comprueba que X no pase el borde derecho de J2.
        y >= jugador2.y &&                                                  // Comprueba que Y haya alcanzado el borde superior de J2.
        y < jugador2.y + jugador2.h                                         // Comprueba que Y no pase el borde inferior de J2.
    ) {
        uint16_t pixel = ColorSpriteJugador(&jugador2, x, y);               // Calcula y guarda posición lineal del píxel.
        if (pixel) color = pixel;                                           // Inicia la comprobación correspondiente a jugador 2.
    }

    Jugador *js[2] = { &jugador1, &jugador2 };                              // Calcula y guarda js.
    for (int p = 0; p < 2; p++) {                                           // Recorre todas las posiciones necesarias de este bloque.
        const Jugador *j = js[p];                                           // Calcula y guarda j.
        int avisoX = j->letreroFijo ? j->letreroX : j->x - 2;               // Calcula y guarda avisoX.
        int avisoY = j->letreroFijo ? j->letreroY :                         // Calcula y guarda avisoY.
                     j->y - PUNTOS_H - 2;                                   // Evalúa esta comparación como parte de la condición actual.
        if (j->letreroVisible && x >= avisoX && x < avisoX + PUNTOS_W &&    // Inicia la comprobación correspondiente a jugador 2.
            y >= avisoY && y < avisoY + PUNTOS_H) {                         // Evalúa esta comparación como parte de la condición actual.
            int localX = x - avisoX, localY = y - avisoY;                   // Calcula y guarda coordenada X local dentro del sprite.
            const uint16_t *sprite = j->letreroValor >= 200U ?              // Calcula y guarda sprite.
                                     puntos200Sprite : puntosSprite;        // Continúa el cálculo de sprite.
            uint16_t pixel = sprite[localY * PUNTOS_W + localX];            // Calcula y guarda posición lineal del píxel.
            /* El primer digito del sprite 200 pasa de 2 a 3. */
            if (j->letreroValor == 300U && localY >= 6 && localY <= 8) {    // Inicia la comprobación correspondiente a jugador 2.
                if (localX == 1) pixel = 0U;                                // Inicia la comprobación correspondiente a jugador 2.
                if (localX == 3) pixel = 0xFFDFU;                           // Inicia la comprobación correspondiente a jugador 2.
            }
            if (pixel) color = pixel;                                       // Inicia la comprobación correspondiente a jugador 2.
        }
    }

    /* La chispa del golpe permanece visible aun despues de borrar el barril. */
    for (int p = 0; p < 2; p++) {                                           // Recorre todas las posiciones necesarias de este bloque.
        const Jugador *j = js[p];                                           // Calcula y guarda j.
        if (j->golpePausaHasta &&                                           // Inicia la comprobación correspondiente a jugador 2.
            (int32_t)(j->golpePausaHasta - HAL_GetTick()) > 0 &&            // Añade esta condición a la comprobación actual.
            x >= j->golpeImpactoX && x < j->golpeImpactoX + 12 &&           // Añade esta condición a la comprobación actual.
            y >= j->golpeImpactoY && y < j->golpeImpactoY + 12) {           // Evalúa esta comparación como parte de la condición actual.
            uint16_t pixel = golpeBarril12[(y - j->golpeImpactoY) * 12 +    // Calcula y guarda posición lineal del píxel.
                                           x - j->golpeImpactoX];           // Evalúa esta comparación como parte de la condición actual.
            if (pixel) color = pixel;                                       // Inicia la comprobación correspondiente a jugador 2.
        }                                                                   // Continúa el cálculo de posición lineal del píxel.
    }                                                                       // Continúa el cálculo de posición lineal del píxel.


    return                                                                  // Termina la función en este punto.
        color;                                                              // Continúa el cálculo de posición lineal del píxel.
}


void RenderizarRegionFinal(                                             // Recalcula y envía a la TFT solo una región pequeña con su imagen final ya compuesta.
    int x,                                                                  // Recibe coordenada X.
    int y,                                                                  // Recibe coordenada Y.
    int w,                                                              // Ancho de la caja o sprite.
    int h                                                               // Alto de la caja o sprite.
)
{
    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        w <= 0 ||                                                           // Añade esta condición a la comprobación actual.
        h <= 0                                                              // Evalúa esta comparación como parte de la condición actual.
    ) {
        return;                                                             // Termina la función en este punto.
    }


    /* Clip */
    if (x < 0) {                                                            // Comprueba si se cumplen las condiciones necesarias para continuar.
        w += x;                                                             // Aumenta ancho con este valor.
        x = 0;                                                              // Actualiza la posición horizontal del objeto.
    }

    if (y < 0) {                                                            // Comprueba si se cumplen las condiciones necesarias para continuar.
        h += y;                                                             // Aumenta alto con este valor.
        y = 0;                                                              // Actualiza la posición vertical del objeto.
    }

    if (x + w > SCREEN_W)                                                   // Comprueba si se cumplen las condiciones necesarias para continuar.
        w = SCREEN_W - x;                                               // Ancho de la caja o sprite.

    if (y + h > SCREEN_H)                                                   // Comprueba si se cumplen las condiciones necesarias para continuar.
        h = SCREEN_H - y;                                               // Alto de la caja o sprite.

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        w <= 0 ||                                                           // Añade esta condición a la comprobación actual.
        h <= 0                                                              // Evalúa esta comparación como parte de la condición actual.
    ) {
        return;                                                             // Termina la función en este punto.
    }


    for (                                                                   // Recorre todas las posiciones necesarias de este bloque.
        int py = y;                                                         // Calcula y guarda altura Y de la plataforma.
        py < y + h;                                                         // Evalúa esta comparación como parte de la condición actual.
        py++                                                                // Incrementa altura Y de la plataforma en una unidad.
    ) {
        int inicioRun =                                                     // Calcula y guarda inicioRun.
            x;                                                              // Continúa el cálculo de inicioRun.

        uint16_t colorRun =                                                 // Calcula y guarda colorRun.
            ObtenerColorFinalPixel(                                         // Continúa el cálculo de colorRun.
                x,                                                          // Continúa el cálculo de colorRun.
                py                                                          // Continúa el cálculo de colorRun.
            );                                                              // Continúa el cálculo de colorRun.

        for (                                                               // Recorre todas las posiciones necesarias de este bloque.
            int px = x + 1;                                                 // Calcula y guarda px.
            px < x + w;                                                     // Evalúa esta comparación como parte de la condición actual.
            px++                                                            // Incrementa px en una unidad.
        ) {
            uint16_t nuevoColor =                                           // Calcula y guarda nuevoColor.
                ObtenerColorFinalPixel(                                     // Continúa el cálculo de nuevoColor.
                    px,                                                     // Continúa el cálculo de nuevoColor.
                    py                                                      // Continúa el cálculo de nuevoColor.
                );                                                          // Continúa el cálculo de nuevoColor.

            if (                                                            // Comprueba si se cumplen las condiciones necesarias para continuar.
                nuevoColor !=                                               // Evalúa esta comparación como parte de la condición actual.
                colorRun
            ) {
                FillRect(                                                   // Ejecuta FillRect().
                    inicioRun,                                              // Indica coordenada X inicial.
                    py,                                                     // Indica coordenada Y inicial.
                    px - inicioRun,                                         // Indica ancho del rectángulo.
                    1,                                                      // Indica alto del rectángulo.
                    colorRun                                                // Indica color RGB565.
                );                                                          // Finaliza la llamada a FillRect().

                inicioRun =                                                 // Actualiza inicioRun.
                    px;                                                     // Continúa el cálculo de inicioRun.

                colorRun =                                                  // Actualiza colorRun.
                    nuevoColor;                                             // Continúa el cálculo de colorRun.
            }
        }

        FillRect(                                                           // Ejecuta FillRect().
            inicioRun,                                                      // Indica coordenada X inicial.
            py,                                                             // Indica coordenada Y inicial.
            x + w - inicioRun,                                              // Indica ancho del rectángulo.
            1,                                                              // Indica alto del rectángulo.
            colorRun                                                        // Indica color RGB565.
        );                                                                  // Finaliza la llamada a FillRect().
    }
}


void RenderizarUnionMovimiento(                                         // Redibuja la zona que cubre la posición vieja y la nueva.
    int oldX,                                                               // Recibe el valor oldX.
    int oldY,                                                               // Recibe el valor oldY.
    int newX,                                                               // Recibe el valor newX.
    int newY,                                                               // Recibe el valor newY.
    int w,                                                              // Ancho de la caja o sprite.
    int h                                                               // Alto de la caja o sprite.
)
{
    int x1 =                                                                // Calcula y guarda x1.
        (oldX < newX)                                                       // Evalúa esta comparación como parte de la condición actual.
        ? oldX                                                              // Usa esta opción cuando la condición del operador ternario es verdadera.
        : newX;                                                             // Usa esta opción cuando la condición del operador ternario es falsa.

    int y1 =                                                                // Calcula y guarda y1.
        (oldY < newY)                                                       // Evalúa esta comparación como parte de la condición actual.
        ? oldY                                                              // Usa esta opción cuando la condición del operador ternario es verdadera.
        : newY;                                                             // Usa esta opción cuando la condición del operador ternario es falsa.

    int oldX2 =                                                             // Calcula y guarda oldX2.
        oldX + w;                                                           // Continúa el cálculo de oldX2.

    int newX2 =                                                             // Calcula y guarda newX2.
        newX + w;                                                           // Continúa el cálculo de newX2.

    int oldY2 =                                                             // Calcula y guarda oldY2.
        oldY + h;                                                           // Continúa el cálculo de oldY2.

    int newY2 =                                                             // Calcula y guarda newY2.
        newY + h;                                                           // Continúa el cálculo de newY2.

    int x2 =                                                                // Calcula y guarda x2.
        (oldX2 > newX2)                                                     // Evalúa esta comparación como parte de la condición actual.
        ? oldX2                                                             // Usa esta opción cuando la condición del operador ternario es verdadera.
        : newX2;                                                            // Usa esta opción cuando la condición del operador ternario es falsa.

    int y2 =                                                                // Calcula y guarda y2.
        (oldY2 > newY2)                                                     // Evalúa esta comparación como parte de la condición actual.
        ? oldY2                                                             // Usa esta opción cuando la condición del operador ternario es verdadera.
        : newY2;                                                            // Usa esta opción cuando la condición del operador ternario es falsa.

    RenderizarRegionFinal(                                              // Redibuja esta región con todas las capas finales.
        x1,                                                                 // Indica X inicial de la región.
        y1,                                                                 // Indica Y inicial de la región.
        x2 - x1,                                                            // Indica ancho de la región.
        y2 - y1                                                             // Indica alto de la región.
    );                                                                      // Finaliza la llamada a RenderizarRegionFinal().
}


/* ============================================================
 * DAÑO, VIDAS E INVULNERABILIDAD
 *
 * Se procesa el daño recibido por Mario o Luigi: se resta una vida y se inicia la animación de muerte.
 * También se aplica una invulnerabilidad temporal para evitar varios golpes seguidos.
 * ============================================================ */

void MarcarGolpeJugador(Jugador *j, uint32_t *golpeHasta)               // Resta una vida e inicia la animación y protección tras el golpe.
{
    uint32_t ahora = HAL_GetTick();                                     // Tick actual para controlar temporizadores.
    if (!j->vidas || j->muriendo || (int32_t)(j->invulnerableHasta - ahora) > 0) // Comprueba si se cumplen las condiciones necesarias para continuar.
        return;                                                             // Termina la función en este punto.
    j->vidas--;                                                             // Reduce vidas restantes en una unidad.
    j->muriendo = 1;                                                    // Bandera de animación/estado de muerte.
    j->muerteDesde = ahora;                                             // Tick en que empezó la muerte.
    j->muerteCuadro = 0;                                                // Cuadro actual de la animación de muerte.
    if (j->golpePausaHasta) {                                               // Comprueba si se cumplen las condiciones necesarias para continuar.
        j->golpePausaHasta = 0;                                         // Fin de la pausa breve posterior a un impacto.
        RenderizarRegionFinal(j->golpeImpactoX, j->golpeImpactoY, 12, 12); // Redibuja esta región con todas las capas finales.
    }
    j->golpePausaHasta = 0;                                             // Fin de la pausa breve posterior a un impacto.
    if (j->martilloActivo) FinalizarMartilloJugador(j);                     // Comprueba si se cumplen las condiciones necesarias para continuar.
    j->letreroVisible = 0;                                              // Marca si debe dibujarse el popup de puntos.
    j->saltoSolicitado = 0;                                             // Bandera que indica que hay un salto pendiente de ejecutar.
    j->escaleraActual = -1;                                             // Índice de la escalera utilizada actualmente; -1 significa ninguna.
    j->vy = 0;                                                          // Velocidad vertical actual.
    Audio_Enviar(0x21U);                                                // Silencia la música durante la muerte.
    Audio_Enviar(0x0AU);                                                // Reproduce el sonido de muerte.
    j->invulnerableHasta = ahora + INVULNERABLE_MS;                     // Tick hasta el que se ignoran nuevos daños.
    j->color = COLOR_GOLPE;                                                 // Guarda el color que debe usar el píxel.
    *golpeHasta = ahora + 250U;                                             // Actualiza golpeHasta.
    hudSucio = 1;                                                           // Activa hudSucio.
}

void ActualizarGolpes(void)                                             // Termina el efecto visual de golpe al vencer su temporizador.
{
    uint32_t ahora = HAL_GetTick();                                     // Tick actual para controlar temporizadores.

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        golpeJ1Hasta != 0 &&                                                // Añade esta condición a la comprobación actual.
        (int32_t)(ahora - golpeJ1Hasta) >= 0                                // Evalúa esta comparación como parte de la condición actual.
    ) {
        jugador1.color = COLOR_J1;                                          // Guarda el color que debe usar el jugador 1.
        golpeJ1Hasta = 0;                                                   // Reinicia golpeJ1Hasta a cero.
    }

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        golpeJ2Hasta != 0 &&                                                // Añade esta condición a la comprobación actual.
        (int32_t)(ahora - golpeJ2Hasta) >= 0                                // Evalúa esta comparación como parte de la condición actual.
    ) {
        jugador2.color = COLOR_J2;                                          // Guarda el color que debe usar el jugador 2.
        golpeJ2Hasta = 0;                                                   // Reinicia golpeJ2Hasta a cero.
    }
}


/* ============================================================
 * JUGADORES - MOVIMIENTO Y FÍSICA
 *
 * Se controla a Mario y Luigi con la misma lógica: caminar, saltar, caer, usar escaleras y mantenerse sobre las plataformas.
 * Las funciones reciben un Jugador para aplicar el mismo comportamiento a J1 o J2.
 * ============================================================ */

int BuscarPlataformaSoporte(const Jugador *j)                           // Busca qué plataforma está suficientemente cerca de los pies del jugador.
{
    int centroX = j->x + j->w / 2;                                      // Calcula el centro para comparar posiciones.
    int pies = j->y + j->h;                                                 // Calcula y guarda coordenada Y de los pies del jugador.
    int mejor = -1;                                                         // Inicializa mejor opción encontrada indicando que todavía no hay una opción válida.
    int mejorDiferencia = 999;                                              // Inicializa una diferencia grande para buscar después la plataforma más cercana.

    for (uint32_t i = 0; i < NUM_PLATAFORMAS; i++) {                        // Recorre todas las plataformas del escenario.
        const Plataforma *p = &plataformas[i];                              // Calcula y guarda p.

        if (centroX < p->x1 || centroX > p->x2)                             // Comprueba si se cumplen las condiciones necesarias para continuar.
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.

        int py = ObtenerYPlataforma(p, centroX);                        // Calcula la altura exacta de la viga inclinada en esta coordenada X.
        int diferencia = AbsInt(pies - py);                                 // Calcula y guarda distancia vertical hasta la plataforma.

        if (                                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            diferencia <= TOLERANCIA_SUELO &&                               // Añade esta condición a la comprobación actual.
            diferencia < mejorDiferencia                                    // Evalúa esta comparación como parte de la condición actual.
        ) {
            mejor = (int)i;                                                 // Calcula y guarda mejor opción encontrada.
            mejorDiferencia = diferencia;                                   // Calcula y guarda menor diferencia encontrada.
        }
    }

    return mejor;                                                           // Devuelve mejor opción encontrada como resultado de la función.
}

int BuscarPlataformaCaida(                                              // Durante una caída, detecta qué plataforma fue cruzada entre la posición anterior y la nueva.
    const Jugador *j,
    int oldBottom,                                                          // Recibe el valor oldBottom.
    int newBottom                                                           // Recibe el valor newBottom.
)
{
    int centroX = j->x + j->w / 2;                                      // Calcula el centro para comparar posiciones.
    int mejor = -1;                                                         // Inicializa mejor opción encontrada indicando que todavía no hay una opción válida.
    int mejorY = 10000;                                                     // Inicializa una altura grande para poder conservar la plataforma válida más cercana.

    for (uint32_t i = 0; i < NUM_PLATAFORMAS; i++) {                        // Recorre todas las plataformas del escenario.
        const Plataforma *p = &plataformas[i];                              // Calcula y guarda p.

        if (centroX < p->x1 || centroX > p->x2)                             // Comprueba si se cumplen las condiciones necesarias para continuar.
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.

        int py = ObtenerYPlataforma(p, centroX);                        // Calcula la altura exacta de la viga inclinada en esta coordenada X.

        if (                                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            oldBottom <= py + 2 &&                                          // Añade esta condición a la comprobación actual.
            newBottom >= py                                                 // Evalúa esta comparación como parte de la condición actual.
        ) {
            if (py < mejorY) {                                              // Comprueba si se cumplen las condiciones necesarias para continuar.
                mejor = (int)i;                                             // Calcula y guarda mejor opción encontrada.
                mejorY = py;                                                // Calcula y guarda menor altura válida encontrada.
            }
        }
    }

    return mejor;                                                           // Devuelve mejor opción encontrada como resultado de la función.
}

static int BuscarPisoDeEscalera(const Escalera *e, uint8_t superior)    // Relaciona un extremo de una escalera con la plataforma más cercana a ese extremo.
{
    int centroX = e->x + ESCALERA_W / 2;                                // Calcula el centro para comparar posiciones.
    int destinoY = superior ? e->y1 : e->y2;                                // Calcula y guarda destinoY.
    int mejor = -1;                                                         // Inicializa mejor opción encontrada indicando que todavía no hay una opción válida.
    int diferenciaMinima = TOLERANCIA_EXTREMO_ESCALERA + 1;                 // Calcula y guarda diferenciaMinima.

    for (uint32_t i = 0; i < NUM_PLATAFORMAS; i++) {                        // Recorre todas las plataformas del escenario.
        const Plataforma *p = &plataformas[i];                              // Calcula y guarda p.
        if (centroX < p->x1 || centroX > p->x2) continue;                   // Comprueba si se cumplen las condiciones necesarias para continuar.
        int py = ObtenerYPlataforma(p, centroX);                        // Calcula la altura exacta de la viga inclinada en esta coordenada X.
        int diferencia = AbsInt(py - destinoY);                             // Calcula y guarda distancia vertical hasta la plataforma.
        if (diferencia < diferenciaMinima) {                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            mejor = (int)i;                                                 // Calcula y guarda mejor opción encontrada.
            diferenciaMinima = diferencia;                                  // Actualiza diferenciaMinima.
        }
    }
    return mejor;                                                           // Devuelve mejor opción encontrada como resultado de la función.
}

int BuscarEscaleraCercana(const Jugador *j)                             // Determina si el jugador está en posición válida para entrar a una escalera con el comando U o D.
{
    if (!j->enSuelo) return -1;                                             // Comprueba si se cumplen las condiciones necesarias para continuar.

    int mejor = -1;                                                         // Inicializa mejor opción encontrada indicando que todavía no hay una opción válida.
    int distanciaMinima = TOLERANCIA_EXTREMO_ESCALERA + 1;                  // Calcula y guarda distanciaMinima.
    int centroX = j->x + j->w / 2;                                      // Calcula el centro para comparar posiciones.
    int pies = j->y + j->h;                                                 // Calcula y guarda coordenada Y de los pies del jugador.
    uint8_t subir = j->direccion == 'U';                                    // Calcula y guarda subir.

    for (uint32_t i = 0; i < NUM_ESCALERAS; i++) {                          // Recorre todas las escaleras del escenario.
        if ((int)i == j->escaleraSalida) continue;                          // Comprueba una condición de la escalera antes de continuar.
        const Escalera *e = &escaleras[i];                                  // Calcula y guarda e.
        int piso = BuscarPisoDeEscalera(e, subir ? 0U : 1U);                // Calcula y guarda plataforma usada como piso.
        if (piso < 0 || j->plataformaActual != piso) continue;              // Comprueba una condición de la plataforma antes de continuar.
        int py = ObtenerYPlataforma(&plataformas[piso], centroX);       // Calcula la altura exacta de la viga inclinada en esta coordenada X.
        int diferenciaY = AbsInt(pies - py);                                // Calcula y guarda diferenciaY.
        int diferenciaX = AbsInt(centroX - (e->x + ESCALERA_W / 2));        // Calcula y guarda diferenciaX.
        if (diferenciaX <= TOLERANCIA_EXTREMO_ESCALERA &&                   // Comprueba si se cumplen las condiciones necesarias para continuar.
            diferenciaY <= TOLERANCIA_EXTREMO_ESCALERA &&                   // Añade esta condición a la comprobación actual.
            diferenciaX < distanciaMinima) {                                // Evalúa esta comparación como parte de la condición actual.
            mejor = (int)i;                                                 // Calcula y guarda mejor opción encontrada.
            distanciaMinima = diferenciaX;                                  // Actualiza distanciaMinima.
        }
    }

    return mejor;                                                           // Devuelve mejor opción encontrada como resultado de la función.
}

int LimitarXDentroPlataforma(                                           // Restringe el movimiento horizontal para que el jugador no salga fuera de la plataforma o de la pantalla.
    const Jugador *j,
    int nuevaX                                                              // Recibe el valor nuevaX.
)
{
    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        j->plataformaActual < 0 ||                                          // Comprueba si existe una plataforma actual válida.
        j->plataformaActual >= (int)NUM_PLATAFORMAS                         // Evalúa esta comparación como parte de la condición actual.
    ) {
        if (nuevaX < 0)                                                     // Comprueba si se cumplen las condiciones necesarias para continuar.
            nuevaX = 0;                                                     // Reinicia nueva posición X a cero.

        if (nuevaX > SCREEN_W - j->w)                                       // Comprueba si se cumplen las condiciones necesarias para continuar.
            nuevaX = SCREEN_W - j->w;                                       // Calcula y guarda nueva posición X.

        return nuevaX;                                                      // Devuelve nueva posición X como resultado de la función.
    }

    const Plataforma *p =                                                   // Calcula y guarda p.
        &plataformas[j->plataformaActual];                                  // Evalúa esta comparación como parte de la condición actual.

    if (nuevaX < p->x1)                                                     // Comprueba una condición de la plataforma antes de continuar.
        nuevaX = p->x1;                                                     // Calcula y guarda nueva posición X.

    if (nuevaX > p->x2 - j->w)                                              // Comprueba una condición de la plataforma antes de continuar.
        nuevaX = p->x2 - j->w;                                              // Calcula y guarda nueva posición X.

    return nuevaX;                                                          // Devuelve nueva posición X como resultado de la función.
}

void InicializarJugadores(void)                                         // Inicializa posición, vidas, control y estados de ambos jugadores.
{
    int piso = (int)NUM_PLATAFORMAS - 1;                                    // Calcula y guarda plataforma usada como piso.

    jugador1.x = 90;                                                        // Actualiza la posición horizontal del jugador 1.
    jugador1.w = JUGADOR_W;                                             // Ancho de la caja o sprite.
    jugador1.h = JUGADOR_H;                                             // Alto de la caja o sprite.
    jugador1.vy = 0;                                                    // Velocidad vertical actual.
    jugador1.direccion = 'N';                                               // Actualiza dirección de movimiento del jugador 1.
    jugador1.ultimoComandoRX = 'N';                                     // Último comando recibido; ayuda a detectar eventos y evitar repeticiones.
    jugador1.saltoSolicitado = 0;                                       // Bandera que indica que hay un salto pendiente de ejecutar.
    jugador1.enSuelo = 1;                                               // Marca si el jugador está apoyado sobre una plataforma.
    jugador1.escaleraActual = -1;                                       // Índice de la escalera utilizada actualmente; -1 significa ninguna.
    jugador1.escaleraSalida = -1;                                       // Guarda la última escalera abandonada para no reentrar inmediatamente.
    jugador1.plataformaActual = piso;                                   // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.
    jugador1.color = COLOR_J1;                                              // Guarda el color que debe usar el jugador 1.
    jugador1.ultimoPaquete = HAL_GetTick();                             // Marca cuándo llegó el último comando.
    jugador1.mirandoDerecha = 1;                                        // Orientación visual: 0 izquierda, 1 derecha.
    jugador1.martilloActivo = 0;                                        // Marca si el power-up de martillo está activo.
    jugador1.faseMartillo = 0;                                          // Fase de animación/ataque del martillo: 0 arriba, 1 frente.
    jugador1.martilloOrigen = -1;                                       // Índice del pickup que originó el martillo.
    jugador1.martilloHasta = 0;                                         // Tick en que expira el power-up.
    jugador1.ultimoGolpeMartillo = 0;                                   // Tick usado para temporizar la alternancia de la animación del martillo.
    jugador1.golpePausaHasta = 0;                                       // Fin de la pausa breve posterior a un impacto.
    jugador1.estadoSprite = SPRITE_JUGADOR_IDLE;                        // Estado lógico del sprite que debería representarse.
    jugador1.frameAnimacion = 0;                                        // Frame actual de la animación.
    jugador1.ultimoFrameAnimacion = HAL_GetTick();                      // Inicia el temporizador de animación.
    jugador1.vidas = VIDAS_INICIALES;                                   // Vidas restantes.
    jugador1.muriendo = 0;                                              // Bandera de animación/estado de muerte.
    jugador1.muerteDesde = 0;                                           // Tick en que empezó la muerte.
    jugador1.muerteCuadro = 0;                                          // Cuadro actual de la animación de muerte.
    jugador1.puntos = 0;                                                // Puntuación acumulada.
    jugador1.invulnerableHasta = 0;                                     // Tick hasta el que se ignoran nuevos daños.
    jugador1.letreroVisible = 0;                                        // Marca si debe dibujarse el popup de puntos.
    jugador1.letreroHasta = 0;                                          // Tick hasta el que permanece visible el popup de puntos.
    jugador1.letreroFijo = 0;                                           // Define si el popup queda fijo en el punto de impacto.
    jugador1.letreroValor = 0;                                          // Valor mostrado por el popup: 100, 200 o 300.

    jugador1.y =                                                            // Actualiza la posición vertical del jugador 1.
        ObtenerYPlataforma(                                             // Calcula la altura exacta de la viga inclinada en esta coordenada X.
            &plataformas[piso],                                             // Indica plataforma consultada.
            jugador1.x + jugador1.w / 2                                     // Indica coordenada X donde se calcula la altura.
        ) - jugador1.h;                                                     // Indica coordenada X donde se calcula la altura.


    jugador2.x = 220;                                                       // Actualiza la posición horizontal del jugador 2.
    jugador2.w = JUGADOR_W;                                             // Ancho de la caja o sprite.
    jugador2.h = JUGADOR_H;                                             // Alto de la caja o sprite.
    jugador2.vy = 0;                                                    // Velocidad vertical actual.
    jugador2.direccion = 'N';                                               // Actualiza dirección de movimiento del jugador 2.
    jugador2.ultimoComandoRX = 'N';                                     // Último comando recibido; ayuda a detectar eventos y evitar repeticiones.
    jugador2.saltoSolicitado = 0;                                       // Bandera que indica que hay un salto pendiente de ejecutar.
    jugador2.enSuelo = 1;                                               // Marca si el jugador está apoyado sobre una plataforma.
    jugador2.escaleraActual = -1;                                       // Índice de la escalera utilizada actualmente; -1 significa ninguna.
    jugador2.escaleraSalida = -1;                                       // Guarda la última escalera abandonada para no reentrar inmediatamente.
    jugador2.plataformaActual = piso;                                   // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.
    jugador2.color = COLOR_J2;                                              // Guarda el color que debe usar el jugador 2.
    jugador2.ultimoPaquete = HAL_GetTick();                             // Marca cuándo llegó el último comando.
    jugador2.mirandoDerecha = 0;                                        // Orientación visual: 0 izquierda, 1 derecha.
    jugador2.martilloActivo = 0;                                        // Marca si el power-up de martillo está activo.
    jugador2.faseMartillo = 0;                                          // Fase de animación/ataque del martillo: 0 arriba, 1 frente.
    jugador2.martilloOrigen = -1;                                       // Índice del pickup que originó el martillo.
    jugador2.martilloHasta = 0;                                         // Tick en que expira el power-up.
    jugador2.ultimoGolpeMartillo = 0;                                   // Tick usado para temporizar la alternancia de la animación del martillo.
    jugador2.golpePausaHasta = 0;                                       // Fin de la pausa breve posterior a un impacto.
    jugador2.estadoSprite = SPRITE_JUGADOR_IDLE;                        // Estado lógico del sprite que debería representarse.
    jugador2.frameAnimacion = 0;                                        // Frame actual de la animación.
    jugador2.ultimoFrameAnimacion = HAL_GetTick();                      // Inicia el temporizador de animación.
    jugador2.vidas = VIDAS_INICIALES;                                   // Vidas restantes.
    jugador2.muriendo = 0;                                              // Bandera de animación/estado de muerte.
    jugador2.muerteDesde = 0;                                           // Tick en que empezó la muerte.
    jugador2.muerteCuadro = 0;                                          // Cuadro actual de la animación de muerte.
    jugador2.puntos = 0;                                                // Puntuación acumulada.
    jugador2.invulnerableHasta = 0;                                     // Tick hasta el que se ignoran nuevos daños.
    jugador2.letreroVisible = 0;                                        // Marca si debe dibujarse el popup de puntos.
    jugador2.letreroHasta = 0;                                          // Tick hasta el que permanece visible el popup de puntos.
    jugador2.letreroValor = 0;                                          // Valor mostrado por el popup: 100, 200 o 300.
    jugador2.letreroFijo = 0;                                           // Define si el popup queda fijo en el punto de impacto.

    jugador2.y =                                                            // Actualiza la posición vertical del jugador 2.
        ObtenerYPlataforma(                                             // Calcula la altura exacta de la viga inclinada en esta coordenada X.
            &plataformas[piso],                                             // Indica plataforma consultada.
            jugador2.x + jugador2.w / 2                                     // Indica coordenada X donde se calcula la altura.
        ) - jugador2.h;                                                     // Indica coordenada X donde se calcula la altura.
}                                                                           // Continúa el cálculo de posición vertical del jugador 2.

void ReaparecerJugador(Jugador *j)                                      // Reposiciona al jugador y activa invulnerabilidad temporal.
{
    int16_t antesX = j->x, antesY = j->y;                                   // Calcula y guarda antesX.
    int piso = (int)NUM_PLATAFORMAS - 1;                                    // Calcula y guarda plataforma usada como piso.
    j->x = (j == &jugador1) ? 90 : 220;                                     // Actualiza la posición horizontal del objeto.
    j->y = ObtenerYPlataforma(&plataformas[piso], j->x + j->w / 2) - j->h; // Calcula la altura exacta de la viga inclinada en esta coordenada X.
    j->vy = 0;                                                          // Velocidad vertical actual.
    j->direccion = 'N';                                                     // Actualiza dirección de movimiento.
    j->ultimoComandoRX = 'N';                                           // Último comando recibido; ayuda a detectar eventos y evitar repeticiones.
    j->saltoSolicitado = 0;                                             // Bandera que indica que hay un salto pendiente de ejecutar.
    j->enSuelo = 1;                                                     // Marca si el jugador está apoyado sobre una plataforma.
    j->plataformaActual = piso;                                         // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.
    j->escaleraActual = -1;                                             // Índice de la escalera utilizada actualmente; -1 significa ninguna.
    j->escaleraSalida = -1;                                             // Guarda la última escalera abandonada para no reentrar inmediatamente.
    j->martilloActivo = 0;                                              // Marca si el power-up de martillo está activo.
    j->golpePausaHasta = 0;                                             // Fin de la pausa breve posterior a un impacto.
    j->muriendo = 0;                                                    // Bandera de animación/estado de muerte.
    j->color = (j == &jugador1) ? COLOR_J1 : COLOR_J2;                      // Guarda el color que debe usar el píxel.
    j->invulnerableHasta = HAL_GetTick() + INVULNERABLE_MS;             // Activa la invulnerabilidad temporal.
    j->ultimoPaquete = HAL_GetTick();                                   // Marca cuándo llegó el último comando.
    RenderizarRegionFinal(antesX, antesY, JUGADOR_W, JUGADOR_H);        // Redibuja esta región con todas las capas finales.
    RenderizarRegionFinal(j->x, j->y, JUGADOR_W, JUGADOR_H);            // Redibuja esta región con todas las capas finales.
    if (jugador1.vidas && jugador2.vidas &&                                 // Comprueba una condición del jugador 1 antes de continuar.
        !jugador1.muriendo && !jugador2.muriendo)                           // Comprueba si el jugador está reproduciendo la animación de muerte.
        Audio_Enviar(musicaActual == MUSICA_MARTILLO ? 0x24U : 0x23U);  // Mantiene la música de martillo o la normal.
}

void DibujarJugador(const Jugador *j)                                   // Solicita al renderer final que recomponga la región ocupada por un jugador.
{
    RenderizarRegionFinal(j->x, j->y, j->w, j->h);                      // Redibuja esta región con todas las capas finales.
}

void DibujarJugadores(void)                                             // Dibuja las regiones de ambos jugadores.
{
    DibujarJugador(&jugador1);                                          // Solicita al renderer final que recomponga la región ocupada por un jugador.
    DibujarJugador(&jugador2);                                          // Solicita al renderer final que recomponga la región ocupada por un jugador.
}

void ActualizarJugador(                                                 // Actualiza control, salto, escaleras, movimiento y gravedad.
    Jugador *j,
    uint8_t pasoMovimiento                                                  // Recibe una bandera que indica si en este ciclo corresponde aplicar movimiento.
)
{
    uint32_t ahora = HAL_GetTick();                                     // Tick actual para controlar temporizadores.

    if (!j->vidas || j->muriendo) return;                                   // Comprueba si se cumplen las condiciones necesarias para continuar.

    if (!permitirMovimientoJugadores) {                                     // Comprueba si se cumplen las condiciones necesarias para continuar.
        j->direccion = 'N';                                                 // Actualiza dirección de movimiento.
        j->saltoSolicitado = 0;                                         // Bandera que indica que hay un salto pendiente de ejecutar.
        j->ultimoComandoRX = 'N';                                       // Último comando recibido; ayuda a detectar eventos y evitar repeticiones.
        return;                                                             // Termina la función en este punto.
    }

    if (ahora - j->ultimoPaquete > CONTROL_TIMEOUT_MS) {                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        j->direccion = 'N';                                                 // Actualiza dirección de movimiento.
        j->ultimoComandoRX = 'N';                                       // Último comando recibido; ayuda a detectar eventos y evitar repeticiones.
    }

    if (j->direccion != 'U' && j->direccion != 'D')                         // Comprueba si se cumplen las condiciones necesarias para continuar.
        j->escaleraSalida = -1;                                         // Guarda la última escalera abandonada para no reentrar inmediatamente.

    /* La breve detencion tras romper un barril congela solo a ese jugador. */
    if (j->golpePausaHasta &&                                               // Comprueba si se cumplen las condiciones necesarias para continuar.
        (int32_t)(j->golpePausaHasta - ahora) > 0) return;                  // Evalúa esta comparación como parte de la condición actual.

    /* Salto */
    if (j->saltoSolicitado) {                                               // Comprueba si se cumplen las condiciones necesarias para continuar.
        if (j->enSuelo && j->escaleraActual < 0) {                          // Comprueba si se cumplen las condiciones necesarias para continuar.
            j->vy = VELOCIDAD_SALTO;                                    // Velocidad vertical actual.
            j->enSuelo = 0;                                             // Marca si el jugador está apoyado sobre una plataforma.
            Audio_Enviar(0x01U);                                        // Reproduce el sonido de salto.
        }

        j->saltoSolicitado = 0;                                         // Bandera que indica que hay un salto pendiente de ejecutar.
    }

    /* Entrar a escalera */
    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        !j->martilloActivo &&                                               // Comprueba si el jugador tiene el martillo activo.
        j->escaleraActual < 0 &&                                            // Comprueba si existe una escalera actual válida.
        (j->direccion == 'U' || j->direccion == 'D')                        // Comprueba si el comando actual indica subir.
    ) {
        int escalera = BuscarEscaleraCercana(j);                            // Calcula y guarda índice de escalera.

        if (escalera >= 0) {                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            j->escaleraActual = escalera;                               // Índice de la escalera utilizada actualmente; -1 significa ninguna.
            j->enSuelo = 0;                                             // Marca si el jugador está apoyado sobre una plataforma.
            j->vy = 0;                                                  // Velocidad vertical actual.

            j->x =                                                          // Actualiza la posición horizontal del objeto.
                escaleras[escalera].x +                                     // Accede a la posición correspondiente del arreglo escaleras.
                ESCALERA_W / 2 -                                            // Continúa el cálculo de posición horizontal.
                j->w / 2;                                                   // Evalúa esta comparación como parte de la condición actual.
        }                                                                   // Continúa el cálculo de posición horizontal.
    }                                                                       // Continúa el cálculo de posición horizontal.

    /* En escalera */
    if (j->escaleraActual >= 0) {                                           // Comprueba una condición de la escalera antes de continuar.
        int indiceEscalera = j->escaleraActual;                             // Calcula y guarda índice de la escalera usada.
        const Escalera *e = &escaleras[indiceEscalera];                     // Calcula y guarda e.
        int pisoSuperior = BuscarPisoDeEscalera(e, 1U);                     // Calcula y guarda pisoSuperior.
        int pisoInferior = BuscarPisoDeEscalera(e, 0U);                     // Calcula y guarda pisoInferior.
        int centroX = j->x + j->w / 2;                                  // Calcula el centro para comparar posiciones.
        int ySuperior = pisoSuperior >= 0 ?                                 // Calcula y guarda ySuperior.
            ObtenerYPlataforma(&plataformas[pisoSuperior], centroX) : e->y1; // Calcula la altura exacta de la viga inclinada en esta coordenada X.
        int yInferior = pisoInferior >= 0 ?                                 // Calcula y guarda yInferior.
            ObtenerYPlataforma(&plataformas[pisoInferior], centroX) : e->y2; // Calcula la altura exacta de la viga inclinada en esta coordenada X.
        int pies = j->y + j->h;                                             // Calcula y guarda coordenada Y de los pies del jugador.
        int pisoDestino = -1;                                               // Inicializa plataforma de salida de la escalera indicando que todavía no hay una opción válida.
        int yDestino = 0;                                                   // Reinicia altura de salida de la escalera a cero.

        if (j->direccion == 'L' || j->direccion == 'R') {                   // Comprueba si se cumplen las condiciones necesarias para continuar.
            /* Salir lateralmente solo al alcanzar el extremo de un piso. */
            if (pisoSuperior >= 0 && AbsInt(pies - ySuperior) <= 3) {       // Comprueba si se cumplen las condiciones necesarias para continuar.
                pisoDestino = pisoSuperior;                                 // Calcula y guarda plataforma de salida de la escalera.
                yDestino = ySuperior;                                       // Calcula y guarda altura de salida de la escalera.
            } else if (pisoInferior >= 0 &&                                 // Añade esta condición a la comprobación actual.
                       AbsInt(pies - yInferior) <= 3) {                     // Evalúa esta comparación como parte de la condición actual.
                pisoDestino = pisoInferior;                                 // Calcula y guarda plataforma de salida de la escalera.
                yDestino = yInferior;                                       // Calcula y guarda altura de salida de la escalera.
            }
        } else if (pasoMovimiento) {                                        // Comprueba si en este ciclo corresponde aplicar un paso de movimiento.
            if (j->direccion == 'U') {                                      // Comprueba si se cumplen las condiciones necesarias para continuar.
                j->y -= VELOCIDAD_ESCALERA;                                 // Reduce posición vertical con este valor.
                if (pisoSuperior >= 0 && j->y + j->h <= ySuperior) {        // Comprueba si se cumplen las condiciones necesarias para continuar.
                    pisoDestino = pisoSuperior;                             // Calcula y guarda plataforma de salida de la escalera.
                    yDestino = ySuperior;                                   // Calcula y guarda altura de salida de la escalera.
                }
            } else if (j->direccion == 'D') {                               // Comprueba si el comando actual indica bajar.
                j->y += VELOCIDAD_ESCALERA;                                 // Aumenta posición vertical con este valor.
                if (pisoInferior >= 0 && j->y + j->h >= yInferior) {        // Comprueba si se cumplen las condiciones necesarias para continuar.
                    pisoDestino = pisoInferior;                             // Calcula y guarda plataforma de salida de la escalera.
                    yDestino = yInferior;                                   // Calcula y guarda altura de salida de la escalera.
                }
            }
        }

        if (pisoDestino >= 0) {                                             // Comprueba si se cumplen las condiciones necesarias para continuar.
            j->y = yDestino - j->h;                                         // Actualiza la posición vertical del objeto.
            j->plataformaActual = pisoDestino;                          // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.
            j->enSuelo = 1;                                             // Marca si el jugador está apoyado sobre una plataforma.
            j->vy = 0;                                                  // Velocidad vertical actual.
            j->escaleraActual = -1;                                     // Índice de la escalera utilizada actualmente; -1 significa ninguna.
            j->escaleraSalida = indiceEscalera;                         // Guarda la última escalera abandonada para no reentrar inmediatamente.
        } else {
            return;                                                         // Termina la función en este punto.
        }
    }

    /* Horizontal */
    if (pasoMovimiento) {                                                   // Comprueba si se cumplen las condiciones necesarias para continuar.
        int nuevaX = j->x;                                                  // Calcula y guarda nueva posición X.
        /* La misma velocidad horizontal se aplica a ambos jugadores. */
        int pasoX = j->enSuelo ? VELOCIDAD_X : VELOCIDAD_X_SALTO;           // Calcula y guarda cantidad de píxeles del movimiento horizontal.

        if (j->direccion == 'L') {                                          // Comprueba si se cumplen las condiciones necesarias para continuar.
            nuevaX -= pasoX;                                                // Reduce nueva posición X con este valor.
            j->mirandoDerecha = 0;                                      // Orientación visual: 0 izquierda, 1 derecha.
        }
        else if (j->direccion == 'R') {                                     // Evalúa otra condición.
            nuevaX += pasoX;                                                // Aumenta nueva posición X con este valor.
            j->mirandoDerecha = 1;                                      // Orientación visual: 0 izquierda, 1 derecha.
        }

        j->x = LimitarXDentroPlataforma(j, nuevaX);                         // Actualiza la posición horizontal del objeto.
    }

    /* Seguir plataforma */
    if (j->enSuelo) {                                                       // Comprueba si se cumplen las condiciones necesarias para continuar.
        int soporte = BuscarPlataformaSoporte(j);                           // Calcula y guarda plataforma que sostiene al jugador.

        if (soporte >= 0) {                                                 // Comprueba si se cumplen las condiciones necesarias para continuar.
            j->plataformaActual = soporte;                              // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.

            int py =                                                        // Calcula y guarda altura Y de la plataforma.
                ObtenerYPlataforma(                                     // Calcula la altura exacta de la viga inclinada en esta coordenada X.
                    &plataformas[soporte],                                  // Indica plataforma consultada.
                    j->x + j->w / 2                                         // Evalúa esta comparación como parte de la condición actual.
                );                                                          // Continúa el cálculo de altura Y de la plataforma.

            j->y = py - j->h;                                               // Actualiza la posición vertical del objeto.
            j->vy = 0;                                                  // Velocidad vertical actual.
        } else {
            j->enSuelo = 0;                                             // Marca si el jugador está apoyado sobre una plataforma.
        }
    }

    /* Gravedad */
    if (!j->enSuelo) {                                                      // Comprueba si se cumplen las condiciones necesarias para continuar.
        int oldBottom = j->y + j->h;                                        // Calcula y guarda posición anterior de los pies.

        j->vy += GRAVEDAD;                                                  // Aumenta velocidad vertical con este valor.

        if (j->vy > MAX_CAIDA)                                              // Comprueba si se cumplen las condiciones necesarias para continuar.
            j->vy = MAX_CAIDA;                                          // Velocidad vertical actual.

        int nuevaY = j->y + j->vy;                                          // Calcula y guarda nueva posición Y.

        if (nuevaY < 0) {                                                   // Comprueba si se cumplen las condiciones necesarias para continuar.
            nuevaY = 0;                                                     // Reinicia nueva posición Y a cero.

            if (j->vy < 0)                                                  // Comprueba si se cumplen las condiciones necesarias para continuar.
                j->vy = 0;                                              // Velocidad vertical actual.
        }

        int newBottom = nuevaY + j->h;                                      // Calcula y guarda nueva posición de los pies.

        if (j->vy >= 0) {                                                   // Comprueba si se cumplen las condiciones necesarias para continuar.
            int plataforma =                                                // Calcula y guarda índice de plataforma.
                BuscarPlataformaCaida(                                      // Continúa el cálculo de índice de plataforma.
                    j,                                                      // Continúa el cálculo de índice de plataforma.
                    oldBottom,                                              // Continúa el cálculo de índice de plataforma.
                    newBottom                                               // Continúa el cálculo de índice de plataforma.
                );                                                      // Índice de la plataforma asociada al martillo.

            if (plataforma >= 0) {                                          // Comprueba si se cumplen las condiciones necesarias para continuar.
                j->plataformaActual = plataforma;                       // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.

                int py =                                                    // Calcula y guarda altura Y de la plataforma.
                    ObtenerYPlataforma(                                 // Calcula la altura exacta de la viga inclinada en esta coordenada X.
                        &plataformas[plataforma],                           // Indica plataforma consultada.
                        j->x + j->w / 2                                     // Evalúa esta comparación como parte de la condición actual.
                    );                                                      // Continúa el cálculo de altura Y de la plataforma.

                j->y = py - j->h;                                           // Actualiza la posición vertical del objeto.
                j->vy = 0;                                              // Velocidad vertical actual.
                j->enSuelo = 1;                                         // Marca si el jugador está apoyado sobre una plataforma.
                return;                                                     // Termina la función en este punto.
            }
        }

        j->y = nuevaY;                                                      // Actualiza la posición vertical del objeto.

        /* No morir por caída */
        if (j->y > SCREEN_H - j->h) {                                       // Comprueba si se cumplen las condiciones necesarias para continuar.
            int piso = (int)NUM_PLATAFORMAS - 1;                            // Calcula y guarda plataforma usada como piso.

            if (                                                            // Comprueba si se cumplen las condiciones necesarias para continuar.
                j->plataformaActual >= 0 &&                                 // Comprueba si existe una plataforma actual válida.
                j->plataformaActual < (int)NUM_PLATAFORMAS                  // Evalúa esta comparación como parte de la condición actual.
            ) {
                piso = j->plataformaActual;                                 // Calcula y guarda plataforma usada como piso.
            }

            int py =                                                        // Calcula y guarda altura Y de la plataforma.
                ObtenerYPlataforma(                                     // Calcula la altura exacta de la viga inclinada en esta coordenada X.
                    &plataformas[piso],                                     // Indica plataforma consultada.
                    j->x + j->w / 2                                         // Evalúa esta comparación como parte de la condición actual.
                );                                                          // Continúa el cálculo de altura Y de la plataforma.

            j->y = py - j->h;                                               // Actualiza la posición vertical del objeto.
            j->vy = 0;                                                  // Velocidad vertical actual.
            j->enSuelo = 1;                                             // Marca si el jugador está apoyado sobre una plataforma.
            j->plataformaActual = piso;                                 // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.
        }
    }
}



/* ============================================================
 * MARTILLOS - RECOGIDA, DURACIÓN Y ATAQUE
 *
 * Se colocan los martillos, se detecta cuándo un jugador los recoge y se controla su duración de 8.5 segundos.
 * También se calcula la zona de golpe, se destruyen barriles o flamas y se cambia la música.
 * ============================================================ */

void Audio_ReiniciarMusicaMartillo(void)                                // Envía al procesador de audio el comando para iniciar o reiniciar la música especial del martillo.
{
    Audio_Enviar(0x22U);                                                // Reinicia la música del martillo.
}

void Audio_ReproducirMusicaNormal(void)                                 // Ordena a la STM32 de audio volver a la música normal del nivel.
{
    Audio_Enviar(0x20U);                                                // Vuelve a la música normal.
}

void InicializarMartillos(void)                                         // Configura los dos pickups de martillo, su plataforma, posición suspendida y estado inicial disponible.
{
    /*
     * ========================================================
     * MARTILLO SUPERIOR
     * ========================================================
     */
    martillos[1].x = 72;                                                    // Actualiza la posición horizontal del martillo.
    martillos[1].w = MARTILLO_W;                                            // Guarda el ancho del martillo.
    martillos[1].h = MARTILLO_H;                                            // Guarda el alto del martillo.
    martillos[1].plataforma = 2;                                            // Calcula y guarda índice de plataforma.
    martillos[1].jugador = 0;                                               // Indica que ningún jugador tiene todavía este martillo.
    martillos[1].estado = MARTILLO_DISPONIBLE;                              // Cambia el estado actual del martillo.
    martillos[1].color = COLOR_MARTILLO;                                    // Guarda el color que debe usar el martillo.

    martillos[1].y =                                                        // Actualiza la posición vertical del martillo.
        ObtenerYPlataforma(                                             // Calcula la altura exacta de la viga inclinada en esta coordenada X.
            &plataformas[martillos[1].plataforma],                          // Indica plataforma consultada.
            martillos[1].x + martillos[1].w / 2                             // Indica coordenada X donde se calcula la altura.
        )                                                                   // Continúa el cálculo de posición vertical del martillo.
        - MARTILLO_H                                                        // Continúa el cálculo de posición vertical del martillo.
        - MARTILLO_SEPARACION_PLATAFORMA;                                   // Continúa el cálculo de posición vertical del martillo.


    /*
     * ========================================================
     * MARTILLO INFERIOR
     * ========================================================
     */
    martillos[0].x = 228;                                                   // Actualiza la posición horizontal del martillo.
    martillos[0].w = MARTILLO_W;                                            // Guarda el ancho del martillo.
    martillos[0].h = MARTILLO_H;                                            // Guarda el alto del martillo.
    martillos[0].plataforma = 5;                                            // Calcula y guarda índice de plataforma.
    martillos[0].jugador = 0;                                               // Indica que ningún jugador tiene todavía este martillo.
    martillos[0].estado = MARTILLO_DISPONIBLE;                              // Cambia el estado actual del martillo.
    martillos[0].color = COLOR_MARTILLO;                                    // Guarda el color que debe usar el martillo.

    martillos[0].y =                                                        // Actualiza la posición vertical del martillo.
        ObtenerYPlataforma(                                             // Calcula la altura exacta de la viga inclinada en esta coordenada X.
            &plataformas[martillos[0].plataforma],                          // Indica plataforma consultada.
            martillos[0].x + martillos[0].w / 2                             // Indica coordenada X donde se calcula la altura.
        )                                                                   // Continúa el cálculo de posición vertical del martillo.
        - MARTILLO_H                                                        // Continúa el cálculo de posición vertical del martillo.
        - MARTILLO_SEPARACION_PLATAFORMA;                                   // Continúa el cálculo de posición vertical del martillo.
}

void DibujarMartilloEscenario(const Martillo *m)                        // Dibuja un martillo disponible usando el sprite ASCII convertido a colores RGB565.
{
    if (m->estado != MARTILLO_DISPONIBLE) return;                           // Comprueba si se cumplen las condiciones necesarias para continuar.
    for (int y = 0; y < MARTILLO_H; y++)                                    // Recorre todas las posiciones necesarias de este bloque.
        for (int x = 0; x < MARTILLO_W; x++) {                              // Recorre todas las posiciones necesarias de este bloque.
            uint16_t pixel = ColorPixelMartillo(spriteMartillo[y][x]);      // Calcula y guarda posición lineal del píxel.
            if (pixel) FillRect(m->x + x, m->y + y, 1, 1, pixel);           // Comprueba si se cumplen las condiciones necesarias para continuar.
        }
}

void DibujarMartillos(void)                                             // Recorre los dos martillos y dibuja los que todavía están disponibles.
{
    for (int i = 0; i < MAX_MARTILLOS; i++)                                 // Recorre los dos martillos del escenario.
        DibujarMartilloEscenario(&martillos[i]);                        // Dibuja un martillo disponible usando el sprite ASCII convertido a colores RGB565.
}

AABB CajaMartilloEscenario(const Martillo *m)                           // Construye la caja AABB de un pickup de martillo.
{
    AABB caja = {                                                           // Calcula y guarda caja.
        m->x,                                                               // Evalúa esta comparación como parte de la condición actual.
        m->y,                                                               // Evalúa esta comparación como parte de la condición actual.
        m->w,                                                               // Evalúa esta comparación como parte de la condición actual.
        m->h                                                                // Evalúa esta comparación como parte de la condición actual.
    };                                                                      // Continúa el cálculo de caja.

    return caja;                                                            // Devuelve caja como resultado de la función.
}

AABB ObtenerCajaMartilloAtaque(const Jugador *j)                        // Calcula la zona de ataque del martillo según si está arriba o al frente del jugador.
{
    AABB caja = {                                                           // Calcula y guarda caja.
        j->x,                                                               // Evalúa esta comparación como parte de la condición actual.
        j->y,                                                               // Evalúa esta comparación como parte de la condición actual.
        0,                                                                  // Continúa el cálculo de caja.
        0                                                                   // Continúa el cálculo de caja.
    };                                                                      // Continúa el cálculo de caja.

    if (!j->martilloActivo)                                                 // Comprueba si se cumplen las condiciones necesarias para continuar.
        return caja;                                                        // Devuelve caja como resultado de la función.

    if (j->faseMartillo == 0) {                                             // Comprueba si se cumplen las condiciones necesarias para continuar.
        /* Martillo arriba. */
        caja.x = j->x - 2;                                                  // Actualiza la posición horizontal del objeto.
        caja.y = j->y - MARTILLO_GOLPE_ARRIBA_H;                            // Actualiza la posición vertical del objeto.
        caja.w = j->w + 4;                                              // Ancho de la caja o sprite.
        caja.h = MARTILLO_GOLPE_ARRIBA_H;                               // Alto de la caja o sprite.
    }
    else {                                                              // Ejecuta esta alternativa cuando la condición anterior fue falsa.
        /* Martillo al frente. */
        caja.w = MARTILLO_GOLPE_FRENTE_W;                               // Ancho de la caja o sprite.
        caja.h = MARTILLO_GOLPE_FRENTE_H;                               // Alto de la caja o sprite.
        caja.y = j->y + 1;                                                  // Actualiza la posición vertical del objeto.

        if (j->mirandoDerecha)                                              // Comprueba si se cumplen las condiciones necesarias para continuar.
            caja.x = j->x + j->w;                                           // Actualiza la posición horizontal del objeto.
        else                                                                // Usa esta alternativa cuando la condición anterior no se cumple.
            caja.x = j->x - MARTILLO_GOLPE_FRENTE_W;                        // Actualiza la posición horizontal del objeto.
    }

    return caja;                                                            // Devuelve caja como resultado de la función.
}

uint8_t ObtenerColorMartilloEscenarioPixel(int x, int y, uint16_t *color) // Comprueba si una coordenada pertenece a un martillo disponible y devuelve su color.
{
    for (int i = 0; i < MAX_MARTILLOS; i++) {                               // Recorre los dos martillos del escenario.
        const Martillo *m = &martillos[i];                                  // Calcula y guarda m.
        int localX = x - m->x, localY = y - m->y;                           // Calcula y guarda coordenada X local dentro del sprite.
        if (m->estado != MARTILLO_DISPONIBLE ||                             // Comprueba si se cumplen las condiciones necesarias para continuar.
            localX < 0 || localX >= MARTILLO_W ||                           // Añade esta condición a la comprobación actual.
            localY < 0 || localY >= MARTILLO_H) continue;                   // Evalúa esta comparación como parte de la condición actual.
        uint16_t pixel = ColorPixelMartillo(spriteMartillo[localY][localX]); // Calcula y guarda posición lineal del píxel.
        if (pixel) { *color = pixel; return 1; }                            // Comprueba si se cumplen las condiciones necesarias para continuar.
    }
    return 0;                                                               // Devuelve 0 para indicar que no se encontró o no se cumple la condición buscada.
}

uint8_t ObtenerColorMartilloJugadorPixel(const Jugador *j,              // Devuelve el píxel del martillo que lleva el jugador.
                                         int x, int y, uint16_t *color)
{
    if (!j->martilloActivo) return 0;                                       // Comprueba si se cumplen las condiciones necesarias para continuar.
    AABB caja = ObtenerCajaMartilloAtaque(j);                               // Calcula y guarda caja.
    uint16_t pixel = 0;                                                     // Reinicia posición lineal del píxel a cero.
    if (j->faseMartillo == 0) {                                             // Comprueba si se cumplen las condiciones necesarias para continuar.
        int lx = x - (j->x + j->w / 2 - MARTILLO_W / 2);                    // Calcula y guarda lx.
        int ly = y - (j->y - MARTILLO_H);                                   // Calcula y guarda ly.
        if (lx >= 0 && lx < MARTILLO_W && ly >= 0 && ly < MARTILLO_H)       // Comprueba si se cumplen las condiciones necesarias para continuar.
            pixel = ColorPixelMartillo(spriteMartillo[ly][lx]);             // Calcula y guarda posición lineal del píxel.
    } else if (x >= caja.x && x < caja.x + MARTILLO_H &&                    // Añade esta condición a la comprobación actual.
               y >= caja.y + 1 && y < caja.y + 1 + MARTILLO_W) {            // Evalúa esta comparación como parte de la condición actual.
        int lx = x - caja.x, ly = y - (caja.y + 1);                         // Calcula y guarda lx.
        /* Girar el mismo sprite hacia el lado que mira cada jugador. */
        char simbolo = j->mirandoDerecha                                    // Calcula y guarda simbolo.
            ? spriteMartillo[MARTILLO_H - 1 - lx][ly]                       // Usa esta opción cuando la condición del operador ternario es verdadera.
            : spriteMartillo[lx][MARTILLO_W - 1 - ly];                      // Usa esta opción cuando la condición del operador ternario es falsa.
        pixel = ColorPixelMartillo(simbolo);                                // Calcula y guarda posición lineal del píxel.
    }
    if (!pixel) return 0;                                                   // Comprueba si se cumplen las condiciones necesarias para continuar.
    *color = pixel;                                                         // Guarda el color que debe usar el píxel.
    return 1;                                                               // Devuelve 1 para indicar que sí se encontró o sí se cumple la condición buscada.
}

void ActivarMartilloJugador(                                            // Activa el martillo durante 8.5 s y cambia la música.
    Jugador *j,
    uint8_t numeroJugador,                                                  // Recibe el valor numeroJugador.
    int indiceMartillo                                                      // Recibe el valor indiceMartillo.
)
{
    uint32_t ahora =                                                        // Calcula y guarda tick actual.
        HAL_GetTick();                                                      // Continúa el cálculo de tick actual.

    j->martilloActivo = 1;                                              // Marca si el power-up de martillo está activo.
    j->faseMartillo = 0;                                                // Fase de animación/ataque del martillo: 0 arriba, 1 frente.
    j->martilloOrigen =                                                     // Actualiza martillo recogido.
        (int8_t)indiceMartillo;                                         // Índice del pickup que originó el martillo.

    j->martilloHasta =                                                      // Actualiza fin del martillo.
        ahora +                                                             // Continúa el cálculo de fin del martillo.
        MARTILLO_DURACION_MS;                                           // Tick en que expira el power-up.

    j->ultimoGolpeMartillo =                                                // Actualiza último cambio de fase del martillo.
        ahora;                                                          // Tick usado para temporizar la alternancia de la animación del martillo.

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        indiceMartillo >= 0 &&                                              // Añade esta condición a la comprobación actual.
        indiceMartillo < MAX_MARTILLOS                                      // Evalúa esta comparación como parte de la condición actual.
    ) {                                                                     // Continúa el cálculo de último cambio de fase del martillo.
        martillos[indiceMartillo].estado =                                  // Cambia el estado actual del martillo.
            MARTILLO_TOMADO;                                                // Continúa el cálculo de estado actual del martillo.

        martillos[indiceMartillo].jugador =                                 // Actualiza jugador dueño del martillo.
            numeroJugador;                                                  // Continúa el cálculo de jugador dueño del martillo.
    }

    /*
     * Cada nuevo pickup reinicia Hammer BGM.
     */
    musicaActual =                                                          // Actualiza musicaActual.
        MUSICA_MARTILLO;                                                    // Continúa el cálculo de musicaActual.

    Audio_ReiniciarMusicaMartillo();                                    // Envía al procesador de audio el comando para iniciar o reiniciar la música especial del martillo.
}

void FinalizarMartilloJugador(                                          // Termina el power-up, marca el pickup como agotado y actualiza la música.
    Jugador *j
)
{
    if (!j->martilloActivo)                                                 // Comprueba si se cumplen las condiciones necesarias para continuar.
        return;                                                             // Termina la función en este punto.

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        j->martilloOrigen >= 0 &&                                           // Añade esta condición a la comprobación actual.
        j->martilloOrigen < MAX_MARTILLOS                                   // Evalúa esta comparación como parte de la condición actual.
    ) {
        Martillo *m =                                                       // Calcula y guarda m.
            &martillos[j->martilloOrigen];                                  // Evalúa esta comparación como parte de la condición actual.

        m->estado =                                                         // Cambia el estado actual del martillo.
            MARTILLO_AGOTADO;                                               // Continúa el cálculo de estado actual del martillo.

        m->jugador = 0;                                                 // Jugador propietario del martillo: 0 ninguno, 1 J1, 2 J2.
    }

    j->martilloActivo = 0;                                              // Marca si el power-up de martillo está activo.
    j->faseMartillo = 0;                                                // Fase de animación/ataque del martillo: 0 arriba, 1 frente.
    j->martilloOrigen = -1;                                             // Índice del pickup que originó el martillo.
    j->martilloHasta = 0;                                               // Tick en que expira el power-up.
    j->ultimoGolpeMartillo = 0;                                         // Tick usado para temporizar la alternancia de la animación del martillo.

    ActualizarMusicaMartillo();                                         // Mantiene la música de martillo o vuelve a la normal.
}

void ActualizarMartilloJugador(                                         // Comprueba expiración del martillo y alterna periódicamente entre golpe arriba y golpe al frente.
    Jugador *j
)
{
    if (!j->martilloActivo)                                                 // Comprueba si se cumplen las condiciones necesarias para continuar.
        return;                                                             // Termina la función en este punto.

    uint32_t ahora =                                                        // Calcula y guarda tick actual.
        HAL_GetTick();                                                      // Continúa el cálculo de tick actual.

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        (int32_t)(
            ahora -
            j->martilloHasta                                                // Evalúa esta comparación como parte de la condición actual.
        ) >= 0                                                              // Evalúa esta comparación como parte de la condición actual.
    ) {
        FinalizarMartilloJugador(j);                                    // Termina el power-up, marca el pickup como agotado y actualiza la música.
        return;                                                             // Termina la función en este punto.
    }

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        ahora -
        j->ultimoGolpeMartillo >=                                           // Evalúa esta comparación como parte de la condición actual.
        MARTILLO_GOLPE_MS
    ) {
        j->ultimoGolpeMartillo =                                            // Actualiza último cambio de fase del martillo.
            ahora;                                                      // Tick usado para temporizar la alternancia de la animación del martillo.

        j->faseMartillo =                                                   // Actualiza fase del martillo.
            (j->faseMartillo == 0)                                          // Evalúa esta comparación como parte de la condición actual.
            ? 1                                                             // Usa esta opción cuando la condición del operador ternario es verdadera.
            : 0;                                                        // Fase de animación/ataque del martillo: 0 arriba, 1 frente.
    }                                                                       // Continúa el cálculo de fase del martillo.
}                                                                           // Continúa el cálculo de fase del martillo.

void ActualizarEstadoSpriteJugador(                                     // Selecciona el estado lógico del sprite que corresponde al movimiento actual y al estado del martillo.
    Jugador *j
)
{
    if (j->martilloActivo) {                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
        if (!j->enSuelo) {                                                  // Comprueba si se cumplen las condiciones necesarias para continuar.
            j->estadoSprite =                                               // Actualiza estado visual.
                (j->faseMartillo == 0)                                      // Evalúa esta comparación como parte de la condición actual.
                ? SPRITE_MARTILLO_SALTANDO_ARRIBA                           // Usa esta opción cuando la condición del operador ternario es verdadera.
                : SPRITE_MARTILLO_SALTANDO_FRENTE;                      // Estado lógico del sprite que debería representarse.
        }                                                                   // Continúa el cálculo de estado visual.
        else if (                                                           // Evalúa otra condición.
            j->direccion == 'L' ||                                          // Comprueba si el comando actual indica movimiento hacia la izquierda.
            j->direccion == 'R'                                             // Comprueba si el comando actual indica movimiento hacia la derecha.
        ) {                                                                 // Continúa el cálculo de estado visual.
            j->estadoSprite =                                               // Actualiza estado visual.
                (j->faseMartillo == 0)                                      // Evalúa esta comparación como parte de la condición actual.
                ? SPRITE_MARTILLO_CAMINANDO_ARRIBA                          // Usa esta opción cuando la condición del operador ternario es verdadera.
                : SPRITE_MARTILLO_CAMINANDO_FRENTE;                     // Estado lógico del sprite que debería representarse.
        }                                                                   // Continúa el cálculo de estado visual.
        else {                                                          // Ejecuta esta alternativa cuando la condición anterior fue falsa.
            j->estadoSprite =                                               // Actualiza estado visual.
                (j->faseMartillo == 0)                                      // Evalúa esta comparación como parte de la condición actual.
                ? SPRITE_MARTILLO_IDLE_ARRIBA                               // Usa esta opción cuando la condición del operador ternario es verdadera.
                : SPRITE_MARTILLO_IDLE_FRENTE;                          // Estado lógico del sprite que debería representarse.
        }                                                                   // Continúa el cálculo de estado visual.

        return;                                                             // Termina la función en este punto.
    }                                                                       // Continúa el cálculo de estado visual.

    if (j->escaleraActual >= 0) {                                           // Comprueba si se cumplen las condiciones necesarias para continuar.
        j->estadoSprite =                                                   // Actualiza estado visual.
            SPRITE_JUGADOR_ESCALERA;                                    // Estado lógico del sprite que debería representarse.
    }                                                                       // Continúa el cálculo de estado visual.
    else if (!j->enSuelo) {                                                 // Evalúa otra condición.
        j->estadoSprite =                                                   // Actualiza estado visual.
            SPRITE_JUGADOR_SALTANDO;                                    // Estado lógico del sprite que debería representarse.
    }                                                                       // Continúa el cálculo de estado visual.
    else if (                                                               // Evalúa otra condición.
        j->direccion == 'L' ||                                              // Comprueba si el comando actual indica movimiento hacia la izquierda.
        j->direccion == 'R'                                                 // Comprueba si el comando actual indica movimiento hacia la derecha.
    ) {                                                                     // Continúa el cálculo de estado visual.
        j->estadoSprite =                                                   // Actualiza estado visual.
            SPRITE_JUGADOR_CAMINANDO;                                   // Estado lógico del sprite que debería representarse.
    }                                                                       // Continúa el cálculo de estado visual.
    else {                                                              // Ejecuta esta alternativa cuando la condición anterior fue falsa.
        j->estadoSprite =                                                   // Actualiza estado visual.
            SPRITE_JUGADOR_IDLE;                                        // Estado lógico del sprite que debería representarse.
    }                                                                       // Continúa el cálculo de estado visual.
}                                                                           // Continúa el cálculo de estado visual.

void ComprobarRecogidaMartillos(void)                                   // Detecta colisión de cada jugador con los pickups disponibles y activa el martillo cuando corresponde.
{
    AABB cajaJ1 = {                                                         // Calcula y guarda cajaJ1.
        jugador1.x,                                                         // Continúa el cálculo de cajaJ1.
        jugador1.y,                                                         // Continúa el cálculo de cajaJ1.
        jugador1.w,                                                         // Continúa el cálculo de cajaJ1.
        jugador1.h                                                          // Continúa el cálculo de cajaJ1.
    };                                                                      // Continúa el cálculo de cajaJ1.

    AABB cajaJ2 = {                                                         // Calcula y guarda cajaJ2.
        jugador2.x,                                                         // Continúa el cálculo de cajaJ2.
        jugador2.y,                                                         // Continúa el cálculo de cajaJ2.
        jugador2.w,                                                         // Continúa el cálculo de cajaJ2.
        jugador2.h                                                          // Continúa el cálculo de cajaJ2.
    };                                                                      // Continúa el cálculo de cajaJ2.

    for (int i = 0; i < MAX_MARTILLOS; i++) {                               // Recorre los dos martillos del escenario.
        Martillo *m =                                                       // Calcula y guarda m.
            &martillos[i];                                                  // Accede a la posición correspondiente del arreglo martillos.

        if (                                                                // Comprueba una condición del martillo antes de continuar.
            m->estado !=                                                    // Evalúa esta comparación como parte de la condición actual.
            MARTILLO_DISPONIBLE                                         // Pickup visible y disponible para ser recogido.
        )
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.

        AABB cajaM =                                                        // Calcula y guarda cajaM.
            CajaMartilloEscenario(m);                                   // Construye la caja AABB de un pickup de martillo.

        if (                                                                // Comprueba una condición del jugador 1 antes de continuar.
            jugador1.vidas && !jugador1.muriendo && !jugador1.martilloActivo && // Comprueba si el jugador está reproduciendo la animación de muerte.
            ColisionAABB(                                               // Comprueba si las dos cajas rectangulares se superponen.
                cajaJ1,                                                     // Indica primera caja de colisión.
                cajaM                                                       // Indica segunda caja de colisión.
            )                                                               // Continúa el cálculo de cajaM.
        ) {                                                                 // Continúa el cálculo de cajaM.
            ActivarMartilloJugador(                                         // Continúa el cálculo de cajaM.
                &jugador1,                                                  // Continúa el cálculo de cajaM.
                1,                                                          // Continúa el cálculo de cajaM.
                i                                                           // Continúa el cálculo de cajaM.
            );                                                              // Continúa el cálculo de cajaM.
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.
        }

        if (                                                                // Comprueba una condición del jugador 2 antes de continuar.
            jugador2.vidas && !jugador2.muriendo && !jugador2.martilloActivo && // Comprueba si el jugador está reproduciendo la animación de muerte.
            ColisionAABB(                                               // Comprueba si las dos cajas rectangulares se superponen.
                cajaJ2,                                                     // Indica primera caja de colisión.
                cajaM                                                       // Indica segunda caja de colisión.
            )
        ) {
            ActivarMartilloJugador(                                         // Ejecuta ActivarMartilloJugador().
                &jugador2,                                                  // Entrega este parámetro a ActivarMartilloJugador().
                2,                                                          // Entrega este parámetro a ActivarMartilloJugador().
                i                                                           // Entrega este parámetro a ActivarMartilloJugador().
            );                                                              // Finaliza la llamada a ActivarMartilloJugador().
        }
    }
}

void ComprobarGolpesMartillo(void)                                      // Detecta golpes del martillo contra barriles y flamas.
{
    Jugador *jugadores[2] = {                                               // Calcula y guarda jugadores.
        &jugador1,                                                          // Continúa el cálculo de jugadores.
        &jugador2                                                           // Continúa el cálculo de jugadores.
    };                                                                      // Continúa el cálculo de jugadores.

    for (int j = 0; j < 2; j++) {                                           // Recorre todas las posiciones necesarias de este bloque.
        Jugador *jugador =                                                  // Calcula y guarda jugador dueño.
            jugadores[j];                                                   // Accede a la posición correspondiente del arreglo jugadores.

        if (jugador->golpePausaHasta) {                                     // Comprueba si se cumplen las condiciones necesarias para continuar.
            if ((int32_t)(HAL_GetTick() - jugador->golpePausaHasta) < 0)    // Comprueba si se cumplen las condiciones necesarias para continuar.
                continue;                                                   // Omite este caso y continúa con la siguiente iteración.
            jugador->golpePausaHasta = 0;                               // Fin de la pausa breve posterior a un impacto.
            RenderizarRegionFinal(jugador->golpeImpactoX,               // Redibuja esta región con todas las capas finales.
                                  jugador->golpeImpactoY, 12, 12);          // Evalúa esta comparación como parte de la condición actual.
        }

        if (!jugador->martilloActivo)                                       // Comprueba si se cumplen las condiciones necesarias para continuar.
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.

        AABB cajaMartillo =                                                 // Calcula y guarda cajaMartillo.
            ObtenerCajaMartilloAtaque(                                      // Continúa el cálculo de cajaMartillo.
                jugador                                                     // Continúa el cálculo de cajaMartillo.
            );                                                              // Continúa el cálculo de cajaMartillo.

        /* Barriles */
        for (int i = 0; i < MAX_BARRILES; i++) {                            // Recorre todos los slots de barriles.
            if (!barriles[i].activo)                                        // Comprueba si se cumplen las condiciones necesarias para continuar.
                continue;                                                   // Omite este caso y continúa con la siguiente iteración.

            AABB cajaBarril = {                                             // Calcula y guarda cajaBarril.
                barriles[i].x,                                              // Accede a la posición correspondiente del arreglo barriles.
                barriles[i].y,                                              // Accede a la posición correspondiente del arreglo barriles.
                barriles[i].w,                                              // Accede a la posición correspondiente del arreglo barriles.
                barriles[i].h                                               // Accede a la posición correspondiente del arreglo barriles.
            };                                                              // Continúa el cálculo de cajaBarril.

            if (                                                            // Comprueba si se cumplen las condiciones necesarias para continuar.
                ColisionAABB(                                           // Comprueba si las dos cajas rectangulares se superponen.
                    cajaMartillo,                                           // Indica primera caja de colisión.
                    cajaBarril                                              // Indica segunda caja de colisión.
                )
            ) {
                int avisoX = barriles[i].x + barriles[i].w / 2 - PUNTOS_W / 2; // Calcula y guarda avisoX.
                int avisoY = barriles[i].y - PUNTOS_H - 2;                  // Calcula y guarda avisoY.
                DesactivarBarril(                                           // Desactiva el barril y libera su slot.
                    &barriles[i]                                            // Indica barril que se elimina.
                );                                                          // Finaliza la llamada a DesactivarBarril().
                jugador->puntos += 200U;                                    // Aumenta puntuación con este valor.
                jugador->letreroVisible = 1U;                           // Marca si debe dibujarse el popup de puntos.
                jugador->letreroValor = 200U;                           // Valor mostrado por el popup: 100, 200 o 300.
                jugador->letreroFijo = 1U;                              // Define si el popup queda fijo en el punto de impacto.
                jugador->letreroX = avisoX;                             // Posición X fija del popup.
                jugador->letreroY = avisoY;                             // Posición Y fija del popup.
                jugador->letreroHasta = HAL_GetTick() + PUNTOS_POPUP_MS;// Define cuándo desaparece el popup de puntos.
                hudSucio = 1;                                               // Activa hudSucio.
                Audio_Enviar(0x02U);                                    // Reproduce el sonido de enemigo destruido.
                jugador->golpeImpactoX = barriles[i].x + BARRIL_W / 2 - 6; // X del efecto visual del impacto.
                jugador->golpeImpactoY = barriles[i].y + BARRIL_H / 2 - 6; // Y del efecto visual del impacto.
                jugador->golpePausaHasta = HAL_GetTick() + GOLPE_PAUSA_MS;// Define cuándo termina la pausa del impacto.
                RenderizarRegionFinal(jugador->golpeImpactoX,           // Redibuja esta región con todas las capas finales.
                                      jugador->golpeImpactoY, 12, 12);      // Evalúa esta comparación como parte de la condición actual.
                break;                                                      // Sale de este caso o bucle y continúa después del bloque.
            }
        }

        /* Flamas */
        for (int i = 0; i < MAX_FIREBALLS; i++) {                           // Recorre todos los slots de flamas.
            if (!fireballs[i].activo)                                       // Comprueba si se cumplen las condiciones necesarias para continuar.
                continue;                                                   // Omite este caso y continúa con la siguiente iteración.

            if (                                                            // Comprueba una condición de la flama antes de continuar.
                fireballs[i].estado ==                                      // Evalúa esta comparación como parte de la condición actual.
                FLAMA_NACIENDO                                          // Flama creada pero todavía oculta dentro del barril de aceite.
            )
                continue;                                                   // Omite este caso y continúa con la siguiente iteración.

            AABB cajaFlama = {                                              // Calcula y guarda cajaFlama.
                fireballs[i].x,                                             // Accede a la posición correspondiente del arreglo fireballs.
                fireballs[i].y,                                             // Accede a la posición correspondiente del arreglo fireballs.
                fireballs[i].w,                                             // Accede a la posición correspondiente del arreglo fireballs.
                fireballs[i].h                                              // Accede a la posición correspondiente del arreglo fireballs.
            };                                                              // Continúa el cálculo de cajaFlama.

            if (                                                            // Comprueba si se cumplen las condiciones necesarias para continuar.
                ColisionAABB(                                           // Comprueba si las dos cajas rectangulares se superponen.
                    cajaMartillo,                                           // Indica primera caja de colisión.
                    cajaFlama                                               // Indica segunda caja de colisión.
                )
            ) {
                int avisoX = fireballs[i].x + fireballs[i].w / 2 - PUNTOS_W / 2; // Calcula y guarda avisoX.
                int avisoY = fireballs[i].y - PUNTOS_H - 2;                 // Calcula y guarda avisoY.
                DesactivarFireball(                                         // Libera una flama activa y actualiza el contador global.
                    &fireballs[i]                                           // Indica flama que se elimina.
                );                                                          // Finaliza la llamada a DesactivarFireball().
                jugador->puntos += 300U;                                    // Aumenta puntuación con este valor.
                jugador->letreroVisible = 1U;                           // Marca si debe dibujarse el popup de puntos.
                jugador->letreroValor = 300U;                           // Valor mostrado por el popup: 100, 200 o 300.
                jugador->letreroFijo = 1U;                              // Define si el popup queda fijo en el punto de impacto.
                jugador->letreroX = avisoX;                             // Posición X fija del popup.
                jugador->letreroY = avisoY;                             // Posición Y fija del popup.
                jugador->letreroHasta = HAL_GetTick() + PUNTOS_POPUP_MS;// Define cuándo desaparece el popup de puntos.
                hudSucio = 1;                                               // Activa hudSucio.
                Audio_Enviar(0x02U);                                    // Reproduce el sonido de puntos/enemigo destruido.
                break;                                                      // Sale de este caso o bucle y continúa después del bloque.
            }
        }
    }
}

void ActualizarMusicaMartillo(void)                                     // Mantiene la música de martillo o vuelve a la normal.
{
    uint8_t hayMartillo =                                                   // Calcula y guarda hayMartillo.
        jugador1.martilloActivo ||                                          // Comprueba si el jugador tiene el martillo activo.
        jugador2.martilloActivo;                                            // Comprueba si el jugador tiene el martillo activo.

    if (hayMartillo) {                                                      // Comprueba si se cumplen las condiciones necesarias para continuar.
        /*
         * No reinicia aquí: reinicia solamente cuando se recoge
         * un nuevo martillo.
         */
        musicaActual =                                                      // Actualiza musicaActual.
            MUSICA_MARTILLO;                                                // Continúa el cálculo de musicaActual.

        return;                                                             // Termina la función en este punto.
    }

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        musicaActual ==                                                     // Evalúa esta comparación como parte de la condición actual.
        MUSICA_MARTILLO                                                 // Debe sonar la música especial mientras existe un martillo activo.
    ) {
        musicaActual =                                                      // Actualiza musicaActual.
            MUSICA_NORMAL;                                                  // Continúa el cálculo de musicaActual.

        Audio_ReproducirMusicaNormal();                                 // Ordena a la STM32 de audio volver a la música normal del nivel.
    }
}


/* ============================================================
 * DONKEY KONG - ANIMACIÓN
 *
 * Se dibujan los frames de DK y se prepara su estado visual durante los lanzamientos.
 * La lógica que decide cuándo liberar cada barril aparece más adelante.
 * ============================================================ */

void DibujarFrameDK(uint8_t frame)                                      // Extrae un frame de la spritesheet de Donkey Kong y lo dibuja línea por línea usando transparencia.
{
    const unsigned int anchoSpritesheet =                                   // Calcula y guarda ancho total de la spritesheet.
        DK_W * DK_FRAMES;                                                   // Multiplica el ancho de un frame por la cantidad total de frames de DK.

    uint16_t fila[DK_W];                                                    // Reserva fila temporal de píxeles para usarla en este bloque.

    if (frame >= DK_FRAMES)                                                 // Comprueba una condición del DK antes de continuar.
        frame = DK_FRAME_IDLE;                                              // Calcula y guarda frame de animación.

    for (unsigned int y = 0; y < DK_H; y++) {                               // Recorre todas las filas del sprite de DK.
        for (unsigned int x = 0; x < DK_W; x++) {                           // Recorre todas las columnas del sprite de DK.
            unsigned int origen =                                           // Calcula y guarda índice del píxel dentro de la spritesheet.
                y * anchoSpritesheet +                                      // Convierte esa fila en un desplazamiento dentro del arreglo de la spritesheet.
                frame * DK_W +                                              // Multiplica el número de frame por el ancho de cada frame.
                x;                                                          // Continúa el cálculo de índice del píxel dentro de la spritesheet.

            fila[x] = dkongAnim[origen];                                    // Calcula y guarda fila temporal de píxeles.
        }

        LCD_BitmapTransparent(                                              // Ejecuta LCD_BitmapTransparent().
            DK_X,                                                           // Indica X inicial del bitmap.
            DK_Y + y,                                                       // Indica Y inicial de la fila.
            DK_W,                                                           // Indica ancho del bitmap.
            1,                                                              // Indica alto enviado en esta llamada.
            fila,                                                           // Indica fila de píxeles RGB565.
            DK_TRANSPARENTE                                                 // Usa el color reservado como transparencia de la spritesheet de DK.
        );                                                                  // Finaliza la llamada a LCD_BitmapTransparent().
    }
}


/* ============================================================
 * BARRILES - CREACIÓN Y FÍSICA
 *
 * Se crean y actualizan los barriles marrones y azules.
 * Cada barril puede rodar, caer, rebotar, bajar escaleras, ir directo o dirigirse al aceite.
 * ============================================================ */

int DireccionBarrilPorPlataforma(int plataforma)                        // Devuelve la dirección horizontal predeterminada de un barril según la plataforma.
{
    switch (plataforma) {                                               // Selecciona una acción según el valor actual de la variable evaluada.
        case 0: return 1;                                                   // Ejecuta la lógica del estado 0.
        case 1: return 1;                                                   // Ejecuta la lógica del estado 1.
        case 2: return -1;                                                  // Ejecuta la lógica del estado 2.
        case 3: return 1;                                                   // Ejecuta la lógica del estado 3.
        case 4: return -1;                                                  // Ejecuta la lógica del estado 4.
        case 5: return 1;                                                   // Ejecuta la lógica del estado 5.
        case 6: return -1;                                                  // Ejecuta la lógica del estado 6.
        default: return 1;                                                  // Usa este caso si ningún estado anterior coincide.
    }
}

void InicializarBarriles(void)                                          // Limpia todos los slots de barriles al comenzar una partida.
{
    for (int i = 0; i < MAX_BARRILES; i++) {                                // Recorre todos los slots de barriles.
        barriles[i].activo = 0;                                             // Marca el barril como inactivo y deja libre su slot.
        barriles[i].estado = BARRIL_INACTIVO;                               // Cambia el estado actual del barril.
        barriles[i].plataformaActual = -1;                                  // Indica que el barril no tiene plataforma actual.
        barriles[i].escaleraActual = -1;                                    // Indica que el barril no tiene escalera actual.
        barriles[i].ultimaEscaleraEvaluada = -1;                            // Indica que el barril no tiene última escalera evaluada.
    }
}

uint8_t ContarBarrilesActivos(void)                                     // Cuenta cuántos barriles están actualmente activos para respetar el límite simultáneo.
{
    uint8_t cantidad = 0;                                                   // Reinicia cantidad encontrada a cero.

    for (int i = 0; i < MAX_BARRILES; i++) {                                // Recorre todos los slots de barriles.
        if (barriles[i].activo)                                             // Comprueba si se cumplen las condiciones necesarias para continuar.
            cantidad++;                                                     // Incrementa cantidad encontrada en una unidad.
    }

    return cantidad;                                                        // Devuelve cantidad encontrada como resultado de la función.
}

void DesactivarBarril(Barril *b)                                        // Desactiva el barril y libera su slot.
{
    b->activo = 0;                                                          // Marca el barril como inactivo y deja libre su slot.
    b->estado = BARRIL_INACTIVO;                                            // Cambia el estado actual del barril.
    b->plataformaActual = -1;                                           // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.
    b->escaleraActual = -1;                                             // Índice de la escalera utilizada actualmente; -1 significa ninguna.
}

Jugador *JugadorObjetivoBarril(const Barril *b)                              // Compara las distancias del barril a J1 y J2 y devuelve el jugador más cercano.
{
    int bx = b->x + b->w / 2;                                               // Calcula y guarda coordenada X del centro del barril.
    int by = b->y + b->h / 2;                                               // Calcula y guarda coordenada Y del centro del barril.

    int distancia1 =                                                        // Calcula y guarda distancia entre el barril y J1.
        AbsInt(jugador1.x + jugador1.w / 2 - bx) +                          // Calcula la distancia horizontal entre el barril y J1.
        AbsInt(jugador1.y + jugador1.h / 2 - by);                           // Añade la distancia vertical entre el barril y J1.

    int distancia2 =                                                        // Calcula y guarda distancia entre el barril y J2.
        AbsInt(jugador2.x + jugador2.w / 2 - bx) +                          // Calcula la distancia horizontal entre el barril y J2.
        AbsInt(jugador2.y + jugador2.h / 2 - by);                           // Añade la distancia vertical entre el barril y J2.

    return (distancia1 <= distancia2)                                       // Devuelve una de las opciones según la condición que se evalúa a continuación.
           ? &jugador1                                                      // Usa esta opción cuando la condición del operador ternario es verdadera.
           : &jugador2;                                                     // Usa esta opción cuando la condición del operador ternario es falsa.
}

void CrearBarril(                                                       // Inicializa un barril libre y define su estado inicial.
    TipoBarril tipo,                                                        // Recibe si el nuevo barril será marrón o azul.
    uint8_t directo,                                                        // Indica si el barril debe usar una trayectoria directa/especial.
    uint8_t contactosIgnorar                                            // Número de contactos con plataformas que un barril directo debe ignorar.
)
{
    for (int i = 0; i < MAX_BARRILES; i++) {                                // Recorre todos los slots de barriles.
        if (!barriles[i].activo) {                                          // Comprueba una condición del barril antes de continuar.
            Barril *b = &barriles[i];                                       // Calcula y guarda b.

            b->activo = 1;                                                  // Marca el barril como activo.
            b->tipo = tipo;                                             // Tipo lógico del barril: marrón o azul.
            b->x = BARRIL_SALIDA_X;                                         // Actualiza la posición horizontal del barril.
            b->y = BARRIL_SALIDA_Y;                                         // Actualiza la posición vertical del barril.
            b->w = BARRIL_W;                                            // Ancho de la caja o sprite.
            b->h = BARRIL_H;                                            // Alto de la caja o sprite.
            b->vy = 0;                                                  // Velocidad vertical actual.
            b->direccion = 1;                                               // Fija la dirección del barril hacia la derecha.
            b->plataformaActual = -1;                                   // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.
            b->escaleraActual = -1;                                     // Índice de la escalera utilizada actualmente; -1 significa ninguna.
            b->ultimaEscaleraEvaluada = -1;                             // Última escalera sobre la que el barril ya tomó una decisión, para no repetirla en cada pixel.
            b->contactosIgnorar = contactosIgnorar;                     // Número de contactos con plataformas que un barril directo debe ignorar.
            b->vueloAceitePaso = 0;                                     // Paso actual de la trayectoria especial del primer barril azul hacia el aceite.
            b->saltado[0] = 0;                                              // Reinicia registro de salto puntuado del barril a cero.
            b->saltado[1] = 0;                                              // Reinicia registro de salto puntuado del barril a cero.

            b->color =                                                      // Guarda el color que debe usar el barril.
                (tipo == BARRIL_AZUL)                                       // Comprueba si el barril es azul.
                ? COLOR_BARRIL_AZUL                                         // Usa esta opción cuando la condición del operador ternario es verdadera.
                : COLOR_BARRIL_MARRON;                                      // Usa esta opción cuando la condición del operador ternario es falsa.

            if (tipo == BARRIL_AZUL && directo && contactosIgnorar >= 5) {  // Comprueba una condición del barril antes de continuar.
                /* Un tiro guiado solo para el primer barril de cada ronda:
                 * desde la mano de DK hasta el aceite, a la vista del jugador. */
                b->estado = BARRIL_A_ACEITE;                                // Cambia el estado actual del barril.
                b->direccion = -1;                                          // Fija la dirección del barril hacia la izquierda.
            } else if (directo) {
                b->estado = BARRIL_DIRECTO;                                 // Cambia el estado actual del barril.

                if (                                                        // Comprueba si se cumplen las condiciones necesarias para continuar.
                    tipo == BARRIL_AZUL &&                              // Tipo lógico del barril: marrón o azul.
                    contactosIgnorar >= 5                                   // Evalúa esta comparación como parte de la condición actual.
                ) {
                    b->direccion = 0;                                       // Detiene temporalmente el movimiento horizontal del barril.
                } else {
                    Jugador *objetivo =                                     // Calcula y guarda jugador objetivo.
                        JugadorObjetivoBarril(b);                       // Calcula cuál jugador está más cerca de un barril usando distancia Manhattan y devuelve su puntero.

                    b->direccion =                                          // Actualiza dirección de movimiento del barril.
                        (objetivo->x < b->x)                                // Evalúa esta comparación como parte de la condición actual.
                        ? -1                                                // Usa esta opción cuando la condición del operador ternario es verdadera.
                        : 1;                                                // Usa esta opción cuando la condición del operador ternario es falsa.
                }
            } else {
                b->estado = BARRIL_CAYENDO;                                 // Cambia el estado actual del barril.
            }

            return;                                                         // Termina la función en este punto.
        }
    }
}

void DibujarBarril(const Barril *b)                                     // Recompone la región del barril activo usando el renderer final.
{
    if (!b->activo) return;                                                 // Comprueba una condición del barril antes de continuar.
    RenderizarRegionFinal(b->x, b->y, b->w, b->h);                      // Redibuja esta región con todas las capas finales.
}

void DibujarBarriles(void)                                              // Recorre el arreglo y dibuja todos los barriles activos.
{
    for (int i = 0; i < MAX_BARRILES; i++) {                                // Recorre todos los slots de barriles.
        if (barriles[i].activo)                                             // Comprueba una condición del barril antes de continuar.
            DibujarBarril(&barriles[i]);                                // Recompone la región del barril activo usando el renderer final.
    }
}

int BuscarPlataformaBarrilCaida(                                        // Detecta qué plataforma cruza un barril mientras cae.
    const Barril *b,
    int oldBottom,                                                          // Recibe el valor oldBottom.
    int newBottom                                                           // Recibe el valor newBottom.
)
{
    int centroX = b->x + b->w / 2;                                      // Calcula el centro para comparar posiciones.
    int mejor = -1;                                                         // Inicializa mejor opción encontrada indicando que todavía no hay una opción válida.
    int mejorY = 10000;                                                     // Inicializa una altura grande para poder conservar la plataforma válida más cercana.

    for (uint32_t i = 0; i < NUM_PLATAFORMAS; i++) {                        // Recorre todas las plataformas del escenario.
        const Plataforma *p = &plataformas[i];                              // Calcula y guarda p.

        if (centroX < p->x1 || centroX > p->x2)                             // Comprueba si se cumplen las condiciones necesarias para continuar.
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.

        int py = ObtenerYPlataforma(p, centroX);                        // Calcula la altura exacta de la viga inclinada en esta coordenada X.

        if (                                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            oldBottom <= py &&                                              // Añade esta condición a la comprobación actual.
            newBottom >= py                                                 // Evalúa esta comparación como parte de la condición actual.
        ) {
            if (py < mejorY) {                                              // Comprueba si se cumplen las condiciones necesarias para continuar.
                mejor = (int)i;                                             // Calcula y guarda mejor opción encontrada.
                mejorY = py;                                                // Calcula y guarda menor altura válida encontrada.
            }
        }
    }

    return mejor;                                                           // Devuelve mejor opción encontrada como resultado de la función.
}

int BuscarPlataformaDestinoEscalera(int escalera)                       // Busca la plataforma inferior conectada con una escalera para que el barril pueda terminar su descenso.
{
    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        escalera < 0 ||                                                     // Añade esta condición a la comprobación actual.
        escalera >= (int)NUM_ESCALERAS                                      // Evalúa esta comparación como parte de la condición actual.
    )
        return -1;                                                          // Devuelve -1 como resultado de la función.

    const Escalera *e = &escaleras[escalera];                               // Calcula y guarda e.

    int top = e->y1;                                                        // Calcula y guarda extremo superior.
    int bottom = e->y2;                                                     // Calcula y guarda extremo inferior.

    if (bottom < top) {                                                     // Comprueba si se cumplen las condiciones necesarias para continuar.
        int temp = top;                                                     // Calcula y guarda temp.
        top = bottom;                                                       // Calcula y guarda extremo superior.
        bottom = temp;                                                      // Calcula y guarda extremo inferior.
    }

    int centroX = e->x + ESCALERA_W / 2;                                // Calcula el centro para comparar posiciones.
    int mejor = -1;                                                         // Inicializa mejor opción encontrada indicando que todavía no hay una opción válida.
    int mejorDiferencia = 999;                                              // Inicializa una diferencia grande para buscar después la plataforma más cercana.

    for (uint32_t i = 0; i < NUM_PLATAFORMAS; i++) {                        // Recorre todas las plataformas del escenario.
        const Plataforma *p = &plataformas[i];                              // Calcula y guarda p.

        if (centroX < p->x1 || centroX > p->x2)                             // Comprueba si se cumplen las condiciones necesarias para continuar.
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.

        int py = ObtenerYPlataforma(p, centroX);                        // Calcula la altura exacta de la viga inclinada en esta coordenada X.

        if (py <= top + 5)                                                  // Comprueba si se cumplen las condiciones necesarias para continuar.
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.

        int diferencia = AbsInt(py - bottom);                               // Calcula y guarda distancia vertical hasta la plataforma.

        if (                                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            diferencia <= 8 &&                                              // Añade esta condición a la comprobación actual.
            diferencia < mejorDiferencia                                    // Evalúa esta comparación como parte de la condición actual.
        ) {
            mejor = (int)i;                                                 // Calcula y guarda mejor opción encontrada.
            mejorDiferencia = diferencia;                                   // Calcula y guarda menor diferencia encontrada.
        }
    }

    return mejor;                                                           // Devuelve mejor opción encontrada como resultado de la función.
}

int BuscarEscaleraParaBarril(const Barril *b)                           // Comprueba si un barril que rueda se encuentra sobre una escalera válida por la que podría bajar.
{
    if (                                                                    // Comprueba una condición del barril antes de continuar.
        b->plataformaActual < 0 ||                                          // Comprueba si existe una plataforma actual válida.
        b->plataformaActual >= (int)NUM_PLATAFORMAS                         // Evalúa esta comparación como parte de la condición actual.
    )
        return -1;                                                          // Devuelve -1 como resultado de la función.

    const Plataforma *p =                                                   // Calcula y guarda p.
        &plataformas[b->plataformaActual];                                  // Evalúa esta comparación como parte de la condición actual.

    int centroBarril = b->x + b->w / 2;                                 // Calcula el centro para comparar posiciones.

    for (uint32_t i = 0; i < NUM_ESCALERAS; i++) {                          // Recorre todas las escaleras del escenario.
        const Escalera *e = &escaleras[i];                                  // Calcula y guarda e.

        int top = e->y1;                                                    // Calcula y guarda extremo superior.
        int bottom = e->y2;                                                 // Calcula y guarda extremo inferior.

        if (bottom < top) {                                                 // Comprueba si se cumplen las condiciones necesarias para continuar.
            int temp = top;                                                 // Calcula y guarda temp.
            top = bottom;                                                   // Calcula y guarda extremo superior.
            bottom = temp;                                                  // Calcula y guarda extremo inferior.
        }

        int centroEscalera =                                                // Calcula y guarda coordenada X del centro de la escalera.
            e->x + ESCALERA_W / 2;                                      // Calcula el centro para comparar posiciones.

        if (BuscarPlataformaDestinoEscalera((int)i) < 0)                    // Comprueba si se cumplen las condiciones necesarias para continuar.
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.

        int py =                                                            // Calcula y guarda altura Y de la plataforma.
            ObtenerYPlataforma(                                         // Calcula la altura exacta de la viga inclinada en esta coordenada X.
                p,                                                          // Indica plataforma consultada.
                centroEscalera                                              // Indica coordenada X donde se calcula la altura.
            );                                                              // Continúa el cálculo de altura Y de la plataforma.

        if (AbsInt(py - top) > 7)                                           // Comprueba si se cumplen las condiciones necesarias para continuar.
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.

        if (                                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            AbsInt(                                                         // Calcula el valor absoluto de un entero.
                centroBarril -                                              // Entrega este parámetro a AbsInt().
                centroEscalera                                              // Entrega este parámetro a AbsInt().
            ) <= 4                                                          // Evalúa esta comparación como parte de la condición actual.
        )
            return (int)i;                                                  // Devuelve una de las opciones según la condición que se evalúa a continuación.
    }

    return -1;                                                              // Devuelve -1 como resultado de la función.
}

uint8_t DebeBajarEscalera(                                              // Decide por probabilidad si el barril baja por la escalera.
    Barril *b,
    int escalera                                                            // Recibe índice de escalera.
)
{
    Jugador *objetivo =                                                     // Calcula y guarda jugador objetivo.
        JugadorObjetivoBarril(b);                                       // Calcula cuál jugador está más cerca de un barril usando distancia Manhattan y devuelve su puntero.

    int escaleraX =                                                         // Calcula y guarda coordenada X del centro de la escalera.
        escaleras[escalera].x +                                             // Accede a la posición correspondiente del arreglo escaleras.
        ESCALERA_W / 2;                                                     // Continúa el cálculo de coordenada X del centro de la escalera.

    int jugadorX =                                                          // Calcula y guarda coordenada X del centro del jugador.
        objetivo->x +                                                       // Evalúa esta comparación como parte de la condición actual.
        objetivo->w / 2;                                                    // Evalúa esta comparación como parte de la condición actual.

    uint8_t probabilidad =                                                  // Calcula y guarda probabilidad calculada.
        10 +                                                                // Continúa el cálculo de probabilidad calculada.
        DIFICULTAD_BARRILES * 5;                                            // Continúa el cálculo de probabilidad calculada.

    if (objetivo->y > b->y + 8)                                             // Comprueba si se cumplen las condiciones necesarias para continuar.
        probabilidad += 15;                                                 // Aumenta probabilidad calculada con este valor.

    if (AbsInt(jugadorX - escaleraX) < 28)                                  // Comprueba si se cumplen las condiciones necesarias para continuar.
        probabilidad += 35;                                                 // Aumenta probabilidad calculada con este valor.

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        (objetivo->direccion == 'R' && jugadorX < escaleraX) ||             // Comprueba si el comando actual indica movimiento hacia la derecha.
        (objetivo->direccion == 'L' && jugadorX > escaleraX)                // Comprueba si el comando actual indica movimiento hacia la izquierda.
    )
        probabilidad += 10;                                                 // Aumenta probabilidad calculada con este valor.

    if (probabilidad > 85)                                                  // Comprueba si se cumplen las condiciones necesarias para continuar.
        probabilidad = 85;                                                  // Calcula y guarda probabilidad calculada.

    return RNG_Porcentaje(probabilidad);                                    // Devuelve RNG_Porcentaje(probabilidad) como resultado de la función.
}

void IniciarBajadaEscalera(                                             // Cambia el barril al estado de escalera, lo centra y prepara su descenso.
    Barril *b,
    int escalera                                                            // Recibe índice de escalera.
)
{
    const Escalera *e = &escaleras[escalera];                               // Calcula y guarda e.

    int top = e->y1;                                                        // Calcula y guarda extremo superior.
    int bottom = e->y2;                                                     // Calcula y guarda extremo inferior.

    if (bottom < top) {                                                     // Comprueba si se cumplen las condiciones necesarias para continuar.
        int temp = top;                                                     // Calcula y guarda temp.
        top = bottom;                                                       // Calcula y guarda extremo superior.
        bottom = temp;                                                      // Calcula y guarda extremo inferior.
    }

    b->estado = BARRIL_ESCALERA;                                            // Cambia el estado actual del barril.
    b->escaleraActual = escalera;                                       // Índice de la escalera utilizada actualmente; -1 significa ninguna.
    b->plataformaActual = -1;                                           // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.
    b->vy = 0;                                                          // Velocidad vertical actual.

    b->x =                                                                  // Actualiza la posición horizontal del barril.
        e->x +                                                              // Evalúa esta comparación como parte de la condición actual.
        ESCALERA_W / 2 -                                                    // Continúa el cálculo de posición horizontal del barril.
        b->w / 2;                                                           // Evalúa esta comparación como parte de la condición actual.

    b->y =                                                                  // Actualiza la posición vertical del barril.
        top -                                                               // Continúa el cálculo de posición vertical del barril.
        b->h;                                                               // Evalúa esta comparación como parte de la condición actual.
}                                                                           // Continúa el cálculo de posición vertical del barril.

void IniciarReboteBarril(                                               // Coloca el barril sobre una plataforma y le da velocidad vertical negativa para simular el rebote.
    Barril *b,
    int plataforma                                                      // Índice de la plataforma asociada al martillo.
)
{
    b->plataformaActual = plataforma;                                   // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.
    b->escaleraActual = -1;                                             // Índice de la escalera utilizada actualmente; -1 significa ninguna.
    b->ultimaEscaleraEvaluada = -1;                                     // Última escalera sobre la que el barril ya tomó una decisión, para no repetirla en cada pixel.

    int py =                                                                // Calcula y guarda altura Y de la plataforma.
        ObtenerYPlataforma(                                             // Calcula la altura exacta de la viga inclinada en esta coordenada X.
            &plataformas[plataforma],                                       // Indica plataforma consultada.
            b->x + b->w / 2                                                 // Evalúa esta comparación como parte de la condición actual.
        );                                                                  // Continúa el cálculo de altura Y de la plataforma.

    b->y = py - b->h;                                                       // Actualiza la posición vertical del barril.
    b->vy = VELOCIDAD_REBOTE;                                           // Velocidad vertical actual.

    b->direccion =                                                          // Actualiza dirección de movimiento del barril.
        DireccionBarrilPorPlataforma(                                       // Continúa el cálculo de dirección de movimiento del barril.
            plataforma                                                      // Continúa el cálculo de dirección de movimiento del barril.
        );                                                                  // Continúa el cálculo de dirección de movimiento del barril.

    b->estado =                                                             // Cambia el estado actual del barril.
        BARRIL_REBOTANDO;                                                   // Continúa el cálculo de estado actual del barril.
}


/* ============================================================
 * FLAMAS - CREACIÓN Y MOVIMIENTO
 *
 * Se crean las flamas que nacen en el aceite y se actualiza su movimiento por el escenario.
 * La IA decide después si patrullan, persiguen o buscan una escalera.
 * ============================================================ */

void InicializarFireballs(void)                                         // Reinicia todos los slots de flamas y el contador de flamas activas.
{
    fireballsActivos = 0;                                                   // Reinicia fireballsActivos a cero.

    for (int i = 0; i < MAX_FIREBALLS; i++) {                               // Recorre todos los slots de flamas.
        fireballs[i].activo = 0;                                            // Marca la flama como inactivo y deja libre su slot.
        fireballs[i].estado = FLAMA_INACTIVA;                               // Cambia el estado actual de la flama.
        fireballs[i].vx = 0;                                                // Actualiza la velocidad horizontal de la flama.
        fireballs[i].vy = 0;                                                // Actualiza la velocidad vertical de la flama.
        fireballs[i].direccion = 1;                                         // Fija la dirección de la flama hacia la derecha.
        fireballs[i].direccionVertical = 0;                                 // Deja la flama sin movimiento vertical hasta que entre a una escalera.
        fireballs[i].plataformaActual = -1;                                 // Indica que la flama no tiene plataforma actual.
        fireballs[i].plataformaDestino = -1;                                // Inicializa plataforma destino de la flama indicando que todavía no hay una opción válida.
        fireballs[i].escaleraActual = -1;                                   // Indica que la flama no tiene escalera actual.
        fireballs[i].ultimaEscalera = -1;                                   // Indica que la flama no tiene última escalera usada.
        fireballs[i].escaleraObjetivo = -1;                                 // Indica que la flama no tiene escalera objetivo.
        fireballs[i].jugadorObjetivo = 1;                                   // Selecciona a J1 como objetivo inicial de la flama.
        fireballs[i].tiempoEstado = 0;                                      // Reinicia inicio del estado actual de la flama a cero.
        fireballs[i].proximaDecision = 0;                                   // Reinicia próxima decisión de IA de la flama a cero.
        fireballs[i].bloqueoEscaleraHasta = 0;                              // Reinicia fin del bloqueo de escalera de la flama a cero.
    }
}

void CrearFireball(void)                                                // Crea una flama en el aceite y prepara su salida.
{
    if (fireballsActivos >= MAX_FIREBALLS)                                  // Comprueba si se cumplen las condiciones necesarias para continuar.
        return;                                                             // Termina la función en este punto.

    for (int i = 0; i < MAX_FIREBALLS; i++) {                               // Recorre todos los slots de flamas.
        if (!fireballs[i].activo) {                                         // Comprueba una condición de la flama antes de continuar.
            Fireball *f = &fireballs[i];                                    // Calcula y guarda f.

            f->activo = 1;                                                  // Marca la flama como activo.
            f->w = FIREBALL_W;                                          // Ancho de la caja o sprite.
            f->h = FIREBALL_H;                                          // Alto de la caja o sprite.

            f->vx = 0;                                                  // Velocidad horizontal actual.
            f->vy = 0;                                                  // Velocidad vertical actual.

            f->x =                                                          // Actualiza la posición horizontal de la flama.
                ACEITE_X +                                                  // Continúa el cálculo de posición horizontal de la flama.
                ACEITE_W / 2 -                                              // Continúa el cálculo de posición horizontal de la flama.
                FIREBALL_W / 2;                                             // Continúa el cálculo de posición horizontal de la flama.

            /*
             * En FLAMA_NACIENDO la flama queda oculta dentro
             * del barril. No se dibuja hasta comenzar a salir.
             */
            f->y =                                                          // Actualiza la posición vertical de la flama.
                AceiteY() +                                                 // Continúa el cálculo de posición vertical de la flama.
                ACEITE_H -                                                  // Continúa el cálculo de posición vertical de la flama.
                FIREBALL_H;                                                 // Continúa el cálculo de posición vertical de la flama.

            f->direccion = 1;                                               // Fija la dirección de la flama hacia la derecha.
            f->direccionVertical = 0;                                   // Dirección por escalera: -1 subir, +1 bajar.

            f->plataformaActual =                                           // Actualiza plataforma actual de la flama.
                (int)NUM_PLATAFORMAS - 1;                               // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.

            f->plataformaDestino = -1;                                  // Plataforma a la que se quiere llegar mediante una escalera.
            f->escaleraActual = -1;                                     // Índice de la escalera utilizada actualmente; -1 significa ninguna.
            f->ultimaEscalera = -1;                                     // Última escalera utilizada por la flama.
            f->escaleraObjetivo = -1;                                   // Escalera hacia la que se está desplazando la flama.

            f->estado = FLAMA_NACIENDO;                                     // Cambia el estado actual de la flama.
            f->color = COLOR_FIREBALL;                                      // Guarda el color que debe usar la flama.

            f->jugadorObjetivo =                                            // Actualiza jugador objetivo de la flama.
                ElegirJugadorObjetivoFireball(f);                       // Elige como objetivo al jugador más cercano.

            f->tiempoEstado =                                               // Actualiza inicio del estado actual de la flama.
                HAL_GetTick();                                          // Marca cuándo comenzó el estado de la flama.

            f->proximaDecision =                                            // Actualiza próxima decisión de IA de la flama.
                HAL_GetTick() +                                             // Continúa el cálculo de próxima decisión de IA de la flama.
                FIREBALL_NACER_MS;                                      // Tick a partir del cual la IA puede volver a decidir.

            f->bloqueoEscaleraHasta = 0;                                // Tick hasta el que se bloquea la reutilización de una escalera.

            fireballsActivos++;                                             // Incrementa fireballsActivos en una unidad.
            return;                                                         // Termina la función en este punto.
        }                                                                   // Continúa el cálculo de próxima decisión de IA de la flama.
    }                                                                       // Continúa el cálculo de próxima decisión de IA de la flama.
}                                                                           // Continúa el cálculo de próxima decisión de IA de la flama.

void DesactivarFireball(Fireball *f)                                    // Libera una flama activa y actualiza el contador global.
{
    if (!f->activo)                                                         // Comprueba una condición de la flama antes de continuar.
        return;                                                             // Termina la función en este punto.

    f->activo = 0;                                                          // Marca la flama como inactivo y deja libre su slot.
    f->estado = FLAMA_INACTIVA;                                             // Cambia el estado actual de la flama.
    f->vx = 0;                                                          // Velocidad horizontal actual.
    f->vy = 0;                                                          // Velocidad vertical actual.
    f->plataformaActual = -1;                                           // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.
    f->plataformaDestino = -1;                                          // Plataforma a la que se quiere llegar mediante una escalera.
    f->escaleraActual = -1;                                             // Índice de la escalera utilizada actualmente; -1 significa ninguna.
    f->ultimaEscalera = -1;                                             // Última escalera utilizada por la flama.
    f->escaleraObjetivo = -1;                                           // Escalera hacia la que se está desplazando la flama.

    if (fireballsActivos > 0)                                               // Comprueba si se cumplen las condiciones necesarias para continuar.
        fireballsActivos--;                                                 // Reduce fireballsActivos en una unidad.
}

void DibujarFireball(const Fireball *f)                                 // Dibuja una flama activa usando el cuadro de animación RGB565 actual.
{
    if (!f->activo || f->estado == FLAMA_NACIENDO) return;                  // Comprueba si se cumplen las condiciones necesarias para continuar.
    for (int y = 0; y < FIREBALL_H; y++)                                    // Recorre todas las posiciones necesarias de este bloque.
        for (int x = 0; x < FIREBALL_W; x++) {                              // Recorre todas las posiciones necesarias de este bloque.
            int cuadro = cuadroFlamaActual;                                 // Calcula y guarda cuadro.
            uint16_t pixel = flamaNueva[(cuadro * FIREBALL_H + y) *         // Calcula y guarda posición lineal del píxel.
                                          FIREBALL_W + x];                  // Continúa el cálculo de posición lineal del píxel.
            if (pixel) FillRect(f->x + x, f->y + y, 1, 1, pixel);           // Comprueba si se cumplen las condiciones necesarias para continuar.
        }
}

void DibujarFireballs(void)                                             // Recorre el arreglo y dibuja las flamas activas.
{
    for (int i = 0; i < MAX_FIREBALLS; i++) {                               // Recorre todos los slots de flamas.
        if (fireballs[i].activo)                                            // Comprueba una condición de la flama antes de continuar.
            DibujarFireball(&fireballs[i]);                             // Dibuja una flama activa usando el cuadro de animación RGB565 actual.
    }
}


/* ============================================================
 * FLAMAS - ELECCIÓN DEL JUGADOR OBJETIVO
 *
 * Se compara la posición de Mario y Luigi para elegir a cuál jugador seguirá una flama.
 * El objetivo puede mantenerse o cambiar cuando llega el siguiente momento de decisión.
 * ============================================================ */

Jugador *ObtenerJugadorObjetivoFireball(                                    // Devuelve el jugador que la flama tiene seleccionado como objetivo.
    const Fireball *f
)
{
    if (f->jugadorObjetivo == 2)                                            // Comprueba una condición del jugador 1 antes de continuar.
        return &jugador2;                                                   // Devuelve jugador2 como resultado de la función.

    return &jugador1;                                                       // Devuelve jugador1 como resultado de la función.
}

uint8_t ElegirJugadorObjetivoFireball(                                  // Compara distancias Manhattan y selecciona al jugador más cercano como objetivo.
    const Fireball *f
)
{
    int fx = f->x + f->w / 2;                                               // Calcula y guarda fx.
    int fy = f->y + f->h / 2;                                               // Calcula y guarda fy.

    int distancia1 =                                                        // Calcula y guarda distancia entre el barril y J1.
        AbsInt(jugador1.x + jugador1.w / 2 - fx) +                          // Calcula la distancia horizontal entre el barril y J1.
        AbsInt(jugador1.y + jugador1.h / 2 - fy);                           // Añade la distancia vertical entre el barril y J1.

    int distancia2 =                                                        // Calcula y guarda distancia entre el barril y J2.
        AbsInt(jugador2.x + jugador2.w / 2 - fx) +                          // Calcula la distancia horizontal entre el barril y J2.
        AbsInt(jugador2.y + jugador2.h / 2 - fy);                           // Añade la distancia vertical entre el barril y J2.

    return (distancia1 <= distancia2) ? 1 : 2;                              // Devuelve una de las opciones según la condición que se evalúa a continuación.
}


/* ============================================================
 * FLAMAS - BÚSQUEDA DE ESCALERAS
 *
 * Se identifica qué plataformas conecta cada escalera.
 * La flama usa esta información para acercarse a un jugador que está en otro nivel.
 * ============================================================ */

int BuscarPlataformaExtremoEscalera(                                    // Identifica la plataforma conectada al extremo superior o inferior de una escalera.
    int escalera,                                                           // Recibe índice de escalera.
    uint8_t superior                                                        // Recibe una bandera que indica si se busca el extremo superior o inferior.
)
{
    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        escalera < 0 ||                                                     // Añade esta condición a la comprobación actual.
        escalera >= (int)NUM_ESCALERAS                                      // Evalúa esta comparación como parte de la condición actual.
    )
        return -1;                                                          // Devuelve -1 como resultado de la función.

    const Escalera *e = &escaleras[escalera];                               // Calcula y guarda e.

    int top = e->y1;                                                        // Calcula y guarda extremo superior.
    int bottom = e->y2;                                                     // Calcula y guarda extremo inferior.

    if (bottom < top) {                                                     // Comprueba si se cumplen las condiciones necesarias para continuar.
        int temp = top;                                                     // Calcula y guarda temp.
        top = bottom;                                                       // Calcula y guarda extremo superior.
        bottom = temp;                                                      // Calcula y guarda extremo inferior.
    }

    int objetivoY =                                                         // Calcula y guarda objetivoY.
        superior ? top : bottom;                                            // Continúa el cálculo de objetivoY.

    int centroX =                                                           // Calcula y guarda coordenada X del centro.
        e->x + ESCALERA_W / 2;                                          // Calcula el centro para comparar posiciones.

    int mejor = -1;                                                         // Inicializa mejor opción encontrada indicando que todavía no hay una opción válida.
    int mejorDiferencia = 999;                                              // Inicializa una diferencia grande para buscar después la plataforma más cercana.

    for (uint32_t i = 0; i < NUM_PLATAFORMAS; i++) {                        // Recorre todas las plataformas del escenario.
        const Plataforma *p = &plataformas[i];                              // Calcula y guarda p.

        if (                                                                // Comprueba una condición de la plataforma antes de continuar.
            centroX < p->x1 ||                                              // Añade esta condición a la comprobación actual.
            centroX > p->x2                                                 // Evalúa esta comparación como parte de la condición actual.
        )
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.

        int py =                                                            // Calcula y guarda altura Y de la plataforma.
            ObtenerYPlataforma(                                         // Calcula la altura exacta de la viga inclinada en esta coordenada X.
                p,                                                          // Indica plataforma consultada.
                centroX                                                     // Indica coordenada X donde se calcula la altura.
            );                                                              // Continúa el cálculo de altura Y de la plataforma.

        int diferencia =                                                    // Calcula y guarda distancia vertical hasta la plataforma.
            AbsInt(                                                         // Continúa el cálculo de distancia vertical hasta la plataforma.
                py - objetivoY                                              // Continúa el cálculo de distancia vertical hasta la plataforma.
            );                                                              // Continúa el cálculo de distancia vertical hasta la plataforma.

        if (                                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            diferencia <= 8 &&                                              // Añade esta condición a la comprobación actual.
            diferencia < mejorDiferencia                                    // Evalúa esta comparación como parte de la condición actual.
        ) {
            mejor = (int)i;                                                 // Calcula y guarda mejor opción encontrada.
            mejorDiferencia = diferencia;                                   // Calcula y guarda menor diferencia encontrada.
        }
    }

    return mejor;                                                           // Devuelve mejor opción encontrada como resultado de la función.
}

int BuscarMejorEscaleraFireball(                                        // Busca la escalera que acerca la flama al jugador.
    const Fireball *f,
    const Jugador *objetivo,
    int8_t *direccionVertical,
    int8_t *plataformaDestino
)
{
    if (                                                                    // Comprueba una condición de la flama antes de continuar.
        f->plataformaActual < 0 ||                                          // Comprueba si existe una plataforma actual válida.
        f->plataformaActual >= (int)NUM_PLATAFORMAS                         // Evalúa esta comparación como parte de la condición actual.
    )
        return -1;                                                          // Devuelve -1 como resultado de la función.

    int centroFlama =                                                       // Calcula y guarda centroFlama.
        f->x + f->w / 2;                                                // Calcula el centro para comparar posiciones.

    int centroJugadorY =                                                    // Calcula y guarda centroJugadorY.
        objetivo->y + objetivo->h / 2;                                      // Evalúa esta comparación como parte de la condición actual.

    int centroFlamaY =                                                      // Calcula y guarda centroFlamaY.
        f->y + f->h / 2;                                                    // Evalúa esta comparación como parte de la condición actual.

    int8_t vertical = 0;                                                    // Reinicia vertical a cero.

    if (centroJugadorY < centroFlamaY - 10)                                 // Comprueba si se cumplen las condiciones necesarias para continuar.
        vertical = -1;                                                      // Inicializa vertical indicando que todavía no hay una opción válida.
    else if (centroJugadorY > centroFlamaY + 10)                            // Evalúa otra condición.
        vertical = 1;                                                       // Activa vertical.
    else                                                                    // Usa esta alternativa cuando la condición anterior no se cumple.
        return -1;                                                          // Devuelve -1 como resultado de la función.

    int mejorEscalera = -1;                                                 // Inicializa mejorEscalera indicando que todavía no hay una opción válida.
    int mejorDestino = -1;                                                  // Inicializa mejorDestino indicando que todavía no hay una opción válida.
    int mejorDistancia = 9999;                                              // Calcula y guarda menor distancia encontrada.

    uint32_t ahora =                                                        // Calcula y guarda tick actual.
        HAL_GetTick();                                                      // Continúa el cálculo de tick actual.

    for (uint32_t i = 0; i < NUM_ESCALERAS; i++) {                          // Recorre todas las escaleras del escenario.
        int plataformaSuperior =                                            // Calcula y guarda plataformaSuperior.
            BuscarPlataformaExtremoEscalera(                                // Continúa el cálculo de plataformaSuperior.
                (int)i,                                                     // Continúa el cálculo de plataformaSuperior.
                1                                                           // Continúa el cálculo de plataformaSuperior.
            );                                                              // Continúa el cálculo de plataformaSuperior.

        int plataformaInferior =                                            // Calcula y guarda plataformaInferior.
            BuscarPlataformaExtremoEscalera(                                // Continúa el cálculo de plataformaInferior.
                (int)i,                                                     // Continúa el cálculo de plataformaInferior.
                0                                                           // Continúa el cálculo de plataformaInferior.
            );                                                              // Continúa el cálculo de plataformaInferior.

        if (                                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            plataformaSuperior < 0 ||                                       // Añade esta condición a la comprobación actual.
            plataformaInferior < 0 ||                                       // Añade esta condición a la comprobación actual.
            plataformaSuperior == plataformaInferior                        // Evalúa esta comparación como parte de la condición actual.
        )
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.

        int destino = -1;                                                   // Inicializa destino indicando que todavía no hay una opción válida.

        if (vertical < 0) {                                                 // Comprueba una condición de la flama antes de continuar.
            if (                                                            // Comprueba una condición de la flama antes de continuar.
                f->plataformaActual !=                                      // Evalúa esta comparación como parte de la condición actual.
                plataformaInferior
            )
                continue;                                                   // Omite este caso y continúa con la siguiente iteración.

            destino = plataformaSuperior;                                   // Actualiza destino.
        } else {
            if (                                                            // Comprueba una condición de la flama antes de continuar.
                f->plataformaActual !=                                      // Evalúa esta comparación como parte de la condición actual.
                plataformaSuperior
            )
                continue;                                                   // Omite este caso y continúa con la siguiente iteración.

            destino = plataformaInferior;                                   // Actualiza destino.
        }

        if (                                                                // Comprueba una condición de la flama antes de continuar.
            (int)i == f->ultimaEscalera &&                                  // Añade esta condición a la comprobación actual.
            (int32_t)(
                ahora -
                f->bloqueoEscaleraHasta                                     // Evalúa esta comparación como parte de la condición actual.
            ) < 0                                                           // Evalúa esta comparación como parte de la condición actual.
        )
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.

        int centroEscalera =                                                // Calcula y guarda coordenada X del centro de la escalera.
            escaleras[i].x +                                                // Accede a la posición correspondiente del arreglo escaleras.
            ESCALERA_W / 2;                                             // Calcula el centro para comparar posiciones.

        int distancia =                                                     // Calcula y guarda distancia.
            AbsInt(                                                         // Continúa el cálculo de distancia.
                centroFlama -                                               // Continúa el cálculo de distancia.
                centroEscalera                                              // Continúa el cálculo de distancia.
            );                                                              // Continúa el cálculo de distancia.

        if (distancia < mejorDistancia) {                                   // Comprueba si se cumplen las condiciones necesarias para continuar.
            mejorDistancia = distancia;                                     // Calcula y guarda menor distancia encontrada.
            mejorEscalera = (int)i;                                         // Actualiza mejorEscalera.
            mejorDestino = destino;                                         // Actualiza mejorDestino.
        }
    }

    if (mejorEscalera >= 0) {                                               // Comprueba si se cumplen las condiciones necesarias para continuar.
        *direccionVertical = vertical;                                      // Actualiza dirección vertical en la escalera.
        *plataformaDestino =                                                // Actualiza plataforma destino.
            (int8_t)mejorDestino;                                           // Continúa el cálculo de plataforma destino.
    }

    return mejorEscalera;                                                   // Devuelve mejorEscalera como resultado de la función.
}


/* ============================================================
 * FLAMAS - ENTRADA A UNA ESCALERA
 *
 * Se coloca a la flama en la escalera elegida y se define si debe subir o bajar.
 * También se guarda la plataforma de destino.
 * ============================================================ */

void IniciarEscaleraFireball(                                           // Alinea la flama con una escalera y configura su estado y destino para subir o bajar.
    Fireball *f,
    int escalera,                                                           // Recibe índice de escalera.
    int8_t direccionVertical,                                           // Dirección por escalera: -1 subir, +1 bajar.
    int8_t plataformaDestino                                            // Plataforma a la que se quiere llegar mediante una escalera.
)
{
    const Escalera *e =                                                     // Calcula y guarda e.
        &escaleras[escalera];                                               // Accede a la posición correspondiente del arreglo escaleras.

    int top = e->y1;                                                        // Calcula y guarda extremo superior.
    int bottom = e->y2;                                                     // Calcula y guarda extremo inferior.

    if (bottom < top) {                                                     // Comprueba si se cumplen las condiciones necesarias para continuar.
        int temp = top;                                                     // Calcula y guarda temp.
        top = bottom;                                                       // Calcula y guarda extremo superior.
        bottom = temp;                                                      // Calcula y guarda extremo inferior.
    }

    f->x =                                                                  // Actualiza la posición horizontal de la flama.
        e->x +                                                              // Evalúa esta comparación como parte de la condición actual.
        ESCALERA_W / 2 -                                                    // Continúa el cálculo de posición horizontal de la flama.
        f->w / 2;                                                           // Evalúa esta comparación como parte de la condición actual.

    f->direccionVertical =                                                  // Actualiza dirección vertical en la escalera de la flama.
        direccionVertical;                                              // Dirección por escalera: -1 subir, +1 bajar.

    f->plataformaDestino =                                                  // Actualiza plataforma destino de la flama.
        plataformaDestino;                                              // Plataforma a la que se quiere llegar mediante una escalera.

    f->escaleraActual =                                                     // Actualiza escalera actual de la flama.
        escalera;                                                       // Índice de la escalera utilizada actualmente; -1 significa ninguna.

    f->escaleraObjetivo =                                                   // Actualiza escalera objetivo de la flama.
        -1;                                                             // Escalera hacia la que se está desplazando la flama.

    f->estado =                                                             // Cambia el estado actual de la flama.
        FLAMA_ESCALERA;                                                     // Continúa el cálculo de estado actual de la flama.

    f->tiempoEstado =                                                       // Actualiza inicio del estado actual de la flama.
        HAL_GetTick();                                                  // Marca cuándo comenzó el estado de la flama.

    f->plataformaActual =                                                   // Actualiza plataforma actual de la flama.
        -1;                                                             // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.

    if (direccionVertical < 0) {                                            // Comprueba una condición de la flama antes de continuar.
        if (f->y + f->h > bottom)                                           // Comprueba una condición de la flama antes de continuar.
            f->y = bottom - f->h;                                           // Actualiza la posición vertical de la flama.
    } else {
        if (f->y + f->h < top)                                              // Comprueba una condición de la flama antes de continuar.
            f->y = top - f->h;                                              // Actualiza la posición vertical de la flama.
    }
}


/* ============================================================
 * FLAMAS - DECISIÓN DE IA
 *
 * La IA decide periódicamente entre perseguir, patrullar o buscar una escalera.
 * La decisión no cambia en cada píxel, por lo que la flama no gira instantáneamente cuando el jugador la esquiva.
 * ============================================================ */

void TomarDecisionFireball(Fireball *f)                                 // Decide si la flama patrulla, persigue o busca escalera.
{
    uint32_t ahora =                                                        // Calcula y guarda tick actual.
        HAL_GetTick();                                                      // Continúa el cálculo de tick actual.

    f->proximaDecision =                                                    // Actualiza próxima decisión de IA de la flama.
        ahora +                                                             // Continúa el cálculo de próxima decisión de IA de la flama.
        FIREBALL_DECISION_MS;                                           // Tick a partir del cual la IA puede volver a decidir.

    f->escaleraObjetivo =                                                   // Actualiza escalera objetivo de la flama.
        -1;                                                             // Escalera hacia la que se está desplazando la flama.

    if (                                                                    // Comprueba una condición de la flama antes de continuar.
        f->jugadorObjetivo < 1 ||                                           // Añade esta condición a la comprobación actual.
        f->jugadorObjetivo > 2 ||                                           // Añade esta condición a la comprobación actual.
        RNG_Porcentaje(                                                     // Continúa el cálculo de escalera objetivo de la flama.
            FIREBALL_PROB_CAMBIAR_OBJETIVO                                  // Indica porcentaje de probabilidad que se evalúa.
        )                                                                   // Continúa el cálculo de escalera objetivo de la flama.
    ) {                                                                     // Continúa el cálculo de escalera objetivo de la flama.
        f->jugadorObjetivo =                                                // Actualiza jugador objetivo de la flama.
            ElegirJugadorObjetivoFireball(f);                           // Elige como objetivo al jugador más cercano.
    }                                                                       // Continúa el cálculo de jugador objetivo de la flama.

    Jugador *objetivo =                                                     // Calcula y guarda jugador objetivo.
        ObtenerJugadorObjetivoFireball(f);                              // Obtiene el puntero a J1 o J2 según jugadorObjetivo.

    int centroFlama =                                                       // Calcula y guarda centroFlama.
        f->x +                                                              // Evalúa esta comparación como parte de la condición actual.
        f->w / 2;                                                       // Calcula el centro para comparar posiciones.

    int centroJugador =                                                     // Calcula y guarda centroJugador.
        objetivo->x +                                                       // Evalúa esta comparación como parte de la condición actual.
        objetivo->w / 2;                                                // Calcula el centro para comparar posiciones.

    int centroFlamaY =                                                      // Calcula y guarda centroFlamaY.
        f->y +                                                              // Evalúa esta comparación como parte de la condición actual.
        f->h / 2;                                                           // Evalúa esta comparación como parte de la condición actual.

    int centroJugadorY =                                                    // Calcula y guarda centroJugadorY.
        objetivo->y +                                                       // Evalúa esta comparación como parte de la condición actual.
        objetivo->h / 2;                                                    // Evalúa esta comparación como parte de la condición actual.

    int diferenciaY =                                                       // Calcula y guarda diferenciaY.
        AbsInt(                                                             // Continúa el cálculo de diferenciaY.
            centroJugadorY -                                                // Continúa el cálculo de diferenciaY.
            centroFlamaY                                                    // Continúa el cálculo de diferenciaY.
        );                                                                  // Continúa el cálculo de diferenciaY.


    /* ========================================================
     * OBJETIVO EN OTRO NIVEL
     * ======================================================== */
    if (diferenciaY > 15) {                                                 // Comprueba si se cumplen las condiciones necesarias para continuar.
        int8_t vertical = 0;                                                // Reinicia vertical a cero.
        int8_t destino = -1;                                                // Inicializa destino indicando que todavía no hay una opción válida.

        int escalera =                                                      // Calcula y guarda índice de escalera.
            BuscarMejorEscaleraFireball(                                    // Continúa el cálculo de índice de escalera.
                f,                                                          // Continúa el cálculo de índice de escalera.
                objetivo,                                                   // Continúa el cálculo de índice de escalera.
                &vertical,                                                  // Continúa el cálculo de índice de escalera.
                &destino                                                    // Continúa el cálculo de índice de escalera.
            );                                                              // Continúa el cálculo de índice de escalera.


        if (                                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            escalera >= 0 &&                                                // Añade esta condición a la comprobación actual.
            RNG_Porcentaje(                                                 // Devuelve 1 si el número aleatorio cae dentro del porcentaje pedido; si no, devuelve 0.
                FIREBALL_PROB_ESCALERA                                      // Indica porcentaje de probabilidad que se evalúa.
            )
        ) {
            f->escaleraObjetivo =                                           // Actualiza escalera objetivo de la flama.
                (int8_t)escalera;                                       // Escalera hacia la que se está desplazando la flama.

            f->direccionVertical =                                          // Actualiza dirección vertical en la escalera de la flama.
                vertical;                                               // Dirección por escalera: -1 subir, +1 bajar.

            f->plataformaDestino =                                          // Actualiza plataforma destino de la flama.
                destino;                                                // Plataforma a la que se quiere llegar mediante una escalera.

            int centroEscalera =                                            // Calcula y guarda coordenada X del centro de la escalera.
                escaleras[escalera].x +                                     // Accede a la posición correspondiente del arreglo escaleras.
                ESCALERA_W / 2;                                         // Calcula el centro para comparar posiciones.

            f->direccion =                                                  // Actualiza dirección de movimiento de la flama.
                (centroEscalera < centroFlama)                              // Evalúa esta comparación como parte de la condición actual.
                ? -1                                                        // Usa esta opción cuando la condición del operador ternario es verdadera.
                : 1;                                                        // Usa esta opción cuando la condición del operador ternario es falsa.

            f->estado =                                                     // Cambia el estado actual de la flama.
                FLAMA_PERSIGUIENDO;                                         // Continúa el cálculo de estado actual de la flama.

            return;                                                         // Termina la función en este punto.
        }

        /*
         * Sin escalera o decide no usarla:
         * patrulla.
         */
        f->estado =                                                         // Cambia el estado actual de la flama.
            FLAMA_PATRULLANDO;                                              // Continúa el cálculo de estado actual de la flama.

        if (                                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            RNG_Porcentaje(                                                 // Devuelve 1 si el número aleatorio cae dentro del porcentaje pedido; si no, devuelve 0.
                FIREBALL_PROB_GIRAR_ALEATORIO                               // Indica porcentaje de probabilidad que se evalúa.
            )
        ) {
            f->direccion =                                                  // Actualiza dirección de movimiento de la flama.
                -f->direccion;                                              // Evalúa esta comparación como parte de la condición actual.
        }                                                                   // Continúa el cálculo de dirección de movimiento de la flama.

        return;                                                             // Termina la función en este punto.
    }                                                                       // Continúa el cálculo de dirección de movimiento de la flama.


    /* ========================================================
     * MISMO NIVEL
     * ======================================================== */

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        RNG_Porcentaje(                                                     // Continúa el cálculo de dirección de movimiento de la flama.
            FIREBALL_PROB_PERSEGUIR                                         // Indica porcentaje de probabilidad que se evalúa.
        )                                                                   // Continúa el cálculo de dirección de movimiento de la flama.
    ) {                                                                     // Continúa el cálculo de dirección de movimiento de la flama.
        if (centroJugador < centroFlama)                                    // Comprueba una condición de la flama antes de continuar.
            f->direccion = -1;                                              // Fija la dirección de la flama hacia la izquierda.
        else if (centroJugador > centroFlama)                               // Evalúa otra condición de la flama.
            f->direccion = 1;                                               // Fija la dirección de la flama hacia la derecha.

        f->estado =                                                         // Cambia el estado actual de la flama.
            FLAMA_PERSIGUIENDO;                                             // Continúa el cálculo de estado actual de la flama.
    } else {

        f->estado =                                                         // Cambia el estado actual de la flama.
            FLAMA_PATRULLANDO;                                              // Continúa el cálculo de estado actual de la flama.
    }
}


/* ============================================================
 * FLAMAS - MÁQUINA DE ESTADOS
 *
 * Se ejecuta la física correspondiente al estado actual de cada flama.
 * Este bloque controla nacimiento, salida del aceite, movimiento horizontal, escaleras, bordes y recuperación.
 * ============================================================ */

void ActualizarFireball(                                                // nacimiento, salto desde el aceite, persecución, patrullaje, recuperación o movimiento por escalera.
    Fireball *f,
    uint8_t pasoMovimiento                                                  // Recibe una bandera que indica si en este ciclo corresponde aplicar movimiento.
)
{
    if (!f->activo)                                                         // Comprueba si se cumplen las condiciones necesarias para continuar.
        return;                                                             // Termina la función en este punto.

    uint32_t ahora =                                                        // Calcula y guarda tick actual.
        HAL_GetTick();                                                      // Continúa el cálculo de tick actual.


    /* ========================================================
     * NACIMIENTO
     *
     * La flama permanece un instante dentro del aceite.
     * NO se cambia su posición de golpe.
     * ======================================================== */
    if (f->estado == FLAMA_NACIENDO) {                                      // Comprueba una condición de la flama antes de continuar.
        if (                                                                // Comprueba una condición de la flama antes de continuar.
            ahora - f->tiempoEstado <                                       // Evalúa esta comparación como parte de la condición actual.
            FIREBALL_NACER_MS
        )
            return;                                                         // Termina la función en este punto.

        /*
         * Comienza el salto real POR ARRIBA del barril.
         *
         * Se coloca centrada en la boca superior y todavía
         * parcialmente solapada con el barril. Primero sube
         * verticalmente y, cuando despeja el borde superior,
         * empieza a avanzar hacia la derecha.
         */
        f->x =                                                              // Actualiza la posición horizontal de la flama.
            ACEITE_X +                                                      // Continúa el cálculo de posición horizontal de la flama.
            ACEITE_W / 2 -                                                  // Continúa el cálculo de posición horizontal de la flama.
            f->w / 2;                                                       // Evalúa esta comparación como parte de la condición actual.

        f->y =                                                              // Actualiza la posición vertical de la flama.
            AceiteY() -                                                     // Continúa el cálculo de posición vertical de la flama.
            2;                                                              // Continúa el cálculo de posición vertical de la flama.

        f->estado =                                                         // Cambia el estado actual de la flama.
            FLAMA_SALIENDO_ACEITE;                                          // Continúa el cálculo de estado actual de la flama.

        f->vx =                                                             // Actualiza la velocidad horizontal de la flama.
            0;                                                          // Velocidad horizontal actual.

        f->vy =                                                             // Actualiza la velocidad vertical de la flama.
            FIREBALL_SALIDA_VY;                                         // Velocidad vertical actual.

        f->direccion =                                                      // Actualiza dirección de movimiento de la flama.
            1;                                                              // Continúa el cálculo de dirección de movimiento de la flama.

        f->direccionVertical =                                              // Actualiza dirección vertical en la escalera de la flama.
            0;                                                          // Dirección por escalera: -1 subir, +1 bajar.

        /*
         * Mientras está en el aire no pertenece todavía
         * a ninguna plataforma.
         */
        f->plataformaActual =                                               // Actualiza plataforma actual de la flama.
            -1;                                                         // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.

        f->tiempoEstado =                                                   // Actualiza inicio del estado actual de la flama.
            ahora;                                                      // Tick en que comenzó el estado actual.

        return;                                                             // Termina la función en este punto.
    }                                                                       // Continúa el cálculo de inicio del estado actual de la flama.


    /* ========================================================
     * SALIENDO DEL BARRIL DE ACEITE
     *
     * Movimiento parabólico:
     *    vx constante hacia la derecha
     *    vy inicia negativo y la gravedad lo hace caer
     *
     * Se actualiza con el mismo paso temporal de las flamas.
     * ======================================================== */
    if (                                                                    // Comprueba una condición de la flama antes de continuar.
        f->estado ==                                                        // Evalúa esta comparación como parte de la condición actual.
        FLAMA_SALIENDO_ACEITE                                           // Flama realizando el salto físico para salir del aceite.
    ) {                                                                     // Continúa el cálculo de inicio del estado actual de la flama.
        if (!pasoMovimiento)                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            return;                                                         // Termina la función en este punto.

        int oldBottom =                                                     // Calcula y guarda posición anterior de los pies.
            f->y + f->h;                                                    // Evalúa esta comparación como parte de la condición actual.

        /*
         * Mientras la flama todavía cruza la boca del barril,
         * sale verticalmente. Cuando su parte inferior despeja
         * el borde superior del aceite, empieza el movimiento X.
         */
        if (                                                                // Comprueba una condición de la flama antes de continuar.
            f->y + f->h <=                                                  // Evalúa esta comparación como parte de la condición actual.
            AceiteY()                                                       // Continúa el cálculo de posición anterior de los pies.
        ) {                                                                 // Continúa el cálculo de posición anterior de los pies.
            f->vx =                                                         // Actualiza la velocidad horizontal de la flama.
                FIREBALL_SALIDA_VX;                                     // Velocidad horizontal actual.
        } else {                                                            // Continúa el cálculo de velocidad horizontal de la flama.
            f->vx =                                                         // Actualiza la velocidad horizontal de la flama.
                0;                                                      // Velocidad horizontal actual.
        }                                                                   // Continúa el cálculo de velocidad horizontal de la flama.

        f->x +=                                                             // Aumenta posición horizontal de la flama con este valor.
            f->vx;                                                          // Evalúa esta comparación como parte de la condición actual.

        f->y +=                                                             // Aumenta posición vertical de la flama con este valor.
            f->vy;                                                          // Evalúa esta comparación como parte de la condición actual.

        f->vy +=                                                            // Aumenta velocidad vertical de la flama con este valor.
            FIREBALL_GRAVEDAD_SALIDA;                                       // Continúa el cálculo de velocidad vertical de la flama.

        if (                                                                // Comprueba una condición de la flama antes de continuar.
            f->vy >                                                         // Evalúa esta comparación como parte de la condición actual.
            FIREBALL_MAX_CAIDA_SALIDA
        ) {
            f->vy =                                                         // Actualiza la velocidad vertical de la flama.
                FIREBALL_MAX_CAIDA_SALIDA;                              // Velocidad vertical actual.
        }                                                                   // Continúa el cálculo de velocidad vertical de la flama.


        /*
         * La flama debe aterrizar en la plataforma inferior.
         */
        int plataforma =                                                    // Calcula y guarda índice de plataforma.
            (int)NUM_PLATAFORMAS - 1;                                   // Índice de la plataforma asociada al martillo.

        const Plataforma *p =                                               // Calcula y guarda p.
            &plataformas[plataforma];                                       // Accede a la posición correspondiente del arreglo plataformas.

        /*
         * Seguridad horizontal.
         */
        if (f->x < p->x1)                                                   // Comprueba una condición de la flama antes de continuar.
            f->x = p->x1;                                                   // Actualiza la posición horizontal de la flama.

        if (                                                                // Comprueba una condición de la flama antes de continuar.
            f->x + f->w >                                                   // Evalúa esta comparación como parte de la condición actual.
            p->x2                                                           // Evalúa esta comparación como parte de la condición actual.
        ) {
            f->x =                                                          // Actualiza la posición horizontal de la flama.
                p->x2 -                                                     // Evalúa esta comparación como parte de la condición actual.
                f->w;                                                       // Evalúa esta comparación como parte de la condición actual.
        }                                                                   // Continúa el cálculo de posición horizontal de la flama.


        int py =                                                            // Calcula y guarda altura Y de la plataforma.
            ObtenerYPlataforma(                                         // Calcula la altura exacta de la viga inclinada en esta coordenada X.
                p,                                                          // Indica plataforma consultada.
                f->x + f->w / 2                                             // Evalúa esta comparación como parte de la condición actual.
            );                                                              // Continúa el cálculo de altura Y de la plataforma.

        int newBottom =                                                     // Calcula y guarda nueva posición de los pies.
            f->y + f->h;                                                    // Evalúa esta comparación como parte de la condición actual.


        /*
         * Solo aterriza cuando ya viene cayendo
         * y cruza la altura de la viga.
         */
        if (                                                                // Comprueba una condición de la flama antes de continuar.
            f->vy >= 0 &&                                                   // Añade esta condición a la comprobación actual.
            oldBottom <= py &&                                              // Añade esta condición a la comprobación actual.
            newBottom >= py                                                 // Evalúa esta comparación como parte de la condición actual.
        ) {                                                                 // Continúa el cálculo de nueva posición de los pies.
            f->y =                                                          // Actualiza la posición vertical de la flama.
                py -                                                        // Continúa el cálculo de posición vertical de la flama.
                f->h;                                                       // Evalúa esta comparación como parte de la condición actual.

            f->vx =                                                         // Actualiza la velocidad horizontal de la flama.
                0;                                                      // Velocidad horizontal actual.

            f->vy =                                                         // Actualiza la velocidad vertical de la flama.
                0;                                                      // Velocidad vertical actual.

            f->plataformaActual =                                           // Actualiza plataforma actual de la flama.
                plataforma;                                             // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.

            f->estado =                                                     // Cambia el estado actual de la flama.
                FLAMA_RECUPERANDO;                                          // Continúa el cálculo de estado actual de la flama.

            f->tiempoEstado =                                               // Actualiza inicio del estado actual de la flama.
                ahora;                                                  // Tick en que comenzó el estado actual.

            f->proximaDecision =                                            // Actualiza próxima decisión de IA de la flama.
                ahora +                                                     // Continúa el cálculo de próxima decisión de IA de la flama.
                FIREBALL_RECUPERAR_MS;                                  // Tick a partir del cual la IA puede volver a decidir.

            return;                                                         // Termina la función en este punto.
        }                                                                   // Continúa el cálculo de próxima decisión de IA de la flama.


        return;                                                             // Termina la función en este punto.
    }                                                                       // Continúa el cálculo de próxima decisión de IA de la flama.


    /* ========================================================
     * RECUPERANDO
     *
     * Sale del aceite o acaba de completar una escalera.
     * Se mueve en la direccion actual sin reconsiderar
     * inmediatamente al jugador.
     * ======================================================== */
    if (f->estado == FLAMA_RECUPERANDO) {                                   // Comprueba si se cumplen las condiciones necesarias para continuar.
        if (                                                                // Comprueba una condición de la flama antes de continuar.
            (int32_t)(                                                      // Continúa el cálculo de próxima decisión de IA de la flama.
                ahora -                                                     // Continúa el cálculo de próxima decisión de IA de la flama.
                f->proximaDecision                                          // Evalúa esta comparación como parte de la condición actual.
            ) >= 0                                                          // Evalúa esta comparación como parte de la condición actual.
        ) {                                                                 // Continúa el cálculo de próxima decisión de IA de la flama.
            TomarDecisionFireball(f);                                   // Decide si la flama patrulla, persigue o busca escalera.
        }                                                                   // Continúa el cálculo de próxima decisión de IA de la flama.
    }                                                                       // Continúa el cálculo de próxima decisión de IA de la flama.


    /* ========================================================
     * PATRULLANDO / PERSIGUIENDO / RECUPERANDO
     * ======================================================== */
    if (                                                                    // Comprueba una condición de la flama antes de continuar.
        f->estado == FLAMA_PATRULLANDO ||                                   // Comprueba si el estado es FLAMA_PATRULLANDO: Flama avanza sin corregir continuamente hacia un jugador.
        f->estado == FLAMA_PERSIGUIENDO ||                                  // Comprueba si el estado es FLAMA_PERSIGUIENDO: Flama intenta acercarse al jugador objetivo.
        f->estado == FLAMA_RECUPERANDO                                      // Comprueba si el estado es FLAMA_RECUPERANDO: Estado temporal tras aterrizar, tocar un borde o terminar una escalera.
    ) {                                                                     // Continúa el cálculo de próxima decisión de IA de la flama.
        if (!pasoMovimiento)                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            return;                                                         // Termina la función en este punto.

        if (                                                                // Comprueba una condición de la flama antes de continuar.
            f->plataformaActual < 0 ||                                      // Comprueba si existe una plataforma actual válida.
            f->plataformaActual >=                                          // Evalúa esta comparación como parte de la condición actual.
            (int)NUM_PLATAFORMAS                                            // Continúa el cálculo de próxima decisión de IA de la flama.
        )                                                                   // Continúa el cálculo de próxima decisión de IA de la flama.
            return;                                                         // Termina la función en este punto.


        /*
         * Solo reconsidera cada cierto tiempo.
         */
        if (                                                                // Comprueba una condición de la flama antes de continuar.
            f->estado != FLAMA_RECUPERANDO &&                               // Añade esta condición a la comprobación actual.
            (int32_t)(                                                      // Continúa el cálculo de próxima decisión de IA de la flama.
                ahora -                                                     // Continúa el cálculo de próxima decisión de IA de la flama.
                f->proximaDecision                                          // Evalúa esta comparación como parte de la condición actual.
            ) >= 0                                                          // Evalúa esta comparación como parte de la condición actual.
        ) {                                                                 // Continúa el cálculo de próxima decisión de IA de la flama.
            TomarDecisionFireball(f);                                   // Decide si la flama patrulla, persigue o busca escalera.
        }                                                                   // Continúa el cálculo de próxima decisión de IA de la flama.


        /*
         * Si tomó la decisión de usar una escalera,
         * avanza hacia su centro.
         */
        if (                                                                // Comprueba una condición de la flama antes de continuar.
            f->escaleraObjetivo >= 0 &&                                     // Añade esta condición a la comprobación actual.
            f->escaleraObjetivo <                                           // Evalúa esta comparación como parte de la condición actual.
            (int8_t)NUM_ESCALERAS                                           // Continúa el cálculo de próxima decisión de IA de la flama.
        ) {                                                                 // Continúa el cálculo de próxima decisión de IA de la flama.
            int escalera =                                                  // Calcula y guarda índice de escalera.
                f->escaleraObjetivo;                                        // Evalúa esta comparación como parte de la condición actual.

            int centroFlama =                                               // Calcula y guarda centroFlama.
                f->x +                                                      // Evalúa esta comparación como parte de la condición actual.
                f->w / 2;                                               // Calcula el centro para comparar posiciones.

            int centroEscalera =                                            // Calcula y guarda coordenada X del centro de la escalera.
                escaleras[escalera].x +                                     // Accede a la posición correspondiente del arreglo escaleras.
                ESCALERA_W / 2;                                         // Calcula el centro para comparar posiciones.

            if (                                                            // Comprueba si se cumplen las condiciones necesarias para continuar.
                AbsInt(                                                     // Continúa el cálculo de coordenada X del centro de la escalera.
                    centroFlama -                                           // Continúa el cálculo de coordenada X del centro de la escalera.
                    centroEscalera                                          // Continúa el cálculo de coordenada X del centro de la escalera.
                ) <=                                                        // Evalúa esta comparación como parte de la condición actual.
                FIREBALL_TOL_ESCALERA                                       // Continúa el cálculo de coordenada X del centro de la escalera.
            ) {                                                             // Continúa el cálculo de coordenada X del centro de la escalera.
                IniciarEscaleraFireball(                                    // Continúa el cálculo de coordenada X del centro de la escalera.
                    f,                                                      // Continúa el cálculo de coordenada X del centro de la escalera.
                    escalera,                                               // Continúa el cálculo de coordenada X del centro de la escalera.
                    f->direccionVertical,                                   // Evalúa esta comparación como parte de la condición actual.
                    f->plataformaDestino                                    // Evalúa esta comparación como parte de la condición actual.
                );                                                          // Continúa el cálculo de coordenada X del centro de la escalera.

                return;                                                     // Termina la función en este punto.
            }

            f->direccion =                                                  // Actualiza dirección de movimiento de la flama.
                (centroEscalera < centroFlama)                              // Evalúa esta comparación como parte de la condición actual.
                ? -1                                                        // Usa esta opción cuando la condición del operador ternario es verdadera.
                : 1;                                                        // Usa esta opción cuando la condición del operador ternario es falsa.
        }


        /* Movimiento horizontal */
        f->x +=                                                             // Aumenta posición horizontal de la flama con este valor.
            f->direccion *                                                  // Evalúa esta comparación como parte de la condición actual.
            VELOCIDAD_FIREBALL;                                             // Continúa el cálculo de posición horizontal de la flama.

        const Plataforma *p =                                               // Calcula y guarda p.
            &plataformas[                                                   // Continúa el cálculo de p.
                f->plataformaActual                                         // Evalúa esta comparación como parte de la condición actual.
            ];                                                              // Continúa el cálculo de p.


        /* Bordes: nunca se tira al vacío */
        if (f->x < p->x1) {                                                 // Comprueba una condición de la flama antes de continuar.
            f->x = p->x1;                                                   // Actualiza la posición horizontal de la flama.
            f->direccion = 1;                                               // Fija la dirección de la flama hacia la derecha.

            f->estado =                                                     // Cambia el estado actual de la flama.
                FLAMA_RECUPERANDO;                                          // Continúa el cálculo de estado actual de la flama.

            f->escaleraObjetivo = -1;                                   // Escalera hacia la que se está desplazando la flama.

            f->proximaDecision =                                            // Actualiza próxima decisión de IA de la flama.
                ahora +                                                     // Continúa el cálculo de próxima decisión de IA de la flama.
                FIREBALL_RECUPERAR_MS;                                  // Tick a partir del cual la IA puede volver a decidir.
        }                                                                   // Continúa el cálculo de próxima decisión de IA de la flama.

        if (                                                                // Comprueba una condición de la flama antes de continuar.
            f->x + f->w >                                                   // Evalúa esta comparación como parte de la condición actual.
            p->x2                                                           // Evalúa esta comparación como parte de la condición actual.
        ) {                                                                 // Continúa el cálculo de próxima decisión de IA de la flama.
            f->x =                                                          // Actualiza la posición horizontal de la flama.
                p->x2 -                                                     // Evalúa esta comparación como parte de la condición actual.
                f->w;                                                       // Evalúa esta comparación como parte de la condición actual.

            f->direccion =                                                  // Actualiza dirección de movimiento de la flama.
                -1;                                                         // Continúa el cálculo de dirección de movimiento de la flama.

            f->estado =                                                     // Cambia el estado actual de la flama.
                FLAMA_RECUPERANDO;                                          // Continúa el cálculo de estado actual de la flama.

            f->escaleraObjetivo = -1;                                   // Escalera hacia la que se está desplazando la flama.

            f->proximaDecision =                                            // Actualiza próxima decisión de IA de la flama.
                ahora +                                                     // Continúa el cálculo de próxima decisión de IA de la flama.
                FIREBALL_RECUPERAR_MS;                                  // Tick a partir del cual la IA puede volver a decidir.
        }                                                                   // Continúa el cálculo de próxima decisión de IA de la flama.


        /* Seguir inclinación de viga */
        int py =                                                            // Calcula y guarda altura Y de la plataforma.
            ObtenerYPlataforma(                                         // Calcula la altura exacta de la viga inclinada en esta coordenada X.
                p,                                                          // Indica plataforma consultada.
                f->x + f->w / 2                                             // Evalúa esta comparación como parte de la condición actual.
            );                                                              // Continúa el cálculo de altura Y de la plataforma.

        f->y =                                                              // Actualiza la posición vertical de la flama.
            py -                                                            // Continúa el cálculo de posición vertical de la flama.
            f->h;                                                           // Evalúa esta comparación como parte de la condición actual.

        return;                                                             // Termina la función en este punto.
    }                                                                       // Continúa el cálculo de posición vertical de la flama.


    /* ========================================================
     * ESCALERA
     * ======================================================== */
    if (f->estado == FLAMA_ESCALERA) {                                      // Comprueba si se cumplen las condiciones necesarias para continuar.
        if (!pasoMovimiento)                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            return;                                                         // Termina la función en este punto.

        if (                                                                // Comprueba una condición de la flama antes de continuar.
            f->escaleraActual < 0 ||                                        // Comprueba si existe una escalera actual válida.
            f->escaleraActual >=                                            // Evalúa esta comparación como parte de la condición actual.
            (int)NUM_ESCALERAS                                              // Continúa el cálculo de posición vertical de la flama.
        )                                                                   // Continúa el cálculo de posición vertical de la flama.
            return;                                                         // Termina la función en este punto.

        const Escalera *e =                                                 // Calcula y guarda e.
            &escaleras[                                                     // Continúa el cálculo de e.
                f->escaleraActual                                           // Evalúa esta comparación como parte de la condición actual.
            ];                                                              // Continúa el cálculo de e.

        int top = e->y1;                                                    // Calcula y guarda extremo superior.
        int bottom = e->y2;                                                 // Calcula y guarda extremo inferior.

        if (bottom < top) {                                                 // Comprueba si se cumplen las condiciones necesarias para continuar.
            int temp = top;                                                 // Calcula y guarda temp.
            top = bottom;                                                   // Calcula y guarda extremo superior.
            bottom = temp;                                                  // Calcula y guarda extremo inferior.
        }


        /* SUBIR */
        if (f->direccionVertical < 0) {                                     // Comprueba una condición de la flama antes de continuar.
            f->y -=                                                         // Reduce posición vertical de la flama con este valor.
                VELOCIDAD_ESCALERA_FIREBALL;                                // Continúa el cálculo de posición vertical de la flama.

            if (                                                            // Comprueba una condición de la flama antes de continuar.
                f->y + f->h <=                                              // Evalúa esta comparación como parte de la condición actual.
                top + 1
            ) {
                int plataforma =                                            // Calcula y guarda índice de plataforma.
                    f->plataformaDestino;                               // Índice de la plataforma asociada al martillo.

                if (                                                        // Comprueba si se cumplen las condiciones necesarias para continuar.
                    plataforma >= 0 &&                                      // Añade esta condición a la comprobación actual.
                    plataforma <                                            // Evalúa esta comparación como parte de la condición actual.
                    (int)NUM_PLATAFORMAS                                    // Continúa el cálculo de índice de plataforma.
                ) {                                                         // Continúa el cálculo de índice de plataforma.
                    int py =                                                // Calcula y guarda altura Y de la plataforma.
                        ObtenerYPlataforma(                             // Calcula la altura exacta de la viga inclinada en esta coordenada X.
                            &plataformas[plataforma],                       // Indica plataforma consultada.
                            f->x + f->w / 2                                 // Evalúa esta comparación como parte de la condición actual.
                        );                                                  // Continúa el cálculo de altura Y de la plataforma.

                    f->y =                                                  // Actualiza la posición vertical de la flama.
                        py -                                                // Continúa el cálculo de posición vertical de la flama.
                        f->h;                                               // Evalúa esta comparación como parte de la condición actual.

                    f->plataformaActual =                                   // Actualiza plataforma actual de la flama.
                        plataforma;                                     // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.
                }                                                           // Continúa el cálculo de plataforma actual de la flama.

                f->ultimaEscalera =                                         // Actualiza última escalera usada de la flama.
                    f->escaleraActual;                                  // Última escalera utilizada por la flama.

                f->bloqueoEscaleraHasta =                                   // Actualiza fin del bloqueo de escalera de la flama.
                    ahora +                                                 // Continúa el cálculo de fin del bloqueo de escalera de la flama.
                    FIREBALL_BLOQUEO_ESCALERA_MS;                       // Tick hasta el que se bloquea la reutilización de una escalera.

                f->escaleraActual = -1;                                 // Índice de la escalera utilizada actualmente; -1 significa ninguna.
                f->escaleraObjetivo = -1;                               // Escalera hacia la que se está desplazando la flama.
                f->plataformaDestino = -1;                              // Plataforma a la que se quiere llegar mediante una escalera.
                f->direccionVertical = 0;                               // Dirección por escalera: -1 subir, +1 bajar.

                f->estado =                                                 // Cambia el estado actual de la flama.
                    FLAMA_RECUPERANDO;                                      // Continúa el cálculo de estado actual de la flama.

                f->tiempoEstado =                                           // Actualiza inicio del estado actual de la flama.
                    ahora;                                              // Tick en que comenzó el estado actual.

                f->proximaDecision =                                        // Actualiza próxima decisión de IA de la flama.
                    ahora +                                                 // Continúa el cálculo de próxima decisión de IA de la flama.
                    FIREBALL_RECUPERAR_MS;                              // Tick a partir del cual la IA puede volver a decidir.

                return;                                                     // Termina la función en este punto.
            }                                                               // Continúa el cálculo de próxima decisión de IA de la flama.
        }                                                                   // Continúa el cálculo de próxima decisión de IA de la flama.


        /* BAJAR */
        else {                                                          // Ejecuta esta alternativa cuando la condición anterior fue falsa.
            f->y +=                                                         // Aumenta posición vertical de la flama con este valor.
                VELOCIDAD_ESCALERA_FIREBALL;                                // Continúa el cálculo de posición vertical de la flama.

            if (                                                            // Comprueba una condición de la flama antes de continuar.
                f->y + f->h >=                                              // Evalúa esta comparación como parte de la condición actual.
                bottom
            ) {
                int plataforma =                                            // Calcula y guarda índice de plataforma.
                    f->plataformaDestino;                               // Índice de la plataforma asociada al martillo.

                if (                                                        // Comprueba si se cumplen las condiciones necesarias para continuar.
                    plataforma >= 0 &&                                      // Añade esta condición a la comprobación actual.
                    plataforma <                                            // Evalúa esta comparación como parte de la condición actual.
                    (int)NUM_PLATAFORMAS                                    // Continúa el cálculo de índice de plataforma.
                ) {                                                         // Continúa el cálculo de índice de plataforma.
                    int py =                                                // Calcula y guarda altura Y de la plataforma.
                        ObtenerYPlataforma(                             // Calcula la altura exacta de la viga inclinada en esta coordenada X.
                            &plataformas[plataforma],                       // Indica plataforma consultada.
                            f->x + f->w / 2                                 // Evalúa esta comparación como parte de la condición actual.
                        );                                                  // Continúa el cálculo de altura Y de la plataforma.

                    f->y =                                                  // Actualiza la posición vertical de la flama.
                        py -                                                // Continúa el cálculo de posición vertical de la flama.
                        f->h;                                               // Evalúa esta comparación como parte de la condición actual.

                    f->plataformaActual =                                   // Actualiza plataforma actual de la flama.
                        plataforma;                                     // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.
                }                                                           // Continúa el cálculo de plataforma actual de la flama.

                f->ultimaEscalera =                                         // Actualiza última escalera usada de la flama.
                    f->escaleraActual;                                  // Última escalera utilizada por la flama.

                f->bloqueoEscaleraHasta =                                   // Actualiza fin del bloqueo de escalera de la flama.
                    ahora +                                                 // Continúa el cálculo de fin del bloqueo de escalera de la flama.
                    FIREBALL_BLOQUEO_ESCALERA_MS;                       // Tick hasta el que se bloquea la reutilización de una escalera.

                f->escaleraActual = -1;                                 // Índice de la escalera utilizada actualmente; -1 significa ninguna.
                f->escaleraObjetivo = -1;                               // Escalera hacia la que se está desplazando la flama.
                f->plataformaDestino = -1;                              // Plataforma a la que se quiere llegar mediante una escalera.
                f->direccionVertical = 0;                               // Dirección por escalera: -1 subir, +1 bajar.

                f->estado =                                                 // Cambia el estado actual de la flama.
                    FLAMA_RECUPERANDO;                                      // Continúa el cálculo de estado actual de la flama.

                f->tiempoEstado =                                           // Actualiza inicio del estado actual de la flama.
                    ahora;                                              // Tick en que comenzó el estado actual.

                f->proximaDecision =                                        // Actualiza próxima decisión de IA de la flama.
                    ahora +                                                 // Continúa el cálculo de próxima decisión de IA de la flama.
                    FIREBALL_RECUPERAR_MS;                              // Tick a partir del cual la IA puede volver a decidir.

                return;                                                     // Termina la función en este punto.
            }                                                               // Continúa el cálculo de próxima decisión de IA de la flama.
        }                                                                   // Continúa el cálculo de próxima decisión de IA de la flama.
    }                                                                       // Continúa el cálculo de próxima decisión de IA de la flama.
}                                                                           // Continúa el cálculo de próxima decisión de IA de la flama.

void ActualizarFireballs(uint8_t pasoMovimiento)                        // Actualiza todas las flamas activas.
{
    for (int i = 0; i < MAX_FIREBALLS; i++) {                               // Recorre todos los slots de flamas.
        if (fireballs[i].activo) {                                          // Comprueba una condición de la flama antes de continuar.
            ActualizarFireball(                                             // Ejecuta ActualizarFireball().
                &fireballs[i],                                              // Indica flama que se actualiza.
                pasoMovimiento                                              // Comprueba si en este ciclo corresponde aplicar un paso de movimiento.
            );                                                              // Finaliza la llamada a ActualizarFireball().
        }
    }
}

void ComprobarColisionesFireballs(void)                                 // Comprueba las cajas de cada flama contra ambos jugadores y aplica daño si colisionan.
{
    AABB cajaJ1 = {                                                         // Calcula y guarda cajaJ1.
        jugador1.x,                                                         // Continúa el cálculo de cajaJ1.
        jugador1.y,                                                         // Continúa el cálculo de cajaJ1.
        jugador1.w,                                                         // Continúa el cálculo de cajaJ1.
        jugador1.h                                                          // Continúa el cálculo de cajaJ1.
    };                                                                      // Continúa el cálculo de cajaJ1.

    AABB cajaJ2 = {                                                         // Calcula y guarda cajaJ2.
        jugador2.x,                                                         // Continúa el cálculo de cajaJ2.
        jugador2.y,                                                         // Continúa el cálculo de cajaJ2.
        jugador2.w,                                                         // Continúa el cálculo de cajaJ2.
        jugador2.h                                                          // Continúa el cálculo de cajaJ2.
    };                                                                      // Continúa el cálculo de cajaJ2.

    for (int i = 0; i < MAX_FIREBALLS; i++) {                               // Recorre todos los slots de flamas.
        if (                                                                // Comprueba una condición de la flama antes de continuar.
            !fireballs[i].activo ||                                         // Comprueba que la flama esté activo.
            fireballs[i].estado == FLAMA_NACIENDO                           // Comprueba si el estado es FLAMA_NACIENDO: Flama creada pero todavía oculta dentro del barril de aceite.
        )
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.

        AABB cajaFlama = {                                                  // Calcula y guarda cajaFlama.
            fireballs[i].x,                                                 // Accede a la posición correspondiente del arreglo fireballs.
            fireballs[i].y,                                                 // Accede a la posición correspondiente del arreglo fireballs.
            fireballs[i].w,                                                 // Accede a la posición correspondiente del arreglo fireballs.
            fireballs[i].h                                                  // Accede a la posición correspondiente del arreglo fireballs.
        };                                                                  // Continúa el cálculo de cajaFlama.

        if (ColisionAABB(cajaJ1, cajaFlama)) {                              // Comprueba una condición del jugador 1 antes de continuar.
            MarcarGolpeJugador(                                             // Resta una vida e inicia la animación y protección tras el golpe.
                &jugador1,                                                  // Indica jugador que recibe el golpe.
                &golpeJ1Hasta                                               // Indica temporizador visual del golpe.
            );                                                              // Finaliza la llamada a MarcarGolpeJugador().
        }

        if (ColisionAABB(cajaJ2, cajaFlama)) {                              // Comprueba una condición del jugador 2 antes de continuar.
            MarcarGolpeJugador(                                             // Resta una vida e inicia la animación y protección tras el golpe.
                &jugador2,                                                  // Indica jugador que recibe el golpe.
                &golpeJ2Hasta                                               // Indica temporizador visual del golpe.
            );                                                              // Finaliza la llamada a MarcarGolpeJugador().
        }
    }
}


/* ============================================================
 * BARRIL AZUL + ACEITE
 *
 * Se procesa la llegada de un barril azul al aceite.
 * El aceite se enciende, se crea una nueva flama y el barril azul se elimina.
 * ============================================================ */

void BarrilLlegaAceite(Barril *b)                                       // Al llegar un barril azul al aceite, enciende el aceite, crea una flama y elimina el barril.
{
    if (                                                                    // Comprueba una condición del barril antes de continuar.
        b->tipo ==                                                          // Evalúa esta comparación como parte de la condición actual.
        BARRIL_AZUL                                                     // Barril especial capaz de activar el aceite y generar una flama.
    ) {
        /*
         * El primer azul enciende permanentemente el barril
         * de aceite. Los siguientes azules mantienen el estado
         * encendido y generan nuevas flamas.
         */
        EncenderAceite();                                               // Enciende el aceite si todavía estaba apagado.

        CrearFireball();                                                // Crea una flama en el aceite y prepara su salida.
    }

    DesactivarBarril(b);                                                // Desactiva el barril y libera su slot.
}


/* ============================================================
 * BARRILES - MÁQUINA DE ESTADOS
 *
 * Se ejecuta la física de cada barril según su estado actual.
 * El bloque controla rodamiento, caída, rebote, escaleras, lanzamientos directos y llegada al aceite.
 * ============================================================ */

void ActualizarBarril(                                                  // Actualiza la máquina de estados y la física del barril.
    Barril *b,
    uint8_t pasoMovimiento                                                  // Recibe una bandera que indica si en este ciclo corresponde aplicar movimiento.
)
{
    if (!b->activo)                                                         // Comprueba una condición del barril antes de continuar.
        return;                                                             // Termina la función en este punto.

    if (b->estado == BARRIL_A_ACEITE) {                                     // Comprueba si se cumplen las condiciones necesarias para continuar.
        if (!pasoMovimiento) return;                                        // Comprueba una condición del barril antes de continuar.
        const int destinoX = ACEITE_X + (ACEITE_W - BARRIL_W) / 2;          // Calcula y guarda destinoX.
        const int pasos = BARRIL_SALIDA_X - destinoX;                       // Calcula y guarda pasos.
        if (b->vueloAceitePaso < pasos) b->vueloAceitePaso++;               // Comprueba una condición del barril antes de continuar.
        b->x = BARRIL_SALIDA_X - b->vueloAceitePaso;                        // Actualiza la posición horizontal del barril.
        b->y = BARRIL_SALIDA_Y +                                            // Actualiza la posición vertical del barril.
            ((AceiteY() - BARRIL_H - BARRIL_SALIDA_Y) *                     // Continúa el cálculo de posición vertical del barril.
             b->vueloAceitePaso) / pasos;                                   // Evalúa esta comparación como parte de la condición actual.
        if (b->vueloAceitePaso >= pasos) BarrilLlegaAceite(b);          // Enciende el aceite, crea una flama y elimina el barril azul.
        return;                                                             // Termina la función en este punto.
    }                                                                       // Continúa el cálculo de posición vertical del barril.


    /* RODANDO */
    if (b->estado == BARRIL_RODANDO) {                                      // Comprueba una condición del barril antes de continuar.
        if (                                                                // Comprueba una condición del barril antes de continuar.
            b->plataformaActual < 0 ||                                      // Comprueba si existe una plataforma actual válida.
            b->plataformaActual >=                                          // Evalúa esta comparación como parte de la condición actual.
            (int)NUM_PLATAFORMAS                                            // Continúa el cálculo de posición vertical del barril.
        ) {                                                                 // Continúa el cálculo de posición vertical del barril.
            DesactivarBarril(b);                                        // Desactiva el barril y libera su slot.
            return;                                                         // Termina la función en este punto.
        }                                                                   // Continúa el cálculo de posición vertical del barril.

        if (!pasoMovimiento)                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            return;                                                         // Termina la función en este punto.

        const Plataforma *p =                                               // Calcula y guarda p.
            &plataformas[                                                   // Continúa el cálculo de p.
                b->plataformaActual                                         // Evalúa esta comparación como parte de la condición actual.
            ];                                                              // Continúa el cálculo de p.

        int escalera =                                                      // Calcula y guarda índice de escalera.
            BuscarEscaleraParaBarril(b);                                    // Continúa el cálculo de índice de escalera.

        if (escalera >= 0) {                                                // Comprueba una condición del barril antes de continuar.
            if (                                                            // Comprueba una condición del barril antes de continuar.
                b->ultimaEscaleraEvaluada !=                                // Evalúa esta comparación como parte de la condición actual.
                escalera
            ) {
                b->ultimaEscaleraEvaluada =                                 // Actualiza última escalera evaluada del barril.
                    escalera;                                           // Última escalera sobre la que el barril ya tomó una decisión, para no repetirla en cada pixel.

                if (                                                        // Comprueba si se cumplen las condiciones necesarias para continuar.
                    DebeBajarEscalera(                                      // Continúa el cálculo de última escalera evaluada del barril.
                        b,                                                  // Continúa el cálculo de última escalera evaluada del barril.
                        escalera                                            // Continúa el cálculo de última escalera evaluada del barril.
                    )                                                       // Continúa el cálculo de última escalera evaluada del barril.
                ) {                                                         // Continúa el cálculo de última escalera evaluada del barril.
                    IniciarBajadaEscalera(                                  // Continúa el cálculo de última escalera evaluada del barril.
                        b,                                                  // Continúa el cálculo de última escalera evaluada del barril.
                        escalera                                            // Continúa el cálculo de última escalera evaluada del barril.
                    );                                                      // Continúa el cálculo de última escalera evaluada del barril.
                    return;                                                 // Termina la función en este punto.
                }
            }
        } else {
            b->ultimaEscaleraEvaluada = -1;                             // Última escalera sobre la que el barril ya tomó una decisión, para no repetirla en cada pixel.
        }

        b->x +=                                                             // Aumenta posición horizontal del barril con este valor.
            VELOCIDAD_BARRIL *                                              // Continúa el cálculo de posición horizontal del barril.
            b->direccion;                                                   // Evalúa esta comparación como parte de la condición actual.

        int py =                                                            // Calcula y guarda altura Y de la plataforma.
            ObtenerYPlataforma(                                         // Calcula la altura exacta de la viga inclinada en esta coordenada X.
                p,                                                          // Indica plataforma consultada.
                b->x + b->w / 2                                             // Evalúa esta comparación como parte de la condición actual.
            );                                                              // Continúa el cálculo de altura Y de la plataforma.

        b->y =                                                              // Actualiza la posición vertical del barril.
            py -                                                            // Continúa el cálculo de posición vertical del barril.
            b->h;                                                           // Evalúa esta comparación como parte de la condición actual.


        /* Última plataforma */
        if (                                                                // Comprueba una condición del barril antes de continuar.
            b->plataformaActual ==                                          // Evalúa esta comparación como parte de la condición actual.
            (int)NUM_PLATAFORMAS - 1                                        // Continúa el cálculo de posición vertical del barril.
        ) {                                                                 // Continúa el cálculo de posición vertical del barril.
            if (                                                            // Comprueba una condición del barril antes de continuar.
                b->tipo == BARRIL_AZUL &&                               // Tipo lógico del barril: marrón o azul.
                BarrilTocaAceite(b)                                         // Continúa el cálculo de posición vertical del barril.
            ) {                                                             // Continúa el cálculo de posición vertical del barril.
                BarrilLlegaAceite(b);                                   // Enciende el aceite, crea una flama y elimina el barril azul.
                return;                                                     // Termina la función en este punto.
            }                                                               // Continúa el cálculo de posición vertical del barril.

            if (                                                            // Comprueba una condición del barril antes de continuar.
                b->direccion < 0 &&                                         // Añade esta condición a la comprobación actual.
                b->x <= p->x1                                               // Evalúa esta comparación como parte de la condición actual.
            ) {                                                             // Continúa el cálculo de posición vertical del barril.
                DesactivarBarril(b);                                    // Desactiva el barril y libera su slot.
                return;                                                     // Termina la función en este punto.
            }                                                               // Continúa el cálculo de posición vertical del barril.
        }                                                                   // Continúa el cálculo de posición vertical del barril.


        if (                                                                // Comprueba una condición del barril antes de continuar.
            b->direccion > 0 &&                                             // Añade esta condición a la comprobación actual.
            b->x + b->w >= p->x2                                            // Evalúa esta comparación como parte de la condición actual.
        ) {                                                                 // Continúa el cálculo de posición vertical del barril.
            b->x = p->x2 + 1;                                               // Actualiza la posición horizontal del barril.
            b->estado = BARRIL_CAYENDO;                                     // Cambia el estado actual del barril.
            b->vy = 0;                                                  // Velocidad vertical actual.
            return;                                                         // Termina la función en este punto.
        }

        if (                                                                // Comprueba una condición del barril antes de continuar.
            b->direccion < 0 &&                                             // Añade esta condición a la comprobación actual.
            b->x <= p->x1                                                   // Evalúa esta comparación como parte de la condición actual.
        ) {
            b->x =                                                          // Actualiza la posición horizontal del barril.
                p->x1 -                                                     // Evalúa esta comparación como parte de la condición actual.
                b->w -                                                      // Evalúa esta comparación como parte de la condición actual.
                1;                                                          // Continúa el cálculo de posición horizontal del barril.

            b->estado =                                                     // Cambia el estado actual del barril.
                BARRIL_CAYENDO;                                             // Continúa el cálculo de estado actual del barril.

            b->vy = 0;                                                  // Velocidad vertical actual.
            return;                                                         // Termina la función en este punto.
        }

        return;                                                             // Termina la función en este punto.
    }


    /* CAYENDO */
    if (b->estado == BARRIL_CAYENDO) {                                      // Comprueba una condición del barril antes de continuar.
        int oldBottom =                                                     // Calcula y guarda posición anterior de los pies.
            b->y + b->h;                                                    // Evalúa esta comparación como parte de la condición actual.

        b->vy +=                                                            // Aumenta velocidad vertical del barril con este valor.
            GRAVEDAD_BARRIL;                                                // Continúa el cálculo de velocidad vertical del barril.

        if (b->vy > MAX_CAIDA_BARRIL)                                       // Comprueba una condición del barril antes de continuar.
            b->vy = MAX_CAIDA_BARRIL;                                   // Velocidad vertical actual.

        int nuevaY =                                                        // Calcula y guarda nueva posición Y.
            b->y + b->vy;                                                   // Evalúa esta comparación como parte de la condición actual.

        int newBottom =                                                     // Calcula y guarda nueva posición de los pies.
            nuevaY + b->h;                                                  // Evalúa esta comparación como parte de la condición actual.

        int plataforma =                                                    // Calcula y guarda índice de plataforma.
            BuscarPlataformaBarrilCaida(                                    // Continúa el cálculo de índice de plataforma.
                b,                                                          // Continúa el cálculo de índice de plataforma.
                oldBottom,                                                  // Continúa el cálculo de índice de plataforma.
                newBottom                                                   // Continúa el cálculo de índice de plataforma.
            );                                                          // Índice de la plataforma asociada al martillo.

        if (plataforma >= 0) {                                              // Comprueba si se cumplen las condiciones necesarias para continuar.
            IniciarReboteBarril(                                            // Continúa el cálculo de índice de plataforma.
                b,                                                          // Continúa el cálculo de índice de plataforma.
                plataforma                                                  // Continúa el cálculo de índice de plataforma.
            );                                                              // Continúa el cálculo de índice de plataforma.
            return;                                                         // Termina la función en este punto.
        }

        b->y = nuevaY;                                                      // Actualiza la posición vertical del barril.

        if (b->y > SCREEN_H)                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            DesactivarBarril(b);                                        // Desactiva el barril y libera su slot.

        return;                                                             // Termina la función en este punto.
    }


    /* REBOTANDO */
    if (b->estado == BARRIL_REBOTANDO) {                                    // Comprueba una condición del barril antes de continuar.
        if (b->plataformaActual < 0) {                                      // Comprueba si se cumplen las condiciones necesarias para continuar.
            DesactivarBarril(b);                                        // Desactiva el barril y libera su slot.
            return;                                                         // Termina la función en este punto.
        }

        const Plataforma *p =                                               // Calcula y guarda p.
            &plataformas[                                                   // Continúa el cálculo de p.
                b->plataformaActual                                         // Evalúa esta comparación como parte de la condición actual.
            ];                                                              // Continúa el cálculo de p.

        int py =                                                            // Calcula y guarda altura Y de la plataforma.
            ObtenerYPlataforma(                                         // Calcula la altura exacta de la viga inclinada en esta coordenada X.
                p,                                                          // Indica plataforma consultada.
                b->x + b->w / 2                                             // Evalúa esta comparación como parte de la condición actual.
            );                                                              // Continúa el cálculo de altura Y de la plataforma.

        b->vy +=                                                            // Aumenta velocidad vertical del barril con este valor.
            GRAVEDAD_BARRIL;                                                // Continúa el cálculo de velocidad vertical del barril.

        int nuevaY =                                                        // Calcula y guarda nueva posición Y.
            b->y +                                                          // Evalúa esta comparación como parte de la condición actual.
            b->vy;                                                          // Evalúa esta comparación como parte de la condición actual.

        if (                                                                // Comprueba una condición del barril antes de continuar.
            b->vy >= 0 &&                                                   // Añade esta condición a la comprobación actual.
            nuevaY + b->h >= py                                             // Evalúa esta comparación como parte de la condición actual.
        ) {                                                                 // Continúa el cálculo de nueva posición Y.
            b->y = py - b->h;                                               // Actualiza la posición vertical del barril.
            b->vy = 0;                                                  // Velocidad vertical actual.
            b->estado = BARRIL_RODANDO;                                     // Cambia el estado actual del barril.
            return;                                                         // Termina la función en este punto.
        }

        b->y = nuevaY;                                                      // Actualiza la posición vertical del barril.
        return;                                                             // Termina la función en este punto.
    }


    /* ESCALERA */
    if (b->estado == BARRIL_ESCALERA) {                                     // Comprueba una condición del barril antes de continuar.
        if (                                                                // Comprueba una condición del barril antes de continuar.
            b->escaleraActual < 0 ||                                        // Comprueba si existe una escalera actual válida.
            b->escaleraActual >=                                            // Evalúa esta comparación como parte de la condición actual.
            (int)NUM_ESCALERAS
        ) {
            DesactivarBarril(b);                                        // Desactiva el barril y libera su slot.
            return;                                                         // Termina la función en este punto.
        }

        if (!pasoMovimiento)                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            return;                                                         // Termina la función en este punto.

        const Escalera *e =                                                 // Calcula y guarda e.
            &escaleras[                                                     // Continúa el cálculo de e.
                b->escaleraActual                                           // Evalúa esta comparación como parte de la condición actual.
            ];                                                              // Continúa el cálculo de e.

        int top = e->y1;                                                    // Calcula y guarda extremo superior.
        int bottom = e->y2;                                                 // Calcula y guarda extremo inferior.

        if (bottom < top) {                                                 // Comprueba si se cumplen las condiciones necesarias para continuar.
            int temp = top;                                                 // Calcula y guarda temp.
            top = bottom;                                                   // Calcula y guarda extremo superior.
            bottom = temp;                                                  // Calcula y guarda extremo inferior.
        }

        b->y +=                                                             // Aumenta posición vertical del barril con este valor.
            VELOCIDAD_ESCALERA_BARRIL;                                      // Continúa el cálculo de posición vertical del barril.

        if (b->y + b->h >= bottom) {                                        // Comprueba una condición del barril antes de continuar.
            int destino =                                                   // Calcula y guarda destino.
                BuscarPlataformaDestinoEscalera(                            // Continúa el cálculo de destino.
                    b->escaleraActual                                       // Evalúa esta comparación como parte de la condición actual.
                );                                                          // Continúa el cálculo de destino.

            if (destino >= 0) {                                             // Comprueba una condición del barril antes de continuar.
                b->plataformaActual = destino;                          // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.

                int py =                                                    // Calcula y guarda altura Y de la plataforma.
                    ObtenerYPlataforma(                                 // Calcula la altura exacta de la viga inclinada en esta coordenada X.
                        &plataformas[destino],                              // Indica plataforma consultada.
                        b->x + b->w / 2                                     // Evalúa esta comparación como parte de la condición actual.
                    );                                                      // Continúa el cálculo de altura Y de la plataforma.

                b->y = py - b->h;                                           // Actualiza la posición vertical del barril.

                b->direccion =                                              // Actualiza dirección de movimiento del barril.
                    DireccionBarrilPorPlataforma(                           // Continúa el cálculo de dirección de movimiento del barril.
                        destino                                             // Continúa el cálculo de dirección de movimiento del barril.
                    );                                                      // Continúa el cálculo de dirección de movimiento del barril.

                b->estado = BARRIL_RODANDO;                                 // Cambia el estado actual del barril.
                b->escaleraActual = -1;                                 // Índice de la escalera utilizada actualmente; -1 significa ninguna.
                b->ultimaEscaleraEvaluada = -1;                         // Última escalera sobre la que el barril ya tomó una decisión, para no repetirla en cada pixel.
            } else {
                b->estado = BARRIL_CAYENDO;                                 // Cambia el estado actual del barril.
                b->escaleraActual = -1;                                 // Índice de la escalera utilizada actualmente; -1 significa ninguna.
                b->vy = 0;                                              // Velocidad vertical actual.
            }
        }

        return;                                                             // Termina la función en este punto.
    }


    /* DIRECTO */
    if (b->estado == BARRIL_DIRECTO) {                                      // Comprueba una condición del barril antes de continuar.
        int oldBottom =                                                     // Calcula y guarda posición anterior de los pies.
            b->y + b->h;                                                    // Evalúa esta comparación como parte de la condición actual.

        if (pasoMovimiento) {                                               // Comprueba una condición del barril antes de continuar.
            b->x +=                                                         // Aumenta posición horizontal del barril con este valor.
                VELOCIDAD_DIRECTA_X *                                       // Continúa el cálculo de posición horizontal del barril.
                b->direccion;                                               // Evalúa esta comparación como parte de la condición actual.
        }                                                                   // Continúa el cálculo de posición horizontal del barril.

        if (b->x < 0) {                                                     // Comprueba una condición del barril antes de continuar.
            b->x = 0;                                                       // Actualiza la posición horizontal del barril.
            b->direccion = 1;                                               // Fija la dirección del barril hacia la derecha.
        }

        if (b->x > SCREEN_W - b->w) {                                       // Comprueba una condición del barril antes de continuar.
            b->x = SCREEN_W - b->w;                                         // Actualiza la posición horizontal del barril.
            b->direccion = -1;                                              // Fija la dirección del barril hacia la izquierda.
        }

        b->vy +=                                                            // Aumenta velocidad vertical del barril con este valor.
            GRAVEDAD_BARRIL;                                                // Continúa el cálculo de velocidad vertical del barril.

        if (b->vy > MAX_CAIDA_DIRECTO)                                      // Comprueba una condición del barril antes de continuar.
            b->vy = MAX_CAIDA_DIRECTO;                                  // Velocidad vertical actual.

        int nuevaY =                                                        // Calcula y guarda nueva posición Y.
            b->y +                                                          // Evalúa esta comparación como parte de la condición actual.
            b->vy;                                                          // Evalúa esta comparación como parte de la condición actual.

        int newBottom =                                                     // Calcula y guarda nueva posición de los pies.
            nuevaY +                                                        // Continúa el cálculo de nueva posición de los pies.
            b->h;                                                           // Evalúa esta comparación como parte de la condición actual.

        int plataforma =                                                    // Calcula y guarda índice de plataforma.
            BuscarPlataformaBarrilCaida(                                    // Continúa el cálculo de índice de plataforma.
                b,                                                          // Continúa el cálculo de índice de plataforma.
                oldBottom,                                                  // Continúa el cálculo de índice de plataforma.
                newBottom                                                   // Continúa el cálculo de índice de plataforma.
            );                                                          // Índice de la plataforma asociada al martillo.

        if (plataforma >= 0) {                                              // Comprueba una condición del barril antes de continuar.
            if (b->contactosIgnorar > 0) {                                  // Comprueba una condición del barril antes de continuar.
                b->contactosIgnorar--;                                      // Reduce plataformas que puede ignorar del barril en una unidad.
            } else {                                                        // Continúa el cálculo de índice de plataforma.
                IniciarReboteBarril(                                        // Continúa el cálculo de índice de plataforma.
                    b,                                                      // Continúa el cálculo de índice de plataforma.
                    plataforma                                              // Continúa el cálculo de índice de plataforma.
                );                                                          // Continúa el cálculo de índice de plataforma.
                return;                                                     // Termina la función en este punto.
            }
        }

        b->y = nuevaY;                                                      // Actualiza la posición vertical del barril.

        if (b->y > SCREEN_H)                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            DesactivarBarril(b);                                        // Desactiva el barril y libera su slot.

        return;                                                             // Termina la función en este punto.
    }
}

void ActualizarBarriles(uint8_t pasoMovimiento)                         // Actualiza todos los barriles activos.
{
    for (int i = 0; i < MAX_BARRILES; i++) {                                // Recorre todos los slots de barriles.
        if (barriles[i].activo) {                                           // Comprueba una condición del barril antes de continuar.
            ActualizarBarril(                                               // Ejecuta ActualizarBarril().
                &barriles[i],                                               // Indica barril que se actualiza.
                pasoMovimiento                                              // Comprueba si en este ciclo corresponde aplicar un paso de movimiento.
            );                                                              // Finaliza la llamada a ActualizarBarril().
        }
    }
}

/* Ultimos tres pixeles del cruce tras ganar +100 por ese barril.
 * Evita que un borde de las cajas quite vida justo al completar el salto.
 * El contacto antes de superar el barril y tras aterrizar sigue activo. */
static uint8_t CruzandoBordeBarril(const Jugador *j, const Barril *b,   // Da un pequeño margen después de acreditar un salto.
                                  uint8_t indiceJugador)
{
    if (!b->saltado[indiceJugador] || j->enSuelo || j->vy < 0)              // Comprueba una condición del barril antes de continuar.
        return 0U;                                                          // Devuelve 0 para indicar que no se encontró o no se cumple la condición buscada.
    return (j->x >= b->x + b->w - 3 && j->x < b->x + b->w) ||               // Devuelve una de las opciones según la condición que se evalúa a continuación.
           (j->x + j->w > b->x && j->x + j->w <= b->x + 3);                 // Evalúa esta comparación como parte de la condición actual.
}

void ComprobarColisionesBarriles(void)                                  // Gestiona daño por contacto con barriles y recompensa de +100 cuando un jugador los salta correctamente.
{
    AABB cajaJ1 = {                                                         // Calcula y guarda cajaJ1.
        jugador1.x,                                                         // Continúa el cálculo de cajaJ1.
        jugador1.y,                                                         // Continúa el cálculo de cajaJ1.
        jugador1.w,                                                         // Continúa el cálculo de cajaJ1.
        jugador1.h                                                          // Continúa el cálculo de cajaJ1.
    };                                                                      // Continúa el cálculo de cajaJ1.

    AABB cajaJ2 = {                                                         // Calcula y guarda cajaJ2.
        jugador2.x,                                                         // Continúa el cálculo de cajaJ2.
        jugador2.y,                                                         // Continúa el cálculo de cajaJ2.
        jugador2.w,                                                         // Continúa el cálculo de cajaJ2.
        jugador2.h                                                          // Continúa el cálculo de cajaJ2.
    };                                                                      // Continúa el cálculo de cajaJ2.

    for (int i = 0; i < MAX_BARRILES; i++) {                                // Recorre todos los slots de barriles.
        if (!barriles[i].activo)                                            // Comprueba si se cumplen las condiciones necesarias para continuar.
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.

        AABB cajaBarril = {                                                 // Calcula y guarda cajaBarril.
            barriles[i].x,                                                  // Accede a la posición correspondiente del arreglo barriles.
            barriles[i].y,                                                  // Accede a la posición correspondiente del arreglo barriles.
            barriles[i].w,                                                  // Accede a la posición correspondiente del arreglo barriles.
            barriles[i].h                                                   // Accede a la posición correspondiente del arreglo barriles.
        };                                                                  // Continúa el cálculo de cajaBarril.

        if (ColisionAABB(cajaJ1, cajaBarril) &&                             // Comprueba una condición del jugador 1 antes de continuar.
            !(!jugador1.enSuelo &&                                          // Comprueba si el jugador está apoyado sobre una plataforma.
              jugador1.y + jugador1.h <= barriles[i].y + 2) &&              // Añade esta condición a la comprobación actual.
            !CruzandoBordeBarril(&jugador1, &barriles[i], 0U)) {            // Accede a la posición correspondiente del arreglo barriles.
            MarcarGolpeJugador(                                             // Resta una vida e inicia la animación y protección tras el golpe.
                &jugador1,                                                  // Indica jugador que recibe el golpe.
                &golpeJ1Hasta                                               // Indica temporizador visual del golpe.
            );                                                              // Finaliza la llamada a MarcarGolpeJugador().
        }

        if (ColisionAABB(cajaJ2, cajaBarril) &&                             // Comprueba una condición del jugador 2 antes de continuar.
            !(!jugador2.enSuelo &&                                          // Comprueba si el jugador está apoyado sobre una plataforma.
              jugador2.y + jugador2.h <= barriles[i].y + 2) &&              // Añade esta condición a la comprobación actual.
            !CruzandoBordeBarril(&jugador2, &barriles[i], 1U)) {            // Accede a la posición correspondiente del arreglo barriles.
            MarcarGolpeJugador(                                             // Resta una vida e inicia la animación y protección tras el golpe.
                &jugador2,                                                  // Indica jugador que recibe el golpe.
                &golpeJ2Hasta                                               // Indica temporizador visual del golpe.
            );                                                              // Finaliza la llamada a MarcarGolpeJugador().
        }

        Jugador *js[2] = { &jugador1, &jugador2 };                          // Calcula y guarda js.
        for (int p = 0; p < 2; p++) {                                       // Recorre todas las posiciones necesarias de este bloque.
            Jugador *j = js[p];                                             // Calcula y guarda j.
            int hueco = barriles[i].y - (j->y + j->h);                      // Calcula y guarda hueco.
            if (!j->vidas || j->muriendo || j->enSuelo || j->escaleraActual >= 0 || // Comprueba una condición del barril antes de continuar.
                barriles[i].saltado[p] || hueco < 1 || hueco > 20 ||        // Añade esta condición a la comprobación actual.
                j->plataformaActual != barriles[i].plataformaActual ||      // Añade esta condición a la comprobación actual.
                j->x >= barriles[i].x + barriles[i].w ||                    // Añade esta condición a la comprobación actual.
                j->x + j->w <= barriles[i].x) continue;                     // Evalúa esta comparación como parte de la condición actual.

            barriles[i].saltado[p] = 1;                                     // Activa registro de salto puntuado del barril.
            j->puntos += PUNTOS_SALTO_BARRIL;                               // Aumenta puntuación con este valor.
            j->letreroVisible = 1;                                      // Marca si debe dibujarse el popup de puntos.
            j->letreroValor = 100U;                                     // Valor mostrado por el popup: 100, 200 o 300.
            j->letreroFijo = 0U;                                        // Define si el popup queda fijo en el punto de impacto.
            j->letreroHasta = HAL_GetTick() + PUNTOS_POPUP_MS;          // Define cuándo desaparece el popup de puntos.
            hudSucio = 1;                                                   // Activa hudSucio.
            Audio_Enviar(0x02U);                                        // Reproduce el sonido de puntos/enemigo destruido.
        }
    }
}


/* ============================================================
 * DONKEY KONG - LANZAMIENTO DE BARRILES
 *
 * Se decide cuándo DK inicia un lanzamiento, qué tipo de barril usa y cuándo lo libera.
 * El primer barril azul activa el aceite; los siguientes lanzamientos siguen las reglas de la partida.
 * ============================================================ */

void SeleccionarSiguienteBarrilDK(void)                                 // Decide el tipo del próximo barril de DK.
{
    /* Primero siempre azul */
    if (!primerAzulLanzado) {                                               // Comprueba si se cumplen las condiciones necesarias para continuar.
        dkTipoPendiente = BARRIL_AZUL;                                      // Actualiza dkTipoPendiente.
        dkDirectoPendiente = 1;                                             // Activa dkDirectoPendiente.
        dkContactosPendientes = 5;                                          // Actualiza dkContactosPendientes.
        primerAzulLanzado = 1;                                              // Activa primerAzulLanzado.
        return;                                                             // Termina la función en este punto.
    }

    uint8_t permitirAzul =                                                  // Calcula y guarda permitirAzul.
        (fireballsActivos < MAX_FIREBALLS);                                 // Evalúa esta comparación como parte de la condición actual.

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        permitirAzul &&                                                     // Añade esta condición a la comprobación actual.
        RNG_Porcentaje(15)                                                  // Continúa el cálculo de permitirAzul.
    ) {                                                                     // Continúa el cálculo de permitirAzul.
        dkTipoPendiente = BARRIL_AZUL;                                      // Actualiza dkTipoPendiente.
        dkDirectoPendiente = 0;                                             // Reinicia dkDirectoPendiente a cero.
        dkContactosPendientes = 0;                                          // Reinicia dkContactosPendientes a cero.
    } else {
        dkTipoPendiente = BARRIL_MARRON;                                    // Actualiza dkTipoPendiente.
        dkDirectoPendiente = RNG_Porcentaje(6);                             // Actualiza dkDirectoPendiente.

        if (dkDirectoPendiente) {                                           // Comprueba si se cumplen las condiciones necesarias para continuar.
            dkContactosPendientes =                                         // Actualiza dkContactosPendientes.
                2 +                                                         // Continúa el cálculo de dkContactosPendientes.
                (RNG_Siguiente() % 3U);                                     // Continúa el cálculo de dkContactosPendientes.
        } else {
            dkContactosPendientes = 0;                                      // Reinicia dkContactosPendientes a cero.
        }
    }
}

uint32_t CalcularIntervaloLanzamiento(void)                             // Calcula un intervalo pseudoaleatorio entre lanzamientos de DK según la dificultad.
{
    uint32_t base =                                                         // Calcula y guarda base.
        3700U -                                                             // Continúa el cálculo de base.
        ((DIFICULTAD_BARRILES - 1U) * 200U);                                // Continúa el cálculo de base.

    return                                                                  // Termina la función en este punto.
        base +
        (RNG_Siguiente() % 700U);                                       // Obtiene un nuevo valor pseudoaleatorio para introducir variación en la partida.
}

void IniciarLanzamientoDK(void)                                         // Inicia la animación de lanzamiento si DK está libre y no se superó el máximo de barriles simultáneos.
{
    if (estadoDK != DK_IDLE)                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
        return;                                                             // Termina la función en este punto.

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        ContarBarrilesActivos() >=                                          // Evalúa esta comparación como parte de la condición actual.
        MAX_BARRILES_SIMULTANEOS
    ) {
        dkProximoLanzamiento =                                              // Actualiza dkProximoLanzamiento.
            HAL_GetTick() + 500;                                            // Continúa el cálculo de dkProximoLanzamiento.
        return;                                                             // Termina la función en este punto.
    }

    SeleccionarSiguienteBarrilDK();                                     // Decide el tipo del próximo barril de DK.

    estadoDK = DK_LANZANDO;                                                 // Actualiza estadoDK.
    dkFrame = DK_FRAME_IDLE;                                                // Actualiza dkFrame.
    dkBarrilLiberado = 0;                                                   // Reinicia dkBarrilLiberado a cero.
    dkUltimoCambio = HAL_GetTick();                                     // Marca el inicio del frame actual de DK.
}

void ActualizarDK(void)                                                 // Avanza la animación de DK, libera el barril en el frame definido y programa el siguiente lanzamiento.
{
    uint32_t ahora =                                                        // Calcula y guarda tick actual.
        HAL_GetTick();                                                      // Continúa el cálculo de tick actual.

    if (estadoDK == DK_IDLE) {                                              // Comprueba si se cumplen las condiciones necesarias para continuar.
        if (                                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            (int32_t)(
                ahora -
                dkProximoLanzamiento
            ) >= 0                                                          // Evalúa esta comparación como parte de la condición actual.
        ) {
            IniciarLanzamientoDK();                                     // Inicia la animación de lanzamiento si DK está libre y no se superó el máximo de barriles simultáneos.
        }

        return;                                                             // Termina la función en este punto.
    }

    if (estadoDK == DK_LANZANDO) {                                          // Comprueba si se cumplen las condiciones necesarias para continuar.
        if (                                                                // Comprueba una condición del DK antes de continuar.
            ahora -
            dkUltimoCambio <                                                // Evalúa esta comparación como parte de la condición actual.
            DK_TIEMPO_FRAME
        )
            return;                                                         // Termina la función en este punto.

        dkUltimoCambio =                                                    // Actualiza dkUltimoCambio.
            ahora;                                                          // Continúa el cálculo de dkUltimoCambio.

        dkFrame++;                                                          // Incrementa dkFrame en una unidad.

        if (                                                                // Comprueba una condición del DK antes de continuar.
            dkFrame ==                                                      // Evalúa esta comparación como parte de la condición actual.
            DK_FRAME_LIBERA_BARRIL &&                                       // Añade esta condición a la comprobación actual.
            !dkBarrilLiberado
        ) {
            dkBarrilLiberado = 1;                                           // Activa dkBarrilLiberado.

            CrearBarril(                                                    // Ejecuta CrearBarril().
                dkTipoPendiente,                                            // Indica tipo de barril.
                dkDirectoPendiente,                                         // Indica si usa trayectoria directa.
                dkContactosPendientes                                       // Indica cantidad de plataformas que puede ignorar.
            );                                                              // Finaliza la llamada a CrearBarril().

            if (!primerBarrilLiberado) {                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
                primerBarrilLiberado = 1;                                   // Activa primerBarrilLiberado.
                permitirMovimientoJugadores = 1;                            // Activa permitirMovimientoJugadores.

                jugador1.direccion = 'N';                                   // Actualiza dirección de movimiento del jugador 1.
                jugador2.direccion = 'N';                                   // Actualiza dirección de movimiento del jugador 2.

                jugador1.ultimoComandoRX = 'N';                         // Último comando recibido; ayuda a detectar eventos y evitar repeticiones.
                jugador2.ultimoComandoRX = 'N';                         // Último comando recibido; ayuda a detectar eventos y evitar repeticiones.

                jugador1.saltoSolicitado = 0;                           // Bandera que indica que hay un salto pendiente de ejecutar.
                jugador2.saltoSolicitado = 0;                           // Bandera que indica que hay un salto pendiente de ejecutar.

                ultimoMovimientoJugador = ahora;                            // Actualiza ultimoMovimientoJugador.
            }
        }

        if (dkFrame >= DK_FRAMES) {                                         // Comprueba una condición del DK antes de continuar.
            dkFrame = DK_FRAME_IDLE;                                        // Actualiza dkFrame.
            estadoDK = DK_IDLE;                                             // Actualiza estadoDK.

            dkProximoLanzamiento =                                          // Actualiza dkProximoLanzamiento.
                ahora +                                                     // Continúa el cálculo de dkProximoLanzamiento.
                CalcularIntervaloLanzamiento();                             // Continúa el cálculo de dkProximoLanzamiento.
        }

        /*
         * Render final directo.
         * No restauramos el fondo primero, por lo que DK y
         * los objetos cercanos no producen un borrado visible.
         */
        RenderizarRegionFinal(                                          // Redibuja esta región con todas las capas finales.
            DK_X,                                                           // Indica X inicial de la región.
            DK_Y,                                                           // Indica Y inicial de la región.
            DK_W,                                                           // Indica ancho de la región.
            DK_H                                                            // Indica alto de la región.
        );                                                                  // Finaliza la llamada a RenderizarRegionFinal().

        /*
         * El barril recién creado puede sobresalir algunos
         * píxeles del rectángulo de DK.
         */
        RenderizarRegionFinal(                                          // Redibuja esta región con todas las capas finales.
            BARRIL_SALIDA_X,                                                // Indica X inicial de la región.
            BARRIL_SALIDA_Y,                                                // Indica Y inicial de la región.
            BARRIL_W,                                                       // Indica ancho de la región.
            BARRIL_H                                                        // Indica alto de la región.
        );                                                                  // Finaliza la llamada a RenderizarRegionFinal().
    }
}


/* ============================================================
 * UART - CONTROLES DESDE EL ESP32
 *
 * USART3 recibe los comandos enviados por el ESP32 y los guarda en un buffer circular mediante interrupciones.
 * Después se identifica si el comando pertenece a J1 o J2 y se aplica L, R, U, D, N o J.
 * ============================================================ */

void UART3_IniciarRecepcion(void)                                       // Inicializa el buffer circular y habilita la interrupción RXNE de USART3.
{
    uartRxHead = 0;                                                         // Reinicia uartRxHead a cero.
    uartRxTail = 0;                                                         // Reinicia uartRxTail a cero.

    HAL_NVIC_SetPriority(                                               // Configura el controlador de interrupciones NVIC para USART3.
        USART3_IRQn,                                                        // Indica interrupción configurada.
        1,                                                                  // Indica prioridad principal.
        0                                                                   // Indica subprioridad.
    );                                                                      // Finaliza la llamada a HAL_NVIC_SetPriority().

    HAL_NVIC_ClearPendingIRQ(                                           // Configura el controlador de interrupciones NVIC para USART3.
        USART3_IRQn                                                         // Indica interrupción cuyo pendiente se limpia.
    );                                                                      // Finaliza la llamada a HAL_NVIC_ClearPendingIRQ().

    HAL_NVIC_EnableIRQ(                                                 // Configura el controlador de interrupciones NVIC para USART3.
        USART3_IRQn                                                         // Indica interrupción que se habilita.
    );                                                                      // Finaliza la llamada a HAL_NVIC_EnableIRQ().

    __HAL_UART_ENABLE_IT(                                               // Habilita la interrupción de recepción de USART3 cuando llega un nuevo byte.
        &huart3,                                                            // Indica UART utilizada.
        UART_IT_RXNE                                                        // Indica interrupción UART que se habilita.
    );                                                                      // Finaliza la llamada a __HAL_UART_ENABLE_IT().
}

/*
 * Si CubeMX ya genera USART3_IRQHandler en stm32f4xx_it.c,
 * NO dejes dos handlers con el mismo nombre.
 */
void USART3_IRQHandler(void)                                            // Rutina de interrupción de USART3.
{
    uint32_t sr =                                                           // Calcula y guarda sr.
        USART3->SR;                                                         // Evalúa esta comparación como parte de la condición actual.

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        sr &                                                                // Continúa el cálculo de sr.
        (                                                                   // Continúa el cálculo de sr.
            USART_SR_RXNE |                                                 // Continúa el cálculo de sr.
            USART_SR_ORE |                                                  // Continúa el cálculo de sr.
            USART_SR_FE |                                                   // Continúa el cálculo de sr.
            USART_SR_NE |                                                   // Continúa el cálculo de sr.
            USART_SR_PE                                                     // Continúa el cálculo de sr.
        )                                                                   // Continúa el cálculo de sr.
    ) {                                                                     // Continúa el cálculo de sr.
        uint8_t dato =                                                      // Calcula y guarda dato.
            (uint8_t)(                                                      // Continúa el cálculo de dato.
                USART3->DR &                                                // Evalúa esta comparación como parte de la condición actual.
                0xFFU                                                       // Continúa el cálculo de dato.
            );                                                              // Continúa el cálculo de dato.

        if (sr & USART_SR_RXNE) {                                           // Comprueba si se cumplen las condiciones necesarias para continuar.
            uint8_t siguiente =                                             // Calcula y guarda siguiente.
                (uint8_t)(                                                  // Continúa el cálculo de siguiente.
                    (uartRxHead + 1U) %                                     // Continúa el cálculo de siguiente.
                    UART_RX_BUFFER_SIZE                                     // Continúa el cálculo de siguiente.
                );                                                          // Continúa el cálculo de siguiente.

            if (siguiente != uartRxTail) {                                  // Comprueba si se cumplen las condiciones necesarias para continuar.
                uartRxBuffer[uartRxHead] =                                  // Actualiza uartRxBuffer.
                    dato;                                                   // Continúa el cálculo de uartRxBuffer.

                uartRxHead =                                                // Actualiza uartRxHead.
                    siguiente;                                              // Continúa el cálculo de uartRxHead.
            } else {
                uartOverflow++;                                             // Incrementa uartOverflow en una unidad.
            }
        }
    }
}

uint8_t UART3_LeerByte(uint8_t *dato)                                   // Extrae un byte pendiente del buffer circular. Devuelve 0 si no hay datos y 1 si entregó un byte.
{
    if (uartRxTail == uartRxHead)                                           // Comprueba si se cumplen las condiciones necesarias para continuar.
        return 0;                                                           // Devuelve 0 para indicar que no se encontró o no se cumple la condición buscada.

    *dato =                                                                 // Actualiza dato.
        uartRxBuffer[                                                       // Continúa el cálculo de dato.
            uartRxTail                                                      // Continúa el cálculo de dato.
        ];                                                                  // Continúa el cálculo de dato.

    uartRxTail =                                                            // Actualiza uartRxTail.
        (uint8_t)(                                                          // Continúa el cálculo de uartRxTail.
            (uartRxTail + 1U) %                                             // Continúa el cálculo de uartRxTail.
            UART_RX_BUFFER_SIZE                                             // Continúa el cálculo de uartRxTail.
        );                                                                  // Continúa el cálculo de uartRxTail.

    return 1;                                                               // Devuelve 1 para indicar que sí se encontró o sí se cumple la condición buscada.
}

void AplicarComando(                                                    // Aplica L/R/U/D/N/J al jugador según el estado del juego.
    Jugador *j,
    char comando                                                            // Recibe comando recibido.
)
{
    j->ultimoPaquete =                                                      // Actualiza momento del último comando recibido.
        HAL_GetTick();                                                  // Marca la llegada del comando recibido.

    if (pantallaActual != JUEGO_ACTIVO) {                                   // Comprueba si se cumplen las condiciones necesarias para continuar.
        /* La pantalla final siempre queda visible; un salto antiguo no
         * debe devolver al menu al cumplirse el tiempo minimo. */
        if (pantallaActual == JUEGO_RESCATE ||                              // Comprueba si se cumplen las condiciones necesarias para continuar.
            (pantallaActual == JUEGO_FIN &&                                 // Añade esta condición a la comprobación actual.
             HAL_GetTick() - finDesde < FIN_MINIMA_MS)) {                   // Evalúa esta comparación como parte de la condición actual.
            comenzarSolicitado = 0;                                         // Reinicia comenzarSolicitado a cero.
            j->ultimoComandoRX = comando;                               // Último comando recibido; ayuda a detectar eventos y evitar repeticiones.
            j->saltoSolicitado = 0;                                     // Bandera que indica que hay un salto pendiente de ejecutar.
            return;                                                         // Termina la función en este punto.
        }
        if (comando == 'J' && j->ultimoComandoRX != 'J')                    // Comprueba si se cumplen las condiciones necesarias para continuar.
            comenzarSolicitado = 1;                                         // Activa comenzarSolicitado.
        j->ultimoComandoRX = comando;                                   // Último comando recibido; ayuda a detectar eventos y evitar repeticiones.
        j->saltoSolicitado = 0;                                         // Bandera que indica que hay un salto pendiente de ejecutar.
        return;                                                             // Termina la función en este punto.
    }

    if (!permitirMovimientoJugadores) {                                     // Comprueba si se cumplen las condiciones necesarias para continuar.
        j->direccion = 'N';                                                 // Actualiza dirección de movimiento.
        j->ultimoComandoRX = 'N';                                       // Último comando recibido; ayuda a detectar eventos y evitar repeticiones.
        j->saltoSolicitado = 0;                                         // Bandera que indica que hay un salto pendiente de ejecutar.
        return;                                                             // Termina la función en este punto.
    }

    if (comando == 'J') {                                                   // Comprueba si se cumplen las condiciones necesarias para continuar.
        if (j->ultimoComandoRX != 'J')                                      // Comprueba si se cumplen las condiciones necesarias para continuar.
            j->saltoSolicitado = 1;                                     // Bandera que indica que hay un salto pendiente de ejecutar.

        j->ultimoComandoRX = 'J';                                       // Último comando recibido; ayuda a detectar eventos y evitar repeticiones.
        return;                                                             // Termina la función en este punto.
    }

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        comando == 'L' ||                                                   // Añade esta condición a la comprobación actual.
        comando == 'R' ||                                                   // Añade esta condición a la comprobación actual.
        comando == 'U' ||                                                   // Añade esta condición a la comprobación actual.
        comando == 'D' ||                                                   // Añade esta condición a la comprobación actual.
        comando == 'N'                                                      // Evalúa esta comparación como parte de la condición actual.
    ) {
        /*
         * Con martillo puede caminar y saltar, pero no subir
         * ni bajar escaleras.
         */
        if (                                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            j->martilloActivo &&                                            // Comprueba si el jugador tiene el martillo activo.
            (comando == 'U' || comando == 'D')                              // Evalúa esta comparación como parte de la condición actual.
        ) {
            j->direccion = 'N';                                             // Actualiza dirección de movimiento.
            j->ultimoComandoRX = 'N';                                   // Último comando recibido; ayuda a detectar eventos y evitar repeticiones.
            return;                                                         // Termina la función en este punto.
        }

        j->direccion = comando;                                             // Actualiza dirección de movimiento.
        j->ultimoComandoRX = comando;                                   // Último comando recibido; ayuda a detectar eventos y evitar repeticiones.

        if (comando == 'L')                                                 // Comprueba si se cumplen las condiciones necesarias para continuar.
            j->mirandoDerecha = 0;                                      // Orientación visual: 0 izquierda, 1 derecha.
        else if (comando == 'R')                                            // Evalúa otra condición.
            j->mirandoDerecha = 1;                                      // Orientación visual: 0 izquierda, 1 derecha.
    }
}

void ProcesarByteESP32(uint8_t dato)                                    // Interpreta jugador + comando recibidos desde el ESP32.
{
    if (dato == '1' || dato == '2') {                                       // Comprueba si se cumplen las condiciones necesarias para continuar.
        jugadorPendiente =                                                  // Actualiza jugadorPendiente.
            dato - '0';                                                     // Continúa el cálculo de jugadorPendiente.
        return;                                                             // Termina la función en este punto.
    }

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        jugadorPendiente != 0 &&                                            // Añade esta condición a la comprobación actual.
        (
            dato == 'L' ||                                                  // Añade esta condición a la comprobación actual.
            dato == 'R' ||                                                  // Añade esta condición a la comprobación actual.
            dato == 'U' ||                                                  // Añade esta condición a la comprobación actual.
            dato == 'D' ||                                                  // Añade esta condición a la comprobación actual.
            dato == 'N' ||                                                  // Añade esta condición a la comprobación actual.
            dato == 'J'                                                     // Evalúa esta comparación como parte de la condición actual.
        )
    ) {
        if (jugadorPendiente == 1) {                                        // Comprueba una condición del jugador 1 antes de continuar.
            AplicarComando(                                                 // Aplica L, R, U, D, N o J al jugador correspondiente.
                &jugador1,                                                  // Indica jugador al que se aplica el control.
                (char)dato                                                  // Indica comando recibido desde el ESP32.
            );                                                              // Finaliza la llamada a AplicarComando().
        } else {
            AplicarComando(                                                 // Aplica L, R, U, D, N o J al jugador correspondiente.
                &jugador2,                                                  // Indica jugador al que se aplica el control.
                (char)dato                                                  // Indica comando recibido desde el ESP32.
            );                                                              // Finaliza la llamada a AplicarComando().
        }

        jugadorPendiente = 0;                                               // Reinicia jugadorPendiente a cero.
        return;                                                             // Termina la función en este punto.
    }

    jugadorPendiente = 0;                                                   // Reinicia jugadorPendiente a cero.
}

/* USER CODE END 0 */



/* Dibujado directo para el menu (el render por regiones se usa en partida). */
static void SpriteEnMenu(const uint16_t *sprite, int ancho, int alto,   // Dibuja un sprite RGB565 en el menú con la escala indicada.
                         int px, int py, int escala)
{
    for (int y = 0; y < alto; y++)                                          // Recorre todas las posiciones necesarias de este bloque.
        for (int x = 0; x < ancho; x++) {                                   // Recorre todas las posiciones necesarias de este bloque.
            uint16_t pixel = sprite[y * ancho + x];                         // Calcula y guarda posición lineal del píxel.
            if (pixel) FillRect(px + x * escala, py + y * escala,           // Comprueba si se cumplen las condiciones necesarias para continuar.
                                escala, escala, pixel);
        }
}

static void Numero5(char *dst, uint32_t valor)                          // Convierte la puntuación a una cadena de 5 dígitos.
{
    if (valor > 99999U) valor = 99999U;                                     // Comprueba si se cumplen las condiciones necesarias para continuar.
    dst[5] = 0;                                                             // Reinicia dst a cero.
    for (int i = 4; i >= 0; i--) {                                          // Recorre todas las posiciones necesarias de este bloque.
        dst[i] = '0' + (char)(valor % 10U);                                 // Actualiza dst.
        valor /= 10U;                                                       // Actualiza valor /.
    }
}


static void DibujarAvisoFinal(uint8_t visible)                          // Limpia y, si corresponde, dibuja el mensaje intermitente que indica cómo volver al menú.
{
    FillRect(56, 196, 208, 16, COLOR_FONDO);                                // Ejecuta FillRect().
    FillRect(104, 213, 112, 16, COLOR_FONDO);                               // Ejecuta FillRect().
    if (visible) {                                                          // Comprueba si se cumplen las condiciones necesarias para continuar.
        LCD_Print("SALTO: VOLVER", 56, 196, 2, 0xFFE0, COLOR_FONDO);    // Ejecuta LCD_Print().
        LCD_Print("AL MENU", 104, 213, 2, 0xFFE0, COLOR_FONDO);         // Ejecuta LCD_Print().
    }
}

void DibujarMarcador(void)                                              // Actualiza el HUD de vidas y puntuación de ambos jugadores.
{
    char valor[6];                                                          // Reserva valor para usarla en este bloque.
    LCD_Print("J1", 4, 2, 1, 0xFFFF, COLOR_FONDO);                      // Ejecuta LCD_Print().
    LCD_Print("J2", 220, 2, 1, 0xFFE0, COLOR_FONDO);                    // Ejecuta LCD_Print().
    for (int i = 0; i < VIDAS_INICIALES; i++) {                             // Recorre todas las posiciones necesarias de este bloque.
        FillRect(25 + i * 9, 4, 7, 7, i < jugador1.vidas ? 0xF800 : 0);     // Evalúa esta comparación como parte de la condición actual.
        FillRect(241 + i * 9, 4, 7, 7, i < jugador2.vidas ? 0x07E0 : 0);    // Evalúa esta comparación como parte de la condición actual.
    }
    Numero5(valor, jugador1.puntos);                                    // Convierte la puntuación a una cadena de 5 dígitos.
    LCD_Print(valor, 55, 2, 1, 0xFFFF, COLOR_FONDO);                    // Ejecuta LCD_Print().
    Numero5(valor, jugador2.puntos);                                    // Convierte la puntuación a una cadena de 5 dígitos.
    LCD_Print(valor, 270, 2, 1, 0xFFE0, COLOR_FONDO);                   // Ejecuta LCD_Print().
}

/* RGB565 little endian, 320 x 240; tres archivos 8.3 en la raiz FAT32. */
static uint8_t MostrarMenuSD(const char nombre83[11])                   // Lee una imagen RGB565 desde la microSD y la dibuja fila por fila.
{
    SD_Archivo archivo;                                                     // Reserva la estructura usada para manejar el archivo de la microSD.
    uint8_t esMenuFinal = memcmp(nombre83, "MENUFIN RGB", 11U) == 0;        // Calcula y guarda esMenuFinal.
    if (!sdDisponible || !SD_Abrir(nombre83, &archivo)) return 0U;          // Comprueba si se cumplen las condiciones necesarias para continuar.
    if (archivo.tamano != SCREEN_W * SCREEN_H * 2U) {                       // Comprueba si se cumplen las condiciones necesarias para continuar.
        SD_ErrorMenu(9U);                                               // Ejecuta SD_ErrorMenu().
        return 0U;                                                          // Devuelve 0 para indicar que no se encontró o no se cumple la condición buscada.
    }
    for (unsigned y = 0; y < SCREEN_H; y++) {                               // Recorre todas las posiciones necesarias de este bloque.
        if (!SD_Leer(&archivo, filaMenuBytes, sizeof(filaMenuBytes))) {     // Comprueba si se cumplen las condiciones necesarias para continuar.
            SD_ErrorMenu(10U);                                          // Ejecuta SD_ErrorMenu().
            return 0U;                                                      // Devuelve 0 para indicar que no se encontró o no se cumple la condición buscada.
        }
        for (unsigned x = 0; x < SCREEN_W; x++) {                           // Recorre todas las posiciones necesarias de este bloque.
            filaMenuPixeles[x] = (uint16_t)filaMenuBytes[2U*x] |            // Actualiza filaMenuPixeles.
                                   ((uint16_t)filaMenuBytes[2U*x+1U] << 8); // Evalúa esta comparación como parte de la condición actual.
            /* Nunca mostrar el texto celeste ni la raya de un MENUFIN.RGB
             * antiguo, ni siquiera durante la carga por filas desde SD. */
            if (esMenuFinal && y >= 195U && y < 230U &&                     // Comprueba si se cumplen las condiciones necesarias para continuar.
                x >= 8U && x < 312U)                                        // Evalúa esta comparación como parte de la condición actual.
                filaMenuPixeles[x] = COLOR_FONDO;                           // Actualiza filaMenuPixeles.
        }
        LCD_Bitmap(0U, y, SCREEN_W, 1U, filaMenuPixeles);               // Ejecuta LCD_Bitmap().
    }
    return 1U;                                                              // Devuelve 1 para indicar que sí se encontró o sí se cumple la condición buscada.
}

static void MostrarFalloSD(uint8_t menuFinal)                           // Traduce el código de error de la microSD a un mensaje legible y lo muestra en la TFT.
{
    const char *mensaje = "ERROR SD DESCONOCIDO";                           // Calcula y guarda mensaje.
    switch (SD_UltimoError()) {                                         // Selecciona una acción según el valor actual de la variable evaluada.
        case 1U: mensaje = "SD 01: SIN CMD0"; break;                        // Actualiza mensaje.
        case 2U: mensaje = "SD 02: FALLO CMD8"; break;                      // Actualiza mensaje.
        case 3U: mensaje = "SD 03: FALLO ACMD41"; break;                    // Actualiza mensaje.
        case 4U: mensaje = "SD 04: FALLO CMD58"; break;                     // Actualiza mensaje.
        case 5U: mensaje = "SD 05: SECTOR 0"; break;                        // Actualiza mensaje.
        case 6U: mensaje = "SD 06: NO FAT32"; break;                        // Actualiza mensaje.
        case 7U: mensaje = "SD 07: ERROR DIRECTORIO"; break;                // Actualiza mensaje.
        case 8U: mensaje = menuFinal ? "NO ESTA MENUFIN.RGB" :              // Actualiza mensaje.
                                  "NO ESTA MENUINI.RGB"; break;             // Continúa el cálculo de mensaje.
        case 9U: mensaje = "SD 09: TAMANO MENU"; break;                     // Actualiza mensaje.
        case 10U: mensaje = "SD 10: LECTURA MENU"; break;                   // Actualiza mensaje.
        default: break;                                                     // Usa este caso si ningún estado anterior coincide.
    }
    LCD_Clear(COLOR_FONDO);                                             // Ejecuta LCD_Clear().
    LCD_Print(mensaje, 28, 106, 1, 0xFFFF, COLOR_FONDO);                // Ejecuta LCD_Print().
    LCD_Print("VER LEEME_SD_FUNCIONAL", 32, 126, 1, 0x07FF, COLOR_FONDO); // Ejecuta LCD_Print().
}

void EntrarMenu(void)                                                   // Configura el estado de menú, carga la imagen inicial y los récords visibles, y activa la música de menú.
{
    char numeroRecord[6];                                                   // Reserva numeroRecord para usarla en este bloque.
    pantallaActual = JUEGO_MENU;                                            // Actualiza pantallaActual.
    musicaActual = MUSICA_NORMAL;                                           // Actualiza musicaActual.
    comenzarSolicitado = 0;                                                 // Reinicia comenzarSolicitado a cero.
    jugador1.ultimoComandoRX = 'N';                                     // Último comando recibido; ayuda a detectar eventos y evitar repeticiones.
    jugador2.ultimoComandoRX = 'N';                                     // Último comando recibido; ayuda a detectar eventos y evitar repeticiones.
    if (!MostrarMenuSD("MENUINI RGB")) {                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        sdDisponible = 0U;                                                  // Reinicia sdDisponible a cero.
        MostrarFalloSD(0U);                                             // Traduce el código de error de la microSD a un mensaje legible y lo muestra en la TFT.
        return;                                                             // Termina la función en este punto.
    }
    SpriteEnMenu(spriteMario + JUGADOR_W * JUGADOR_H, JUGADOR_W,            // Ejecuta SpriteEnMenu().
                 JUGADOR_H, 55, 104, 1);                                    // Entrega este parámetro a SpriteEnMenu().
    SpriteEnMenu(spriteLuigi + JUGADOR_W * JUGADOR_H, JUGADOR_W,            // Ejecuta SpriteEnMenu().
                 JUGADOR_H, 173, 104, 1);                                   // Entrega este parámetro a SpriteEnMenu().
    Numero5(numeroRecord, recordMario);                                 // Convierte la puntuación a una cadena de 5 dígitos.
    LCD_Print(numeroRecord, 98, 172, 1, 0xFFFF, COLOR_FONDO);           // Ejecuta LCD_Print().
    Numero5(numeroRecord, recordLuigi);                                 // Convierte la puntuación a una cadena de 5 dígitos.
    LCD_Print(numeroRecord, 226, 172, 1, 0xFFE0, COLOR_FONDO);          // Ejecuta LCD_Print().
    if (!sdPuntajesOk)                                                      // Comprueba si se cumplen las condiciones necesarias para continuar.
        LCD_Print("SCORES.DAT NO DISPONIBLE", 28, 222, 1, 0xF800, COLOR_FONDO); // Ejecuta LCD_Print().
    /* El tema de menu comienza y permanece en bucle hasta iniciar. */
    Audio_Enviar(0x21U);                                                // Silencia la música actual.
    Audio_Enviar(0x25U);                                                // Activa o mantiene la música del menú.
}

void IniciarRescate(uint8_t quien, uint32_t ahora)                      // Entra al estado de rescate, guarda al ganador, muestra el corazón y activa el audio de victoria.
{
    if (pantallaActual != JUEGO_ACTIVO) return;                             // Comprueba si se cumplen las condiciones necesarias para continuar.
    pantallaActual = JUEGO_RESCATE;                                         // Actualiza pantallaActual.
    rescateDesde = ahora;                                                   // Actualiza rescateDesde.
    ganador = quien;                                                        // Actualiza ganador.
    finPorRescate = 1;                                                      // Activa finPorRescate.
    corazonVisible = 1;                                                     // Activa corazonVisible.
    comenzarSolicitado = 0;                                                 // Reinicia comenzarSolicitado a cero.
    Audio_Enviar(0x21U);                                                // Silencia la música actual.
    Audio_Enviar(0x06U);                                                // Reproduce el sonido de victoria.
    RenderizarRegionFinal(CORAZON_X, CORAZON_Y, CORAZON_W, CORAZON_H);  // Redibuja esta región con todas las capas finales.
}

void IniciarPartida(void)                                                   // Reinicia todos los estados y objetos necesarios para comenzar una partida nueva.
{
    LCD_Clear(COLOR_FONDO);                                             // Ejecuta LCD_Clear().
    pantallaActual = JUEGO_ACTIVO;                                          // Actualiza pantallaActual.
    comenzarSolicitado = 0;                                                 // Reinicia comenzarSolicitado a cero.
    ganador = 0;                                                            // Reinicia ganador a cero.
    finPorRescate = 0;                                                      // Reinicia finPorRescate a cero.
    corazonVisible = 0;                                                     // Reinicia corazonVisible a cero.
    ayudaVisible = 1;                                                       // Activa ayudaVisible.
    hudSucio = 1;                                                           // Activa hudSucio.

    GenerarFondoBuffer();                                               // Precalcula vigas y escaleras en RAM para poder restaurar zonas rápidamente.
    DibujarEscenario();                                                 // Recorre todas las plataformas y escaleras y las dibuja al iniciar una partida.

    InicializarJugadores();                                             // Inicializa posición, vidas, control y estados de ambos jugadores.
    InicializarBarriles();                                              // Limpia todos los slots de barriles al comenzar una partida.
    InicializarFireballs();                                             // Reinicia todos los slots de flamas y el contador de flamas activas.
    InicializarMartillos();                                             // Configura los dos pickups de martillo, su plataforma, posición suspendida y estado inicial disponible.
    primerAzulLanzado = 0; /* siempre lanzar el azul que enciende el aceite */ // Actualiza primerAzulLanzado.

    /*
     * El barril de aceite comienza apagado.
     */
    estadoAceite =                                                          // Actualiza estadoAceite.
        ACEITE_APAGADO;                                                     // Continúa el cálculo de estadoAceite.

    aceiteNecesitaRedibujar =                                               // Actualiza aceiteNecesitaRedibujar.
        0;                                                                  // Continúa el cálculo de aceiteNecesitaRedibujar.

    DibujarAceite();                                                    // Dibuja el aceite y su llama píxel por píxel.

    for (int i = 0; i < MAX_MARTILLOS; i++) {                               // Recorre los dos martillos del escenario.
        RenderizarRegionFinal(                                          // Redibuja esta región con todas las capas finales.
            martillos[i].x,                                                 // Indica X inicial de la región.
            martillos[i].y,                                                 // Indica Y inicial de la región.
            martillos[i].w,                                                 // Indica ancho de la región.
            martillos[i].h                                                  // Indica alto de la región.
        );                                                                  // Finaliza la llamada a RenderizarRegionFinal().
    }

    musicaActual =                                                          // Actualiza musicaActual.
        MUSICA_NORMAL;                                                      // Continúa el cálculo de musicaActual.

    Audio_Enviar(0x20U);                                                // Activa la música normal de la partida.
    Audio_Enviar(0x05U);                                                // Reproduce la fanfarria de inicio.

    permitirMovimientoJugadores = 0;                                        // Reinicia permitirMovimientoJugadores a cero.
    primerBarrilLiberado = 0;                                               // Reinicia primerBarrilLiberado a cero.

    estadoDK = DK_IDLE;                                                     // Actualiza estadoDK.
    dkFrame = DK_FRAME_IDLE;                                                // Actualiza dkFrame.
    dkUltimoCambio = HAL_GetTick();                                     // Marca el inicio del frame actual de DK.

    /* Espera inicial antes de que DK empiece */
    dkProximoLanzamiento =                                                  // Actualiza dkProximoLanzamiento.
        HAL_GetTick() + 600;                                                // Continúa el cálculo de dkProximoLanzamiento.

    rngState ^=                                                             // Actualiza rngState mediante una operación bit a bit.
        HAL_GetTick() +                                                     // Continúa el cálculo de rngState.
        0x13579BDFU;                                                        // Continúa el cálculo de rngState.

    ultimoTickFisica =                                                      // Actualiza ultimoTickFisica.
        HAL_GetTick();                                                      // Continúa el cálculo de ultimoTickFisica.

    ultimoMovimientoJugador =                                               // Actualiza ultimoMovimientoJugador.
        HAL_GetTick();                                                      // Continúa el cálculo de ultimoMovimientoJugador.

    ultimoMovimientoBarril =                                                // Actualiza ultimoMovimientoBarril.
        HAL_GetTick();                                                      // Continúa el cálculo de ultimoMovimientoBarril.

    ultimoMovimientoFireball =                                              // Actualiza ultimoMovimientoFireball.
        HAL_GetTick();                                                      // Continúa el cálculo de ultimoMovimientoFireball.

    DibujarFrameDK(dkFrame);                                            // Extrae un frame de la spritesheet de Donkey Kong y lo dibuja línea por línea usando transparencia.
    RenderizarRegionFinal(54, 49, BARRIL_W, 3 * BARRIL_H);              // Redibuja esta región con todas las capas finales.
    DibujarJugadores();                                                 // Dibuja las regiones de ambos jugadores.
    RenderizarRegionFinal(PEACH_X, PEACH_Y, PEACH_W, PEACH_H);          // Redibuja esta región con todas las capas finales.
    RenderizarRegionFinal(HELP_X, HELP_Y, HELP_W, HELP_H);              // Redibuja esta región con todas las capas finales.
    DibujarMarcador();                                                  // Actualiza el HUD de vidas y puntuación de ambos jugadores.
    hudSucio = 0;                                                           // Reinicia hudSucio a cero.

}

void TerminarPartida(uint8_t quien)                                     // Guarda resultados y muestra la pantalla final de la partida.
{
    pantallaActual = JUEGO_FIN;                                             // Actualiza pantallaActual.
    ganador = quien;                                                        // Actualiza ganador.
    finDesde = HAL_GetTick();                                           // Marca el inicio de la pantalla final.
    comenzarSolicitado = 0;                                                 // Reinicia comenzarSolicitado a cero.
    jugador1.ultimoComandoRX = 'J';                                     // Último comando recibido; ayuda a detectar eventos y evitar repeticiones.
    jugador2.ultimoComandoRX = 'J';                                     // Último comando recibido; ayuda a detectar eventos y evitar repeticiones.
    if (!finPorRescate) {                                                   // Comprueba si se cumplen las condiciones necesarias para continuar.
        Audio_Enviar(0x21U);                                            // Silencia la música actual.
        Audio_Enviar(quien ? 0x06U : 0x07U);                            // Reproduce victoria o derrota según el resultado.
    }
    if (sdPuntajesOk && !SD_Scores_Guardar(jugador1.puntos, jugador2.puntos)) // Comprueba una condición del jugador 1 antes de continuar.
        sdPuntajesOk = 0U;                                                  // Reinicia sdPuntajesOk a cero.
    if (jugador1.puntos > recordMario) recordMario = jugador1.puntos;       // Comprueba una condición del jugador 2 antes de continuar.
    if (jugador2.puntos > recordLuigi) recordLuigi = jugador2.puntos;       // Comprueba si se cumplen las condiciones necesarias para continuar.
    if (!MostrarMenuSD("MENUFIN RGB")) {                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        sdDisponible = 0U;                                                  // Reinicia sdDisponible a cero.
        MostrarFalloSD(1U);                                             // Traduce el código de error de la microSD a un mensaje legible y lo muestra en la TFT.
        return;                                                             // Termina la función en este punto.
    }
    FillRect(8, 195, 304, 35, COLOR_FONDO);                                 // Ejecuta FillRect().
    avisoFinalVisible = 1U;                                                 // Activa avisoFinalVisible.
    DibujarAvisoFinal(avisoFinalVisible);                               // Limpia y, si corresponde, dibuja el mensaje intermitente que indica cómo volver al menú.
    if (finPorRescate)                                                      // Comprueba si se cumplen las condiciones necesarias para continuar.
        LCD_Print("PRINCESA RESCATADA", 94, 121, 1, 0xFB56, COLOR_FONDO); // Ejecuta LCD_Print().
    SpriteEnMenu(spritePeach, PEACH_W, PEACH_H, 145, 79, 2);            // Dibuja un sprite RGB565 en el menú con la escala indicada.
    if (finPorRescate)                                                      // Comprueba si se cumplen las condiciones necesarias para continuar.
        SpriteEnMenu(corazonSprite, CORAZON_W, CORAZON_H, 148, 56, 2);  // Dibuja un sprite RGB565 en el menú con la escala indicada.
    if (quien == 1U) LCD_Print("GANA MARIO", 105, 133, 1, 0xFFFF, COLOR_FONDO); // Comprueba si se cumplen las condiciones necesarias para continuar.
    else if (quien == 2U) LCD_Print("GANA LUIGI", 105, 133, 1, 0xFFE0, COLOR_FONDO); // Evalúa otra condición del jugador 1.
    else LCD_Print("EMPATE", 130, 133, 1, 0xFFFF, COLOR_FONDO);             // Usa esta alternativa cuando la condición anterior no se cumple.
    char numero[6];                                                         // Reserva numero para usarla en este bloque.
    Numero5(numero, jugador1.puntos);                                   // Convierte la puntuación a una cadena de 5 dígitos.
    LCD_Print("J1:", 70, 160, 1, 0xFFFF, COLOR_FONDO);                  // Ejecuta LCD_Print().
    LCD_Print(numero, 102, 160, 1, 0xFFFF, COLOR_FONDO);                // Ejecuta LCD_Print().
    Numero5(numero, jugador2.puntos);                                   // Convierte la puntuación a una cadena de 5 dígitos.
    LCD_Print("J2:", 194, 160, 1, 0xFFE0, COLOR_FONDO);                 // Ejecuta LCD_Print().
    LCD_Print(numero, 226, 160, 1, 0xFFE0, COLOR_FONDO);                // Ejecuta LCD_Print().
    Numero5(numero, recordMario);                                       // Convierte la puntuación a una cadena de 5 dígitos.
    LCD_Print(numero, 98, 181, 1, 0xFFFF, COLOR_FONDO);                 // Ejecuta LCD_Print().
    Numero5(numero, recordLuigi);                                       // Convierte la puntuación a una cadena de 5 dígitos.
    LCD_Print(numero, 226, 181, 1, 0xFFE0, COLOR_FONDO);                // Ejecuta LCD_Print().
    if (!sdPuntajesOk) LCD_Print("SD NO", 245, 146, 1, 0xF800, COLOR_FONDO); // Comprueba si se cumplen las condiciones necesarias para continuar.
}

/* ============================================================
 * MAIN - CICLO PRINCIPAL DEL JUEGO
 *
 * Se inicializan STM32, TFT, microSD, SPI, UART y los datos de la partida.
 * El while(1) recibe controles, actualiza mecánicas, revisa colisiones y redibuja únicamente las zonas que cambian.
 * ============================================================ */

int main(void)                                                          // Punto de entrada.
{
    HAL_Init();                                                         // Inicializa la capa HAL, SysTick y el estado básico del STM32 antes de configurar periféricos.

    SystemClock_Config();                                               // Configura el reloj principal del microcontrolador.

    MX_GPIO_Init();                                                     // Configura los GPIO usados por la pantalla y otros periféricos.
    MX_SPI1_Init();                                                     // Configura SPI1 para la comunicación con la microSD.
    MX_USART3_UART_Init();                                              // Configura USART3: PC11 recibe del ESP32 y PC10 transmite hacia la STM32 de audio.

    /* USER CODE BEGIN 2 */

    UART3_IniciarRecepcion();                                           // Activa la recepción por interrupciones para recibir controles desde el ESP32.

    /* ========================================================
     * INICIALIZACION DE TFT Y MICROSD
     *
     * Se espera brevemente para estabilizar la alimentación,
     * se inicializa la pantalla y luego se intenta montar la SD.
     * Si la SD falla en el primer intento, se realiza un segundo
     * intento antes de entrar al menú.
     * ======================================================== */

    HAL_Delay(250U);                                                     // Espera 250 ms para que la TFT y la microSD tengan alimentación estable.

    LCD_Init();                                                         // Inicializa el controlador y el bus paralelo de la pantalla ILI9341.

    LCD_Clear(COLOR_FONDO);                                             // Limpia la pantalla antes de mostrar el estado de carga.

    LCD_Print("CARGANDO MENU...", 80, 106, 1, 0xFFFF, COLOR_FONDO);    // Informa que se está intentando cargar el contenido de la microSD.

    sdDisponible =
        SD_Iniciar(&hspi1);                                             // Intenta inicializar la microSD mediante SPI1.

    if (!sdDisponible) {                                                // Entra aquí si la microSD no respondió en el primer intento.
        LCD_Print("REINTENTANDO SD...", 72, 126, 1,
                  0xFFE0, COLOR_FONDO);                                 // Muestra que se realizará un segundo intento de inicialización.

        HAL_Delay(200U);                                                // Da 200 ms adicionales para que el módulo SD termine de estabilizarse.

        sdDisponible =
            SD_Iniciar(&hspi1);                                         // Realiza el segundo intento de montar la microSD.
    }

    if (sdDisponible)                                                   // Solo intenta leer récords cuando la microSD está disponible.
        sdPuntajesOk =
            SD_Scores_Cargar(&recordMario, &recordLuigi);               // Carga desde la SD los récords guardados de Mario y Luigi.

    EntrarMenu();                                                       // Intenta cargar y mostrar el menú inicial.

    ultimoIntentoSD =
        HAL_GetTick();                                                  // Guarda cuándo terminó el último intento de acceso a la microSD.

    /* USER CODE END 2 */


    /* USER CODE BEGIN WHILE */
    while (1)                                                               // Repite este bloque mientras la condición indicada siga activa.
    {
        uint8_t datoUART;                                                   // Reserva datoUART para usarla en este bloque.

        while (                                                             // Repite este bloque mientras la condición indicada siga activa.
            UART3_LeerByte(                                                 // Extrae un byte pendiente del buffer circular. Devuelve 0 si no hay datos y 1 si entregó un byte.
                &datoUART                                                   // Entrega este parámetro a UART3_LeerByte().
            )
        ) {
            ProcesarByteESP32(                                              // Interpreta jugador + comando recibidos desde el ESP32.
                datoUART                                                    // Entrega este parámetro a ProcesarByteESP32().
            );                                                              // Finaliza la llamada a ProcesarByteESP32().
        }

        uint32_t ahora =                                                    // Calcula y guarda tick actual.
            HAL_GetTick();                                                  // Continúa el cálculo de tick actual.


        if (pantallaActual == JUEGO_MENU && sdDisponible &&                 // Comprueba si se cumplen las condiciones necesarias para continuar.
            ahora - ultimoEstadoAudio >= 1500U) {                           // Evalúa esta comparación como parte de la condición actual.
            ultimoEstadoAudio = ahora;                                      // Actualiza ultimoEstadoAudio.
            Audio_Enviar(0x25U);                                        // Restablece la música del menú si audio se reinició.
        }
        if (pantallaActual == JUEGO_ACTIVO &&                               // Comprueba una condición del jugador 1 antes de continuar.
            jugador1.vidas && jugador2.vidas &&                             // Añade esta condición a la comprobación actual.
            !jugador1.muriendo && !jugador2.muriendo &&                     // Comprueba si el jugador está reproduciendo la animación de muerte.
            ahora - ultimoEstadoAudio >= 1500U) {                           // Evalúa esta comparación como parte de la condición actual.
            ultimoEstadoAudio = ahora;                                      // Actualiza ultimoEstadoAudio.
            Audio_Enviar(musicaActual == MUSICA_MARTILLO ? 0x24U : 0x23U);// Mantiene la música de martillo o la normal.
        }

        /* ========================================================
         * MENU Y RECUPERACION DE MICROSD
         *
         * Mientras el juego permanece en el menú, la STM32 vuelve
         * a intentar montar la microSD cada 3 segundos si falló.
         * Esto permite recuperar el sistema sin reiniciar la placa.
         * ======================================================== */

        if (pantallaActual == JUEGO_MENU) {                             // Ejecuta este bloque mientras la consola permanezca en el menú.

            if (!sdDisponible &&                                       // Comprueba que la microSD siga sin estar disponible...
                ahora - ultimoIntentoSD >= 3000U) {                     // ...y que hayan pasado al menos 3 segundos desde el último intento.

                ultimoIntentoSD =
                    ahora;                                              // Registra el momento del nuevo intento para no repetirlo continuamente.

                LCD_Clear(COLOR_FONDO);                                 // Limpia el mensaje anterior antes de mostrar el estado de recuperación.

                LCD_Print("REINTENTANDO SD...", 72, 106, 1,
                          0xFFE0, COLOR_FONDO);                          // Informa en pantalla que se vuelve a intentar montar la SD.

                sdDisponible =
                    SD_Iniciar(&hspi1);                                 // Intenta recuperar la comunicación con la microSD.

                if (sdDisponible) {                                     // Continúa aquí cuando la microSD ya respondió correctamente.
                    sdPuntajesOk =
                        SD_Scores_Cargar(&recordMario,
                                         &recordLuigi);                  // Recupera los récords almacenados una vez restablecida la SD.

                    EntrarMenu();                                       // Vuelve a cargar el menú desde la microSD.
                } else {
                    MostrarFalloSD(0U);                                 // Mantiene visible el aviso de fallo si la SD todavía no responde.
                }

                ultimoIntentoSD =
                    HAL_GetTick();                                      // Reinicia el temporizador para esperar otros 3 segundos antes del siguiente intento.
            }

            if (comenzarSolicitado && sdDisponible)                     // Solo permite iniciar cuando se pidió comenzar y la SD está disponible.
                IniciarPartida();                                       // Reinicia variables y entra a una nueva partida.

            continue;                                                   // Evita ejecutar la lógica de juego mientras se permanece en el menú.
        }
        if (pantallaActual == JUEGO_RESCATE) {                              // Comprueba si se cumplen las condiciones necesarias para continuar.
            uint8_t visible = ((ahora - rescateDesde) / 180U) % 2U == 0U;   // Calcula y guarda visible.
            if (visible != corazonVisible) {                                // Comprueba si se cumplen las condiciones necesarias para continuar.
                corazonVisible = visible;                                   // Actualiza corazonVisible.
                RenderizarRegionFinal(CORAZON_X, CORAZON_Y, CORAZON_W, CORAZON_H); // Redibuja esta región con todas las capas finales.
            }
            if (ahora - rescateDesde >= RESCATE_DURACION_MS)                // Comprueba si se cumplen las condiciones necesarias para continuar.
                TerminarPartida(ganador);                               // Guarda resultados y muestra la pantalla final de la partida.
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.
        }
        if (pantallaActual == JUEGO_FIN) {                                  // Comprueba si se cumplen las condiciones necesarias para continuar.
            if (sdDisponible) {                                             // Comprueba si se cumplen las condiciones necesarias para continuar.
                uint8_t visible = ((ahora - finDesde) / 450U) % 2U == 0U;   // Calcula y guarda visible.
                if (visible != avisoFinalVisible) {                         // Comprueba si se cumplen las condiciones necesarias para continuar.
                    avisoFinalVisible = visible;                            // Actualiza avisoFinalVisible.
                    DibujarAvisoFinal(visible);                         // Limpia y, si corresponde, dibuja el mensaje intermitente que indica cómo volver al menú.
                }
            }
            if (comenzarSolicitado && ahora - finDesde >= FIN_MINIMA_MS)    // Comprueba si se cumplen las condiciones necesarias para continuar.
                EntrarMenu();                                           // Carga y muestra el menú inicial antes de entrar al bucle infinito.
            continue;                                                       // Omite este caso y continúa con la siguiente iteración.
        }

        if (                                                                // Comprueba si se cumplen las condiciones necesarias para continuar.
            ahora -
            ultimoTickFisica >=                                             // Evalúa esta comparación como parte de la condición actual.
            FISICA_MS
        ) {
            ultimoTickFisica =                                              // Actualiza ultimoTickFisica.
                ahora;                                                      // Continúa el cálculo de ultimoTickFisica.


            /* Paso jugador */
            uint8_t pasoJugador = 0;                                        // Reinicia pasoJugador a cero.

            if (                                                            // Comprueba si se cumplen las condiciones necesarias para continuar.
                ahora -
                ultimoMovimientoJugador >=                                  // Evalúa esta comparación como parte de la condición actual.
                MOVIMIENTO_JUGADOR_MS
            ) {
                ultimoMovimientoJugador =                                   // Actualiza ultimoMovimientoJugador.
                    ahora;                                                  // Continúa el cálculo de ultimoMovimientoJugador.

                pasoJugador = 1;                                            // Activa pasoJugador.
            }


            /* Paso barril */
            uint8_t pasoBarril = 0;                                         // Reinicia pasoBarril a cero.

            if (                                                            // Comprueba si se cumplen las condiciones necesarias para continuar.
                ahora -
                ultimoMovimientoBarril >=                                   // Evalúa esta comparación como parte de la condición actual.
                MOVIMIENTO_BARRIL_MS
            ) {
                ultimoMovimientoBarril =                                    // Actualiza ultimoMovimientoBarril.
                    ahora;                                                  // Continúa el cálculo de ultimoMovimientoBarril.

                pasoBarril = 1;                                             // Activa pasoBarril.
            }


            /* Paso flama */
            uint8_t pasoFireball = 0;                                       // Reinicia pasoFireball a cero.

            if (                                                            // Comprueba si se cumplen las condiciones necesarias para continuar.
                ahora -
                ultimoMovimientoFireball >=                                 // Evalúa esta comparación como parte de la condición actual.
                MOVIMIENTO_FIREBALL_MS
            ) {
                ultimoMovimientoFireball =                                  // Actualiza ultimoMovimientoFireball.
                    ahora;                                                  // Continúa el cálculo de ultimoMovimientoFireball.

                pasoFireball = 1;                                           // Activa pasoFireball.
            }


            /* Guardar jugadores */
            int16_t oldJ1X = jugador1.x;                                    // Calcula y guarda oldJ1X.
            int16_t oldJ1Y = jugador1.y;                                    // Calcula y guarda oldJ1Y.

            int16_t oldJ2X = jugador2.x;                                    // Calcula y guarda oldJ2X.
            int16_t oldJ2Y = jugador2.y;                                    // Calcula y guarda oldJ2Y.

            uint16_t oldJ1Color =                                           // Calcula y guarda oldJ1Color.
                jugador1.color;                                             // Continúa el cálculo de oldJ1Color.

            uint16_t oldJ2Color =                                           // Calcula y guarda oldJ2Color.
                jugador2.color;                                             // Continúa el cálculo de oldJ2Color.

            uint8_t oldJ1MartilloActivo =                                   // Calcula y guarda oldJ1MartilloActivo.
                jugador1.martilloActivo;                                    // Comprueba si el jugador tiene el martillo activo.

            uint8_t oldJ2MartilloActivo =                                   // Calcula y guarda oldJ2MartilloActivo.
                jugador2.martilloActivo;                                    // Comprueba si el jugador tiene el martillo activo.

            uint8_t oldJ1FaseMartillo =                                     // Calcula y guarda oldJ1FaseMartillo.
                jugador1.faseMartillo;                                      // Continúa el cálculo de oldJ1FaseMartillo.

            uint8_t oldJ2FaseMartillo =                                     // Calcula y guarda oldJ2FaseMartillo.
                jugador2.faseMartillo;                                      // Continúa el cálculo de oldJ2FaseMartillo.

            EstadoSpriteJugador oldJ1EstadoSprite =                         // Actualiza oldJ1EstadoSprite.
                jugador1.estadoSprite;                                      // Continúa el cálculo de oldJ1EstadoSprite.

            EstadoSpriteJugador oldJ2EstadoSprite =                         // Actualiza oldJ2EstadoSprite.
                jugador2.estadoSprite;                                      // Continúa el cálculo de oldJ2EstadoSprite.
            uint8_t oldJ1Letrero = jugador1.letreroVisible;                 // Calcula y guarda oldJ1Letrero.
            uint8_t oldJ2Letrero = jugador2.letreroVisible;                 // Calcula y guarda oldJ2Letrero.
            uint16_t oldJ1LetreroValor = jugador1.letreroValor;             // Calcula y guarda oldJ1LetreroValor.
            uint16_t oldJ2LetreroValor = jugador2.letreroValor;             // Calcula y guarda oldJ2LetreroValor.
            int16_t oldAvisoX[2] = {                                        // Calcula y guarda oldAvisoX.
                jugador1.letreroFijo ? jugador1.letreroX : jugador1.x - 2,  // Continúa el cálculo de oldAvisoX.
                jugador2.letreroFijo ? jugador2.letreroX : jugador2.x - 2   // Continúa el cálculo de oldAvisoX.
            };                                                              // Continúa el cálculo de oldAvisoX.
            int16_t oldAvisoY[2] = {                                        // Calcula y guarda oldAvisoY.
                jugador1.letreroFijo ? jugador1.letreroY : jugador1.y - PUNTOS_H - 2, // Continúa el cálculo de oldAvisoY.
                jugador2.letreroFijo ? jugador2.letreroY : jugador2.y - PUNTOS_H - 2 // Continúa el cálculo de oldAvisoY.
            };                                                              // Continúa el cálculo de oldAvisoY.


            /* Guardar martillos del escenario */
            EstadoMartillo oldMEstado[MAX_MARTILLOS];                       // Accede a la posición correspondiente del arreglo oldMEstado.

            for (int i = 0; i < MAX_MARTILLOS; i++) {                       // Recorre los dos martillos del escenario.
                oldMEstado[i] =                                             // Actualiza oldMEstado.
                    martillos[i].estado;                                    // Accede a la posición correspondiente del arreglo martillos.
            }


            /* Guardar barriles */
            int16_t oldBX[MAX_BARRILES];                                    // Reserva oldBX para usarla en este bloque.
            int16_t oldBY[MAX_BARRILES];                                    // Reserva oldBY para usarla en este bloque.
            uint8_t oldBActivo[MAX_BARRILES];                               // Reserva oldBActivo para usarla en este bloque.

            for (int i = 0; i < MAX_BARRILES; i++) {                        // Recorre todos los slots de barriles.
                oldBX[i] = barriles[i].x;                                   // Actualiza oldBX.
                oldBY[i] = barriles[i].y;                                   // Actualiza oldBY.
                oldBActivo[i] = barriles[i].activo;                         // Actualiza oldBActivo.
            }


            /* Guardar flamas */
            int16_t oldFX[MAX_FIREBALLS];                                   // Reserva oldFX para usarla en este bloque.
            int16_t oldFY[MAX_FIREBALLS];                                   // Reserva oldFY para usarla en este bloque.
            uint8_t oldFActivo[MAX_FIREBALLS];                              // Reserva oldFActivo para usarla en este bloque.
            EstadoFlama oldFEstado[MAX_FIREBALLS];                          // Accede a la posición correspondiente del arreglo oldFEstado.

            for (int i = 0; i < MAX_FIREBALLS; i++) {                       // Recorre todos los slots de flamas.
                oldFX[i] = fireballs[i].x;                                  // Actualiza oldFX.
                oldFY[i] = fireballs[i].y;                                  // Actualiza oldFY.
                oldFActivo[i] = fireballs[i].activo;                        // Actualiza oldFActivo.
                oldFEstado[i] = fireballs[i].estado;                        // Actualiza oldFEstado.
            }


            /* Mecánicas */

            ActualizarMartilloJugador(                                      // Ejecuta ActualizarMartilloJugador().
                &jugador1                                                   // Entrega este parámetro a ActualizarMartilloJugador().
            );                                                              // Finaliza la llamada a ActualizarMartilloJugador().

            ActualizarMartilloJugador(                                      // Ejecuta ActualizarMartilloJugador().
                &jugador2                                                   // Entrega este parámetro a ActualizarMartilloJugador().
            );                                                              // Finaliza la llamada a ActualizarMartilloJugador().

            ActualizarJugador(                                              // Ejecuta ActualizarJugador().
                &jugador1,                                                  // Entrega este parámetro a ActualizarJugador().
                pasoJugador                                                 // Entrega este parámetro a ActualizarJugador().
            );                                                              // Finaliza la llamada a ActualizarJugador().

            ActualizarJugador(                                              // Ejecuta ActualizarJugador().
                &jugador2,                                                  // Entrega este parámetro a ActualizarJugador().
                pasoJugador                                                 // Entrega este parámetro a ActualizarJugador().
            );                                                              // Finaliza la llamada a ActualizarJugador().

            ComprobarRecogidaMartillos();                               // Detecta colisión de cada jugador con los pickups disponibles y activa el martillo cuando corresponde.

            ActualizarEstadoSpriteJugador(                                  // Ejecuta ActualizarEstadoSpriteJugador().
                &jugador1                                                   // Entrega este parámetro a ActualizarEstadoSpriteJugador().
            );                                                              // Finaliza la llamada a ActualizarEstadoSpriteJugador().

            ActualizarEstadoSpriteJugador(                                  // Ejecuta ActualizarEstadoSpriteJugador().
                &jugador2                                                   // Entrega este parámetro a ActualizarEstadoSpriteJugador().
            );                                                              // Finaliza la llamada a ActualizarEstadoSpriteJugador().

            ActualizarBarriles(                                             // Actualiza todos los barriles activos.
                pasoBarril                                                  // Entrega este parámetro a ActualizarBarriles().
            );                                                              // Finaliza la llamada a ActualizarBarriles().

            ActualizarFireballs(                                            // Actualiza todas las flamas activas.
                pasoFireball                                                // Entrega este parámetro a ActualizarFireballs().
            );                                                              // Finaliza la llamada a ActualizarFireballs().
            uint8_t anteriorCuadroFlama = cuadroFlamaActual;                // Calcula y guarda anteriorCuadroFlama.
            cuadroFlamaActual = (uint8_t)((ahora / 130U) & 3U);             // Actualiza cuadroFlamaActual.


            /* Colisiones */
            ComprobarGolpesMartillo();                                  // Detecta golpes del martillo contra barriles y flamas.
            ComprobarColisionesBarriles();                              // Gestiona daño por contacto con barriles y recompensa de +100 cuando un jugador los salta correctamente.
            ComprobarColisionesFireballs();                             // Comprueba las cajas de cada flama contra ambos jugadores y aplica daño si colisionan.
            ActualizarGolpes();                                         // Termina el efecto visual de golpe al vencer su temporizador.
            ActualizarMusicaMartillo();                                 // Mantiene la música de martillo o vuelve a la normal.

            /* La imagen RGB565 cambia de cuadro sin desplazar al personaje. */
            Jugador *muertos[2] = { &jugador1, &jugador2 };                 // Calcula y guarda muertos.
            for (int p = 0; p < 2; p++) {                                   // Recorre todas las posiciones necesarias de este bloque.
                Jugador *j = muertos[p];                                    // Calcula y guarda j.
                if (j->muriendo) {                                          // Comprueba si se cumplen las condiciones necesarias para continuar.
                    if (!j->enSuelo) {                                      // Comprueba si se cumplen las condiciones necesarias para continuar.
                        int piesAntes = j->y + j->h;                        // Calcula y guarda piesAntes.
                        if (j->vy < MAX_CAIDA) j->vy++;                     // Comprueba si se cumplen las condiciones necesarias para continuar.
                        int nuevoY = j->y + j->vy;                          // Calcula y guarda nuevoY.
                        int piso = BuscarPlataformaCaida(j, piesAntes, nuevoY + j->h); // Calcula y guarda plataforma usada como piso.
                        if (piso >= 0) {                                    // Comprueba una condición de la plataforma antes de continuar.
                            j->plataformaActual = piso;                 // Índice de la plataforma actual; -1 indica que no está sobre una plataforma.
                            j->y = ObtenerYPlataforma(                      // Actualiza la posición vertical del objeto.
                                &plataformas[piso], j->x + j->w / 2) - j->h; // Calcula la altura exacta de la viga inclinada en esta coordenada X.
                            j->enSuelo = 1;                             // Marca si el jugador está apoyado sobre una plataforma.
                            j->vy = 0;                                  // Velocidad vertical actual.
                        } else if (nuevoY >= SCREEN_H - j->h) {             // Comprueba que la coordenada permanezca dentro del alto de la pantalla.
                            j->y = SCREEN_H - j->h;                         // Actualiza la posición vertical del objeto.
                            j->enSuelo = 1;                             // Marca si el jugador está apoyado sobre una plataforma.
                        } else j->y = nuevoY;                               // Actualiza la posición vertical del objeto.
                    }
                    uint32_t cuadro = (ahora - j->muerteDesde) / MUERTE_FRAME_MS; // Calcula y guarda cuadro.
                    if (cuadro >= MUERTE_CUADROS) cuadro = MUERTE_CUADROS - 1U; // Comprueba si se cumplen las condiciones necesarias para continuar.
                    if (j->muerteCuadro != cuadro) {                        // Comprueba si se cumplen las condiciones necesarias para continuar.
                        j->muerteCuadro = (uint8_t)cuadro;              // Cuadro actual de la animación de muerte.
                        RenderizarRegionFinal(j->x, j->y, JUGADOR_W, JUGADOR_H); // Redibuja esta región con todas las capas finales.
                    }
                }
            }

            if (jugador1.letreroVisible &&                                  // Comprueba una condición del jugador 1 antes de continuar.
                (int32_t)(ahora - jugador1.letreroHasta) >= 0)              // Evalúa esta comparación como parte de la condición actual.
                jugador1.letreroVisible = 0;                            // Marca si debe dibujarse el popup de puntos.
            if (jugador2.letreroVisible &&                                  // Comprueba una condición del jugador 2 antes de continuar.
                (int32_t)(ahora - jugador2.letreroHasta) >= 0)              // Evalúa esta comparación como parte de la condición actual.
                jugador2.letreroVisible = 0;                            // Marca si debe dibujarse el popup de puntos.

            uint8_t parpadeoHelp = ((ahora / 450U) & 1U) == 0U;             // Calcula y guarda parpadeoHelp.
            if (parpadeoHelp != ayudaVisible) {                             // Comprueba si se cumplen las condiciones necesarias para continuar.
                ayudaVisible = parpadeoHelp;                                // Actualiza ayudaVisible.
                RenderizarRegionFinal(HELP_X, HELP_Y, HELP_W, HELP_H);  // Redibuja esta región con todas las capas finales.
            }


            /*
             * Si el barril de aceite acaba de cambiar de
             * ACEITE_APAGADO a ACEITE_ENCENDIDO, recompone toda
             * su zona visual, incluida la llama superior.
             */
            if (                                                            // Comprueba si se cumplen las condiciones necesarias para continuar.
                aceiteNecesitaRedibujar
            ) {
                RenderizarRegionFinal(                                  // Redibuja esta región con todas las capas finales.
                    ACEITE_X,                                               // Indica X inicial de la región.
                    AceiteY() - ACEITE_FUEGO_H,                             // Indica Y inicial de la región.
                    ACEITE_W,                                               // Indica ancho de la región.
                    ACEITE_H + ACEITE_FUEGO_H                               // Indica alto de la región.
                );                                                          // Finaliza la llamada a RenderizarRegionFinal().

                aceiteNecesitaRedibujar =                                   // Actualiza aceiteNecesitaRedibujar.
                    0;                                                      // Continúa el cálculo de aceiteNecesitaRedibujar.
            }


            /*
             * Un martillo disponible desaparece al ser recogido.
             * Recompone su zona directamente con el estado final.
             */
            for (int i = 0; i < MAX_MARTILLOS; i++) {                       // Recorre los dos martillos del escenario.
                if (                                                        // Comprueba una condición del martillo antes de continuar.
                    oldMEstado[i] !=                                        // Evalúa esta comparación como parte de la condición actual.
                    martillos[i].estado                                     // Accede a la posición correspondiente del arreglo martillos.
                ) {
                    RenderizarRegionFinal(                              // Redibuja esta región con todas las capas finales.
                        martillos[i].x,                                     // Indica X inicial de la región.
                        martillos[i].y,                                     // Indica Y inicial de la región.
                        martillos[i].w,                                     // Indica ancho de la región.
                        martillos[i].h                                      // Indica alto de la región.
                    );                                                      // Finaliza la llamada a RenderizarRegionFinal().
                }
            }


            /* =================================================
             * RENDER DIRTY-RECT FINAL
             * ================================================= */


            /* JUGADOR 1 + MARTILLO */
            if (                                                            // Comprueba una condición del jugador 1 antes de continuar.
                oldJ1X != jugador1.x ||                                     // Añade esta condición a la comprobación actual.
                oldJ1Y != jugador1.y ||                                     // Añade esta condición a la comprobación actual.
                oldJ1Color != jugador1.color ||                             // Añade esta condición a la comprobación actual.
                oldJ1MartilloActivo != jugador1.martilloActivo ||           // Comprueba si el jugador tiene el martillo activo.
                oldJ1FaseMartillo != jugador1.faseMartillo ||               // Añade esta condición a la comprobación actual.
                oldJ1Letrero != jugador1.letreroVisible ||                  // Añade esta condición a la comprobación actual.
                oldJ1LetreroValor != jugador1.letreroValor ||               // Añade esta condición a la comprobación actual.
                oldJ1EstadoSprite != jugador1.estadoSprite                  // Evalúa esta comparación como parte de la condición actual.
            ) {
                int x1 =                                                    // Calcula y guarda x1.
                    ((oldJ1X < jugador1.x)                                  // Evalúa esta comparación como parte de la condición actual.
                     ? oldJ1X                                               // Usa esta opción cuando la condición del operador ternario es verdadera.
                     : jugador1.x)                                          // Usa esta opción cuando la condición del operador ternario es falsa.
                    - MARTILLO_RENDER_PADDING;                              // Continúa el cálculo de x1.

                int y1 =                                                    // Calcula y guarda y1.
                    ((oldJ1Y < jugador1.y)                                  // Evalúa esta comparación como parte de la condición actual.
                     ? oldJ1Y                                               // Usa esta opción cuando la condición del operador ternario es verdadera.
                     : jugador1.y)                                          // Usa esta opción cuando la condición del operador ternario es falsa.
                    - MARTILLO_RENDER_PADDING;                              // Continúa el cálculo de y1.

                int oldX2 =                                                 // Calcula y guarda oldX2.
                    oldJ1X +                                                // Continúa el cálculo de oldX2.
                    jugador1.w +                                            // Continúa el cálculo de oldX2.
                    MARTILLO_RENDER_PADDING;                                // Continúa el cálculo de oldX2.

                int newX2 =                                                 // Calcula y guarda newX2.
                    jugador1.x +                                            // Continúa el cálculo de newX2.
                    jugador1.w +                                            // Continúa el cálculo de newX2.
                    MARTILLO_RENDER_PADDING;                                // Continúa el cálculo de newX2.

                int oldY2 =                                                 // Calcula y guarda oldY2.
                    oldJ1Y +                                                // Continúa el cálculo de oldY2.
                    jugador1.h +                                            // Continúa el cálculo de oldY2.
                    MARTILLO_RENDER_PADDING;                                // Continúa el cálculo de oldY2.

                int newY2 =                                                 // Calcula y guarda newY2.
                    jugador1.y +                                            // Continúa el cálculo de newY2.
                    jugador1.h +                                            // Continúa el cálculo de newY2.
                    MARTILLO_RENDER_PADDING;                                // Continúa el cálculo de newY2.

                int x2 =                                                    // Calcula y guarda x2.
                    (oldX2 > newX2)                                         // Evalúa esta comparación como parte de la condición actual.
                    ? oldX2                                                 // Usa esta opción cuando la condición del operador ternario es verdadera.
                    : newX2;                                                // Usa esta opción cuando la condición del operador ternario es falsa.

                int y2 =                                                    // Calcula y guarda y2.
                    (oldY2 > newY2)                                         // Evalúa esta comparación como parte de la condición actual.
                    ? oldY2                                                 // Usa esta opción cuando la condición del operador ternario es verdadera.
                    : newY2;                                                // Usa esta opción cuando la condición del operador ternario es falsa.

                RenderizarRegionFinal(                                  // Redibuja esta región con todas las capas finales.
                    x1,                                                     // Indica X inicial de la región.
                    y1,                                                     // Indica Y inicial de la región.
                    x2 - x1,                                                // Indica ancho de la región.
                    y2 - y1                                                 // Indica alto de la región.
                );                                                          // Finaliza la llamada a RenderizarRegionFinal().
            }


            /* JUGADOR 2 + MARTILLO */
            if (                                                            // Comprueba una condición del jugador 2 antes de continuar.
                oldJ2X != jugador2.x ||                                     // Añade esta condición a la comprobación actual.
                oldJ2Y != jugador2.y ||                                     // Añade esta condición a la comprobación actual.
                oldJ2Color != jugador2.color ||                             // Añade esta condición a la comprobación actual.
                oldJ2MartilloActivo != jugador2.martilloActivo ||           // Comprueba si el jugador tiene el martillo activo.
                oldJ2FaseMartillo != jugador2.faseMartillo ||               // Añade esta condición a la comprobación actual.
                oldJ2Letrero != jugador2.letreroVisible ||                  // Añade esta condición a la comprobación actual.
                oldJ2LetreroValor != jugador2.letreroValor ||               // Añade esta condición a la comprobación actual.
                oldJ2EstadoSprite != jugador2.estadoSprite                  // Evalúa esta comparación como parte de la condición actual.
            ) {
                int x1 =                                                    // Calcula y guarda x1.
                    ((oldJ2X < jugador2.x)                                  // Evalúa esta comparación como parte de la condición actual.
                     ? oldJ2X                                               // Usa esta opción cuando la condición del operador ternario es verdadera.
                     : jugador2.x)                                          // Usa esta opción cuando la condición del operador ternario es falsa.
                    - MARTILLO_RENDER_PADDING;                              // Continúa el cálculo de x1.

                int y1 =                                                    // Calcula y guarda y1.
                    ((oldJ2Y < jugador2.y)                                  // Evalúa esta comparación como parte de la condición actual.
                     ? oldJ2Y                                               // Usa esta opción cuando la condición del operador ternario es verdadera.
                     : jugador2.y)                                          // Usa esta opción cuando la condición del operador ternario es falsa.
                    - MARTILLO_RENDER_PADDING;                              // Continúa el cálculo de y1.

                int oldX2 =                                                 // Calcula y guarda oldX2.
                    oldJ2X +                                                // Continúa el cálculo de oldX2.
                    jugador2.w +                                            // Continúa el cálculo de oldX2.
                    MARTILLO_RENDER_PADDING;                                // Continúa el cálculo de oldX2.

                int newX2 =                                                 // Calcula y guarda newX2.
                    jugador2.x +                                            // Continúa el cálculo de newX2.
                    jugador2.w +                                            // Continúa el cálculo de newX2.
                    MARTILLO_RENDER_PADDING;                                // Continúa el cálculo de newX2.

                int oldY2 =                                                 // Calcula y guarda oldY2.
                    oldJ2Y +                                                // Continúa el cálculo de oldY2.
                    jugador2.h +                                            // Continúa el cálculo de oldY2.
                    MARTILLO_RENDER_PADDING;                                // Continúa el cálculo de oldY2.

                int newY2 =                                                 // Calcula y guarda newY2.
                    jugador2.y +                                            // Continúa el cálculo de newY2.
                    jugador2.h +                                            // Continúa el cálculo de newY2.
                    MARTILLO_RENDER_PADDING;                                // Continúa el cálculo de newY2.

                int x2 =                                                    // Calcula y guarda x2.
                    (oldX2 > newX2)                                         // Evalúa esta comparación como parte de la condición actual.
                    ? oldX2                                                 // Usa esta opción cuando la condición del operador ternario es verdadera.
                    : newX2;                                                // Usa esta opción cuando la condición del operador ternario es falsa.

                int y2 =                                                    // Calcula y guarda y2.
                    (oldY2 > newY2)                                         // Evalúa esta comparación como parte de la condición actual.
                    ? oldY2                                                 // Usa esta opción cuando la condición del operador ternario es verdadera.
                    : newY2;                                                // Usa esta opción cuando la condición del operador ternario es falsa.

                RenderizarRegionFinal(                                  // Redibuja esta región con todas las capas finales.
                    x1,                                                     // Indica X inicial de la región.
                    y1,                                                     // Indica Y inicial de la región.
                    x2 - x1,                                                // Indica ancho de la región.
                    y2 - y1                                                 // Indica alto de la región.
                );                                                          // Finaliza la llamada a RenderizarRegionFinal().
            }


            /* BARRILES */
            for (                                                           // Recorre todos los slots de barriles.
                int i = 0;                                                  // Reinicia i a cero.
                i < MAX_BARRILES;                                           // Mantiene el recorrido dentro de todos los slots de barriles.
                i++                                                         // Incrementa i en una unidad.
            ) {
                if (                                                        // Comprueba una condición del barril antes de continuar.
                    oldBActivo[i] &&                                        // Añade esta condición a la comprobación actual.
                    barriles[i].activo                                      // Comprueba que el barril esté activo.
                ) {
                    if (                                                    // Comprueba una condición del barril antes de continuar.
                        oldBX[i] !=                                         // Evalúa esta comparación como parte de la condición actual.
                        barriles[i].x ||                                    // Añade esta condición a la comprobación actual.
                        oldBY[i] !=                                         // Evalúa esta comparación como parte de la condición actual.
                        barriles[i].y                                       // Accede a la posición correspondiente del arreglo barriles.
                    ) {
                        RenderizarUnionMovimiento(                      // Redibuja la unión de la posición anterior y la nueva.
                            oldBX[i],                                       // Indica X anterior.
                            oldBY[i],                                       // Indica Y anterior.
                            barriles[i].x,                                  // Indica X nueva.
                            barriles[i].y,                                  // Indica Y nueva.
                            BARRIL_W,                                       // Indica ancho del objeto.
                            BARRIL_H                                        // Indica alto del objeto.
                        );                                                  // Finaliza la llamada a RenderizarUnionMovimiento().
                    }
                }

                else if (                                                   // Evalúa otra condición del barril.
                    oldBActivo[i] &&                                        // Añade esta condición a la comprobación actual.
                    !barriles[i].activo                                     // Comprueba que el barril esté activo.
                ) {

                    RenderizarRegionFinal(                              // Redibuja esta región con todas las capas finales.
                        oldBX[i],                                           // Indica X inicial de la región.
                        oldBY[i],                                           // Indica Y inicial de la región.
                        BARRIL_W,                                           // Indica ancho de la región.
                        BARRIL_H                                            // Indica alto de la región.
                    );                                                      // Finaliza la llamada a RenderizarRegionFinal().
                }

                else if (                                                   // Evalúa otra condición del barril.
                    !oldBActivo[i] &&                                       // Añade esta condición a la comprobación actual.
                    barriles[i].activo                                      // Comprueba que el barril esté activo.
                ) {
                    /*
                     * Barril recién creado.
                     */
                    RenderizarRegionFinal(                              // Redibuja esta región con todas las capas finales.
                        barriles[i].x,                                      // Indica X inicial de la región.
                        barriles[i].y,                                      // Indica Y inicial de la región.
                        BARRIL_W,                                           // Indica ancho de la región.
                        BARRIL_H                                            // Indica alto de la región.
                    );                                                      // Finaliza la llamada a RenderizarRegionFinal().
                }
            }


            /* FLAMAS */
            for (                                                           // Recorre todos los slots de flamas.
                int i = 0;                                                  // Reinicia i a cero.
                i < MAX_FIREBALLS;                                          // Mantiene el recorrido dentro de todos los slots de flamas.
                i++                                                         // Incrementa i en una unidad.
            ) {
                if (                                                        // Comprueba una condición de la flama antes de continuar.
                    oldFActivo[i] &&                                        // Añade esta condición a la comprobación actual.
                    fireballs[i].activo                                     // Comprueba que la flama esté activo.
                ) {
                    if (                                                    // Comprueba una condición de la flama antes de continuar.
                        oldFX[i] !=                                         // Evalúa esta comparación como parte de la condición actual.
                        fireballs[i].x ||                                   // Añade esta condición a la comprobación actual.
                        oldFY[i] !=                                         // Evalúa esta comparación como parte de la condición actual.
                        fireballs[i].y ||                                   // Añade esta condición a la comprobación actual.
                        oldFEstado[i] !=                                    // Evalúa esta comparación como parte de la condición actual.
                        fireballs[i].estado                                 // Accede a la posición correspondiente del arreglo fireballs.
                    ) {
                        RenderizarUnionMovimiento(                      // Redibuja la unión de la posición anterior y la nueva.
                            oldFX[i],                                       // Indica X anterior.
                            oldFY[i],                                       // Indica Y anterior.
                            fireballs[i].x,                                 // Indica X nueva.
                            fireballs[i].y,                                 // Indica Y nueva.
                            FIREBALL_W,                                     // Indica ancho del objeto.
                            FIREBALL_H                                      // Indica alto del objeto.
                        );                                                  // Finaliza la llamada a RenderizarUnionMovimiento().
                    }
                }

                else if (                                                   // Evalúa otra condición de la flama.
                    oldFActivo[i] &&                                        // Añade esta condición a la comprobación actual.
                    !fireballs[i].activo                                    // Comprueba que la flama esté activo.
                ) {
                    RenderizarRegionFinal(                              // Redibuja esta región con todas las capas finales.
                        oldFX[i],                                           // Indica X inicial de la región.
                        oldFY[i],                                           // Indica Y inicial de la región.
                        FIREBALL_W,                                         // Indica ancho de la región.
                        FIREBALL_H                                          // Indica alto de la región.
                    );                                                      // Finaliza la llamada a RenderizarRegionFinal().
                }

                else if (                                                   // Evalúa otra condición de la flama.
                    !oldFActivo[i] &&                                       // Añade esta condición a la comprobación actual.
                    fireballs[i].activo                                     // Comprueba que la flama esté activo.
                ) {
                    /*
                     * Nueva flama nacida por contacto
                     * del barril azul con el aceite.
                     */
                    RenderizarRegionFinal(                              // Redibuja esta región con todas las capas finales.
                        fireballs[i].x,                                     // Indica X inicial de la región.
                        fireballs[i].y,                                     // Indica Y inicial de la región.
                        FIREBALL_W,                                         // Indica ancho de la región.
                        FIREBALL_H                                          // Indica alto de la región.
                    );                                                      // Finaliza la llamada a RenderizarRegionFinal().
                }
                else if (fireballs[i].activo && anteriorCuadroFlama != cuadroFlamaActual && // Evalúa otra condición de la flama.
                         fireballs[i].estado != FLAMA_NACIENDO) {           // Evalúa esta comparación como parte de la condición actual.
                    /* Cambiar de sprite aunque la flama no se desplace. */
                    RenderizarRegionFinal(fireballs[i].x, fireballs[i].y, // Redibuja esta región con todas las capas finales.
                                          FIREBALL_W, FIREBALL_H);          // Indica X inicial de la región.
                }
            }

            /* Redibujar el +100/+200 en su posicion propia, incluso si el
             * barril se destruyo lejos del centro del jugador o el aviso
             * acaba de desaparecer. */
            Jugador *avisos[2] = { &jugador1, &jugador2 };                  // Calcula y guarda avisos.
            uint8_t visiblesAntes[2] = { oldJ1Letrero, oldJ2Letrero };      // Calcula y guarda visiblesAntes.
            uint16_t valoresAntes[2] = { oldJ1LetreroValor, oldJ2LetreroValor }; // Calcula y guarda valoresAntes.
            for (int p = 0; p < 2; p++) {                                   // Recorre todas las posiciones necesarias de este bloque.
                Jugador *j = avisos[p];                                     // Calcula y guarda j.
                int nuevoX = j->letreroFijo ? j->letreroX : j->x - 2;       // Calcula y guarda nuevoX.
                int nuevoY = j->letreroFijo ? j->letreroY :                 // Calcula y guarda nuevoY.
                             j->y - PUNTOS_H - 2;                           // Evalúa esta comparación como parte de la condición actual.
                if (visiblesAntes[p] != j->letreroVisible ||                // Comprueba si se cumplen las condiciones necesarias para continuar.
                    (j->letreroVisible &&                                   // Añade esta condición a la comprobación actual.
                     (oldAvisoX[p] != nuevoX || oldAvisoY[p] != nuevoY ||   // Añade esta condición a la comprobación actual.
                      valoresAntes[p] != j->letreroValor))) {               // Evalúa esta comparación como parte de la condición actual.
                    if (visiblesAntes[p])                                   // Comprueba si se cumplen las condiciones necesarias para continuar.
                        RenderizarRegionFinal(oldAvisoX[p], oldAvisoY[p], // Redibuja esta región con todas las capas finales.
                                             PUNTOS_W, PUNTOS_H);           // Indica X inicial de la región.
                    if (j->letreroVisible)                                  // Comprueba si se cumplen las condiciones necesarias para continuar.
                        RenderizarRegionFinal(nuevoX, nuevoY,           // Redibuja esta región con todas las capas finales.
                                             PUNTOS_W, PUNTOS_H);           // Indica X inicial de la región.
                }                                                           // Continúa el cálculo de nuevoY.
            }                                                               // Continúa el cálculo de nuevoY.
        }                                                                   // Continúa el cálculo de nuevoY.


        /* Animación DK */
        ActualizarDK();                                                 // Avanza la animación de DK, libera el barril en el frame definido y programa el siguiente lanzamiento.

        if (pantallaActual == JUEGO_ACTIVO) {                               // Comprueba una condición del jugador 1 antes de continuar.
            Jugador *js[2] = { &jugador1, &jugador2 };                      // Calcula y guarda js.
            for (int p = 0; p < 2; p++) {                                   // Recorre todas las posiciones necesarias de este bloque.
                Jugador *j = js[p];                                         // Calcula y guarda j.
                if (j->muriendo && ahora - j->muerteDesde >= MUERTE_DURACION_MS) { // Comprueba si se cumplen las condiciones necesarias para continuar.
                    if (j->vidas) ReaparecerJugador(j);                     // Comprueba si se cumplen las condiciones necesarias para continuar.
                    else j->muriendo = 0; /* ultimo cuadro hasta cambiar pantalla */ // Usa esta alternativa cuando la condición anterior no se cumple.
                }
            }
            uint8_t muertePendiente = jugador1.muriendo || jugador2.muriendo; // Calcula y guarda muertePendiente.
            for (int p = 0; p < 2; p++) {                                   // Recorre todas las posiciones necesarias de este bloque.
                Jugador *j = js[p];                                         // Calcula y guarda j.
                if (!muertePendiente && jugador1.vidas && jugador2.vidas && // Comprueba si se cumplen las condiciones necesarias para continuar.
                    j->vidas && j->enSuelo && j->plataformaActual == 0 &&   // Comprueba si el jugador está apoyado sobre una plataforma.
                    j->x < PEACH_X + PEACH_W && j->x + j->w > PEACH_X) {    // Evalúa esta comparación como parte de la condición actual.
                    j->puntos += PUNTOS_RESCATE;                            // Aumenta puntuación con este valor.
                    IniciarRescate((uint8_t)(p + 1), ahora);            // Entra al estado de rescate, guarda al ganador, muestra el corazón y activa el audio de victoria.
                    break;                                                  // Sale de este caso o bucle y continúa después del bloque.
                }
            }
            if (pantallaActual == JUEGO_ACTIVO && !muertePendiente &&       // Comprueba una condición del jugador 1 antes de continuar.
                (!jugador1.vidas || !jugador2.vidas)) {
                uint8_t quien = jugador1.vidas ? 1U :                       // Calcula y guarda quien.
                    (jugador2.vidas ? 2U :                                  // Continúa el cálculo de quien.
                    (jugador1.puntos > jugador2.puntos ? 1U :               // Evalúa esta comparación como parte de la condición actual.
                     (jugador2.puntos > jugador1.puntos ? 2U : 0U)));       // Evalúa esta comparación como parte de la condición actual.
                TerminarPartida(quien);                                 // Guarda resultados y muestra la pantalla final de la partida.
            }                                                               // Continúa el cálculo de quien.
            if (pantallaActual == JUEGO_ACTIVO && hudSucio) {               // Comprueba si se cumplen las condiciones necesarias para continuar.
                DibujarMarcador();                                      // Actualiza el HUD de vidas y puntuación de ambos jugadores.
                hudSucio = 0;                                               // Reinicia hudSucio a cero.
            }
        }

        /* USER CODE END WHILE */
    }

    /* USER CODE BEGIN 3 */
    /* USER CODE END 3 */
}


/* ============================================================
 * RELOJ DEL STM32
 *
 * Se configura la frecuencia de trabajo del microcontrolador y los buses internos.
 * El reloj resultante alimenta la CPU y los periféricos usados por el juego.
 * ============================================================ */

void SystemClock_Config(void)                                           // Configura el reloj del STM32 usando HSI y PLL, incluyendo divisores AHB/APB y latencia de Flash.
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};                             // Actualiza RCC_OscInitStruct.
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};                             // Actualiza RCC_ClkInitStruct.

    __HAL_RCC_PWR_CLK_ENABLE();                                             // Ejecuta __HAL_RCC_PWR_CLK_ENABLE().
    __HAL_PWR_VOLTAGESCALING_CONFIG(                                        // Ejecuta __HAL_PWR_VOLTAGESCALING_CONFIG().
        PWR_REGULATOR_VOLTAGE_SCALE3                                        // Entrega este parámetro a __HAL_PWR_VOLTAGESCALING_CONFIG().
    );                                                                      // Finaliza la llamada a __HAL_PWR_VOLTAGESCALING_CONFIG().

    RCC_OscInitStruct.OscillatorType =                                      // Actualiza OscillatorType.
        RCC_OSCILLATORTYPE_HSI;                                         // Selecciona el oscilador interno HSI.

    RCC_OscInitStruct.HSIState =                                            // Actualiza HSIState.
        RCC_HSI_ON;                                                     // Enciende el oscilador HSI.

    RCC_OscInitStruct.HSICalibrationValue =                                 // Actualiza HSICalibrationValue.
        RCC_HSICALIBRATION_DEFAULT;                                         // Continúa el cálculo de HSICalibrationValue.

    RCC_OscInitStruct.PLL.PLLState =                                        // Actualiza PLLState.
        RCC_PLL_ON;                                                         // Continúa el cálculo de PLLState.

    RCC_OscInitStruct.PLL.PLLSource =                                       // Actualiza PLLSource.
        RCC_PLLSOURCE_HSI;                                                  // Continúa el cálculo de PLLSource.

    RCC_OscInitStruct.PLL.PLLM = 8;                                         // Actualiza PLLM.
    RCC_OscInitStruct.PLL.PLLN = 80;                                        // Actualiza PLLN.
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;                             // Actualiza PLLP.
    RCC_OscInitStruct.PLL.PLLQ = 2;                                         // Actualiza PLLQ.
    RCC_OscInitStruct.PLL.PLLR = 2;                                         // Actualiza PLLR.

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        HAL_RCC_OscConfig(                                                  // Ejecuta HAL_RCC_OscConfig().
            &RCC_OscInitStruct                                              // Entrega este parámetro a HAL_RCC_OscConfig().
        ) != HAL_OK                                                         // Evalúa esta comparación como parte de la condición actual.
    ) {
        Error_Handler();                                                // Detiene la ejecución porque ocurrió un error de inicialización.
    }

    RCC_ClkInitStruct.ClockType =                                           // Actualiza ClockType.
        RCC_CLOCKTYPE_HCLK |                                                // Continúa el cálculo de ClockType.
        RCC_CLOCKTYPE_SYSCLK |                                              // Continúa el cálculo de ClockType.
        RCC_CLOCKTYPE_PCLK1 |                                               // Continúa el cálculo de ClockType.
        RCC_CLOCKTYPE_PCLK2;                                                // Continúa el cálculo de ClockType.

    RCC_ClkInitStruct.SYSCLKSource =                                        // Actualiza SYSCLKSource.
        RCC_SYSCLKSOURCE_PLLCLK;                                        // Usa la salida del PLL como reloj del sistema.

    RCC_ClkInitStruct.AHBCLKDivider =                                       // Actualiza AHBCLKDivider.
        RCC_SYSCLK_DIV1;                                                // AHB trabaja a la misma frecuencia que SYSCLK.

    RCC_ClkInitStruct.APB1CLKDivider =                                      // Actualiza APB1CLKDivider.
        RCC_HCLK_DIV2;                                                  // Divide HCLK entre 2 para APB1.

    RCC_ClkInitStruct.APB2CLKDivider =                                      // Actualiza APB2CLKDivider.
        RCC_HCLK_DIV1;                                                  // APB2 trabaja a la misma frecuencia que HCLK.

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        HAL_RCC_ClockConfig(                                                // Continúa el cálculo de APB2CLKDivider.
            &RCC_ClkInitStruct,                                             // Continúa el cálculo de APB2CLKDivider.
            FLASH_LATENCY_2                                                 // Continúa el cálculo de APB2CLKDivider.
        ) != HAL_OK                                                         // Evalúa esta comparación como parte de la condición actual.
    ) {                                                                     // Continúa el cálculo de APB2CLKDivider.
        Error_Handler();                                                // Detiene la ejecución porque ocurrió un error de inicialización.
    }                                                                       // Continúa el cálculo de APB2CLKDivider.
}                                                                           // Continúa el cálculo de APB2CLKDivider.


/* ============================================================
 * SPI1 - COMUNICACIÓN CON LA MICROSD
 *
 * Se configura SPI1 como maestro para leer la microSD.
 * La microSD almacena las imágenes de menú y las puntuaciones.
 * ============================================================ */

static void MX_SPI1_Init(void)                                          // Configura SPI1 como maestro de 8 bits.
{
    hspi1.Instance = SPI1;                                              // Usa el periférico SPI1 para la microSD.
    hspi1.Init.Mode = SPI_MODE_MASTER;                                    // STM32 actúa como maestro SPI.
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;                           // Usa MOSI y MISO.
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;                               // Envía y recibe bytes de 8 bits.
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;                             // Reloj en bajo cuando está inactivo.
    hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;                                 // Captura datos en el primer flanco.
    hspi1.Init.NSS = SPI_NSS_SOFT;                                        // El CS se controla por software.
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;                // Divide el reloj SPI entre 2.
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;                                // Transmite primero el bit más significativo.
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;                                // Desactiva el modo TI.
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;                 // No calcula CRC por hardware.
    hspi1.Init.CRCPolynomial = 10;                                          // Actualiza CRCPolynomial.

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        HAL_SPI_Init(                                                       // Ejecuta HAL_SPI_Init().
            &hspi1                                                          // Entrega este parámetro a HAL_SPI_Init().
        ) != HAL_OK                                                         // Evalúa esta comparación como parte de la condición actual.
    ) {
        Error_Handler();                                                // Detiene la ejecución porque ocurrió un error de inicialización.
    }
}


/* ============================================================
 * USART3 - CONTROLES Y AUDIO
 *
 * PC11 recibe los controles enviados por el ESP32 y PC10 transmite eventos hacia la STM32 de audio.
 * La comunicación trabaja a 9600 baudios con 8 bits, sin paridad y un bit de parada.
 * ============================================================ */

static void MX_USART3_UART_Init(void)                                   // Configura PC10 como USART3_TX y PC11 como USART3_RX a 9600 8N1.
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};                                 // Actualiza GPIO_InitStruct.

    __HAL_RCC_GPIOC_CLK_ENABLE();                                           // Ejecuta __HAL_RCC_GPIOC_CLK_ENABLE().
    __HAL_RCC_USART3_CLK_ENABLE();                                          // Ejecuta __HAL_RCC_USART3_CLK_ENABLE().

    GPIO_InitStruct.Pin =                                                   // Actualiza Pin.
        GPIO_PIN_10 | GPIO_PIN_11;                                      // Selecciona PC10 y PC11 para USART3.

    GPIO_InitStruct.Mode =                                                  // Actualiza Mode.
        GPIO_MODE_AF_PP;                                                // Configura los pines como función alternativa push-pull.

    GPIO_InitStruct.Pull =                                                  // Actualiza Pull.
        GPIO_NOPULL;                                                    // No activa pull-up ni pull-down internos.

    GPIO_InitStruct.Speed =                                                 // Actualiza Speed.
        GPIO_SPEED_FREQ_VERY_HIGH;                                      // Usa velocidad alta de conmutación.

    GPIO_InitStruct.Alternate =                                             // Actualiza Alternate.
        GPIO_AF7_USART3;                                                // Asigna PC10/PC11 a la función alternativa USART3.

    HAL_GPIO_Init(                                                      // Aplica esta configuración a los GPIO seleccionados.
        GPIOC,                                                              // Indica puerto GPIO.
        &GPIO_InitStruct                                                    // Indica configuración aplicada.
    );                                                                      // Continúa el cálculo de Alternate.

    huart3.Instance =                                                       // Actualiza Instance.
        USART3;                                                         // Usa el periférico USART3.

    huart3.Init.BaudRate =                                                  // Actualiza BaudRate.
        9600;                                                           // Velocidad de comunicación: 9600 baudios.

    huart3.Init.WordLength =                                                // Actualiza WordLength.
        UART_WORDLENGTH_8B;                                               // Usa 8 bits de datos por trama.

    huart3.Init.StopBits =                                                  // Actualiza StopBits.
        UART_STOPBITS_1;                                                    // Usa un bit de parada.

    huart3.Init.Parity =                                                    // Actualiza Parity.
        UART_PARITY_NONE;                                                   // No usa bit de paridad.

    /* Ambos sentidos son necesarios: RX recibe controles del ESP32 y
     * TX transmite eventos de sonido por PC10 a la otra STM32. */
    huart3.Init.Mode =                                                      // Actualiza Mode.
        UART_MODE_TX_RX;                                                     // RX recibe controles; TX envía eventos de audio.

    huart3.Init.HwFlowCtl =                                                 // Actualiza HwFlowCtl.
        UART_HWCONTROL_NONE;                                                // No usa RTS/CTS.

    huart3.Init.OverSampling =                                              // Actualiza OverSampling.
        UART_OVERSAMPLING_16;                                                 // Muestrea cada bit 16 veces.

    if (                                                                    // Comprueba si se cumplen las condiciones necesarias para continuar.
        HAL_UART_Init(                                                      // Continúa el cálculo de OverSampling.
            &huart3                                                         // Continúa el cálculo de OverSampling.
        ) != HAL_OK                                                         // Evalúa esta comparación como parte de la condición actual.
    ) {                                                                     // Continúa el cálculo de OverSampling.
        Error_Handler();                                                // Detiene la ejecución porque ocurrió un error de inicialización.
    }                                                                       // Continúa el cálculo de OverSampling.
}                                                                           // Continúa el cálculo de OverSampling.


/* ============================================================
 * GPIO - PINES DE TFT Y MICROSD
 *
 * Se habilitan y configuran los pines digitales usados por la pantalla y el chip select de la microSD.
 * También se fijan sus niveles iniciales antes de comenzar el juego.
 * ============================================================ */

static void MX_GPIO_Init(void)                                          // Configura los GPIO de la TFT y el CS de la microSD.
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};                                 // Actualiza GPIO_InitStruct.

    __HAL_RCC_GPIOH_CLK_ENABLE();                                           // Ejecuta __HAL_RCC_GPIOH_CLK_ENABLE().
    __HAL_RCC_GPIOC_CLK_ENABLE();                                           // Ejecuta __HAL_RCC_GPIOC_CLK_ENABLE().
    __HAL_RCC_GPIOA_CLK_ENABLE();                                           // Ejecuta __HAL_RCC_GPIOA_CLK_ENABLE().
    __HAL_RCC_GPIOB_CLK_ENABLE();                                           // Ejecuta __HAL_RCC_GPIOB_CLK_ENABLE().

    HAL_GPIO_WritePin(                                                      // Ejecuta HAL_GPIO_WritePin().
        GPIOC,                                                              // Indica puerto GPIO.
        LCD_RST_Pin |                                                       // Indica pines afectados.
        LCD_D1_Pin,                                                         // Indica pines afectados.
        GPIO_PIN_RESET                                                      // Indica nivel lógico aplicado.
    );                                                                      // Finaliza la llamada a HAL_GPIO_WritePin().

    HAL_GPIO_WritePin(                                                      // Ejecuta HAL_GPIO_WritePin().
        GPIOA,                                                              // Indica puerto GPIO.
        LCD_RD_Pin |                                                        // Indica pines afectados.
        LCD_WR_Pin |                                                        // Indica pines afectados.
        LCD_RS_Pin |                                                        // Indica pines afectados.
        LCD_D7_Pin |                                                        // Indica pines afectados.
        LCD_D0_Pin |                                                        // Indica pines afectados.
        LCD_D2_Pin,                                                         // Indica pines afectados.
        GPIO_PIN_RESET                                                      // Indica nivel lógico aplicado.
    );                                                                      // Finaliza la llamada a HAL_GPIO_WritePin().

    HAL_GPIO_WritePin(                                                      // Ejecuta HAL_GPIO_WritePin().
        GPIOB,                                                              // Indica puerto GPIO.
        LCD_CS_Pin |                                                        // Indica pines afectados.
        LCD_D6_Pin |                                                        // Indica pines afectados.
        LCD_D3_Pin |                                                        // Indica pines afectados.
        LCD_D5_Pin |                                                        // Indica pines afectados.
        LCD_D4_Pin,                                                         // Indica pines afectados.
        GPIO_PIN_RESET                                                      // Indica nivel lógico aplicado.
    );                                                                      // Finaliza la llamada a HAL_GPIO_WritePin().


    HAL_GPIO_WritePin(SD_SS_GPIO_Port, SD_SS_Pin, GPIO_PIN_SET);            // Ejecuta HAL_GPIO_WritePin().

    GPIO_InitStruct.Mode =                                                  // Actualiza Mode.
        GPIO_MODE_OUTPUT_PP;                                                // Continúa el cálculo de Mode.

    GPIO_InitStruct.Pull =                                                  // Actualiza Pull.
        GPIO_NOPULL;                                                    // No activa pull-up ni pull-down internos.

    GPIO_InitStruct.Speed =                                                 // Actualiza Speed.
        GPIO_SPEED_FREQ_HIGH;                                           // Usa velocidad alta de conmutación.

    GPIO_InitStruct.Pin =                                                   // Actualiza Pin.
        LCD_RST_Pin |                                                       // Continúa el cálculo de Pin.
        LCD_D1_Pin;                                                         // Continúa el cálculo de Pin.

    HAL_GPIO_Init(                                                      // Aplica esta configuración a los GPIO seleccionados.
        GPIOC,                                                              // Indica puerto GPIO.
        &GPIO_InitStruct                                                    // Indica configuración aplicada.
    );                                                                      // Finaliza la llamada a HAL_GPIO_Init().

    GPIO_InitStruct.Pin =                                                   // Actualiza Pin.
        LCD_RD_Pin |                                                        // Continúa el cálculo de Pin.
        LCD_WR_Pin |                                                        // Continúa el cálculo de Pin.
        LCD_RS_Pin |                                                        // Continúa el cálculo de Pin.
        LCD_D7_Pin |                                                        // Continúa el cálculo de Pin.
        LCD_D0_Pin |                                                        // Continúa el cálculo de Pin.
        LCD_D2_Pin;                                                         // Continúa el cálculo de Pin.

    HAL_GPIO_Init(                                                      // Aplica esta configuración a los GPIO seleccionados.
        GPIOA,                                                              // Indica puerto GPIO.
        &GPIO_InitStruct                                                    // Indica configuración aplicada.
    );                                                                      // Finaliza la llamada a HAL_GPIO_Init().

    GPIO_InitStruct.Pin =                                                   // Actualiza Pin.
        LCD_CS_Pin |                                                        // Continúa el cálculo de Pin.
        LCD_D6_Pin |                                                        // Continúa el cálculo de Pin.
        LCD_D3_Pin |                                                        // Continúa el cálculo de Pin.
        LCD_D5_Pin |                                                        // Continúa el cálculo de Pin.
        LCD_D4_Pin |                                                        // Continúa el cálculo de Pin.
        SD_SS_Pin;                                                          // Continúa el cálculo de Pin.

    HAL_GPIO_Init(                                                      // Aplica esta configuración a los GPIO seleccionados.
        GPIOB,                                                              // Indica puerto GPIO.
        &GPIO_InitStruct                                                    // Indica configuración aplicada.
    );                                                                      // Finaliza la llamada a HAL_GPIO_Init().
}


/* ============================================================
 * MANEJO DE ERRORES
 *
 * Si falla una inicialización crítica, se deshabilitan las interrupciones y el programa se detiene.
 * Esto evita continuar con periféricos mal configurados.
 * ============================================================ */

void Error_Handler(void)                                                // Detiene la ejecución porque ocurrió un error de inicialización.
{
    __disable_irq();                                                    // Deshabilita todas las interrupciones.

    while (1) {                                                             // Repite este bloque mientras la condición indicada siga activa.
    }
}

#ifdef USE_FULL_ASSERT

void assert_failed(                                                     // Hook de depuración usado cuando están habilitadas las aserciones completas de HAL; aquí se deja vacío.
    uint8_t *file,
    uint32_t line                                                           // Recibe el valor line.
)
{
}

#endif
