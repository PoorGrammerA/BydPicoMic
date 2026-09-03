#!/bin/sh
set -eu

grep -q '#define USB_VID 0x4661' src/usb_descriptors.c
grep -q '#define USB_PID 0x0002' src/usb_descriptors.c
grep -q '"SM"' src/usb_descriptors.c
grep -q 'ITF_NUM_AUDIO_CONTROL = 0' src/usb_descriptors.c
grep -q 'ITF_NUM_AUDIO_STREAMING' src/usb_descriptors.c
grep -q 'ITF_NUM_HID' src/usb_descriptors.c
grep -q 'ITF_NUM_HID_KEYBOARD' src/usb_descriptors.c
grep -q 'TUD_HID_INOUT_DESCRIPTOR' src/usb_descriptors.c
grep -q 'TUD_HID_REPORT_DESC_KEYBOARD' src/usb_descriptors.c

echo 'Static descriptor/layout checks passed.'
