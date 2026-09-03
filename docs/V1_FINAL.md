# BydPicoMic v1 최종 구성

## 1. 기준점

| 항목 | 값 |
|---|---|
| Branch | `main` |
| Tag | `v1` |
| Commit | `806cfcf` |
| Board | Waveshare RP2040-Zero |
| Microphone | INMP441, left channel |
| SDK | Raspberry Pi Pico SDK 1.5.1 |
| USB identity | `4661:0002`, `SM`, `BYD-micTS02`, `P3U1ST04` |

`v1`은 PC Audacity 입력과 BYD 차량의 MiniKaraoke 경로에서 실시간 음성,
차량 volume/reverb/mode 연동을 확인한 기준점이다.

## 2. 배선

| INMP441 pin | RP2040-Zero | 펌웨어 정의 |
|---|---|---|
| VDD | 3V3 | 3.3 V only |
| GND | GND | common ground |
| SCK | GP12 | `INMP441_BCLK_PIN` |
| WS | GP13 | `INMP441_WS_PIN` |
| SD | GP14 | `INMP441_SD_PIN` |
| L/R | GND | WS=0 left slot |

## 3. 실시간 오디오 파이프라인

```text
INMP441 24-bit I2S slots
  → PIO0 clock/data capture
  → DMA 16 × 1 ms ring
  → I2S one-bit alignment correction
  → signed mono 16-bit PCM
  → 100 Hz high-pass, three one-pole stages
  → 10 kHz second-order Butterworth low-pass
  → vehicle mode/reverb DSP
  → vehicle volume attenuation
  → +3.00 dB preamp
  → -2.5 dBFS threshold, 9:1 limiter
  → mono sample duplicated to stereo
  → UAC1, 192 bytes every 1 ms
```

시스템 clock은 120 MHz다. PIO divider `19.53125`로 I2S sample rate가
정확히 48 kHz가 되며 USB SOF와 장시간 drift하는 문제를 줄였다.

USB endpoint queue는 약 8 ms를 목표로 유지하고, I2S DMA는 최근 16 ms를
보관한다. 데이터가 아직 준비되지 않았을 때 임의의 1 ms zero block을 끼우지
않아 로봇 음성과 주기적인 무음 틈을 방지한다.

## 4. DSP와 차량 제어

| 항목 | v1 동작 |
|---|---|
| 차량 volume | UI 0~10을 내부 0~15 attenuation으로 변환, 기본 8 |
| Preamp | +3.00 dB (`11572 / 8192`) |
| Limiter | threshold 24576, 9:1 |
| Reverb | 차량 dial을 기본 최대 50% wet mix로 변환, mode scale 후 최대 75% |
| Mode | Recording Studio, KTV, Music Hall, Original |
| Original | reverb delay path를 완전히 bypass |
| Low cut | 100 Hz one-pole HPF 3단, 82 Hz에서 약 -12 dB |
| High cut | 10 kHz 2차 Butterworth, 약 20 kHz에서 -27 dB |

리버브는 2111/3041 sample 고정 지연선을 사용한다. reverb가 큰 차량 설정에서는
flutter echo 또는 금속성 ringing처럼 들릴 수 있으며, 이는 무선/I2S 전송 오류가
아니라 현재 effect 구조의 특성이다.

## 5. USB와 버튼 상태

- Interface 0/1: UAC1 AudioControl/AudioStreaming
- Interface 2: PA10 HID interrupt IN/OUT
- Interface 3: vendor class + HID-shaped control compatibility
- Interface 4: 표준 HID keyboard, endpoint `0x86`

F3/F4/F5가 각각 차량 menu, volume up, volume down으로 동작하는 것은 실차에서
확인했다. 자동 버튼 시험 코드는 삭제됐고 v1에는 GPIO debounce/keyboard report
생성 코드가 없다. Interface 4는 향후 물리 버튼을 위한 호환 통로로만 유지된다.
*(참고: `v1` 태그 이후 `main` 브랜치에는 GP8/GP9/GP10 물리 버튼 25 ms 디바운스 및 Interface 4 HID 키보드 리포트 전송 기능이 구현되어 적용되었습니다.)*

## 6. 검증 완료 항목

- Windows 장치 인식과 stereo 48 kHz format
- Audacity timeline 시작 및 연속 PCM capture
- I2S bit alignment, DMA ring, USB queue 안정화
- 차량에서 실시간 사람 음성 출력
- 차량 volume/reverb/mode 명령 수신
- 82 Hz 저역 feedback 완화
- +3 dB gain에서 최종 차량 사용성 확인

## 7. 범위 밖 또는 보류

- GPIO 물리 버튼 구현
- 리버브 알고리즘의 diffusion/damping 개선
- 정식 제품용 VID/PID 및 인증
- RP2040 Pico W 네트워크 음원

Pico W/WebSocket/UDP 구상은 `codex/pico-w-websocket-mic` 브랜치에서 설계만
검토한 뒤 중단했다. 해당 브랜치에는 v1 이후 펌웨어 변경이 없다.

## 8. 과거 WAV asset

`assets/vocal_mono.wav`와 `tools/generate_test_wav.ps1`은 차량 USB audio route를
검증하던 단계의 자료다. 현재 `CMakeLists.txt`는 WAV를 object로 변환하지 않으며
v1 펌웨어는 INMP441만 입력원으로 사용한다.
