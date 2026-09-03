#include "inmp441_i2s.h"

#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "inmp441_i2s.pio.h"

#define INMP441_SLOTS_PER_SAMPLE 2u
#define INMP441_BLOCK_WORDS \
    (INMP441_BLOCK_SAMPLES * INMP441_SLOTS_PER_SAMPLE)
#define INMP441_CAPTURE_BLOCKS 16u

static uint32_t g_capture[INMP441_CAPTURE_BLOCKS][INMP441_BLOCK_WORDS]
    __attribute__((aligned(4)));
static PIO g_pio = pio0;
static uint g_sm;
static int g_dma_channel = -1;
static volatile uint32_t g_produced_blocks;
static uint32_t g_consumed_blocks;
static volatile uint32_t g_dropped_blocks;
static volatile uint8_t g_active_buffer;
static volatile bool g_capture_running;

static void __isr inmp441_dma_irq_handler(void)
{
    dma_irqn_acknowledge_channel(0, (uint)g_dma_channel);
    if (!g_capture_running) {
        return;
    }

    uint8_t const next = (uint8_t)((g_active_buffer + 1u) %
                                   INMP441_CAPTURE_BLOCKS);
    g_active_buffer = next;
    ++g_produced_blocks;

    /* The DMA control block remains configured; writing this trigger alias
     * starts the next fixed-size transfer without interrupting the PIO clock. */
    dma_hw->ch[g_dma_channel].al2_write_addr_trig =
        (uintptr_t)g_capture[next];
}

bool inmp441_i2s_init(void)
{
    g_sm = pio_claim_unused_sm(g_pio, false);
    if (g_sm == (uint)-1) {
        return false;
    }

    uint const offset = pio_add_program(g_pio, &inmp441_i2s_program);
    inmp441_i2s_program_init(g_pio, g_sm, offset, INMP441_SAMPLE_RATE,
                             INMP441_SD_PIN, INMP441_BCLK_PIN);

    g_dma_channel = dma_claim_unused_channel(false);
    if (g_dma_channel < 0) {
        return false;
    }

    dma_channel_config config = dma_channel_get_default_config(g_dma_channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
    channel_config_set_read_increment(&config, false);
    channel_config_set_write_increment(&config, true);
    channel_config_set_dreq(&config, pio_get_dreq(g_pio, g_sm, false));
    dma_channel_configure(g_dma_channel, &config, g_capture[0],
                          &g_pio->rxf[g_sm], INMP441_BLOCK_WORDS, false);

    dma_irqn_set_channel_enabled(0, (uint)g_dma_channel, true);
    irq_set_exclusive_handler(DMA_IRQ_0, inmp441_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    return true;
}

void inmp441_i2s_start(void)
{
    g_capture_running = false;
    dma_irqn_set_channel_enabled(0, (uint)g_dma_channel, false);
    pio_sm_set_enabled(g_pio, g_sm, false);
    dma_channel_abort(g_dma_channel);
    dma_irqn_acknowledge_channel(0, (uint)g_dma_channel);

    g_produced_blocks = 0;
    g_consumed_blocks = 0;
    g_dropped_blocks = 0;
    g_active_buffer = 0;

    pio_sm_clear_fifos(g_pio, g_sm);
    pio_sm_restart(g_pio, g_sm);
    dma_channel_set_write_addr(g_dma_channel, g_capture[0], false);
    dma_channel_set_trans_count(g_dma_channel, INMP441_BLOCK_WORDS, false);
    g_capture_running = true;
    dma_irqn_set_channel_enabled(0, (uint)g_dma_channel, true);
    dma_channel_start(g_dma_channel);
    pio_sm_set_enabled(g_pio, g_sm, true);
}

bool inmp441_i2s_read_block(int16_t destination[INMP441_BLOCK_SAMPLES])
{
    uint32_t const produced = g_produced_blocks;
    if (produced == g_consumed_blocks) {
        return false;
    }

    /* One ring entry is always the DMA's active write target, so at most
     * CAPTURE_BLOCKS - 1 completed blocks are safe to read. */
    if (produced - g_consumed_blocks >= INMP441_CAPTURE_BLOCKS) {
        g_consumed_blocks = produced - (INMP441_CAPTURE_BLOCKS - 1u);
        ++g_dropped_blocks;
    }

    uint32_t const block = g_consumed_blocks++ % INMP441_CAPTURE_BLOCKS;
    for (uint32_t sample = 0; sample < INMP441_BLOCK_SAMPLES; ++sample) {
        /* I2S changes WS one clock before the next word's MSB. The PIO keeps
         * that transition bit as bit 31, so discard it by shifting once and
         * then keep the upper 16 bits of the aligned 24-bit sample. Without
         * this correction the real sign bit is interpreted as magnitude,
         * producing a large DC-like clipped waveform. */
        uint32_t const aligned =
            g_capture[block][sample * 2u] << 1u;
        destination[sample] =
            (int16_t)((int32_t)aligned >> 16u);
    }
    return true;
}

uint32_t inmp441_i2s_dropped_blocks(void)
{
    return g_dropped_blocks;
}
