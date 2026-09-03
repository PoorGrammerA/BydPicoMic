#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "bsp/board_api.h"
#include "tusb.h"
#include "inmp441_i2s.h"
#include "ws2812.pio.h"

static int16_t g_audio_packet[CFG_TUD_AUDIO_EP_SZ_IN / sizeof(int16_t)];
#define AUDIO_USB_TARGET_QUEUE_PACKETS 8u
#ifndef ENABLE_USB_DEBUG
#define ENABLE_USB_DEBUG 0
#endif
static uint8_t g_uac1_mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1];
static int16_t g_uac1_volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1];

/* BPA10 hardware controls. The vehicle maps each 0..10 UI dial onto the
 * receiver's 0..15 range. Volume is linear; reverb reaches a 50% wet mix. */
#define BPA10_CONTROL_MAX          15u
#define PA10_UI_CONTROL_MAX        10u
#define REVERB_DELAY_A_SAMPLES     2111u
#define REVERB_DELAY_B_SAMPLES     3041u
#define REVERB_FEEDBACK_Q15        18022
#define REVERB_MAX_MIX_Q15         16384
/* INMP441 preamp: 11572 / 8192 = 1.413 (+3.00 dB). The vehicle's 0..10
 * microphone-volume control remains a post-effect 0..100% attenuator. */
#define MIC_PREAMP_GAIN_Q13        11572
#define MIC_LIMITER_THRESHOLD      24576
#define MIC_LIMITER_RATIO          9
/* Vocal band shaping at Fs=48 kHz. Three stable one-pole HPF stages produce
 * an 18 dB/octave roll-off below 100 Hz; the 10 kHz Butterworth LPF strongly
 * suppresses the measured 20 kHz metallic tone while preserving speech. */
#define MIC_HPF_ALPHA_Q30          1059778168LL
#define MIC_LPF_B0_Q30             236432259LL
#define MIC_LPF_B1_Q30             472864518LL
#define MIC_LPF_B2_Q30             236432259LL
#define MIC_LPF_A1_Q30            (-330246864LL)
#define MIC_LPF_A2_Q30             202234077LL
#define MIC_HPF_STAGES             3u
static uint8_t g_mic_volume = BPA10_CONTROL_MAX;
static uint8_t g_mic_reverb;
static uint8_t g_pa10_volume = PA10_UI_CONTROL_MAX;
static uint8_t g_pa10_reverb;
static uint8_t g_pa10_effect_mode = 0xFFu;
static int32_t g_mic_volume_q15 = 32767;
static int32_t g_mic_reverb_mix_q15;
static int16_t g_reverb_delay_a[REVERB_DELAY_A_SAMPLES];
static int16_t g_reverb_delay_b[REVERB_DELAY_B_SAMPLES];
static uint16_t g_reverb_delay_a_index;
static uint16_t g_reverb_delay_b_index;
typedef struct {
    int32_t previous_input;
    int32_t previous_output;
} mic_hpf_state_t;
static mic_hpf_state_t g_mic_hpf[MIC_HPF_STAGES];
static int64_t g_mic_lpf_state_1;
static int64_t g_mic_lpf_state_2;
static volatile bool g_audio_alt_active;
static volatile uint32_t g_audio_tx_count;
static volatile uint32_t g_last_audio_tx_ms;
static volatile uint32_t g_audio_alt_select_count;
static volatile uint32_t g_audio_tx_total;
static volatile uint32_t g_audio_tx_callback_total;
static volatile uint32_t g_audio_tx_zero_total;
static volatile uint8_t g_audio_last_alt;
static volatile uint8_t g_audio_rate_control_count;

/* BPA10 receiver emulation -------------------------------------------------
 * MiniKaraoke uses HID instance 0 (USB interface #2) for receiver commands.
 * Interface #3 is vendor class but accepts the app's HID-shaped control
 * transfers. HID instance 1 (interface #4) is a standard keyboard reserved
 * for the microphone's future GPIO-backed F3/F4/F5 physical buttons.
 */
#define BPA10_REPORT_ID          0x4Du
#define BPA10_CMD_GET_EFFECT_MD5 0x01u
#define BPA10_CMD_SET_CONTROL    0x02u
#define BPA10_CMD_RECEIVER_VER   0x06u
#define BPA10_CMD_SAVE_EFFECT    0x0Bu
#define BPA10_CMD_MUTE           0x0Du
#define BPA10_CMD_MIC_INFO       0x16u
#define BPA10_CMD_CONNECT_STATE  0x1Cu
#define BPA10_CMD_KEY_PRESS      0x1Du
#define BPA10_AUDIO_EP_IN        0x81u
#define PA10_DEFAULT_UI_VOLUME   8u

static uint8_t const g_fake_mic_mac[6] = {
    0x02, 0xA1, 0x10, 0x00, 0x00, 0x01
};

/* BPA10/EFFECT/ktv.bin MD5 requested as effect ID 10 by this QCOM vehicle.
 * Reporting the installed value prevents a futile effect upload followed by
 * a ten-second timeout before receiver initialization can continue. */
static uint8_t const g_effect_10_md5[16] = {
    0xB4, 0x4F, 0x62, 0x61, 0xDB, 0x98, 0x02, 0xEA,
    0x80, 0xB8, 0x5F, 0x32, 0xDE, 0x7F, 0x51, 0xE9
};

static uint8_t g_hid_reply[CFG_TUD_HID_EP_BUFSIZE];
static uint8_t g_hid_reply_len;
static uint8_t g_feature_reply[CFG_TUD_HID_EP_BUFSIZE];
static uint8_t g_feature_reply_len;
static bool g_feature_reply_pending;
static uint8_t g_vendor_control_buffer[256] __attribute__((aligned(4)));
static bool g_connection_report_pending;
static uint g_status_led_sm;

/* Physical buttons use the RP2040's internal pull-ups and close to GND.
 * The vehicle has already been verified to handle F3/F4/F5 on HID interface
 * #4 as menu, volume up, and volume down respectively. */
#define BUTTON_VOLUME_UP_PIN       8u
#define BUTTON_VOLUME_DOWN_PIN     9u
#define BUTTON_MENU_PIN           10u
#define BUTTON_DEBOUNCE_MS        25u
#define BUTTON_REPORT_QUEUE_SIZE   8u
#define HID_KEYBOARD_INSTANCE      1u

typedef struct {
    uint8_t pin;
    uint8_t keycode;
    bool raw_pressed;
    bool stable_pressed;
    uint32_t raw_changed_ms;
} button_state_t;

static button_state_t g_buttons[] = {
    {BUTTON_VOLUME_UP_PIN, HID_KEY_F4, false, false, 0u},
    {BUTTON_VOLUME_DOWN_PIN, HID_KEY_F5, false, false, 0u},
    {BUTTON_MENU_PIN, HID_KEY_F3, false, false, 0u},
};
static uint8_t g_button_report_queue[BUTTON_REPORT_QUEUE_SIZE][6];
static uint8_t g_button_report_head;
static uint8_t g_button_report_count;

static void dump_packet(char const *prefix, uint8_t const *data, uint16_t len);
static void service_audio_stream(void);
static void reset_mic_effects(void);
static void buttons_init(void);
static void service_buttons(void);

enum {
    PA10_EFFECT_RECORDING_STUDIO = 0u,
    PA10_EFFECT_KTV = 1u,
    PA10_EFFECT_MUSIC_HALL = 2u,
    PA10_EFFECT_ORIGINAL = 3u,
    PA10_EFFECT_IDLE = 0xFFu
};

static void status_led_write(uint8_t red, uint8_t green, uint8_t blue)
{
    /* WS2812 consumes GRB, MSB first. Keep brightness modest inside a car. */
    uint32_t const grb = ((uint32_t)green << 16u) |
                         ((uint32_t)red << 8u) |
                         (uint32_t)blue;
    pio_sm_put_blocking(pio1, g_status_led_sm, grb << 8u);
}

static void status_led_show_effect(uint8_t mode)
{
    switch (mode) {
    case PA10_EFFECT_RECORDING_STUDIO:
        status_led_write(8u, 28u, 40u);  /* sky blue */
        break;
    case PA10_EFFECT_KTV:
        status_led_write(40u, 0u, 0u);   /* red */
        break;
    case PA10_EFFECT_MUSIC_HALL:
        status_led_write(40u, 4u, 18u);  /* pink */
        break;
    case PA10_EFFECT_ORIGINAL:
        status_led_write(40u, 12u, 0u);  /* orange */
        break;
    default:
        status_led_write(24u, 24u, 24u); /* idle: white */
        break;
    }
}

static void status_led_init(void)
{
    g_status_led_sm = pio_claim_unused_sm(pio1, true);
    uint const offset = pio_add_program(pio1, &ws2812_program);
    ws2812_program_init(pio1, g_status_led_sm, offset,
                        PICO_DEFAULT_WS2812_PIN, 800000.0f);
    status_led_show_effect(PA10_EFFECT_IDLE);
}

static void queue_button_report(void)
{
    uint8_t keycodes[6] = {0};
    uint8_t key_count = 0u;

    for (uint8_t i = 0u; i < TU_ARRAY_SIZE(g_buttons); ++i) {
        if (g_buttons[i].stable_pressed && key_count < TU_ARRAY_SIZE(keycodes)) {
            keycodes[key_count++] = g_buttons[i].keycode;
        }
    }

    /* A full queue should be practically unreachable with 25 ms debounce and
     * a 10 ms interrupt endpoint. Preserve the newest complete keyboard state
     * if it ever happens so a dropped release cannot leave a key stuck. */
    if (g_button_report_count == BUTTON_REPORT_QUEUE_SIZE) {
        g_button_report_head = 0u;
        g_button_report_count = 0u;
    }

    uint8_t const tail = (uint8_t)(
        (g_button_report_head + g_button_report_count) % BUTTON_REPORT_QUEUE_SIZE);
    memcpy(g_button_report_queue[tail], keycodes, sizeof(keycodes));
    ++g_button_report_count;
}

static void buttons_init(void)
{
    uint32_t const now_ms = to_ms_since_boot(get_absolute_time());

    for (uint8_t i = 0u; i < TU_ARRAY_SIZE(g_buttons); ++i) {
        gpio_init(g_buttons[i].pin);
        gpio_set_dir(g_buttons[i].pin, GPIO_IN);
        gpio_pull_up(g_buttons[i].pin);

        bool const pressed = !gpio_get(g_buttons[i].pin);
        g_buttons[i].raw_pressed = pressed;
        g_buttons[i].stable_pressed = pressed;
        g_buttons[i].raw_changed_ms = now_ms;
    }
}

static void service_buttons(void)
{
    uint32_t const now_ms = to_ms_since_boot(get_absolute_time());
    bool stable_state_changed = false;

    for (uint8_t i = 0u; i < TU_ARRAY_SIZE(g_buttons); ++i) {
        bool const pressed = !gpio_get(g_buttons[i].pin);

        if (pressed != g_buttons[i].raw_pressed) {
            g_buttons[i].raw_pressed = pressed;
            g_buttons[i].raw_changed_ms = now_ms;
        } else if (pressed != g_buttons[i].stable_pressed &&
                   (uint32_t)(now_ms - g_buttons[i].raw_changed_ms) >=
                       BUTTON_DEBOUNCE_MS) {
            g_buttons[i].stable_pressed = pressed;
            stable_state_changed = true;
        }
    }

    if (stable_state_changed) {
        queue_button_report();
    }

    if (g_button_report_count != 0u &&
        tud_hid_n_ready(HID_KEYBOARD_INSTANCE)) {
        uint8_t const *keycodes = g_button_report_queue[g_button_report_head];
        if (tud_hid_n_keyboard_report(HID_KEYBOARD_INSTANCE, 0u, 0u,
                                      keycodes)) {
            g_button_report_head = (uint8_t)(
                (g_button_report_head + 1u) % BUTTON_REPORT_QUEUE_SIZE);
            --g_button_report_count;
        }
    }
}

static void set_pa10_effect_mode(uint8_t mode)
{
    if (mode > PA10_EFFECT_ORIGINAL) {
        return;
    }
    if (g_pa10_effect_mode != mode) {
        reset_mic_effects();
    }
    g_pa10_effect_mode = mode;
    status_led_show_effect(mode);
}

/* BYD-micTS02 is also selected by the vehicle's Thunder/TS controller. That
 * path does not send the PA10 Effect byte; it uploads a bundled effect file as
 * 16-byte chunks in A5 5A 88 12 frames. The table fingerprints every distinct
 * TS02 asset shipped in the recovered MiniKaraoke APK. The one identical
 * Studio/Acoustic asset pair is deliberately omitted because USB cannot
 * distinguish two byte-for-byte identical streams. */
typedef struct {
    uint32_t adler32;
    uint16_t chunks;
    uint8_t mode;
} ts02_effect_signature_t;

static ts02_effect_signature_t const g_ts02_effect_signatures[] = {
    {0x0AF8D76Eu, 155u, 1u}, {0x278CD86Eu, 155u, 3u},
    {0x27A4D86Eu, 155u, 0u}, {0x28F7D86Eu, 155u, 2u},
    {0x28FED86Eu, 155u, 1u}, {0x2EB8D96Eu, 155u, 3u},
    {0x3899D86Eu, 155u, 0u}, {0x38CCD86Eu, 155u, 2u},
    {0x38CDD86Eu, 155u, 3u}, {0x45EFD76Eu, 155u, 0u},
    {0x479BD86Eu, 155u, 0u}, {0x48FBD86Eu, 155u, 3u},
    {0x490CD86Eu, 155u, 2u}, {0x4913D86Eu, 155u, 1u},
    {0x4914D86Eu, 155u, 3u}, {0x492CD86Eu, 155u, 0u},
    {0x4A2BD76Eu, 155u, 2u}, {0x4A87D86Eu, 155u, 2u},
    {0x5754DB6Eu, 155u, 0u}, {0x5882DB6Eu, 155u, 3u},
    {0x5893DB6Eu, 155u, 2u}, {0x59C0DB6Eu, 155u, 1u},
    {0x6AF8D46Eu, 155u, 3u}, {0x6B10D46Eu, 155u, 0u},
    {0x6C90D46Eu, 155u, 2u}, {0x6C97D46Eu, 155u, 1u},
    {0x7735DB6Eu, 155u, 0u}, {0x773BD46Eu, 155u, 1u},
    {0x9EE8D76Eu, 155u, 1u}, {0xA4FCDA6Eu, 155u, 0u},
    {0xA763DA6Eu, 155u, 2u}, {0xA771D46Eu, 155u, 1u},
    {0xA9A4DA6Eu, 155u, 1u}, {0xAD7DD66Eu, 155u, 3u},
    {0xB23FD26Eu, 155u, 3u}, {0xB250D26Eu, 155u, 0u},
    {0xCF36DA6Eu, 155u, 3u}, {0xCF47DA6Eu, 155u, 2u},
    {0xDB9ED36Eu, 155u, 2u}, {0xF245D56Eu, 155u, 3u},
    {0xF25DD56Eu, 155u, 0u}, {0xF3EED56Eu, 155u, 2u},
    {0xF40DD56Eu, 155u, 1u}, {0xF9C6DA6Eu, 155u, 1u},
    {0x20492A3Du, 188u, 0u}, {0x235E2A3Du, 188u, 3u},
    {0x23722A3Du, 188u, 1u}, {0x23D82E3Du, 188u, 0u},
    {0x24332A3Du, 188u, 2u}, {0x287A2E3Du, 188u, 2u},
    {0x29482E3Du, 188u, 3u}, {0x29562E3Du, 188u, 2u},
    {0x29DE2E3Du, 188u, 3u}, {0x3D81263Du, 188u, 1u},
    {0x3D8F263Du, 188u, 1u}, {0x70D2223Du, 188u, 3u},
    {0x70F2223Du, 188u, 1u}, {0x71602C3Du, 188u, 3u},
    {0x71712C3Du, 188u, 0u}, {0x71822C3Du, 188u, 0u},
    {0x72892C3Du, 188u, 1u}, {0x72B02C3Du, 188u, 0u},
    {0x72C02C3Du, 188u, 3u}, {0x77D32C3Du, 188u, 2u},
    {0x77D62C3Du, 188u, 3u}, {0x77E72C3Du, 188u, 2u},
    {0x797C2C3Du, 188u, 2u}, {0xC52B223Du, 188u, 1u},
    {0xC8052B3Du, 188u, 1u}, {0xCA80223Du, 188u, 2u},
    {0xCC932B3Du, 188u, 2u}, {0xD08A273Du, 188u, 1u},
    {0xE0A52E3Du, 188u, 0u}, {0xFDB1253Du, 188u, 1u}
};

static uint32_t g_ts02_adler_a = 1u;
static uint32_t g_ts02_adler_b;
static uint16_t g_ts02_effect_chunks;

static void process_ts02_effect_chunk(uint8_t const *buffer)
{
    uint16_t const index = ((uint16_t)buffer[4] << 8u) | buffer[5];
    if (index == 0u) {
        g_ts02_adler_a = 1u;
        g_ts02_adler_b = 0u;
        g_ts02_effect_chunks = 0u;
    } else if (index != g_ts02_effect_chunks) {
        return;
    }

    for (uint8_t i = 6u; i < 22u; ++i) {
        g_ts02_adler_a = (g_ts02_adler_a + buffer[i]) % 65521u;
        g_ts02_adler_b = (g_ts02_adler_b + g_ts02_adler_a) % 65521u;
    }
    g_ts02_effect_chunks = (uint16_t)(index + 1u);

    uint32_t const fingerprint =
        (g_ts02_adler_b << 16u) | g_ts02_adler_a;
    for (size_t i = 0; i < TU_ARRAY_SIZE(g_ts02_effect_signatures); ++i) {
        ts02_effect_signature_t const *signature = &g_ts02_effect_signatures[i];
        if (signature->chunks == g_ts02_effect_chunks &&
            signature->adler32 == fingerprint) {
            set_pa10_effect_mode(signature->mode);
            break;
        }
    }
}

static uint32_t probe_time_ms(void)
{
    return (uint32_t)(time_us_64() / 1000u);
}

static int16_t saturate_i16(int32_t sample)
{
    if (sample > INT16_MAX) {
        return INT16_MAX;
    }
    if (sample < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)sample;
}

static int16_t filter_mic_vocal_band(int16_t input)
{
    int32_t high_passed = input;
    for (uint32_t stage_index = 0; stage_index < MIC_HPF_STAGES;
         ++stage_index) {
        mic_hpf_state_t *state = &g_mic_hpf[stage_index];
        int64_t const delta = (int64_t)state->previous_output + high_passed -
                              state->previous_input;
        int32_t const output =
            (int32_t)((MIC_HPF_ALPHA_Q30 * delta) >> 30);
        state->previous_input = high_passed;
        state->previous_output = output;
        high_passed = output;
    }

    /* 10 kHz, second-order Butterworth LPF in transposed direct form II. */
    int64_t const accumulator =
        MIC_LPF_B0_Q30 * high_passed + g_mic_lpf_state_1;
    int32_t const low_passed = (int32_t)(accumulator >> 30);
    g_mic_lpf_state_1 =
        MIC_LPF_B1_Q30 * high_passed - MIC_LPF_A1_Q30 * low_passed +
        g_mic_lpf_state_2;
    g_mic_lpf_state_2 =
        MIC_LPF_B2_Q30 * high_passed - MIC_LPF_A2_Q30 * low_passed;
    return saturate_i16(low_passed);
}

static void reset_mic_input_filter(void)
{
    memset(g_mic_hpf, 0, sizeof(g_mic_hpf));
    g_mic_lpf_state_1 = 0;
    g_mic_lpf_state_2 = 0;
}

static int16_t apply_mic_output_gain(int32_t sample)
{
    /* Apply the vehicle volume first so both intermediate multiplies remain
     * inside int32_t even for a full-scale microphone sample at volume 10. */
    int32_t amplified = (sample * g_mic_volume_q15) >> 15;
    amplified = (amplified * MIC_PREAMP_GAIN_Q13) >> 13;

    /* A high-threshold 9:1 limiter is transparent during normal speech and
     * protects the USB PCM output from unexpected full-scale transients. */
    if (amplified > MIC_LIMITER_THRESHOLD) {
        amplified = MIC_LIMITER_THRESHOLD +
                    (amplified - MIC_LIMITER_THRESHOLD) /
                        MIC_LIMITER_RATIO;
    } else if (amplified < -MIC_LIMITER_THRESHOLD) {
        amplified = -MIC_LIMITER_THRESHOLD +
                    (amplified + MIC_LIMITER_THRESHOLD) /
                        MIC_LIMITER_RATIO;
    }
    return saturate_i16(amplified);
}

static void set_mic_volume(uint8_t value)
{
    g_mic_volume = TU_MIN(value, BPA10_CONTROL_MAX);
    g_pa10_volume = (uint8_t)(((uint16_t)g_mic_volume * PA10_UI_CONTROL_MAX +
                               (BPA10_CONTROL_MAX / 2u)) /
                              BPA10_CONTROL_MAX);
    g_mic_volume_q15 =
        ((int32_t)g_mic_volume * 32767) / BPA10_CONTROL_MAX;
}

static void set_mic_reverb(uint8_t value)
{
    g_mic_reverb = TU_MIN(value, BPA10_CONTROL_MAX);
    g_pa10_reverb = (uint8_t)(((uint16_t)g_mic_reverb * PA10_UI_CONTROL_MAX +
                               (BPA10_CONTROL_MAX / 2u)) /
                              BPA10_CONTROL_MAX);
    g_mic_reverb_mix_q15 =
        ((int32_t)g_mic_reverb * REVERB_MAX_MIX_Q15) /
        BPA10_CONTROL_MAX;
}

static uint8_t pa10_ui_to_control(uint8_t value)
{
    value = TU_MIN(value, PA10_UI_CONTROL_MAX);
    return (uint8_t)(((uint16_t)value * BPA10_CONTROL_MAX +
                      (PA10_UI_CONTROL_MAX / 2u)) /
                     PA10_UI_CONTROL_MAX);
}

static void reset_mic_effects(void)
{
    memset(g_reverb_delay_a, 0, sizeof(g_reverb_delay_a));
    memset(g_reverb_delay_b, 0, sizeof(g_reverb_delay_b));
    g_reverb_delay_a_index = 0;
    g_reverb_delay_b_index = 0;
}

static int16_t process_mic_effects(int16_t input)
{
    /* Original is a true effect bypass. The microphone-volume control still
     * applies, but neither the reverb dial nor stale delay data colors it. */
    if (g_pa10_effect_mode == PA10_EFFECT_ORIGINAL) {
        return apply_mic_output_gain(input);
    }

    int32_t const delayed_a = g_reverb_delay_a[g_reverb_delay_a_index];
    int32_t const delayed_b = g_reverb_delay_b[g_reverb_delay_b_index];

    int32_t feedback_q15 = REVERB_FEEDBACK_Q15;
    int32_t wet_scale_q15 = 32767;
    int32_t wet = (delayed_a + delayed_b) / 2;
    bool cross_feedback = false;

    switch (g_pa10_effect_mode) {
    case PA10_EFFECT_RECORDING_STUDIO:
        /* Subtle short-room ambience that keeps the vocal close and clear. */
        feedback_q15 = 10486; /* 32% */
        wet_scale_q15 = 20480; /* 0.625 x reverb dial */
        break;
    case PA10_EFFECT_KTV:
        /* Short, clearly audible vocal echo. */
        feedback_q15 = 14746; /* 45% */
        wet_scale_q15 = 40960; /* 1.25 x reverb dial */
        wet = (delayed_a + (delayed_b * 3)) / 4;
        break;
    case PA10_EFFECT_MUSIC_HALL:
        /* Longer, denser tail by cross-feeding the two unequal delays. */
        feedback_q15 = 22937; /* 70% */
        wet_scale_q15 = 49152; /* 1.50 x reverb dial */
        cross_feedback = true;
        break;
    default:
        /* Preserve the original generic reverb before a mode is selected. */
        break;
    }

    g_reverb_delay_a[g_reverb_delay_a_index] = saturate_i16(
        (int32_t)input +
        (((cross_feedback ? delayed_b : delayed_a) * feedback_q15) >> 15));
    g_reverb_delay_b[g_reverb_delay_b_index] = saturate_i16(
        (int32_t)input +
        (((cross_feedback ? delayed_a : delayed_b) * feedback_q15) >> 15));

    if (++g_reverb_delay_a_index == REVERB_DELAY_A_SAMPLES) {
        g_reverb_delay_a_index = 0;
    }
    if (++g_reverb_delay_b_index == REVERB_DELAY_B_SAMPLES) {
        g_reverb_delay_b_index = 0;
    }

    int32_t effect_mix_q15 =
        (g_mic_reverb_mix_q15 * wet_scale_q15) >> 15;
    effect_mix_q15 = TU_MIN(effect_mix_q15, 24576); /* at most 75% wet */
    int32_t const dry_mix_q15 = 32767 - effect_mix_q15;
    int32_t const effected =
        (((int32_t)input * dry_mix_q15) +
         (wet * effect_mix_q15)) >> 15;
    return apply_mic_output_gain(effected);
}

static void set_idle_control_reply(uint8_t command)
{
    uint8_t const idle[] = {0xA5, 0x5A, command, 0x00, 0x16};
    memcpy(g_feature_reply, idle, sizeof(idle));
    g_feature_reply_len = sizeof(idle);
}

static void finish_control_reply(void)
{
    if (g_feature_reply_pending) {
        uint8_t command = g_feature_reply_len >= 3 ? g_feature_reply[2] : 0;
        set_idle_control_reply(command);
        g_feature_reply_pending = false;
    }
}

static void handle_control_frame(uint8_t const *buffer, uint16_t bufsize)
{
    if (bufsize < 5 || buffer[0] != 0xA5 || buffer[1] != 0x5A) {
        return;
    }

    uint8_t payload_len = buffer[3];
    uint16_t frame_len = (uint16_t)payload_len + 5u;
    if (frame_len > bufsize || frame_len > sizeof(g_feature_reply) ||
        buffer[frame_len - 1u] != 0x16) {
        return;
    }

    uint8_t command = buffer[2];
    if (command == 0xFC && payload_len == 4 &&
        buffer[4] == 0xB0 && buffer[5] == 0xB0 &&
        (buffer[6] == 0x01 || buffer[6] == 0x02)) {
        if (buffer[6] == 0x01) {
            set_mic_volume(buffer[7]);
        } else {
            set_mic_reverb(buffer[7]);
        }
        printf("Mic controls: volume=%u reverb=%u\r\n",
               g_mic_volume, g_mic_reverb);
        memcpy(g_feature_reply, buffer, frame_len);
        g_feature_reply_len = (uint8_t)frame_len;
        g_feature_reply_pending = true;
    } else if (command == 0xFC && payload_len == 3 &&
               buffer[4] == 0xB0 && buffer[5] == 0xA1) {
        set_pa10_effect_mode(buffer[6]);
        memcpy(g_feature_reply, buffer, frame_len);
        g_feature_reply_len = (uint8_t)frame_len;
        g_feature_reply_pending = true;
    } else if (command == 0xFC && payload_len == 2 &&
               buffer[4] == 0xC0 && buffer[5] == 0xA1) {
        uint8_t const effect =
            g_pa10_effect_mode <= PA10_EFFECT_ORIGINAL
                ? g_pa10_effect_mode
                : PA10_EFFECT_KTV;
        uint8_t const reply[] = {
            0xA5, 0x5A, 0xFC, 0x03, 0xA0, 0xA1, effect, 0x16
        };
        memcpy(g_feature_reply, reply, sizeof(reply));
        g_feature_reply_len = sizeof(reply);
        g_feature_reply_pending = true;
    } else if (command == 0x88 && payload_len == 0x12u) {
        process_ts02_effect_chunk(buffer);
        /* mic_load_effect() checks response[0] against request[2] after every
         * chunk and aborts the upload after ten mismatches. */
        g_feature_reply[0] = command;
        g_feature_reply_len = 1u;
        g_feature_reply_pending = true;
    } else if (command == 0xFC && payload_len == 2 &&
               buffer[4] == 0xC0 &&
               (buffer[5] == 0x08 || buffer[5] == 0x07)) {
        uint8_t const control_value =
            buffer[5] == 0x08 ? g_mic_volume : g_mic_reverb;
        uint8_t const reply[] = {
            0xA5, 0x5A, 0xFC, 0x03, 0xA0, buffer[5], control_value, 0x16
        };
        memcpy(g_feature_reply, reply, sizeof(reply));
        g_feature_reply_len = sizeof(reply);
        g_feature_reply_pending = true;
    } else if (command == 0xFC && payload_len == 2 &&
        buffer[4] == 0x98 && buffer[5] == 0x01) {
        uint8_t const reply[] = {
            0xA5, 0x5A, 0xFC, 0x03, 0xB0, 0x00, 0x20, 0x16
        };
        memcpy(g_feature_reply, reply, sizeof(reply));
        g_feature_reply_len = sizeof(reply);
        g_feature_reply_pending = true;
    } else if (payload_len == 0) {
        /* Read-poll marker: preserve a response queued by the preceding
         * SET_REPORT, otherwise keep a valid empty response available. */
        if (!g_feature_reply_pending) {
            set_idle_control_reply(command);
        }
    } else {
        memcpy(g_feature_reply, buffer, frame_len);
        g_feature_reply_len = (uint8_t)frame_len;
        g_feature_reply_pending = true;
    }
}

static uint8_t bpa10_checksum(uint8_t const *packet, uint8_t len)
{
    uint8_t sum = 0;
    for (uint8_t i = 4; i + 1 < len; i++) {
        sum = (uint8_t)(sum + packet[i]);
    }
    return sum;
}

static void write_u16_le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
}

static void queue_hid_reply(uint8_t const *packet, uint8_t len)
{
    if (len > sizeof(g_hid_reply)) {
        return;
    }
    memcpy(g_hid_reply, packet, len);
    g_hid_reply_len = len;
}

static void queue_connection_state(void)
{
    enum {
        connection_frame_len = 13u,
        diagnostic_offset = 48u
    };
    uint8_t packet[CFG_TUD_HID_EP_BUFSIZE] = {
        BPA10_REPORT_ID, BPA10_CMD_CONNECT_STATE, 0x00, 0x08,
        0x02, 0xA1, 0x10, 0x00, 0x00, 0x01,
        0x01, 0x01, 0x00
    };

    /* Preserve the original 13-byte BPA10 frame exactly.  MiniKaraoke reads
     * a 64-byte interrupt buffer and ignores bytes after the logical frame,
     * so expose probe counters in that otherwise-zero tail for logcat:
     *   48..51 "P3DG", 52 version, 53 active, 54 last alt,
     *   55 sample-rate control count,
     *   56..57 SET_INTERFACE alt>0 count, 58..59 all IN callbacks,
     *   60 volume, 61 reverb, 62..63 PCM completions.
     */
    packet[connection_frame_len - 1u] =
        bpa10_checksum(packet, connection_frame_len);
    packet[diagnostic_offset + 0u] = 'P';
    packet[diagnostic_offset + 1u] = '3';
    packet[diagnostic_offset + 2u] = 'D';
    packet[diagnostic_offset + 3u] = 'G';
    packet[diagnostic_offset + 4u] = 22u;
    packet[diagnostic_offset + 5u] = g_audio_alt_active ? 1u : 0u;
    packet[diagnostic_offset + 6u] = g_audio_last_alt;
    packet[diagnostic_offset + 7u] = g_audio_rate_control_count;
    write_u16_le(&packet[diagnostic_offset + 8u], g_audio_alt_select_count);
    write_u16_le(&packet[diagnostic_offset + 10u], g_audio_tx_callback_total);
    packet[diagnostic_offset + 12u] = g_mic_volume;
    packet[diagnostic_offset + 13u] = g_mic_reverb;
    write_u16_le(&packet[diagnostic_offset + 14u], g_audio_tx_total);
    queue_hid_reply(packet, sizeof(packet));
}

static void queue_receiver_version(void)
{
    static char const version[] = "RX-1-20250324-1";
    uint8_t packet[4u + sizeof(version) - 1u + 1u] = {
        BPA10_REPORT_ID, BPA10_CMD_RECEIVER_VER, 0x01,
        (uint8_t)(sizeof(version) - 1u)
    };
    memcpy(&packet[4], version, sizeof(version) - 1u);
    packet[sizeof(packet) - 1u] = bpa10_checksum(packet, sizeof(packet));
    queue_hid_reply(packet, sizeof(packet));
}

static void queue_mic_info(void)
{
    static char const version[] = "TX-20241109-1";
    static char const model[16] = "BYD Wireless Mic";
    enum {
        payload_len = sizeof(g_fake_mic_mac) + sizeof(version) - 1u + sizeof(model),
        packet_len = 4u + payload_len + 1u
    };
    uint8_t packet[packet_len] = {
        BPA10_REPORT_ID, BPA10_CMD_MIC_INFO,
        (uint8_t)(payload_len >> 8), (uint8_t)payload_len
    };
    uint8_t offset = 4u;
    memcpy(&packet[offset], g_fake_mic_mac, sizeof(g_fake_mic_mac));
    offset += sizeof(g_fake_mic_mac);
    memcpy(&packet[offset], version, sizeof(version) - 1u);
    offset += sizeof(version) - 1u;
    memcpy(&packet[offset], model, sizeof(model));
    packet[sizeof(packet) - 1u] = bpa10_checksum(packet, sizeof(packet));
    queue_hid_reply(packet, sizeof(packet));
}

static void queue_pa10_control_value(uint8_t subcommand,
                                     uint8_t const label[6],
                                     uint8_t value)
{
    uint8_t reply[12] = {
        BPA10_REPORT_ID, BPA10_CMD_GET_EFFECT_MD5, subcommand, 0x07
    };
    memcpy(&reply[4], label, 6);
    reply[10] = value;
    reply[11] = bpa10_checksum(reply, sizeof(reply));
    queue_hid_reply(reply, sizeof(reply));
}

static void handle_bpa10_command(uint8_t const *packet, uint16_t len)
{
    if (len < 4 || packet[0] != BPA10_REPORT_ID) {
        return;
    }

    switch (packet[1]) {
    case BPA10_CMD_GET_EFFECT_MD5: {
        static uint8_t const mic_volume_label[6] = {'M', 'I', 'C', 'v', 'o', 'l'};
        static uint8_t const reverb_label[6] = {'R', 'e', 'v', 'e', 'r', 'b'};
        static uint8_t const effect_label[6] = {'E', 'f', 'f', 'e', 'c', 't'};

        if (packet[2] == 0x04u) {
            queue_pa10_control_value(packet[2], mic_volume_label,
                                     g_pa10_volume);
            break;
        }
        if (packet[2] == 0x05u) {
            queue_pa10_control_value(packet[2], reverb_label,
                                     g_pa10_reverb);
            break;
        }
        if (packet[2] == 0x06u) {
            uint8_t const reported_mode =
                g_pa10_effect_mode <= PA10_EFFECT_ORIGINAL
                    ? g_pa10_effect_mode
                    : PA10_EFFECT_KTV;
            queue_pa10_control_value(packet[2], effect_label, reported_mode);
            break;
        }

        uint8_t reply[21] = {BPA10_REPORT_ID, BPA10_CMD_GET_EFFECT_MD5,
                             packet[2], 0x10};
        if (packet[2] == 10u) {
            memcpy(&reply[4], g_effect_10_md5, sizeof(g_effect_10_md5));
        }
        reply[20] = bpa10_checksum(reply, sizeof(reply));
        queue_hid_reply(reply, sizeof(reply));
        break;
    }

    case BPA10_CMD_SET_CONTROL: {
        static uint8_t const mic_volume_label[6] = {'M', 'I', 'C', 'v', 'o', 'l'};
        static uint8_t const reverb_label[6] = {'R', 'e', 'v', 'e', 'r', 'b'};
        static uint8_t const effect_label[6] = {'E', 'f', 'f', 'e', 'c', 't'};

        if (len < 12u || packet[3] != 0x07u ||
            packet[11] != bpa10_checksum(packet, 12u)) {
            break;
        }

        if (packet[2] == 0x03u &&
            memcmp(&packet[4], mic_volume_label, sizeof(mic_volume_label)) == 0) {
            set_mic_volume(pa10_ui_to_control(packet[10]));
        } else if (packet[2] == 0x04u &&
                   memcmp(&packet[4], reverb_label, sizeof(reverb_label)) == 0) {
            set_mic_reverb(pa10_ui_to_control(packet[10]));
        } else if (packet[2] == 0x05u &&
                   memcmp(&packet[4], effect_label, sizeof(effect_label)) == 0) {
            set_pa10_effect_mode(packet[10]);
        }
        break;
    }

    case BPA10_CMD_RECEIVER_VER:
        queue_receiver_version();
        break;

    case BPA10_CMD_SAVE_EFFECT: {
        static uint8_t const save_label[4] = {'S', 'A', 'V', 'E'};

        /* MiniKaraoke sends this immediately before the three 0x4D/0x0A
         * DSP parameter blocks whenever the user selects one of its four
         * acoustic modes:
         *   4D 0B 00 05 "SAVE" MM CS
         * MM is 0=Studio, 1=KTV, 2=Hall, 3=Original. */
        if (len >= 10u && packet[2] == 0x00u && packet[3] == 0x05u &&
            memcmp(&packet[4], save_label, sizeof(save_label)) == 0 &&
            packet[8] <= PA10_EFFECT_ORIGINAL &&
            packet[9] == bpa10_checksum(packet, 10u)) {
            set_pa10_effect_mode(packet[8]);
        }
        break;
    }

    case BPA10_CMD_MUTE: {
        uint8_t reply[] = {BPA10_REPORT_ID, BPA10_CMD_MUTE, 0x00, 0x01, 0x01, 0x00};
        reply[sizeof(reply) - 1] = bpa10_checksum(reply, sizeof(reply));
        queue_hid_reply(reply, sizeof(reply));
        break;
    }

    case BPA10_CMD_MIC_INFO:
        queue_mic_info();
        break;

    case BPA10_CMD_CONNECT_STATE:
        queue_connection_state();
        break;

    default:
        /* Other receiver configuration commands are accepted silently. */
        break;
    }
}

static void service_bpa10_hid(void)
{
    if (g_connection_report_pending && g_hid_reply_len == 0) {
        queue_connection_state();
        g_connection_report_pending = false;
    }

    if (g_hid_reply_len != 0 && tud_hid_n_ready(0)) {
        if (tud_hid_n_report(0, 0, g_hid_reply, g_hid_reply_len)) {
            dump_packet("BPA10 IN", g_hid_reply, g_hid_reply_len);
            g_hid_reply_len = 0;
        }
    }
}

static void dump_packet(char const *prefix, uint8_t const *data, uint16_t len)
{
#if ENABLE_USB_DEBUG
    /* A full 64-byte hexadecimal dump takes many milliseconds at 115200 baud.
     * Do not let startup diagnostics starve the isochronous audio FIFO once
     * the host has selected the streaming alternate setting. */
    if (g_audio_alt_active) {
        return;
    }

    printf("%s len=%u :", prefix, (unsigned)len);
    for (uint16_t i = 0; i < len; i++) {
        printf(" %02X", data[i]);
    }
    printf("\r\n");
#else
    (void)prefix;
    (void)data;
    (void)len;
#endif
}

int main(void)
{
    /* 120 MHz / 19.53125 / 128 = exactly 48 kHz. At the SDK's default
     * 125 MHz the PIO's 8-bit fractional divider cannot represent the
     * required ratio exactly, so capture slowly walks against USB SOF. */
    set_sys_clock_khz(120000u, true);
    board_init();
    stdio_init_all();
    status_led_init();
    buttons_init();

    bool const inmp441_ready = inmp441_i2s_init();
    if (inmp441_ready) {
        inmp441_i2s_start();
    }

    /*
     * Initialize the RP2040 root hub explicitly as a full-speed USB device.
     * pico-sdk's tinyusb_device target does not provide a legacy default
     * root-hub mode for this project-local TinyUSB checkout.
     */
    tusb_rhport_init_t const device_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL
    };
    tusb_init(0, &device_init);

    printf("\r\nPA10/BPA10 firmware starting\r\n");
    printf("VID=4661 PID=0002 Manufacturer=SM\r\n");
    printf("Interfaces: 0=AudioControl, 1=AudioStreaming, 2=HID data, 3=vendor control, 4=HID keyboard\r\n");
    printf("Audio EP: no-sync (bmAttributes=0x01), INMP441 input\r\n");
    printf("INMP441: GP12=BCLK GP13=WS GP14=SD: %s\r\n",
           inmp441_ready ? "ready" : "INIT FAILED");
    printf("Buttons: GP8=volume+ GP9=volume- GP10=menu (active-low)\r\n");
    while (true) {
        tud_task();
        service_bpa10_hid();
        service_buttons();
        service_audio_stream();
        tight_loop_contents();
    }
}

void tud_mount_cb(void)
{
    printf("USB mounted/enumerated\r\n");
    g_connection_report_pending = true;
    set_mic_volume(pa10_ui_to_control(PA10_DEFAULT_UI_VOLUME));
    queue_button_report();
}

void tud_umount_cb(void)
{
    g_audio_alt_active = false;
    g_audio_tx_count = 0;
    g_pa10_effect_mode = PA10_EFFECT_IDLE;
    g_button_report_head = 0u;
    g_button_report_count = 0u;
    status_led_show_effect(PA10_EFFECT_IDLE);
    printf("USB unmounted\r\n");
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    printf("USB suspended\r\n");
}

void tud_resume_cb(void)
{
    printf("USB resumed\r\n");
}

/* -------------------------------------------------------------------------
 * HID probe
 * -------------------------------------------------------------------------
 * The data endpoint implements the initial BPA10 receiver handshake; the
 * control endpoint retains the last framed Feature/Output response for the
 * app's GET_REPORT request.
 */
uint16_t tud_hid_get_report_cb(uint8_t instance,
                               uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)report_id;

    (void)instance;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer,
                           uint16_t bufsize)
{
    (void)report_id;

    if (instance == 0 && report_type == HID_REPORT_TYPE_OUTPUT) {
        dump_packet("HID DATA OUT", buffer, bufsize);
        handle_bpa10_command(buffer, bufsize);
    }
}

/* Interface #3 is deliberately vendor-class so Android does not bind its HID
 * kernel driver.  MiniKaraoke still addresses it with HID class request values:
 * SET_REPORT (0x09, OUT) and GET_REPORT (0x01, IN). */
bool tud_vendor_control_xfer_cb(uint8_t rhport,
                                uint8_t stage,
                                tusb_control_request_t const *request)
{
    if (request->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE ||
        tu_u16_low(request->wIndex) != 3 ||
        request->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS) {
        return false;
    }

    if (request->bRequest == HID_REQ_CONTROL_SET_REPORT) {
        if (stage == CONTROL_STAGE_SETUP) {
            if (request->wLength > sizeof(g_vendor_control_buffer)) {
                return false;
            }
            return tud_control_xfer(rhport, request, g_vendor_control_buffer,
                                    request->wLength);
        }
        if (stage == CONTROL_STAGE_ACK) {
            dump_packet("VENDOR CTRL SET", g_vendor_control_buffer,
                        request->wLength);
            handle_control_frame(g_vendor_control_buffer, request->wLength);
        }
        return true;
    }

    if (request->bRequest == HID_REQ_CONTROL_GET_REPORT) {
        if (stage == CONTROL_STAGE_SETUP) {
            if (g_feature_reply_len == 0) {
                set_idle_control_reply(0);
            }
            uint16_t length = TU_MIN(request->wLength, g_feature_reply_len);
            memcpy(g_vendor_control_buffer, g_feature_reply, length);
            dump_packet("VENDOR CTRL GET", g_vendor_control_buffer, length);
            return tud_control_xfer(rhport, request, g_vendor_control_buffer,
                                    length);
        }
        if (stage == CONTROL_STAGE_ACK) {
            finish_control_reply();
        }
        return true;
    }

    return false;
}

/* -------------------------------------------------------------------------
 * USB microphone audio processing pipeline
 * -------------------------------------------------------------------------
 * The UAC1 driver consumes PCM queued by tud_audio_write().  Feed exactly one
 * millisecond of 48 kHz mono 16-bit PCM per iteration; unlike the older UAC2
 * driver, it does not invoke a pre-load callback to request this data.
 */
static bool fill_audio_packet(void)
{
    int16_t captured[INMP441_BLOCK_SAMPLES];
    /* Never manufacture a 1 ms zero block between adjacent microphone
     * blocks. Those hard discontinuities sound like a high tone/robotic
     * voice even though the zoomed-out waveform looks plausible. */
    if (!inmp441_i2s_read_block(captured)) {
        return false;
    }

    for (size_t i = 0; i < TU_ARRAY_SIZE(g_audio_packet); i += 2) {
        int16_t const filtered =
            filter_mic_vocal_band(captured[i / 2u]);
        int16_t const sample = process_mic_effects(filtered);

        g_audio_packet[i] = sample;
        g_audio_packet[i + 1] = sample;
    }
    return true;
}

static void service_audio_stream(void)
{
    if (!g_audio_alt_active) {
        return;
    }

    /* Hold an eight-packet cushion. The I2S side retains sixteen recent
     * blocks, so USB can remain several milliseconds behind the producer
     * instead of inserting silence whenever the two 1 ms clocks have a
     * slightly different phase. */
    tu_fifo_t *fifo = tud_audio_get_ep_in_ff();
    while (tu_fifo_count(fifo) <
               AUDIO_USB_TARGET_QUEUE_PACKETS * sizeof(g_audio_packet) &&
           tu_fifo_remaining(fifo) >= sizeof(g_audio_packet)) {
        if (!fill_audio_packet()) {
            break;
        }
        if (tud_audio_write(g_audio_packet, sizeof(g_audio_packet)) !=
            sizeof(g_audio_packet)) {
            break;
        }
    }
}

bool tud_audio_set_itf_cb(uint8_t rhport,
                          tusb_control_request_t const *request)
{
    (void)rhport;
    uint8_t const interface_number = TU_U16_LOW(request->wIndex);
    uint8_t const alternate_setting = TU_U16_LOW(request->wValue);

    if (interface_number == 1u) {
        g_audio_last_alt = alternate_setting;
        g_audio_alt_active = alternate_setting != 0u;
        if (g_audio_alt_active) {
            ++g_audio_alt_select_count;
            g_audio_tx_count = 0;
            g_last_audio_tx_ms = probe_time_ms();
            /* I2S capture already runs continuously. Restarting DMA from
             * inside this USB SET_INTERFACE control callback can delay or
             * race the status stage, preventing Windows from opening the
             * isochronous stream. */
            reset_mic_input_filter();
            reset_mic_effects();
            (void)tud_audio_clear_ep_in_ff();
        }
    }
    return true;
}

bool tud_audio_tx_done_isr(uint8_t rhport,
                           uint16_t n_bytes_sent,
                           uint8_t func_id,
                           uint8_t ep_in,
                           uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)func_id;
    (void)ep_in;

    if (cur_alt_setting != 0u) {
        g_audio_alt_active = true;
        ++g_audio_tx_callback_total;
        if (n_bytes_sent == 0u) {
            ++g_audio_tx_zero_total;
        } else {
            ++g_audio_tx_count;
            ++g_audio_tx_total;
        }
        g_last_audio_tx_ms = probe_time_ms();
    }
    return true;
}

/* UAC1 sampling-frequency endpoint control -------------------------------
 * The class-specific isochronous endpoint descriptor advertises bit 0
 * (sampling-frequency control).  QCOM's USB capture path programs 48 kHz
 * after selecting AS alt=1.  Leaving the weak TinyUSB callbacks in place
 * stalls that request, so the host never submits ISO IN URBs even though the
 * alternate setting remains selected.  This source is fixed-rate: accept
 * SET_CUR only for 48 kHz and report the same value for all GET queries.
 */
static void write_u24_le(uint8_t destination[3], uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
    destination[2] = (uint8_t)(value >> 16u);
}

bool tud_audio_set_req_ep_cb(uint8_t rhport,
                             tusb_control_request_t const *request,
                             uint8_t *buffer)
{
    (void)rhport;
    uint8_t const endpoint = TU_U16_LOW(request->wIndex);
    uint8_t const control = TU_U16_HIGH(request->wValue);

    if (endpoint != BPA10_AUDIO_EP_IN ||
        control != AUDIO10_EP_CTRL_SAMPLING_FREQ ||
        request->bRequest != AUDIO10_CS_REQ_SET_CUR ||
        request->wLength != 3u) {
        return false;
    }

    uint32_t const requested_rate =
        (uint32_t)buffer[0] |
        ((uint32_t)buffer[1] << 8u) |
        ((uint32_t)buffer[2] << 16u);
    printf("UAC1 EP SET_RATE=%lu\r\n", (unsigned long)requested_rate);
    if (requested_rate != CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE) {
        return false;
    }

    ++g_audio_rate_control_count;
    return true;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport,
                             tusb_control_request_t const *request)
{
    static uint8_t rate[3];
    static uint8_t resolution[3] = {1u, 0u, 0u};
    uint8_t const endpoint = TU_U16_LOW(request->wIndex);
    uint8_t const control = TU_U16_HIGH(request->wValue);

    if (endpoint != BPA10_AUDIO_EP_IN ||
        control != AUDIO10_EP_CTRL_SAMPLING_FREQ ||
        request->wLength != 3u) {
        return false;
    }

    ++g_audio_rate_control_count;
    if (request->bRequest == AUDIO10_CS_REQ_GET_RES) {
        return tud_control_xfer(rhport, request, resolution, sizeof(resolution));
    }
    if (request->bRequest != AUDIO10_CS_REQ_GET_CUR &&
        request->bRequest != AUDIO10_CS_REQ_GET_MIN &&
        request->bRequest != AUDIO10_CS_REQ_GET_MAX) {
        return false;
    }

    write_u24_le(rate, CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE);
    return tud_control_xfer(rhport, request, rate, sizeof(rate));
}

/* UAC1 Feature Unit controls ------------------------------------------------
 * Windows opens the microphone only after querying its declared mute and
 * volume controls.  The UAC1 descriptor advertises Feature Unit #2, so reply
 * to its standard GET_* requests instead of stalling them.
 */
bool tud_audio_set_req_entity_cb(uint8_t rhport,
                                 tusb_control_request_t const *request,
                                 uint8_t *buffer)
{
    (void)rhport;
    uint8_t channel = TU_U16_LOW(request->wValue);
    uint8_t control = TU_U16_HIGH(request->wValue);
    uint8_t entity = TU_U16_HIGH(request->wIndex);

    if (entity != 2 || channel > CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX ||
        request->bRequest != AUDIO10_CS_REQ_SET_CUR) {
        return false;
    }

    if (control == AUDIO10_FU_CTRL_MUTE && request->wLength == 1) {
        g_uac1_mute[channel] = buffer[0];
        return true;
    }
    if (control == AUDIO10_FU_CTRL_VOLUME && request->wLength == 2) {
        g_uac1_volume[channel] = (int16_t)tu_unaligned_read16(buffer);
        return true;
    }
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport,
                                 tusb_control_request_t const *request)
{
    static int16_t volume_min = -90 * 256;
    static int16_t volume_max = 0;
    static int16_t volume_res = 1 * 256;
    uint8_t channel = TU_U16_LOW(request->wValue);
    uint8_t control = TU_U16_HIGH(request->wValue);
    uint8_t entity = TU_U16_HIGH(request->wIndex);

    if (entity != 2 || channel > CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX) {
        return false;
    }

    if (control == AUDIO10_FU_CTRL_MUTE && request->bRequest == AUDIO10_CS_REQ_GET_CUR) {
        return tud_audio_buffer_and_schedule_control_xfer(
            rhport, request, &g_uac1_mute[channel], sizeof(g_uac1_mute[channel]));
    }

    if (control == AUDIO10_FU_CTRL_VOLUME) {
        switch (request->bRequest) {
        case AUDIO10_CS_REQ_GET_CUR:
            return tud_audio_buffer_and_schedule_control_xfer(
                rhport, request, &g_uac1_volume[channel], sizeof(g_uac1_volume[channel]));
        case AUDIO10_CS_REQ_GET_MIN:
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &volume_min, sizeof(volume_min));
        case AUDIO10_CS_REQ_GET_MAX:
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &volume_max, sizeof(volume_max));
        case AUDIO10_CS_REQ_GET_RES:
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &volume_res, sizeof(volume_res));
        default:
            return false;
        }
    }

    return false;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport,
                                   tusb_control_request_t const *request)
{
    (void)rhport;
    if (TU_U16_LOW(request->wIndex) == 1u &&
        TU_U16_LOW(request->wValue) == 0u) {
        g_audio_last_alt = 0u;
        g_audio_alt_active = false;
        g_audio_tx_count = 0;
    }
    reset_mic_effects();
    return true;
}
