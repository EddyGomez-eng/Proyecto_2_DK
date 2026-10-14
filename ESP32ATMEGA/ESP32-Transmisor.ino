#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>

/* ============================================================
 * CONFIGURACION GENERAL
 * ============================================================ */

#define CANAL_ESPNOW      1
#define TIEMPO_ENVIO      100

#define RX_J1             32
#define RX_J2             33

HardwareSerial UART_J1(1);
HardwareSerial UART_J2(2);


/* ============================================================
 * MAC DEL RECEPTOR
 * D4:E9:F4:BD:C1:D0
 * ============================================================ */

uint8_t direccionReceptor[] = {
    0xD4, 0xE9, 0xF4,
    0xBD, 0xC1, 0xD0
};


/* ============================================================
 * PAQUETE ESP-NOW
 *
 * Guarda el movimiento actual de ambos jugadores,
 * los contadores de salto y el número de paquete.
 * ============================================================ */

typedef struct struct_message
{
    char estadoJ1;
    char estadoJ2;

    uint8_t saltoJ1;
    uint8_t saltoJ2;

    uint32_t numeroPaquete;

} struct_message;


struct_message datos;


/* ============================================================
 * ESTADO DE LOS DOS CONTROLES
 * ============================================================ */

char estadoJ1 = 'N';
char estadoJ2 = 'N';

uint8_t saltoJ1 = 0;
uint8_t saltoJ2 = 0;

uint32_t numeroPaquete = 0;


/* ============================================================
 * CONTROL DEL ENVIO
 *
 * Evita solicitar otro envío mientras ESP-NOW todavía
 * está procesando el paquete anterior.
 * ============================================================ */

volatile bool txPendiente = false;


/* ============================================================
 * ESTADISTICAS
 * ============================================================ */

uint32_t contadorTX = 0;

uint32_t requestOK = 0;
uint32_t requestError = 0;

uint32_t deliveryOK = 0;
uint32_t deliveryFail = 0;

uint32_t busySkip = 0;

uint32_t ultimoEnvio = 0;
uint32_t ultimoReporte = 0;


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
 * LEER CONTROL J1
 *
 * Las direcciones sustituyen el estado anterior.
 * J aumenta el contador de salto.
 * ============================================================ */

void LeerJ1()
{
    while (UART_J1.available())
    {
        char dato = UART_J1.read();

        if (dato == 'J')
        {
            saltoJ1++;
        }
        else if (EsDireccion(dato))
        {
            estadoJ1 = dato;
        }
    }
}


/* ============================================================
 * LEER CONTROL J2
 * ============================================================ */

void LeerJ2()
{
    while (UART_J2.available())
    {
        char dato = UART_J2.read();

        if (dato == 'J')
        {
            saltoJ2++;
        }
        else if (EsDireccion(dato))
        {
            estadoJ2 = dato;
        }
    }
}


/* ============================================================
 * CONFIRMACION DE ENTREGA ESP-NOW
 *
 * Se ejecuta cuando ESP-NOW termina de intentar entregar
 * el paquete al receptor.
 * ============================================================ */

#if ESP_IDF_VERSION_MAJOR >= 5

void OnDataSent(
    const wifi_tx_info_t *info,
    esp_now_send_status_t status
)
#else

void OnDataSent(
    const uint8_t *mac_addr,
    esp_now_send_status_t status
)
#endif
{
    txPendiente = false;

    if (status == ESP_NOW_SEND_SUCCESS)
    {
        deliveryOK++;
    }
    else
    {
        deliveryFail++;
    }
}


/* ============================================================
 * SETUP
 * ============================================================ */

void setup()
{
    Serial.begin(115200);


    /* --------------------------------------------------------
     * UART DE LOS DOS NANOS
     * -------------------------------------------------------- */

    UART_J1.begin(
        9600,
        SERIAL_8N1,
        RX_J1,
        -1
    );

    UART_J2.begin(
        9600,
        SERIAL_8N1,
        RX_J2,
        -1
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


    if (esp_now_init() != ESP_OK)
    {
        Serial.println("ERROR ESP-NOW");
        return;
    }


    esp_now_register_send_cb(
        OnDataSent
    );


    /* --------------------------------------------------------
     * REGISTRAR AL RECEPTOR
     * -------------------------------------------------------- */

    esp_now_peer_info_t peerInfo = {};

    memcpy(
        peerInfo.peer_addr,
        direccionReceptor,
        6
    );

    peerInfo.channel = CANAL_ESPNOW;
    peerInfo.encrypt = false;
    peerInfo.ifidx = WIFI_IF_STA;


    if (
        esp_now_add_peer(&peerInfo) !=
        ESP_OK
    )
    {
        Serial.println("ERROR AL AGREGAR RECEPTOR");
        return;
    }


    Serial.println("ESP32 TRANSMISOR LISTO");

    Serial.print("MAC TX: ");
    Serial.println(WiFi.macAddress());
}


/* ============================================================
 * LOOP
 *
 * Lee continuamente los dos controles.
 * Cada 100 ms reúne los datos de ambos y envía un paquete.
 * ============================================================ */

void loop()
{
    LeerJ1();
    LeerJ2();


    /* --------------------------------------------------------
     * ENVIO ESP-NOW
     * -------------------------------------------------------- */

    if (
        millis() - ultimoEnvio >=
        TIEMPO_ENVIO
    )
    {
        ultimoEnvio = millis();


        if (txPendiente)
        {
            busySkip++;
        }
        else
        {
            datos.estadoJ1 = estadoJ1;
            datos.estadoJ2 = estadoJ2;

            datos.saltoJ1 = saltoJ1;
            datos.saltoJ2 = saltoJ2;

            datos.numeroPaquete =
                numeroPaquete++;


            txPendiente = true;


            esp_err_t resultado =
                esp_now_send(
                    direccionReceptor,
                    (uint8_t *)&datos,
                    sizeof(datos)
                );


            contadorTX++;


            if (resultado == ESP_OK)
            {
                requestOK++;
            }
            else
            {
                requestError++;

                txPendiente = false;
            }
        }
    }


    /* --------------------------------------------------------
     * ESTADISTICAS
     * -------------------------------------------------------- */

    if (
        millis() - ultimoReporte >=
        5000
    )
    {
        ultimoReporte = millis();


        Serial.print("TX=");
        Serial.print(contadorTX);

        Serial.print(" | REQUEST OK=");
        Serial.print(requestOK);

        Serial.print(" | ERROR=");
        Serial.print(requestError);

        Serial.print(" | DELIVERY OK=");
        Serial.print(deliveryOK);

        Serial.print(" | FAIL=");
        Serial.print(deliveryFail);

        Serial.print(" | BUSY SKIP=");
        Serial.println(busySkip);
    }
}