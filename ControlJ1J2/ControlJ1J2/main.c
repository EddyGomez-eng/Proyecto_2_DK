/*
 * ============================================================
 * DONKEY KONG
 * CONTROL DIGITAL - ATmega328P / ARDUINO NANO
 * MICROCHIP STUDIO
 *
 * Este programa lee la palanca y el botón de salto del control.
 * Después convierte cada acción en un carácter y lo envía por
 * UART hacia el ESP32.
 *
 * COMANDOS ENVIADOS:
 *
 * U = Arriba
 * D = Abajo
 * L = Izquierda
 * R = Derecha
 * N = Sin movimiento
 * J = Salto
 *
 * ============================================================
 *
 * PALANCA:
 *
 * ARRIBA     -> D7 = PD7
 * ABAJO      -> D6 = PD6
 * IZQUIERDA  -> D5 = PD5
 * DERECHA    -> D4 = PD4
 *
 * SALTO:
 *
 * BOTON      -> D2 = PD2
 *
 * UART:
 *
 * TX         -> D1 = PD1
 *
 * BAUD RATE  = 9600
 *
 * Las entradas usan PULL-UP interno:
 *
 * SUELTO     = 1
 * PRESIONADO = 0
 *
 * ============================================================
 */


/* ============================================================
 * FRECUENCIA DEL ATmega328P
 *
 * Indica que el microcontrolador trabaja a 16 MHz.
 * Este valor también permite que _delay_ms() calcule
 * correctamente los tiempos.
 * ============================================================ */

#define F_CPU 16000000UL


/* ============================================================
 * LIBRERIAS
 *
 * Se cargan las herramientas necesarias para controlar
 * registros del ATmega, usar retardos y tipos uint8_t.
 * ============================================================ */

#include <avr/io.h>                                                     // Permite acceder a registros como DDRD, PORTD, UCSR0B, etc.
#include <util/delay.h>                                                 // Permite utilizar _delay_ms().
#include <stdint.h>                                                     // Permite utilizar tipos como uint8_t.


/* ============================================================
 * DEFINICION DE PINES
 *
 * Se asigna un nombre fácil de reconocer a cada entrada
 * conectada al puerto D del ATmega328P.
 * ============================================================ */

#define PIN_ARRIBA     PD7                                              // D7 del Nano recibe la dirección arriba.
#define PIN_ABAJO      PD6                                              // D6 del Nano recibe la dirección abajo.
#define PIN_IZQUIERDA  PD5                                              // D5 del Nano recibe la dirección izquierda.
#define PIN_DERECHA    PD4                                              // D4 del Nano recibe la dirección derecha.
#define PIN_SALTO      PD2                                              // D2 del Nano recibe el botón de salto.


/* ============================================================
 * UART - INICIALIZACION
 *
 * Configura el UART0 del ATmega328P para transmitir datos
 * hacia el ESP32 a 9600 baudios usando el pin D1 / PD1.
 *
 * Configuración:
 * 9600 baudios
 * 8 bits de datos
 * sin paridad
 * 1 bit de parada
 * ============================================================ */

void UART_Init(void)
{
    /* --------------------------------------------------------
     * VELOCIDAD UART
     *
     * Con un reloj de 16 MHz y 9600 baudios se utiliza
     * UBRR = 103.
     * -------------------------------------------------------- */

    UBRR0H = 0;                                                        // Guarda la parte alta del valor 103; en este caso es 0.

    UBRR0L = 103;                                                      // Guarda la parte baja y establece aproximadamente 9600 baudios.


    /* --------------------------------------------------------
     * ACTIVAR TRANSMISION
     *
     * TXEN0 habilita el transmisor UART del ATmega.
     * Solo se necesita transmitir hacia el ESP32.
     * -------------------------------------------------------- */

    UCSR0B =
        (1 << TXEN0);                                                  // Activa TX del UART0.


    /* --------------------------------------------------------
     * FORMATO DE LOS DATOS
     *
     * UCSZ01 = 1
     * UCSZ00 = 1
     *
     * Esto configura palabras de 8 bits.
     * Al no activar otros bits se mantiene:
     * sin paridad y 1 bit de parada.
     * -------------------------------------------------------- */

    UCSR0C =
        (1 << UCSZ01) |                                                // Primer bit necesario para seleccionar 8 bits de datos.
        (1 << UCSZ00);                                                 // Segundo bit necesario para seleccionar 8 bits de datos.
}


/* ============================================================
 * UART - ENVIAR UN CARACTER
 *
 * Envía un único comando al ESP32.
 *
 * Ejemplo:
 * UART_Enviar('R') -> envía DERECHA.
 * UART_Enviar('J') -> envía SALTO.
 * ============================================================ */

void UART_Enviar(char dato)
{
    /* --------------------------------------------------------
     * ESPERAR A QUE UART ESTE LISTO
     *
     * UDRE0 vale 1 cuando el registro de transmisión
     * puede recibir un nuevo dato.
     * -------------------------------------------------------- */

    while (
        !(UCSR0A & (1 << UDRE0))                                       // Continúa esperando mientras UART todavía esté ocupado.
    )
    {
        ;                                                              // No realiza otra acción durante la espera.
    }


    /* --------------------------------------------------------
     * ENVIAR EL CARACTER
     *
     * Al escribir en UDR0 el hardware UART comienza
     * automáticamente la transmisión por D1 / PD1.
     * -------------------------------------------------------- */

    UDR0 = dato;                                                       // Coloca el comando en UART para enviarlo al ESP32.
}


/* ============================================================
 * ENTRADAS DEL CONTROL
 *
 * Configura la palanca y el botón de salto como entradas.
 * También activa las resistencias pull-up internas.
 *
 * Con pull-up:
 *
 * 1 = botón o dirección suelta.
 * 0 = botón o dirección presionada.
 * ============================================================ */

void Controles_Init(void)
{
    /* --------------------------------------------------------
     * CONFIGURAR COMO ENTRADAS
     *
     * En DDRD:
     * 0 = entrada
     * 1 = salida
     *
     * Se colocan en 0 únicamente los bits correspondientes
     * a los cinco controles.
     * -------------------------------------------------------- */

    DDRD &=
        ~(
            (1 << PIN_ARRIBA)    |                                    // PD7 queda configurado como entrada.
            (1 << PIN_ABAJO)     |                                    // PD6 queda configurado como entrada.
            (1 << PIN_IZQUIERDA) |                                    // PD5 queda configurado como entrada.
            (1 << PIN_DERECHA)   |                                    // PD4 queda configurado como entrada.
            (1 << PIN_SALTO)                                           // PD2 queda configurado como entrada.
        );


    /* --------------------------------------------------------
     * ACTIVAR PULL-UP
     *
     * Cuando un pin está configurado como entrada, escribir
     * un 1 en PORTD activa su resistencia pull-up interna.
     *
     * Esto evita que las entradas queden flotantes.
     * -------------------------------------------------------- */

    PORTD |=
        (
            (1 << PIN_ARRIBA)    |                                    // Activa pull-up de ARRIBA.
            (1 << PIN_ABAJO)     |                                    // Activa pull-up de ABAJO.
            (1 << PIN_IZQUIERDA) |                                    // Activa pull-up de IZQUIERDA.
            (1 << PIN_DERECHA)   |                                    // Activa pull-up de DERECHA.
            (1 << PIN_SALTO)                                           // Activa pull-up del botón SALTO.
        );
}


/* ============================================================
 * LEER DIRECCION DE LA PALANCA
 *
 * Lee los cuatro microswitches de la palanca y devuelve
 * un carácter que representa la dirección actual.
 *
 * U = arriba
 * D = abajo
 * L = izquierda
 * R = derecha
 * N = ninguna dirección
 * ============================================================ */

char Leer_Direccion(void)
{
    uint8_t arriba;                                                    // Guarda si ARRIBA está presionado.
    uint8_t abajo;                                                     // Guarda si ABAJO está presionado.
    uint8_t izquierda;                                                 // Guarda si IZQUIERDA está presionado.
    uint8_t derecha;                                                   // Guarda si DERECHA está presionado.


    /* --------------------------------------------------------
     * LEER ENTRADAS
     *
     * PIND contiene el estado eléctrico de los pines.
     *
     * Como las entradas usan pull-up:
     * presionado = 0.
     *
     * El operador ! invierte el resultado para trabajar así:
     * presionado = 1.
     * -------------------------------------------------------- */

    arriba =
        !(PIND & (1 << PIN_ARRIBA));                                   // Vale 1 cuando se presiona ARRIBA.


    abajo =
        !(PIND & (1 << PIN_ABAJO));                                    // Vale 1 cuando se presiona ABAJO.


    izquierda =
        !(PIND & (1 << PIN_IZQUIERDA));                                // Vale 1 cuando se presiona IZQUIERDA.


    derecha =
        !(PIND & (1 << PIN_DERECHA));                                  // Vale 1 cuando se presiona DERECHA.


    /* ========================================================
     * ARRIBA
     *
     * Solo acepta ARRIBA cuando ABAJO no está presionado.
     * Esto evita enviar dos direcciones verticales opuestas.
     * ======================================================== */

    if (
        arriba &&                                                       // ARRIBA está presionado.
        !abajo                                                          // ABAJO no está presionado.
    )
    {
        return 'U';                                                     // Devuelve U para indicar movimiento hacia arriba.
    }


    /* ========================================================
     * ABAJO
     *
     * Solo acepta ABAJO cuando ARRIBA no está presionado.
     * ======================================================== */

    if (
        abajo &&                                                        // ABAJO está presionado.
        !arriba                                                         // ARRIBA no está presionado.
    )
    {
        return 'D';                                                     // Devuelve D para indicar movimiento hacia abajo.
    }


    /* ========================================================
     * IZQUIERDA
     *
     * Solo acepta IZQUIERDA cuando DERECHA no está presionado.
     * ======================================================== */

    if (
        izquierda &&                                                    // IZQUIERDA está presionado.
        !derecha                                                        // DERECHA no está presionado.
    )
    {
        return 'L';                                                     // Devuelve L para indicar movimiento hacia la izquierda.
    }


    /* ========================================================
     * DERECHA
     *
     * Solo acepta DERECHA cuando IZQUIERDA no está presionado.
     * ======================================================== */

    if (
        derecha &&                                                      // DERECHA está presionado.
        !izquierda                                                       // IZQUIERDA no está presionado.
    )
    {
        return 'R';                                                     // Devuelve R para indicar movimiento hacia la derecha.
    }


    /* ========================================================
     * SIN MOVIMIENTO
     *
     * Si ninguna condición anterior se cumple se envía N.
     * ======================================================== */

    return 'N';                                                         // N indica que no hay movimiento válido.
}


/* ============================================================
 * MAIN
 *
 * Inicializa las entradas y UART.
 * Después ejecuta continuamente tres tareas:
 *
 * 1. Detecta nuevos saltos.
 * 2. Lee la dirección actual de la palanca.
 * 3. Envía los comandos al ESP32.
 * ============================================================ */

int main(void)
{
    /* ========================================================
     * INICIALIZACION
     *
     * Prepara primero las entradas del control y después
     * el UART utilizado para comunicarse con el ESP32.
     * ======================================================== */

    Controles_Init();                                                   // Configura palanca y salto como entradas con pull-up.

    UART_Init();                                                        // Configura UART0 a 9600 baudios.


    /* ========================================================
     * VARIABLES DEL SALTO
     *
     * Se comparan el estado anterior y el actual del botón.
     * Esto permite enviar J una sola vez por cada pulsación.
     * ======================================================== */

    uint8_t saltoAnterior = 0;                                          // Guarda si el botón estaba presionado en la lectura anterior.

    uint8_t saltoActual;                                                // Guarda el estado actual del botón.


    /* ========================================================
     * LOOP PRINCIPAL
     *
     * Este ciclo nunca termina mientras el control esté
     * encendido.
     * ======================================================== */

    while (1)
    {
        /* ====================================================
         * LEER BOTON DE SALTO
         *
         * Convierte la entrada activa en bajo a una variable:
         *
         * 1 = presionado
         * 0 = suelto
         * ==================================================== */

        saltoActual =
            !(PIND & (1 << PIN_SALTO));                                // Lee D2 y convierte LOW en botón presionado.


        /* ====================================================
         * DETECTAR UNA NUEVA PULSACION
         *
         * El salto solamente se genera cuando ocurre:
         *
         * antes  = 0
         * ahora  = 1
         *
         * Por eso mantener presionado el botón no genera
         * continuamente nuevos saltos.
         * ==================================================== */

        if (
            saltoActual &&                                              // El botón está presionado ahora.
            !saltoAnterior                                              // En la lectura anterior estaba suelto.
        )
        {
            /* ------------------------------------------------
             * DEBOUNCE
             *
             * Se esperan 10 ms para ignorar los pequeños
             * rebotes eléctricos del pulsador.
             * ------------------------------------------------ */

            _delay_ms(10);                                              // Espera a que la señal del botón se estabilice.


            /* ------------------------------------------------
             * CONFIRMAR SALTO
             *
             * Después del debounce se vuelve a leer el botón.
             * Si sigue presionado, la pulsación se considera
             * válida.
             * ------------------------------------------------ */

            if (
                !(PIND & (1 << PIN_SALTO))                              // Comprueba nuevamente que D2 continúe en LOW.
            )
            {
                UART_Enviar('J');                                       // Envía J una sola vez para solicitar un salto.
            }
        }


        /* ----------------------------------------------------
         * GUARDAR ESTADO DEL BOTON
         *
         * La lectura actual se convierte en la lectura anterior
         * para la siguiente vuelta del while.
         * ---------------------------------------------------- */

        saltoAnterior =
            saltoActual;                                                // Permite detectar el próximo cambio de suelto a presionado.


        /* ====================================================
         * LEER MOVIMIENTO
         *
         * Leer_Direccion() devuelve U, D, L, R o N según
         * la posición actual de la palanca.
         * ==================================================== */

        char direccion =
            Leer_Direccion();                                           // Obtiene el movimiento actual del jugador.


        /* ====================================================
         * ENVIAR MOVIMIENTO
         *
         * La dirección se transmite continuamente.
         *
         * Mantener DERECHA:
         * R R R R R R...
         *
         * Mantener IZQUIERDA:
         * L L L L L L...
         *
         * Soltar la palanca:
         * N N N N N N...
         *
         * Esto permite que el receptor siempre conozca
         * el estado actual del control.
         * ==================================================== */

        UART_Enviar(
            direccion                                                   // Envía U, D, L, R o N al ESP32.
        );


        /* ====================================================
         * VELOCIDAD DE ACTUALIZACION
         *
         * Se esperan 20 ms entre cada lectura.
         *
         * 1000 ms / 20 ms = aproximadamente 50 ciclos/s.
         * ==================================================== */

        _delay_ms(20);                                                  // Limita el envío continuo a aproximadamente 50 actualizaciones por segundo.
    }


    return 0;                                                           // No se alcanza porque el while(1) nunca termina.
}