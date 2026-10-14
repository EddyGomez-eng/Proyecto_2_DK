/* SDHC/SDXC con LBA por SPI1 y FAT32 (sin exFAT): menus RGB565 y pares de puntajes en archivos preexistentes.
 * No asigna clusters ni modifica FAT/directorios. */
#include "sd_store.h"
#include "main.h"
#include <string.h>

static SPI_HandleTypeDef *sd_spi;
static uint8_t sd_sector[512];
static uint8_t sd_lista;
static uint8_t sd_error;
static uint32_t sd_fat, sd_datos, sd_raiz;
static uint8_t sd_spc;
static SD_Archivo sd_scores;
static uint32_t sd_slot_libre;
#define SD_SCORES_TAM 32768UL
#define SD_SCORES_SLOTS (SD_SCORES_TAM / 16U)

uint8_t SD_UltimoError(void) { return sd_error; }
void SD_ErrorMenu(uint8_t codigo) { sd_error = codigo; }

static uint16_t sd_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t sd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t sd_byte(uint8_t tx)
{
    uint8_t rx = 0;
    if (HAL_SPI_TransmitReceive(sd_spi, &tx, &rx, 1U, 100U) != HAL_OK)
        return 0U;
    return rx;
}

static void sd_cs_alto(void)
{
    HAL_GPIO_WritePin(SD_SS_GPIO_Port, SD_SS_Pin, GPIO_PIN_SET);
    (void)sd_byte(0xFFU);
}

static uint8_t sd_esperar_libre(uint32_t timeout)
{
    uint32_t inicio = HAL_GetTick();
    do {
        if (sd_byte(0xFFU) == 0xFFU) return 1U;
    } while (HAL_GetTick() - inicio < timeout);
    return 0U;
}

static uint8_t sd_comando(uint8_t cmd, uint32_t arg, uint8_t crc)
{
    uint8_t r = 0xFFU;
    sd_cs_alto();
    HAL_GPIO_WritePin(SD_SS_GPIO_Port, SD_SS_Pin, GPIO_PIN_RESET);
    if (!sd_esperar_libre(60U)) return 0xFFU;
    (void)sd_byte((uint8_t)(0x40U | cmd));
    (void)sd_byte((uint8_t)(arg >> 24));
    (void)sd_byte((uint8_t)(arg >> 16));
    (void)sd_byte((uint8_t)(arg >> 8));
    (void)sd_byte((uint8_t)arg);
    (void)sd_byte(crc);
    for (int i = 0; i < 10; i++) {
        r = sd_byte(0xFFU);
        if ((r & 0x80U) == 0U) break;
    }
    return r; /* CS permanece bajo para datos o respuesta larga */
}

static uint8_t sd_leer_sector(uint32_t lba, uint8_t *dst)
{
    if (sd_comando(17U, lba, 0x01U) != 0U) {
        sd_cs_alto();
        return 0U;
    }
    uint32_t inicio = HAL_GetTick();
    uint8_t token;
    do {
        token = sd_byte(0xFFU);
        if (token == 0xFEU) break;
        if (token != 0xFFU) { sd_cs_alto(); return 0U; }
    } while (HAL_GetTick() - inicio < 300U);
    if (token != 0xFEU) { sd_cs_alto(); return 0U; }
    for (unsigned i = 0; i < 512U; i++) dst[i] = sd_byte(0xFFU);
    (void)sd_byte(0xFFU); (void)sd_byte(0xFFU); /* CRC de datos */
    sd_cs_alto();
    return 1U;
}

static uint8_t sd_escribir_sector(uint32_t lba, const uint8_t *src)
{
    if (sd_comando(24U, lba, 0x01U) != 0U) {
        sd_cs_alto();
        return 0U;
    }
    (void)sd_byte(0xFFU);
    (void)sd_byte(0xFEU);
    for (unsigned i = 0; i < 512U; i++) (void)sd_byte(src[i]);
    (void)sd_byte(0xFFU); (void)sd_byte(0xFFU);
    uint8_t respuesta = 0xFFU;
    for (unsigned i = 0; i < 10U; i++) {
        respuesta = sd_byte(0xFFU);
        if (respuesta != 0xFFU) break;
    }
    uint8_t aceptada = (respuesta & 0x1FU) == 0x05U;
    if (aceptada) aceptada = sd_esperar_libre(900U);
    sd_cs_alto();
    return aceptada;
}

static void sd_velocidad(uint32_t divisor)
{
    __HAL_SPI_DISABLE(sd_spi);
    MODIFY_REG(sd_spi->Instance->CR1, SPI_CR1_BR, divisor);
    __HAL_SPI_ENABLE(sd_spi);
}

uint8_t SD_Iniciar(SPI_HandleTypeDef *spi)
{
    sd_spi = spi;
    sd_lista = 0U;
    sd_error = 0U;
    sd_velocidad(SPI_BAUDRATEPRESCALER_256); /* <= 400 kHz durante CMD0 */
    sd_cs_alto();
    HAL_Delay(2U);
    for (int i = 0; i < 10; i++) (void)sd_byte(0xFFU); /* >= 80 pulsos */

    uint32_t inicio = HAL_GetTick();
    uint8_t r;
    do {
        r = sd_comando(0U, 0U, 0x95U);
        sd_cs_alto();
    } while (r != 1U && HAL_GetTick() - inicio < 600U);
    if (r != 1U) { sd_error = 1U; return 0U; }

    if (sd_comando(8U, 0x1AAU, 0x87U) != 1U) {
        sd_cs_alto();
        sd_error = 2U;
        return 0U; /* requiere SD v2 y direccionamiento por bloques */
    }
    uint8_t r7[4];
    for (int i = 0; i < 4; i++) r7[i] = sd_byte(0xFFU);
    sd_cs_alto();
    if (r7[2] != 0x01U || r7[3] != 0xAAU) {
        sd_error = 2U; return 0U;
    }

    inicio = HAL_GetTick();
    do {
        r = sd_comando(55U, 0U, 0x01U);
        sd_cs_alto();
        if (r > 1U) { sd_error = 3U; return 0U; }
        r = sd_comando(41U, 0x40000000UL, 0x01U); /* ACMD41 con HCS */
        sd_cs_alto();
    } while (r != 0U && HAL_GetTick() - inicio < 1500U);
    if (r != 0U) { sd_error = 3U; return 0U; }

    if (sd_comando(58U, 0U, 0x01U) != 0U) {
        sd_cs_alto(); sd_error = 4U; return 0U;
    }
    uint8_t ocr[4];
    for (int i = 0; i < 4; i++) ocr[i] = sd_byte(0xFFU);
    sd_cs_alto();
    if ((ocr[0] & 0x40U) == 0U) {
        sd_error = 4U; return 0U;
    } /* LBA de SDHC o SDXC */
    sd_velocidad(SPI_BAUDRATEPRESCALER_8);

    if (!sd_leer_sector(0U, sd_sector) ||
        sd_sector[510] != 0x55U || sd_sector[511] != 0xAAU) {
        sd_error = 5U; return 0U;
    }
    uint32_t volumen = 0U;
    if (sd_u16(&sd_sector[11]) != 512U) {
        uint8_t hayFat32 = 0U;
        for (unsigned p = 0; p < 4; p++) {
            unsigned off = 446U + 16U * p;
            if (sd_sector[off+4U] == 0x0BU || sd_sector[off+4U] == 0x0CU) {
                volumen = sd_u32(&sd_sector[off+8U]);
                hayFat32 = volumen != 0U;
                break;
            }
        }
        if (!hayFat32 || !sd_leer_sector(volumen, sd_sector)) {
            sd_error = 6U; return 0U;
        }
    }
    if (sd_u16(&sd_sector[11]) != 512U ||
        sd_u16(&sd_sector[17]) != 0U || /* FAT32 no usa raiz FAT16 */
        sd_u16(&sd_sector[22]) != 0U ||
        sd_sector[510] != 0x55U || sd_sector[511] != 0xAAU) {
        sd_error = 6U; return 0U;
    }

    uint8_t spc = sd_sector[13], fats = sd_sector[16];
    uint16_t reservados = sd_u16(&sd_sector[14]);
    uint32_t fatTam = sd_u32(&sd_sector[36]);
    uint32_t raiz = sd_u32(&sd_sector[44]);
    if (!spc || (spc & (spc-1U)) || !fats || !reservados ||
        !fatTam || raiz < 2U || raiz >= 0x0FFFFFF0UL) {
        sd_error = 6U; return 0U;
    }
    sd_fat = volumen + reservados;
    sd_datos = sd_fat + (uint32_t)fats * fatTam;
    sd_raiz = raiz;
    sd_spc = spc;
    sd_lista = 1U;
    return 1U;
}

static uint32_t sd_siguiente(uint32_t cluster)
{
    if (!sd_leer_sector(sd_fat + cluster/128U, sd_sector)) return 0U;
    return sd_u32(&sd_sector[(cluster%128U)*4U]) & 0x0FFFFFFFUL;
}

uint8_t SD_Abrir(const char nombre83[11], SD_Archivo *f)
{
    if (!sd_lista || !f) return 0U;
    sd_error = 0U;
    uint32_t cluster = sd_raiz;
    for (unsigned c=0; c<32U && cluster>=2U && cluster<0x0FFFFFF8UL; c++) {
        for (uint8_t sec=0; sec<sd_spc; sec++) {
            if (!sd_leer_sector(sd_datos+(cluster-2U)*sd_spc+sec,sd_sector))
                { sd_error = 7U; return 0U; }
            for (unsigned off=0; off<512U; off+=32U) {
                const uint8_t *e=sd_sector+off;
                if (e[0]==0U) { sd_error = 8U; return 0U; }
                if (e[0]==0xE5U || e[11]==0x0FU || (e[11]&0x18U)) continue;
                if (memcmp(e,nombre83,11U)!=0) continue;
                uint32_t inicio=(uint32_t)sd_u16(e+26U) |
                                 ((uint32_t)sd_u16(e+20U)<<16);
                if (inicio<2U || inicio>=0x0FFFFFF8UL) {
                    sd_error = 7U; return 0U;
                }
                f->primero=inicio;
                f->cluster=inicio;
                f->indiceCluster=0U;
                f->tamano=sd_u32(e+28U);
                f->posicion=0U;
                return 1U;
            }
        }
        cluster=sd_siguiente(cluster);
    }
    sd_error = 8U;
    return 0U;
}

static uint8_t sd_sector_archivo(SD_Archivo *f, uint32_t sector,
                                 uint32_t *lba)
{
    uint32_t n=sector/sd_spc;
    if (n<f->indiceCluster) { f->cluster=f->primero; f->indiceCluster=0U; }
    while (f->indiceCluster<n) {
        f->cluster=sd_siguiente(f->cluster);
        if (f->cluster<2U || f->cluster>=0x0FFFFFF8UL) return 0U;
        f->indiceCluster++;
    }
    *lba=sd_datos+(f->cluster-2U)*sd_spc+(sector%sd_spc);
    return 1U;
}

uint8_t SD_Leer(SD_Archivo *f,uint8_t *dst,uint32_t bytes)
{
    if (!sd_lista || !f || !dst || bytes>f->tamano ||
        f->posicion>f->tamano-bytes) return 0U;
    while (bytes) {
        uint32_t lba;
        if (!sd_sector_archivo(f,f->posicion/512U,&lba) ||
            !sd_leer_sector(lba,sd_sector)) return 0U;
        uint32_t off=f->posicion%512U;
        uint32_t n=512U-off;
        if (n>bytes) n=bytes;
        memcpy(dst,sd_sector+off,n);
        dst+=n; bytes-=n; f->posicion+=n;
    }
    return 1U;
}

static void sd_put_u32(uint8_t *p,uint32_t v)
{
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8);
    p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

uint8_t SD_Scores_Cargar(uint32_t *mejorMario,uint32_t *mejorLuigi)
{
    if (!mejorMario || !mejorLuigi ||
        !SD_Abrir("SCORES  DAT",&sd_scores) ||
        sd_scores.tamano!=SD_SCORES_TAM) return 0U;
    *mejorMario=0U; *mejorLuigi=0U;
    sd_slot_libre=SD_SCORES_SLOTS;
    uint8_t bloque[512];
    for (uint32_t sec=0; sec<SD_SCORES_TAM/512U; sec++) {
        if (!SD_Leer(&sd_scores,bloque,sizeof(bloque))) return 0U;
        for (unsigned i=0; i<512U; i+=16U) {
            const uint8_t *p=bloque+i;
            uint32_t slot=sec*32U+i/16U;
            if (p[0]==0xFFU && p[1]==0xFFU && p[2]==0xFFU && p[3]==0xFFU) {
                sd_slot_libre=slot;
                return 1U;
            }
            if (memcmp(p,"DKS1",4U)!=0) continue;
            uint32_t mario=sd_u32(p+4U),luigi=sd_u32(p+8U);
            if (sd_u32(p+12U)!=(mario^luigi^0xA5C31F27UL)) continue;
            if (mario>*mejorMario) *mejorMario=mario;
            if (luigi>*mejorLuigi) *mejorLuigi=luigi;
        }
    }
    return 1U; /* archivo lleno: se muestran los records, sin sobreescribir */
}

uint8_t SD_Scores_Guardar(uint32_t mario,uint32_t luigi)
{
    if (!sd_lista || sd_slot_libre>=SD_SCORES_SLOTS) return 0U;
    uint32_t pos=sd_slot_libre*16U, lba;
    SD_Archivo f=sd_scores;
    if (!sd_sector_archivo(&f,pos/512U,&lba) ||
        !sd_leer_sector(lba,sd_sector)) return 0U;
    uint8_t registro[16];
    memcpy(registro,"DKS1",4U);
    sd_put_u32(registro+4U,mario);
    sd_put_u32(registro+8U,luigi);
    sd_put_u32(registro+12U,mario^luigi^0xA5C31F27UL);
    memcpy(sd_sector+(pos%512U),registro,16U);
    if (!sd_escribir_sector(lba,sd_sector)) return 0U;
    f.posicion=pos;
    uint8_t verificacion[16];
    if (!SD_Leer(&f,verificacion,sizeof(verificacion)) ||
        memcmp(registro,verificacion,16U)!=0) return 0U;
    sd_slot_libre++;
    return 1U;
}
