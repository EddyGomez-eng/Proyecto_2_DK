#ifndef RETRO_SPRITES_H
#define RETRO_SPRITES_H
#include <stdint.h>
/* RGB565, cero = pixel transparente. Mario/Luigi: 2 fotogramas 12x12. */
extern const uint16_t spriteMario[2 * 12 * 12];
extern const uint16_t spriteLuigi[2 * 12 * 12];
/* Vista posterior al subir, reescalada de 16x16 a 12x12. */
extern const uint16_t escaleraMario12[12 * 12];
extern const uint16_t escaleraLuigi12[12 * 12];
/* Cuatro cuadros de flama, reescalados a la caja de 10x12. */
extern const uint16_t flamaNueva[4 * 10 * 12];
/* Destello breve al romper un barril con el martillo. */
extern const uint16_t golpeBarril12[12 * 12];
/* El mismo estilo y tamano del aviso 100 existente, con cifra 200. */
extern const uint16_t puntos200Sprite[16 * 13];
extern const uint16_t spriteBarriles[2 * 2 * 8 * 8];
extern const uint16_t spritePeach[15 * 20];
/* Cinco cuadros de muerte por personaje, RGB565 de 12x12 en flash interna. */
#define MUERTE_CUADROS 5U
extern const uint16_t muerteMario12[MUERTE_CUADROS * 12 * 12];
extern const uint16_t muerteLuigi12[MUERTE_CUADROS * 12 * 12];
#define CORAZON_W 11
#define CORAZON_H 9
extern const uint16_t corazonSprite[CORAZON_W * CORAZON_H];
#endif
