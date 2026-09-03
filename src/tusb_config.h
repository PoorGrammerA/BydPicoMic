#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined by the board/SDK configuration
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE
#endif

#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT 0
#endif

#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED OPT_MODE_DEFAULT_SPEED
#endif

#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED BOARD_TUD_MAX_SPEED
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE 64

/* Audio, one interrupt HID data channel, one HID keyboard, and one unclaimed
 * vendor control interface are needed for the Phase-3 compatibility probe. */
#define CFG_TUD_CDC    0
#define CFG_TUD_MSC    0
#define CFG_TUD_MIDI   0
#define CFG_TUD_VENDOR 1
#define CFG_TUD_AUDIO  1
#define CFG_TUD_HID    2

#define CFG_TUD_VENDOR_RX_BUFSIZE 64
#define CFG_TUD_VENDOR_TX_BUFSIZE 64
#define CFG_TUD_VENDOR_RX_EPSIZE  64
#define CFG_TUD_VENDOR_TX_EPSIZE  64
#define CFG_TUD_VENDOR_EP_INT_OUT 1
#define CFG_TUD_VENDOR_EP_INT_IN  1
#define CFG_TUD_VENDOR_EP_INT_OUT_BUFSIZE 64
#define CFG_TUD_VENDOR_EP_INT_IN_BUFSIZE  64

/* Generic HID IN/OUT endpoint size. */
#define CFG_TUD_HID_EP_BUFSIZE 64

/*
 * UAC1 USB microphone for vehicle-head-unit compatibility.  The descriptor
 * provides the stable interface layout
 * interface numbers are 0=Audio Control, 1=Audio Streaming, 2=HID data,
 * 3=HID control compatibility, and 4=HID keyboard button probe.
 */
#define CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE              48000
#define CFG_TUD_AUDIO_ENABLE_EP_IN                    1
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX            2
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX    2
/* The QCOM karaoke HAL uses a fixed 1 ms, 48-frame capture period.  Advertise
 * the exact nominal packet (48 frames * 2 channels * 2 bytes) rather than
 * TinyUSB's 196-byte clock-tolerance packet. */
#define CFG_TUD_AUDIO_EP_SZ_IN                         192
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX             CFG_TUD_AUDIO_EP_SZ_IN
/* Keep enough PCM queued to survive the BPA10 startup handshake.  Its UART
 * diagnostics can briefly delay the main loop; a four-packet FIFO underruns
 * during that burst and is heard as noise until initialization settles. */
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ          (64 * CFG_TUD_AUDIO_EP_SZ_IN)

#define PA10_UAC1_STEREO_MIC_DESC_LEN ( \
    TUD_AUDIO10_DESC_STD_AC_LEN + TUD_AUDIO10_DESC_CS_AC_LEN(1) + \
    TUD_AUDIO10_DESC_INPUT_TERM_LEN + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN + \
    TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2) + (2 * TUD_AUDIO10_DESC_STD_AS_LEN) + \
    TUD_AUDIO10_DESC_CS_AS_INT_LEN + TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(1) + \
    TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN + TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN)
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN      PA10_UAC1_STEREO_MIC_DESC_LEN
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT        1
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ     64

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */
