/*
 * ============================================================
 * DONKEY KONG - STM32 DE AUDIO
 * STM32F446RE / STM32CubeIDE
 *
 * Esta segunda Nucleo se encarga únicamente del audio del juego.
 *
 * ENTRADA:
 * PC11 / USART3_RX recibe comandos desde PC10 / USART3_TX
 * de la STM32 principal a 9600 baudios, 8N1.
 *
 * SALIDA:
 * PB8 / TIM4_CH3 genera una señal PWM hacia la entrada S/IN
 * del módulo de buzzer pasivo.
 *
 * El programa utiliza HSI a 16 MHz y no necesita DAC, ADC
 * ni bibliotecas externas de audio.
 * ============================================================
 */

#include "main.h"                                                       // Incluye registros, HAL y definiciones de la STM32F446RE.
#include <stdint.h>                                                     // Habilita uint8_t, uint16_t y uint32_t.


/* ============================================================
 * TIPOS DE DATOS DEL SISTEMA DE AUDIO
 *
 * Se definen las estructuras utilizadas para representar notas,
 * efectos y prioridades.
 * ============================================================ */

typedef struct {
    uint16_t freq;                                                      // Frecuencia de la nota en Hz; 0 representa silencio.
    uint16_t ms;                                                        // Duración de la nota en milisegundos.
} Nota;                                                                 // Representa una nota o pausa de una melodía.

typedef enum {
    SFX_SALTO,                                                          // Sonido reproducido cuando un jugador salta.
    SFX_PUNTO,                                                          // Sonido usado al obtener puntos.
    SFX_PISO,                                                           // Sonido asociado al contacto o evento de piso.
    SFX_GOLPE,                                                          // Sonido de golpe o daño.
    SFX_INICIO,                                                         // Sonido de inicio utilizado también en el autotest.
    SFX_VICTORIA,                                                       // Secuencia reproducida al ganar.
    SFX_DERROTA,                                                        // Secuencia reproducida al perder.
    SFX_MAZO,                                                           // Efecto relacionado con el martillo.
    SFX_EXPLOSION,                                                      // Efecto usado al destruir un enemigo.
    SFX_MUERTE,                                                         // Secuencia reproducida al perder la última vida.
    SFX_NUM                                                             // Cantidad total de efectos definidos.
} EfectoSonido;                                                         // Estados disponibles de efectos de sonido.

typedef struct {
    const Nota *notas;                                                  // Apunta al arreglo de notas que forma el efecto.
    uint8_t cantidad;                                                   // Número de notas que contiene el efecto.
    uint8_t prioridad;                                                  // Define qué efectos pueden interrumpir a otros.
} DefEfecto;                                                            // Describe una secuencia completa de efecto de sonido.


/* ============================================================
 * CONFIGURACION DEL PWM
 *
 * TIM4 canal 3 genera el tono en PB8.
 * El timer trabaja con una base de 1 MHz para que cada cuenta
 * represente aproximadamente 1 us.
 * ============================================================ */

#define SONIDO_TIM          TIM4                                       // Timer encargado de generar el PWM.
#define SONIDO_TICK_HZ      1000000UL                                  // Base del timer: 1 MHz = 1 us por cuenta.
#define SONIDO_DUTY_SFX     50U                                        // Duty cycle usado por los efectos de sonido.
#define SONIDO_DUTY_MUSICA  35U                                        // Duty menor para la música de fondo.


/* ============================================================
 * FRECUENCIAS DE LAS NOTAS
 *
 * Cada constante guarda la frecuencia aproximada en Hz de
 * una nota musical utilizada por melodías y efectos.
 * ============================================================ */

#define NOTA_E3    165                                                  // Mi 3 = 165 Hz.
#define NOTA_F3    175                                                  // Fa 3 = 175 Hz.
#define NOTA_A3    220                                                  // La 3 = 220 Hz.
#define NOTA_B3    247                                                  // Si 3 = 247 Hz.
#define NOTA_C4    262                                                  // Do 4 = 262 Hz.
#define NOTA_E4    330                                                  // Mi 4 = 330 Hz.
#define NOTA_G4    392                                                  // Sol 4 = 392 Hz.
#define NOTA_A4    440                                                  // La 4 = 440 Hz.
#define NOTA_C5    523                                                  // Do 5 = 523 Hz.
#define NOTA_E5    659                                                  // Mi 5 = 659 Hz.
#define NOTA_G5    784                                                  // Sol 5 = 784 Hz.
#define NOTA_A5    880                                                  // La 5 = 880 Hz.
#define NOTA_B5    988                                                  // Si 5 = 988 Hz.
#define NOTA_C6   1047                                                  // Do 6 = 1047 Hz.
#define NOTA_E6   1319                                                  // Mi 6 = 1319 Hz.
#define NOTA_AS5   932                                                  // La sostenido 5 = 932 Hz.
#define NOTA_D6   1175                                                  // Re 6 = 1175 Hz.
#define NOTA_F6   1397                                                  // Fa 6 = 1397 Hz.
#define NOTA_G6   1568                                                  // Sol 6 = 1568 Hz.
#define NOTA_A6   1760                                                  // La 6 = 1760 Hz.
#define NOTA_AS6  1865                                                  // La sostenido 6 = 1865 Hz.
#define NOTA_C7   2093                                                  // Do 7 = 2093 Hz.
#define NOTA_D7   2349                                                  // Re 7 = 2349 Hz.


/* ============================================================
 * EFECTOS DE SONIDO
 *
 * Cada arreglo contiene pares {frecuencia, duración}.
 * Las notas se reproducen una después de otra para formar
 * el efecto completo.
 * ============================================================ */

static const Nota sfxSalto[] = {
    { NOTA_E4, 65 }, { NOTA_G4, 70 }, { NOTA_C5, 105 }                 // Ascenso rápido de tonos para representar el salto.
};

static const Nota sfxPunto[] = {
    { NOTA_B5, 50 }, { NOTA_E6, 90 }                                   // Dos tonos agudos para indicar puntos obtenidos.
};

static const Nota sfxPiso[] = {
    { 660, 40 }, { NOTA_A5, 60 }                                       // Efecto corto formado por dos tonos.
};

static const Nota sfxGolpe[] = {
    { 440, 60 }, { 330, 60 }, { 220, 60 }, { 150, 140 }                // Frecuencias descendentes para representar un golpe.
};

static const Nota sfxInicio[] = {
    { NOTA_C4, 105 }, { NOTA_E4, 105 }, { NOTA_G4, 115 },
    { NOTA_C5, 245 }, { 0, 70 }, { NOTA_E5, 140 }                      // Secuencia de inicio con una pausa de 70 ms.
};

static const Nota sfxVictoria[] = {
    { NOTA_C5, 110 }, { NOTA_E5, 110 }, { NOTA_G5, 120 },
    { NOTA_C6, 320 }, { 0, 60 },
    { NOTA_G5, 130 }, { NOTA_C6, 500 }                                 // Melodía ascendente y prolongada para la victoria.
};

static const Nota sfxDerrota[] = {
    { NOTA_G4, 200 }, { NOTA_E4, 230 }, { NOTA_C4, 280 },
    { NOTA_A3, 450 }                                                    // Secuencia descendente para indicar derrota.
};

static const Nota sfxMazo[] = {
    { NOTA_C5, 75 }, { NOTA_G5, 75 }, { NOTA_C6, 120 }                 // Efecto corto asociado al martillo.
};

static const Nota sfxExplosion[] = {
    { NOTA_C5, 65 }, { NOTA_A4, 75 }, { NOTA_E4, 90 },
    { NOTA_A3, 170 }                                                    // Caída rápida de frecuencia al destruir un enemigo.
};

static const Nota sfxMuerte[] = {
    { NOTA_A5, 65 }, { NOTA_E5, 75 }, { NOTA_A4, 85 },
    { NOTA_E4, 100 }, { NOTA_A3, 180 }, { 0, 45 },
    { NOTA_E3, 180 }                                                    // Secuencia usada al perder la última vida antes del menú final.
};


/* ============================================================
 * TABLA GENERAL DE EFECTOS
 *
 * Relaciona cada valor de EfectoSonido con:
 * - su arreglo de notas,
 * - la cantidad de notas,
 * - su prioridad.
 *
 * Un valor de prioridad mayor evita que un efecto importante
 * sea reemplazado por otro de menor prioridad.
 * ============================================================ */

#define SFX_CANT(a)  ((uint8_t)(sizeof(a) / sizeof((a)[0])))            // Calcula automáticamente cuántas notas hay en un arreglo.

static const DefEfecto sfxTabla[SFX_NUM] = {
    { sfxSalto,     SFX_CANT(sfxSalto),     3 },                        // Salto: prioridad 3.
    { sfxPunto,     SFX_CANT(sfxPunto),     2 },                        // Puntos: prioridad 2.
    { sfxPiso,      SFX_CANT(sfxPiso),      2 },                        // Piso: prioridad 2.
    { sfxGolpe,     SFX_CANT(sfxGolpe),     3 },                        // Golpe: prioridad 3.
    { sfxInicio,    SFX_CANT(sfxInicio),    4 },                        // Inicio: prioridad 4.
    { sfxVictoria,  SFX_CANT(sfxVictoria),  5 },                        // Victoria: prioridad máxima.
    { sfxDerrota,   SFX_CANT(sfxDerrota),   5 },                        // Derrota: prioridad máxima.
    { sfxMazo,      SFX_CANT(sfxMazo),      3 },                        // Martillo: prioridad 3.
    { sfxExplosion, SFX_CANT(sfxExplosion), 3 },                        // Explosión: prioridad 3.
    { sfxMuerte,    SFX_CANT(sfxMuerte),    5 }                         // Muerte final: prioridad máxima.
};


/* ============================================================
 * MUSICA DEL MENU
 *
 * Contiene la melodía reproducida mientras se muestra el menú.
 * Cada entrada indica frecuencia y duración.
 * Una frecuencia igual a 0 crea una pausa.
 * ============================================================ */

static const Nota musicaMenu[] = {
    { NOTA_D6, 240 }, { NOTA_D6, 240 }, { NOTA_A6, 1440 },
    { NOTA_F6, 240 }, { NOTA_G6, 240 }, { NOTA_F6, 240 },
    { NOTA_D6, 1440 }, { NOTA_D6, 240 }, { NOTA_D6, 240 },
    { NOTA_AS6, 1440 }, { NOTA_G6, 240 }, { NOTA_A6, 240 },
    { NOTA_G6, 240 }, { NOTA_E6, 1440 }, { NOTA_E6, 240 },
    { NOTA_E6, 240 }, { NOTA_C7, 1440 }, { NOTA_A6, 240 },
    { NOTA_AS6, 240 }, { NOTA_C7, 240 }, { NOTA_D7, 1440 },
    { NOTA_F6, 240 }, { NOTA_G6, 240 }, { NOTA_A6, 1440 },
    { NOTA_E6, 240 }, { NOTA_F6, 240 }, { NOTA_G6, 240 },
    { NOTA_F6, 1920 }
};


/* ============================================================
 * MUSICA NORMAL DE LA PARTIDA
 *
 * DK_FRASE guarda un patrón corto que se repite cuatro veces.
 * Se utiliza una macro para no escribir la misma secuencia
 * repetidamente dentro del arreglo.
 * ============================================================ */

#define DK_FRASE \
    { NOTA_AS5, 214 }, { 0, 214 }, { NOTA_D6, 214 }, { 0, 107 }, \
    { NOTA_F6, 161 }, { NOTA_G6, 161 }, { NOTA_F6, 161 }

static const Nota musicaJuego[] = {
    { 0, 54 },                                                          // Pequeña pausa antes de comenzar la secuencia.
    DK_FRASE, DK_FRASE, DK_FRASE, DK_FRASE                             // Repite cuatro veces la frase principal.
};

#undef DK_FRASE                                                         // Elimina la macro después de construir musicaJuego.


/* ============================================================
 * MUSICA DEL MARTILLO
 *
 * Reemplaza temporalmente la música normal mientras un jugador
 * tiene activo el poder del martillo.
 * ============================================================ */

static const Nota musicaMartillo[] = {
    { NOTA_C5, 110 }, { NOTA_G5, 110 }, { NOTA_A5, 110 }, { NOTA_G5, 110 },
    { NOTA_E5, 160 }, { 0, 40 }, { NOTA_G5, 110 }, { NOTA_C6, 180 },
    { 0, 50 }, { NOTA_G5, 100 }, { NOTA_A5, 100 }, { NOTA_C6, 200 }
};


/* ============================================================
 * ESTADO DEL EFECTO ACTUAL
 *
 * Estas variables permiten avanzar un efecto nota por nota
 * sin utilizar delays que detengan el programa.
 * ============================================================ */

const Nota *sfxSeq             = 0;                                     // Apunta al arreglo del efecto que se reproduce actualmente.
uint8_t     sfxLen             = 0;                                     // Cantidad total de notas del efecto actual.
uint8_t     sfxIdx             = 0;                                     // Índice de la nota actual.
uint8_t     sfxActivo          = 0;                                     // 1 mientras existe un efecto en reproducción.
uint8_t     sfxPrioridadActual = 0;                                     // Prioridad del efecto que está sonando.
uint32_t    sfxFin             = 0;                                     // Tick en que debe terminar la nota actual del efecto.


/* ============================================================
 * ESTADO DE LA MUSICA
 *
 * Controla qué melodía se reproduce y qué nota corresponde
 * tocar en cada momento.
 * ============================================================ */

uint8_t     musicaActiva = 0;                                           // 1 cuando alguna música debe mantenerse reproduciendo.
uint8_t     modoMartillo = 0;                                           // 1 selecciona musicaMartillo.
uint8_t     modoMenu     = 0;                                           // 1 selecciona musicaMenu.
uint8_t     musicaIdx    = 0;                                           // Índice de la nota actual de la música.
uint32_t    musicaFin    = 0;                                           // Tick en que debe cambiar a la siguiente nota.


/* ============================================================
 * PROTOTIPOS
 *
 * Permiten utilizar estas funciones antes de su implementación.
 * ============================================================ */

void Sonido_Init(void);                                                 // Configura PB8 y TIM4 para generar PWM.
void Sonido_Actualizar(uint32_t ahora);                                 // Avanza efectos y música según el tiempo.
void Sonido_Efecto(EfectoSonido id);                                   // Inicia un efecto de sonido.
void Musica_Iniciar(void);                                              // Inicia la música normal de la partida.
void Musica_Detener(void);                                              // Detiene la música cuando no hay un efecto activo.


/* ============================================================
 * RELOJ REAL DE TIM4
 *
 * Obtiene la frecuencia de APB1.
 * Si APB1 utiliza prescaler, los timers reciben el doble de
 * la frecuencia del bus en esta familia STM32.
 * ============================================================ */

static uint32_t Sonido_RelojTimer(void)
{
    uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();                            // Obtiene la frecuencia actual del bus APB1.

    if ((RCC->CFGR & RCC_CFGR_PPRE1) >= RCC_CFGR_PPRE1_2)              // Comprueba si APB1 está usando un divisor distinto de 1.
    {
        return pclk1 * 2U;                                              // Los timers trabajan al doble de PCLK1 cuando APB1 está dividido.
    }

    return pclk1;                                                       // Sin división, el timer utiliza directamente PCLK1.
}


/* ============================================================
 * GENERAR UN TONO PWM
 *
 * Recibe una frecuencia y un duty cycle.
 * El periodo del PWM se obtiene modificando ARR.
 * CCR3 define cuánto tiempo permanece activa la salida PB8.
 *
 * freq = 0 produce silencio.
 * ============================================================ */

static void Sonido_Tono(uint16_t freq, uint32_t duty)
{
    if (freq == 0U)                                                     // Comprueba si la entrada representa una pausa.
    {
        SONIDO_TIM->CCR3 = 0U;                                         // Coloca el duty en 0 para silenciar PB8.
        return;                                                         // No necesita calcular un periodo.
    }

    uint32_t arr = (SONIDO_TICK_HZ / freq) - 1U;                       // Calcula el periodo necesario para obtener la frecuencia solicitada.

    SONIDO_TIM->ARR = arr;                                              // Actualiza el periodo del PWM.
    SONIDO_TIM->CCR3 = ((arr + 1U) * duty) / 100U;                     // Calcula el duty del canal 3 como porcentaje del periodo.
}


/* ============================================================
 * INICIALIZACION DEL SONIDO
 *
 * Configura PB8 como TIM4_CH3 y prepara TIM4 en PWM modo 1.
 * El timer queda encendido desde el inicio; los tonos cambian
 * modificando ARR y CCR3.
 * ============================================================ */

void Sonido_Init(void)
{
    /* --------------------------------------------------------
     * RELOJES DE GPIOB Y TIM4
     * -------------------------------------------------------- */

    __HAL_RCC_GPIOB_CLK_ENABLE();                                       // Habilita el reloj del puerto donde está PB8.
    RCC->APB1ENR |= RCC_APB1ENR_TIM4EN;                                // Habilita directamente el reloj de TIM4.
    (void)RCC->APB1ENR;                                                 // Fuerza la lectura del registro para asegurar que el cambio se aplique.

    /* --------------------------------------------------------
     * PB8 COMO TIM4_CH3
     *
     * PB8 utiliza la función alternativa AF2 correspondiente
     * al canal 3 de TIM4.
     * -------------------------------------------------------- */

    GPIOB->MODER =
        (GPIOB->MODER & ~(3UL << (2U * 8U))) |
        (2UL << (2U * 8U));                                             // Configura PB8 como función alternativa.

    GPIOB->OTYPER &= ~(1UL << 8U);                                      // Configura PB8 como salida push-pull.
    GPIOB->OSPEEDR |= (3UL << (2U * 8U));                               // Selecciona velocidad alta para la señal PWM.
    GPIOB->PUPDR &= ~(3UL << (2U * 8U));                                // Desactiva pull-up y pull-down internos.
    GPIOB->AFR[1] =
        (GPIOB->AFR[1] & ~(0xFUL << 0U)) |
        (2UL << 0U);                                                    // Asigna AF2 a PB8 para conectar físicamente TIM4_CH3.

    /* --------------------------------------------------------
     * BASE DE TIEMPO DE TIM4
     *
     * PSC reduce el reloj del timer hasta 1 MHz.
     * ARR empieza en 999 únicamente como periodo inicial.
     * CCR3 = 0 mantiene silencio durante la inicialización.
     * -------------------------------------------------------- */

    SONIDO_TIM->PSC =
        (Sonido_RelojTimer() / SONIDO_TICK_HZ) - 1U;                   // Ajusta el prescaler para obtener una cuenta cada 1 us.

    SONIDO_TIM->ARR = 999U;                                             // Establece un periodo inicial antes de seleccionar una nota.
    SONIDO_TIM->CCR3 = 0U;                                              // Inicia la salida PWM en silencio.

    /* --------------------------------------------------------
     * PWM MODO 1 - CANAL 3
     *
     * OC3M = 110 selecciona PWM mode 1.
     * OC3PE activa el preload de CCR3.
     * -------------------------------------------------------- */

    SONIDO_TIM->CCMR2 =
        (SONIDO_TIM->CCMR2 & ~(TIM_CCMR2_OC3M | TIM_CCMR2_CC3S))
        | (6UL << TIM_CCMR2_OC3M_Pos)
        | TIM_CCMR2_OC3PE;                                              // Configura el canal 3 como salida PWM modo 1 con preload.

    SONIDO_TIM->CCER |= TIM_CCER_CC3E;                                  // Habilita físicamente la salida del canal 3.
    SONIDO_TIM->CR1 |= TIM_CR1_ARPE;                                    // Habilita preload del registro ARR.
    SONIDO_TIM->EGR = TIM_EGR_UG;                                       // Fuerza una actualización para cargar PSC, ARR y CCR.
    SONIDO_TIM->CR1 |= TIM_CR1_CEN;                                     // Inicia el contador TIM4.
}


/* ============================================================
 * REPRODUCIR LA NOTA ACTUAL DE LA MUSICA
 *
 * Selecciona automáticamente la melodía correcta:
 * menú, martillo o juego normal.
 * Después genera la nota actual y calcula cuándo termina.
 * ============================================================ */

static void Sonido_NotaMusica(uint32_t ahora)
{
    const Nota *notas =
        modoMenu ? musicaMenu :
        (modoMartillo ? musicaMartillo : musicaJuego);                  // Selecciona el arreglo correspondiente al modo actual.

    Sonido_Tono(
        notas[musicaIdx].freq,
        SONIDO_DUTY_MUSICA
    );                                                                  // Genera la frecuencia de la nota actual con duty de música.

    musicaFin =
        ahora + notas[musicaIdx].ms;                                    // Calcula el tick en que debe avanzar a la siguiente nota.
}


/* ============================================================
 * INICIAR UN EFECTO DE SONIDO
 *
 * Busca el efecto solicitado en sfxTabla.
 * Un efecto nuevo solo reemplaza al actual si su prioridad
 * es igual o mayor.
 *
 * Los efectos tienen prioridad sobre la música.
 * ============================================================ */

void Sonido_Efecto(EfectoSonido id)
{
    if (id >= SFX_NUM)                                                  // Comprueba que el identificador exista en la tabla.
    {
        return;                                                         // Ignora identificadores fuera del rango válido.
    }

    const DefEfecto *def =
        &sfxTabla[id];                                                   // Obtiene notas, cantidad y prioridad del efecto solicitado.

    if (
        sfxActivo &&
        def->prioridad < sfxPrioridadActual
    )                                                                   // Comprueba si ya está sonando un efecto de mayor prioridad.
    {
        return;                                                         // Mantiene el efecto más importante sin interrumpirlo.
    }

    uint32_t ahora =
        HAL_GetTick();                                                   // Obtiene el tiempo actual del sistema en milisegundos.

    sfxSeq = def->notas;                                                // Selecciona el arreglo de notas del nuevo efecto.
    sfxLen = def->cantidad;                                             // Guarda cuántas notas deberá reproducir.
    sfxIdx = 0;                                                         // Comienza desde la primera nota.
    sfxPrioridadActual = def->prioridad;                                // Guarda la prioridad del nuevo efecto.
    sfxActivo = 1;                                                      // Marca que la música queda temporalmente reemplazada por un efecto.

    Sonido_Tono(
        sfxSeq[0].freq,
        SONIDO_DUTY_SFX
    );                                                                  // Reproduce inmediatamente la primera nota del efecto.

    sfxFin =
        ahora + sfxSeq[0].ms;                                           // Calcula cuándo debe terminar la primera nota.
}


/* ============================================================
 * INICIAR MUSICA NORMAL
 *
 * Selecciona musicaJuego y comienza desde su primera nota.
 * Si un efecto está sonando, la música queda preparada pero
 * espera hasta que termine el efecto.
 * ============================================================ */

void Musica_Iniciar(void)
{
    modoMenu = 0;                                                       // Desactiva la selección de música de menú.
    modoMartillo = 0;                                                   // Desactiva la selección de música de martillo.
    musicaActiva = 1;                                                   // Indica que la música debe mantenerse reproduciendo.
    musicaIdx = 0;                                                      // Reinicia la melodía desde su primera nota.

    if (!sfxActivo)                                                     // Comprueba que ningún efecto tenga prioridad en este momento.
    {
        Sonido_NotaMusica(HAL_GetTick());                               // Inicia inmediatamente la primera nota de musicaJuego.
    }
}


/* ============================================================
 * INICIAR MUSICA DEL MENU
 *
 * Selecciona musicaMenu y comienza desde la primera nota.
 * El estado no se reinicia mientras ya se encuentre en menú.
 * ============================================================ */

static void Musica_IniciarMenu(void)
{
    modoMenu = 1U;                                                      // Selecciona la melodía del menú.
    modoMartillo = 0U;                                                  // Evita seleccionar simultáneamente música de martillo.
    musicaActiva = 1U;                                                  // Mantiene habilitada la reproducción de música.
    musicaIdx = 0U;                                                     // Comienza desde la primera nota.

    if (!sfxActivo)
        Sonido_NotaMusica(HAL_GetTick());                               // Inicia el menú si ningún efecto está ocupando la salida.
}


/* ============================================================
 * DETENER MUSICA
 *
 * Desactiva la música de fondo.
 * Si existe un efecto, este continúa hasta terminar.
 * ============================================================ */

void Musica_Detener(void)
{
    musicaActiva = 0;                                                   // Impide que se reproduzcan nuevas notas de música.

    if (!sfxActivo)                                                     // Comprueba si la salida PWM no está siendo utilizada por un efecto.
    {
        Sonido_Tono(0U, 0U);                                            // Silencia inmediatamente PB8.
    }
}


/* ============================================================
 * ACTUALIZAR EFECTOS Y MUSICA
 *
 * Esta función se ejecuta continuamente desde main().
 * Utiliza HAL_GetTick en lugar de delays, por lo que el sistema
 * puede recibir UART mientras las melodías siguen avanzando.
 *
 * Prioridad:
 * 1. Si existe un efecto, se actualiza primero.
 * 2. Al terminar, regresa a la música activa.
 * 3. Si no hay efecto, avanza la melodía de fondo.
 * ============================================================ */

void Sonido_Actualizar(uint32_t ahora)
{
    /* --------------------------------------------------------
     * EFECTO ACTIVO
     * -------------------------------------------------------- */

    if (sfxActivo)                                                      // Da prioridad completa al efecto sobre la música.
    {
        if ((int32_t)(ahora - sfxFin) < 0)                             // Comprueba si la nota actual todavía no ha terminado.
        {
            return;                                                     // Mantiene la misma frecuencia hasta llegar a sfxFin.
        }

        sfxIdx++;                                                       // Avanza a la siguiente nota del efecto.

        if (sfxIdx < sfxLen)                                            // Comprueba si todavía quedan notas por reproducir.
        {
            Sonido_Tono(
                sfxSeq[sfxIdx].freq,
                SONIDO_DUTY_SFX
            );                                                          // Cambia PB8 a la frecuencia de la nueva nota.

            sfxFin =
                ahora + sfxSeq[sfxIdx].ms;                              // Programa cuándo terminará esta nota.

            return;                                                     // Mantiene el control en el efecto hasta que termine toda la secuencia.
        }

        /* ----------------------------------------------------
         * FIN DEL EFECTO
         *
         * Libera el canal de efectos y devuelve la salida
         * a la música que estaba activa antes.
         * ---------------------------------------------------- */

        sfxActivo = 0;                                                  // Indica que ya no existe un efecto reproduciéndose.
        sfxPrioridadActual = 0;                                         // Libera la prioridad para futuros efectos.

        if (musicaActiva)                                               // Comprueba si alguna música debe continuar.
        {
            Sonido_NotaMusica(ahora);                                   // Retoma la melodía seleccionada.
        }
        else
        {
            Sonido_Tono(0U, 0U);                                        // Mantiene silencio si la música estaba detenida.
        }

        return;
    }

    /* --------------------------------------------------------
     * MUSICA DE FONDO
     * -------------------------------------------------------- */

    if (
        musicaActiva &&
        (int32_t)(ahora - musicaFin) >= 0
    )                                                                   // Avanza cuando la nota actual ya cumplió su duración.
    {
        musicaIdx++;                                                    // Selecciona la siguiente nota.

        uint8_t cantidad =
            modoMenu
                ? (uint8_t)(sizeof(musicaMenu) / sizeof(musicaMenu[0]))
                : (modoMartillo
                    ? (uint8_t)(sizeof(musicaMartillo) / sizeof(musicaMartillo[0]))
                    : (uint8_t)(sizeof(musicaJuego) / sizeof(musicaJuego[0]))); // Obtiene el tamaño de la melodía que está activa.

        if (musicaIdx >= cantidad)                                      // Comprueba si llegó al final del arreglo de notas.
        {
            musicaIdx = 0;                                              // Regresa al inicio para repetir la melodía en bucle.
        }

        Sonido_NotaMusica(ahora);                                       // Reproduce la siguiente nota y programa su final.
    }
}


/* ============================================================
 * PROTOCOLO UART DESDE LA STM32 PRINCIPAL
 *
 * Cada mensaje válido contiene tres bytes:
 *
 * BYTE 1: 0xA5                     -> cabecera
 * BYTE 2: comando                  -> acción de audio
 * BYTE 3: comando XOR 0xFF         -> comprobación simple
 *
 * COMANDOS:
 * 0x01..0x0A -> efectos según EfectoSonido
 * 0x20       -> iniciar/reiniciar música normal
 * 0x21       -> detener música
 * 0x22       -> reiniciar música de martillo
 * 0x23       -> mantener música normal sin reiniciarla si ya está correcta
 * 0x24       -> mantener música de martillo sin reiniciarla si ya está correcta
 * 0x25       -> música del menú sin reiniciarla si ya está correcta
 * ============================================================ */

static uint8_t rxEstado = 0, rxComando = 0;                             // Guarda qué byte de la trama UART se está esperando.
static uint8_t enlaceRecibido = 0;                                      // Cambia a 1 después de recibir el primer comando válido.
static uint32_t ledHasta = 0;                                           // Tick hasta el que permanece encendido el LED de confirmación.


/* ============================================================
 * LED DE CONFIRMACION UART
 *
 * LD2 en PA5 se enciende durante 140 ms cuando llega una
 * trama válida. Sirve para verificar visualmente el enlace.
 * ============================================================ */

static void Audio_LED_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();                                       // Habilita el reloj de GPIOA.

    GPIO_InitTypeDef gpio = {0};                                        // Crea una estructura limpia de configuración GPIO.

    gpio.Pin = GPIO_PIN_5;                                              // Selecciona PA5, conectado al LED LD2.
    gpio.Mode = GPIO_MODE_OUTPUT_PP;                                    // Configura PA5 como salida digital push-pull.
    gpio.Pull = GPIO_NOPULL;                                            // No utiliza resistencias internas.
    gpio.Speed = GPIO_SPEED_FREQ_LOW;                                   // Velocidad baja es suficiente para un LED.

    HAL_GPIO_Init(GPIOA, &gpio);                                        // Aplica la configuración a PA5.
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);               // Inicia el LED apagado.
}


/* ============================================================
 * INTERPRETAR COMANDO DE AUDIO
 *
 * Recibe el byte central de una trama UART ya validada.
 * También enciende LD2 para confirmar que llegó correctamente.
 *
 * Los comandos 0x23, 0x24 y 0x25 se utilizan como estado:
 * solo cambian la música cuando el modo recibido es diferente.
 * Así se evita reiniciar la melodía cada vez que la STM32
 * principal vuelve a enviar el mismo estado.
 * ============================================================ */

static void Audio_Comando(uint8_t cmd)
{
    enlaceRecibido = 1;                                                 // Desactiva el autotest repetitivo después del primer comando válido.

    HAL_GPIO_WritePin(
        GPIOA,
        GPIO_PIN_5,
        GPIO_PIN_SET
    );                                                                  // Enciende LD2 para indicar recepción correcta.

    ledHasta =
        HAL_GetTick() + 140U;                                           // Programa el apagado automático del LED en 140 ms.

    /* --------------------------------------------------------
     * 0x23 - ESTADO DE MUSICA NORMAL
     * -------------------------------------------------------- */

    if (cmd == 0x23U)
    {
        if (!musicaActiva || modoMartillo || modoMenu)
            Musica_Iniciar();                                           // Cambia a música normal solo si el estado actual es diferente.

        return;
    }

    /* --------------------------------------------------------
     * 0x24 - ESTADO DE MUSICA DEL MARTILLO
     * -------------------------------------------------------- */

    if (cmd == 0x24U)
    {
        if (!musicaActiva || !modoMartillo || modoMenu)
        {
            modoMenu = 0U;                                              // Sale del modo de menú.
            modoMartillo = 1U;                                          // Selecciona la melodía del martillo.
            musicaActiva = 1U;                                          // Mantiene la música habilitada.
            musicaIdx = 0U;                                             // Comienza el modo martillo desde el inicio.

            if (!sfxActivo)
                Sonido_NotaMusica(HAL_GetTick());                       // Inicia la melodía solo si no existe un efecto prioritario.
        }

        return;
    }

    /* --------------------------------------------------------
     * 0x20 - INICIAR MUSICA NORMAL
     * -------------------------------------------------------- */

    if (cmd == 0x20U)
    {
        Musica_Iniciar();                                               // Reinicia explícitamente musicaJuego.
        return;
    }

    /* --------------------------------------------------------
     * 0x21 - DETENER MUSICA
     * -------------------------------------------------------- */

    if (cmd == 0x21U)
    {
        Musica_Detener();                                               // Apaga la música después del efecto activo, si existe.
        return;
    }

    /* --------------------------------------------------------
     * 0x25 - ESTADO DE MUSICA DEL MENU
     * -------------------------------------------------------- */

    if (cmd == 0x25U)
    {
        if (!musicaActiva || !modoMenu)
            Musica_IniciarMenu();                                       // Entra al menú sin reiniciar la melodía si ya está sonando.

        return;
    }

    /* --------------------------------------------------------
     * 0x22 - REINICIAR MUSICA DEL MARTILLO
     * -------------------------------------------------------- */

    if (cmd == 0x22U)
    {
        modoMenu = 0U;                                                  // Sale del modo de menú.
        modoMartillo = 1U;                                              // Selecciona la música del martillo.
        musicaActiva = 1U;                                              // Activa la reproducción.
        musicaIdx = 0U;                                                 // Fuerza el reinicio desde la primera nota.

        if (!sfxActivo)
            Sonido_NotaMusica(HAL_GetTick());                           // La reproduce inmediatamente si ningún SFX está activo.

        return;
    }

    /* --------------------------------------------------------
     * 0x01..0x0A - EFECTOS DE SONIDO
     * -------------------------------------------------------- */

    if (cmd >= 1U && cmd <= SFX_NUM)
        Sonido_Efecto((EfectoSonido)(cmd - 1U));                        // Convierte el comando UART al índice correspondiente de EfectoSonido.
}


/* ============================================================
 * RECIBIR UNA TRAMA BYTE POR BYTE
 *
 * rxEstado funciona como una pequeña máquina de estados:
 *
 * 0 -> espera 0xA5
 * 1 -> espera el comando
 * 2 -> espera comando XOR 0xFF
 *
 * Solo una trama completa y correcta llega a Audio_Comando().
 * ============================================================ */

static void Audio_Byte(uint8_t b)
{
    if (rxEstado == 0U)                                                 // Estado inicial: espera la cabecera.
    {
        if (b == 0xA5U)
            rxEstado = 1U;                                              // Una cabecera válida hace que el siguiente byte sea tratado como comando.
    }
    else if (rxEstado == 1U)                                            // Ya se recibió la cabecera.
    {
        if (b == 0xA5U)
            return;                                                     // Ignora cabeceras repetidas y continúa esperando el comando.

        rxComando = b;                                                  // Guarda el byte que representa la acción de audio.
        rxEstado = 2U;                                                  // El siguiente byte deberá validar este comando.
    }
    else                                                                // Estado 2: espera el byte de comprobación.
    {
        if (b == (uint8_t)(rxComando ^ 0xFFU))
            Audio_Comando(rxComando);                                   // Ejecuta el comando únicamente si el complemento es correcto.

        rxEstado =
            b == 0xA5U ? 1U : 0U;                                      // Si el último byte también es cabecera, comienza una nueva trama; si no, reinicia.
    }
}


/* ============================================================
 * USART3 - RECEPCION DESDE LA STM32 PRINCIPAL
 *
 * PC11 se configura como USART3_RX a 9600 baudios.
 * Solo se habilita recepción porque esta Nucleo de audio
 * no necesita responder por UART.
 * ============================================================ */

static void Audio_UART_Init(void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();                                       // Habilita el reloj del puerto C.
    __HAL_RCC_USART3_CLK_ENABLE();                                      // Habilita el periférico USART3.

    GPIO_InitTypeDef gpio = {0};                                        // Estructura usada para configurar PC11.

    gpio.Pin = GPIO_PIN_11;                                             // Selecciona PC11.
    gpio.Mode = GPIO_MODE_AF_PP;                                        // Usa función alternativa para conectar el pin con USART3.
    gpio.Pull = GPIO_PULLUP;                                            // Mantiene RX en nivel alto cuando no se recibe información.
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;                                  // Permite cambios rápidos de la señal UART.
    gpio.Alternate = GPIO_AF7_USART3;                                   // AF7 conecta PC11 con USART3_RX.

    HAL_GPIO_Init(GPIOC, &gpio);                                        // Aplica la configuración de PC11.

    USART3->CR1 = 0U;                                                   // Limpia la configuración anterior de control 1.
    USART3->CR2 = 0U;                                                   // Limpia la configuración de formato adicional.
    USART3->CR3 = 0U;                                                   // Limpia opciones adicionales de USART3.

    USART3->BRR =
        (HAL_RCC_GetPCLK1Freq() + 4800U) / 9600U;                       // Calcula el divisor necesario para trabajar aproximadamente a 9600 baudios.

    USART3->CR1 =
        USART_CR1_RE |
        USART_CR1_UE;                                                   // Habilita el receptor y enciende USART3.
}


/* ============================================================
 * MAIN
 *
 * Inicializa LED, UART y PWM.
 * Después mantiene tres tareas principales:
 *
 * 1. Recibir y validar comandos UART.
 * 2. Actualizar continuamente música y efectos.
 * 3. Ejecutar el autotest de audio hasta recibir el primer
 *    comando desde la STM32 principal.
 * ============================================================ */

int main(void)
{
    HAL_Init();                                                         // Inicializa HAL, SysTick y recursos básicos de la STM32.

    /* --------------------------------------------------------
     * INICIALIZACION DEL SISTEMA DE AUDIO
     * -------------------------------------------------------- */

    Audio_LED_Init();                                                   // Prepara LD2 para confirmar tramas válidas.
    Audio_UART_Init();                                                  // Configura PC11 como USART3_RX a 9600 baudios.
    Sonido_Init();                                                      // Configura PB8 y TIM4_CH3 para generar PWM.

    /* --------------------------------------------------------
     * AUTOTEST INICIAL
     *
     * El sonido de inicio se reproduce aun si la STM32 principal
     * está desconectada. Esto permite verificar buzzer y PWM.
     * -------------------------------------------------------- */

    Sonido_Efecto(SFX_INICIO);                                          // Reproduce inmediatamente la secuencia de inicio.

    uint32_t proximaPrueba =
        HAL_GetTick() + 3000U;                                          // Programa otro autotest dentro de 3 segundos si aún no existe enlace.

    /* --------------------------------------------------------
     * LAZO PRINCIPAL
     * -------------------------------------------------------- */

    for (;;)
    {
        /* ----------------------------------------------------
         * LEER USART3
         *
         * SR indica si llegó un byte o si ocurrió un error.
         * La lectura de DR obtiene el byte recibido.
         * ---------------------------------------------------- */

        uint32_t sr =
            USART3->SR;                                                 // Captura una vez el estado actual de USART3.

        if (
            sr &
            (USART_SR_RXNE |
             USART_SR_ORE |
             USART_SR_FE |
             USART_SR_NE)
        )                                                               // Entra si llegó un byte o se detectó overrun, framing o noise error.
        {
            uint8_t b =
                (uint8_t)USART3->DR;                                    // Lee DR; junto con la lectura previa de SR limpia las condiciones de error.

            if (sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE))
                rxEstado = 0U;                                          // Descarta una trama incompleta si UART reportó un error.
            else
                Audio_Byte(b);                                          // Entrega el byte válido a la máquina de estados del protocolo.
        }

        /* ----------------------------------------------------
         * ACTUALIZAR AUDIO
         * ---------------------------------------------------- */

        uint32_t ahora =
            HAL_GetTick();                                               // Obtiene el tiempo actual una sola vez en esta vuelta.

        Sonido_Actualizar(ahora);                                       // Cambia notas y restaura música sin bloquear el programa.

        /* ----------------------------------------------------
         * APAGAR LED DE CONFIRMACION
         * ---------------------------------------------------- */

        if (
            ledHasta &&
            (int32_t)(ahora - ledHasta) >= 0
        )
        {
            HAL_GPIO_WritePin(
                GPIOA,
                GPIO_PIN_5,
                GPIO_PIN_RESET
            );                                                          // Apaga LD2 después de los 140 ms programados.

            ledHasta = 0;                                               // Indica que ya no hay un apagado pendiente.
        }

        /* ----------------------------------------------------
         * REPETIR AUTOTEST SIN ENLACE
         *
         * Mientras nunca haya llegado una trama válida, el efecto
         * de inicio vuelve a sonar cada 3 segundos.
         * ---------------------------------------------------- */

        if (
            !enlaceRecibido &&
            (int32_t)(ahora - proximaPrueba) >= 0
        )
        {
            if (!sfxActivo)
                Sonido_Efecto(SFX_INICIO);                              // Reproduce otra prueba únicamente cuando el efecto anterior ya terminó.

            proximaPrueba =
                ahora + 3000U;                                          // Programa la siguiente prueba tres segundos después.
        }
    }
}


/* ============================================================
 * ERROR HANDLER
 *
 * Si una parte externa del proyecto llama esta función,
 * la STM32 queda detenida permanentemente.
 * ============================================================ */

void Error_Handler(void)
{
    for (;;)
    {
    }
}
