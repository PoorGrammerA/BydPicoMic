# Third-party software

This repository vendors a modified TinyUSB checkout under
`third_party/tinyusb-uac1` so the tested UAC1 build is reproducible without a
separate submodule or patch step.

- Project: TinyUSB
- Upstream: https://github.com/hathach/tinyusb
- Base commit: `fd70160a2f5fd23de1abfbaefb6399746a90b588`
- License: MIT; see `third_party/tinyusb-uac1/LICENSE`
- Local change: UAC1 no-sync capture IN endpoints are classified as audio data
  endpoints instead of feedback endpoints. The two changes are in
  `src/class/audio/audio_device.c`.

`tools/generate_test_wav.ps1` is a script for generating legacy test assets and is not linked into the v1
firmware. Verify redistribution rights before replacing or publishing audio
assets; the project license does not grant rights to unrelated recordings.
