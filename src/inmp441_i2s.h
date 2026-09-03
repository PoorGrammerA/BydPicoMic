#ifndef INMP441_I2S_H
#define INMP441_I2S_H

#include <stdbool.h>
#include <stdint.h>

/* INMP441 wiring on the RP2040-Zero header:
 * GP12 = SCK/BCLK, GP13 = WS/LRCLK, GP14 = SD, L/R = GND (left channel). */
#define INMP441_BCLK_PIN 12u
#define INMP441_WS_PIN   13u
#define INMP441_SD_PIN   14u

#define INMP441_SAMPLE_RATE 48000u
#define INMP441_BLOCK_SAMPLES 48u

bool inmp441_i2s_init(void);
void inmp441_i2s_start(void);

/* Copy one millisecond of signed mono PCM. Returns false until DMA has
 * completed a capture block; callers should wait rather than insert a gap. */
bool inmp441_i2s_read_block(int16_t destination[INMP441_BLOCK_SAMPLES]);

uint32_t inmp441_i2s_dropped_blocks(void);

#endif /* INMP441_I2S_H */
