#include <string.h>
#include "tusb.h"

/*
 * Phase-3 compatibility probe values derived from the MiniKaraoke application:
 *   VID          0x4661
 *   PID          0x0002
 *   Manufacturer "SM"
 *
 * Use these values only for private interoperability testing. A shipping
 * product should use an assigned VID/PID and an agreed integration contract.
 */
#define USB_VID 0x4661
#define USB_PID 0x0002
#define USB_BCD 0x0100

/* Endpoints: EP1 IN = audio, EP2 OUT + EP3 IN = BPA10 HID data,
 * EP4 OUT + EP5 IN = BPA10 HID control compatibility, EP6 IN = keyboard.
 */
#define EPNUM_AUDIO_IN 0x81
#define EPNUM_HID_OUT  0x02
#define EPNUM_HID_IN   0x83
#define EPNUM_HID_CTRL_OUT 0x04
#define EPNUM_HID_CTRL_IN  0x85
#define EPNUM_HID_KEYBOARD_IN 0x86

enum {
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING,
    ITF_NUM_HID,
    ITF_NUM_HID_CONTROL,
    ITF_NUM_HID_KEYBOARD,
    ITF_NUM_TOTAL
};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL
};

/* UAC1 is exposed as a conventional per-interface composite function. */
static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,

    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,

    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = USB_BCD,

    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,

    .bNumConfigurations = 1
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

/* Generic HID with one interrupt OUT endpoint and one interrupt IN endpoint. */
uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(CFG_TUD_HID_EP_BUFSIZE)
};

uint8_t const desc_hid_keyboard_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    if (instance == 0) {
        return desc_hid_report;
    }
    if (instance == 1) {
        return desc_hid_keyboard_report;
    }
    return NULL;
}

#define CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + PA10_UAC1_STEREO_MIC_DESC_LEN + \
     TUD_HID_INOUT_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_HID_DESC_LEN)

/*
 * Interface layout intentionally matches the layout MiniKaraoke expects:
 *   #0 Audio Control
 *   #1 Audio Streaming
 *   #2 HID data (must be class 3 and claimable)
 *   #3 vendor control (MiniKaraoke sends HID-shaped class requests here)
 *   #4 HID keyboard (F3/F4/F5 physical-button compatibility probe)
 */
static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    /* UAC1 48 kHz, 16-bit, stereo capture. MiniKaraoke opens its loopback
     * AudioRecord with CHANNEL_IN_STEREO, so both channels carry the same
     * processed mono microphone sample from the INMP441 I2S pipeline. */
    TUD_AUDIO10_DESC_STD_AC(ITF_NUM_AUDIO_CONTROL, 0, 0),
    TUD_AUDIO10_DESC_CS_AC(0x0100,
        TUD_AUDIO10_DESC_INPUT_TERM_LEN + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN +
        TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2), ITF_NUM_AUDIO_STREAMING),
    TUD_AUDIO10_DESC_INPUT_TERM(1, AUDIO_TERM_TYPE_IN_GENERIC_MIC, 3, 2,
        AUDIO10_CHANNEL_CONFIG_LEFT_FRONT | AUDIO10_CHANNEL_CONFIG_RIGHT_FRONT, 0, 0),
    TUD_AUDIO10_DESC_OUTPUT_TERM(3, AUDIO_TERM_TYPE_USB_STREAMING, 1, 2, 0),
    TUD_AUDIO10_DESC_FEATURE_UNIT(2, 1, 0,
        AUDIO10_FU_CONTROL_BM_MUTE | AUDIO10_FU_CONTROL_BM_VOLUME,
        AUDIO10_FU_CONTROL_BM_MUTE | AUDIO10_FU_CONTROL_BM_VOLUME,
        AUDIO10_FU_CONTROL_BM_MUTE | AUDIO10_FU_CONTROL_BM_VOLUME),
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0, 0, 0),
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 1, 1, 0),
    TUD_AUDIO10_DESC_CS_AS_INT(3, 1, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(2,
        CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX,
        CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX * 8,
        CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE),
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_IN,
        TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_NO_SYNC,
        CFG_TUD_AUDIO_EP_SZ_IN, 1, 0),
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ,
        AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 1),

    TUD_HID_INOUT_DESCRIPTOR(
        ITF_NUM_HID,
        0,
        HID_ITF_PROTOCOL_NONE,
        sizeof(desc_hid_report),
        EPNUM_HID_OUT,
        EPNUM_HID_IN,
        CFG_TUD_HID_EP_BUFSIZE,
        10
    ),

    /* Keep interface #3 vendor-class so Android's HID driver does not retain
     * it.  libTsService nevertheless scans this interface for interrupt IN
     * and OUT endpoints, so expose the original transfer type and interval
     * while main.c continues to handle its HID-shaped class requests. */
    9, TUSB_DESC_INTERFACE, ITF_NUM_HID_CONTROL, 0, 2,
        TUSB_CLASS_VENDOR_SPECIFIC, 0x00, 0x00, 0,
    7, TUSB_DESC_ENDPOINT, EPNUM_HID_CTRL_OUT, TUSB_XFER_INTERRUPT,
        U16_TO_U8S_LE(64), 10,
    7, TUSB_DESC_ENDPOINT, EPNUM_HID_CTRL_IN, TUSB_XFER_INTERRUPT,
        U16_TO_U8S_LE(64), 10,

    TUD_HID_DESCRIPTOR(
        ITF_NUM_HID_KEYBOARD,
        0,
        HID_ITF_PROTOCOL_KEYBOARD,
        sizeof(desc_hid_keyboard_report),
        EPNUM_HID_KEYBOARD_IN,
        CFG_TUD_HID_EP_BUFSIZE,
        10
    )
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04}, /* English (0x0409) */
    "SM",                      /* exact MiniKaraoke BPA10 manufacturer match */
    /* MiniKaraoke's USB audio loopback allow-list includes this exact
     * product name.  The BPA10 VID/PID/HID match alone opens the settings
     * UI, but the audio loop is otherwise rejected before it starts.
     */
    "BYD-micTS02",
    /* Change the serial for the stereo UAC1 descriptor so Windows creates a
     * fresh audio-device instance instead of reusing its cached mono profile.
     */
    "P3U1ST04"
};

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;

    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= (sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
            return NULL;
        }

        char const *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) {
            chr_count = 31;
        }

        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = (uint8_t)str[i];
        }
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
