#ifndef SD_STORE_H
#define SD_STORE_H

#include <stdint.h>
#include "stm32f4xx_hal.h"

/* Tres archivos FAT32 8.3 precreados: MENUINI.RGB, MENUFIN.RGB y SCORES.DAT.
 * Los menus se leen de SD; SCORES.DAT registra una pareja de puntos por ronda. */
typedef struct {
    uint32_t primero, cluster, indiceCluster, tamano, posicion;
} SD_Archivo;

uint8_t SD_Iniciar(SPI_HandleTypeDef *spi);
/* Diagnostico del ultimo intento: 0 OK, 1 CMD0, 2 CMD8, 3 ACMD41,
 * 4 CMD58, 5 sector inicial, 6 FAT32/particion, 7 E/S directorio,
 * 8 archivo ausente, 9 tamano de menu, 10 lectura de menu. */
uint8_t SD_UltimoError(void);
void SD_ErrorMenu(uint8_t codigo);
uint8_t SD_Abrir(const char nombre83[11], SD_Archivo *f);
uint8_t SD_Leer(SD_Archivo *f, uint8_t *dst, uint32_t bytes);
uint8_t SD_Scores_Cargar(uint32_t *mejorMario, uint32_t *mejorLuigi);
uint8_t SD_Scores_Guardar(uint32_t mario, uint32_t luigi);

#endif
