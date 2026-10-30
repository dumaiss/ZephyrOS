/* The placement probe (tests/test_exeplace.py): one function per kind of
 * crossing between the zones its sources are pinned to (Makefile). */
#ifndef PLACE_H
#define PLACE_H

#include "kos/types.h"

/* resident (RESIDENT=place_res.c): the handler and a callback target */
extern volatile uint16_t res_ticks;
void res_on_tick(unsigned int count, unsigned int missed);
uint16_t res_inc(uint16_t x);

/* home (place_main.c) */
uint16_t home_square(uint16_t x);

/* far 1 (place_far1.c) */
uint16_t f1_add(uint16_t a, uint16_t b);
uint32_t f1_mix(uint8_t a, uint16_t b, uint32_t c, uint8_t d, uint16_t e);
uint16_t f1_nested(uint16_t x);
uint16_t f1_spin(uint16_t events);

/* far 2 (place_far2.c) */
uint8_t f2_twice(uint8_t x);
uint16_t f2_apply(uint16_t (*fn)(uint16_t), uint16_t x);

#endif
