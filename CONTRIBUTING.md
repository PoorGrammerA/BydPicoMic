# Contributing

Contributions are welcome. Keep protocol observations separate from
assumptions, and include the vehicle/app version and relevant log excerpts
when reporting compatibility results.

Before opening a change:

1. Configure and build for `waveshare_rp2040_zero`.
2. Run `sh check_layout.sh` when a POSIX shell is available.
3. Confirm that the UAC1 audio, PA10 handshake, and HID keyboard interfaces
   still enumerate.
4. For audio-path changes, verify 48 kHz capture on a PC before testing in a
   vehicle. Record the vehicle volume, reverb, and effect mode used for the test.
5. Keep `main` compatible with the RP2040-Zero + INMP441 v1 hardware. Network
   source experiments belong on a separate branch and target.
6. Do not commit proprietary APKs, decompiled binaries, vehicle logs containing
   personal data, or recordings without redistribution permission.
