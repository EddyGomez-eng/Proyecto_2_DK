#ifndef GAME_SPRITES_H
#define GAME_SPRITES_H
#include <stdint.h>
/* 0x0000 significa transparencia al dibujar estos recursos. */
#define HELP_W 27
#define HELP_H 12
#define PUNTOS_W 16
#define PUNTOS_H 13
#define EXPLOSION_W 19
#define EXPLOSION_H 26
#define MAZO_W 12
#define MAZO_H 13
#define MUERTE_FRAMES 5
extern const uint16_t ayudaSprite[HELP_W * HELP_H];
extern const uint16_t puntosSprite[PUNTOS_W * PUNTOS_H];
extern const uint16_t explosionSprite[EXPLOSION_W * EXPLOSION_H];
extern const uint16_t mazoSprite[MAZO_W * MAZO_H];
extern const uint16_t muerteMario[MUERTE_FRAMES * 16 * 16];
extern const uint16_t muerteLuigi[MUERTE_FRAMES * 16 * 16];
#endif
