#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>

/* ============================================================
 * CONFIGURACION GENERAL
 * ============================================================ */

#define CANAL_ESPNOW       1

#define TX_NUCLEO          25

#define TIEMPO_UART        20
#define TIMEOUT_ESPNOW     500


HardwareSerial UART_NUCLEO(2);


/* ============================================================
 * MAC DEL TRANSMISOR
 * D4:E9:F4:FA:9D:00
 * ============================================================ */

uint8_t direccionTransmisor[] = {
    0xD4, 0xE9, 0xF4,
    0xFA, 0x9D, 0x00
};


/* ============================================================
 * PAQUETE ESP-NOW
 *
 * Debe ser idéntico al utilizado por el transmisor.
 * ============================================================ */

typedef struct struct_message
{
    char estadoJ1;
    char estadoJ2;

    uint8_t saltoJ1;
    uint8_t saltoJ2;

    uint32_t numeroPaquete;

} struct_message;


/* ============================================================
 * DATOS RECIBIDOS
 * ============================================================ */

struct_message datosRecibidos;

volatile bool nuevoPaquete = false;


/* ============================================================
 * ESTADO ACTUAL DE LOS JUGADORES
 * ============================================================ */

char estadoJ1 = 'N';
char estadoJ2 = 'N';

uint8_t ultimoSaltoJ1 = 0;
uint8_t ultimoSaltoJ2 = 0;


/* ============================================================
 * TEMPORIZACION Y DIAGNOSTICO
 * ============================================================ */

uint32_t ultimoPaquete = 0;
uint32_t ultimoUART = 0;
uint32_t ultimoReporte = 0;

uint32_t contadorRX = 0;


/* ============================================================
 * COMPROBAR DIRECCION
 * ============================================================ */

bool EsDireccion(char dato)
{
    return (
        dato == 'U' ||
        dato == 'D' ||
        dato == 'L' ||
        dato == 'R' ||
        dato == 'N'
    );
}


/* ============================================================
 * ENVIAR COMANDO A LA NUCLEO
 *
 * El protocolo utiliza dos bytes:
 *
 * 1R
 * 1L
 * 1J
 * 2U
 * 2N
 * etc.
 * ============================================================ */

void EnviarNucleo(
    char jugador,
    char comando
)
{
    UART_NUCLEO.write(jugador);
    UART_NUCLEO.write(comando);
}


/* ============================================================
 * COMPROBAR MAC
 *
 * Solo se aceptan paquetes del ESP32 transmisor.
 * ============================================================ */

bool EsMACTransmisor(const uint8_t *mac)
{
    return (
        memcmp(
            mac,
            direccionTransmisor,
            6
        ) == 0
    );
}


/* ============================================================
 * RECEPCION ESP-NOW
 *
 * ESP-NOW ejecuta esta función automáticamente
 * cada vez que llega un paquete.
 * ============================================================ */

#if ESP_IDF_VERSION_MAJOR >= 5

void OnDataRecv(
    const esp_now_recv_info_t *info,
    const uint8_t *incomingData,
    int len
)
{
    if (!EsMACTransmisor(info->src_addr))
    {
        return;
    }

#else

void OnDataRecv(
    const uint8_t *mac,
    const uint8_t *incomingData,
    int len
)
{
    if (!EsMACTransmisor(mac))
    {
        return;
    }

#endif

    if (
        len !=
        sizeof(struct_message)
    )
    {
        return;
    }


    memcpy(
        &datosRecibidos,
        incomingData,
        sizeof(datosRecibidos)
    );


    nuevoPaquete = true;

    ultimoPaquete = millis();

    contadorRX++;
}


/* ============================================================
 * PROCESAR PAQUETE
 *
 * Actualiza las direcciones.
 *
 * Los saltos solamente se envían cuando cambia su contador,
 * evitando repetir continuamente el comando J.
 * ============================================================ */

void ProcesarPaquete()
{
    if (!nuevoPaquete)
    {
        return;
    }


    nuevoPaquete = false;


    /* --------------------------------------------------------
     * MOVIMIENTO J1
     * -------------------------------------------------------- */

    if (
        EsDireccion(
            datosRecibidos.estadoJ1
        )
    )
    {
        estadoJ1 =
            datosRecibidos.estadoJ1;
    }


    /* --------------------------------------------------------
     * MOVIMIENTO J2
     * -------------------------------------------------------- */

    if (
        EsDireccion(
            datosRecibidos.estadoJ2
        )
    )
    {
        estadoJ2 =
            datosRecibidos.estadoJ2;
    }


    /* --------------------------------------------------------
     * SALTO J1
     *
     * Si el contador cambió significa que el botón
     * fue presionado nuevamente.
     * -------------------------------------------------------- */

    if (
        datosRecibidos.saltoJ1 !=
        ultimoSaltoJ1
    )
    {
        ultimoSaltoJ1 =
            datosRecibidos.saltoJ1;

        EnviarNucleo(
            '1',
            'J'
        );
    }


    /* --------------------------------------------------------
     * SALTO J2
     * -------------------------------------------------------- */

    if (
        datosRecibidos.saltoJ2 !=
        ultimoSaltoJ2
    )
    {
        ultimoSaltoJ2 =
            datosRecibidos.saltoJ2;

        EnviarNucleo(
            '2',
            'J'
        );
    }
}


/* ============================================================
 * SETUP
 * ============================================================ */

void setup()
{
    Serial.begin(115200);


    /* --------------------------------------------------------
     * UART HACIA LA NUCLEO
     *
     * GPIO25 funciona como TX.
     * -------------------------------------------------------- */

    UART_NUCLEO.begin(
        9600,
        SERIAL_8N1,
        -1,
        TX_NUCLEO
    );


    /* --------------------------------------------------------
     * WIFI / ESP-NOW
     * -------------------------------------------------------- */

    WiFi.mode(WIFI_STA);

    WiFi.disconnect();


    esp_wifi_set_channel(
        CANAL_ESPNOW,
        WIFI_SECOND_CHAN_NONE
    );


    if (
        esp_now_init() !=
        ESP_OK
    )
    {
        Serial.println("ERROR ESP-NOW");
        return;
    }


    esp_now_register_recv_cb(
        OnDataRecv
    );


    ultimoPaquete = millis();


    Serial.println("ESP32 RECEPTOR LISTO");

    Serial.print("MAC RX: ");
    Serial.println(WiFi.macAddress());
}


/* ============================================================
 * LOOP
 *
 * 1. Procesa los paquetes ESP-NOW.
 * 2. Detecta nuevos saltos.
 * 3. Reenvía direcciones a la Nucleo cada 20 ms.
 * 4. Detiene jugadores si se pierde ESP-NOW.
 * ============================================================ */

void loop()
{
    /* ========================================================
     * PROCESAR DATOS INALAMBRICOS
     * ======================================================== */

    ProcesarPaquete();


    /* ========================================================
     * FAILSAFE
     *
     * Después de 500 ms sin paquetes se fuerza N para
     * evitar que un jugador continúe caminando.
     * ======================================================== */

    if (
        millis() - ultimoPaquete >
        TIMEOUT_ESPNOW
    )
    {
        estadoJ1 = 'N';
        estadoJ2 = 'N';
    }


    /* ========================================================
     * UART HACIA NUCLEO
     *
     * Los movimientos se repiten cada 20 ms.
     *
     * Los saltos no aparecen aquí porque J solamente
     * se envía cuando cambia el contador de salto.
     * ======================================================== */

    if (
        millis() - ultimoUART >=
        TIEMPO_UART
    )
    {
        ultimoUART = millis();


        EnviarNucleo(
            '1',
            estadoJ1
        );


        EnviarNucleo(
            '2',
            estadoJ2
        );
    }


    /* ========================================================
     * MONITOR SERIAL
     * ======================================================== */

    if (
        millis() - ultimoReporte >=
        5000
    )
    {
        ultimoReporte = millis();


        Serial.print("RX=");
        Serial.print(contadorRX);

        Serial.print(" | J1=");
        Serial.print(estadoJ1);

        Serial.print(" | J2=");
        Serial.print(estadoJ2);


        if (
            millis() - ultimoPaquete <=
            TIMEOUT_ESPNOW
        )
        {
            Serial.println(
                " | ESP-NOW: RECIBIENDO"
            );
        }
        else
        {
            Serial.println(
                " | ESP-NOW: SIN DATOS"
            );
        }
    }
}