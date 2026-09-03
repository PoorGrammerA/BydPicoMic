# BydPicoMic v1 Final Configuration

## 1. Release Baseline

| Property | Value |
|---|---|
| Branch | `main` |
| Tag | `v1` |
| Board | Waveshare RP2040-Zero |
| Microphone | INMP441, left channel |
| SDK | Raspberry Pi Pico SDK 1.5.1 |
| USB identity | `4661:0002`, `SM`, `BYD-micTS02`, `P3U1ST04` |

`v1` is the baseline release point verifying real-time voice, vehicle volume/reverb/mode integration on PC Audacity input and BYD vehicle MiniKaraoke audio routing.

## 2. Wiring

| INMP441 pin | RP2040-Zero | Firmware Definition |
|---|---|---|
| VDD | 3V3 | 3.3 V only |
| GND | GND | Common ground |
| SCK | GP12 | `INMP441_BCLK_PIN` |
| WS | GP13 | `INMP441_WS_PIN` |
| SD | GP14 | `INMP441_SD_PIN` |
| L/R | GND | WS=0 left slot |

## 3. Real-Time Audio Pipeline

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

System clock is 120 MHz. The PIO divider `19.53125` yields an exact 48 kHz I2S sample rate, minimizing long-term drift against USB SOF.

The USB endpoint queue is kept at a target cushion of ~8 ms, and I2S DMA retains the most recent 16 ms. When data is not yet ready, no arbitrary 1 ms zero block is inserted, preventing robotic voice artifacts and periodic silent gaps.

## 4. DSP and Vehicle Control

| Feature | v1 Behavior |
|---|---|
| Vehicle volume | Converts UI 0..10 to internal 0..15 attenuation, default 8 |
| Preamp | +3.00 dB (`11572 / 8192`) |
| Limiter | Threshold 24576, 9:1 ratio |
| Reverb | Converts vehicle dial to max 50% wet mix, scaling by mode up to max 75% |
| Mode | Recording Studio, KTV, Music Hall, Original |
| Original | Completely bypasses reverb delay path |
| Low cut | 100 Hz one-pole HPF (3 stages), ~-12 dB at 82 Hz |
| High cut | 10 kHz 2nd-order Butterworth, ~-27 dB at 20 kHz |

Reverb uses fixed delay lines of 2111 / 3041 samples. At high reverb dial settings, flutter echo or metallic ringing may occur, which is a characteristic of the current effect structure rather than wireless or I2S transmission errors.

## 5. USB and Button Status

- Interface 0/1: UAC1 AudioControl / AudioStreaming
- Interface 2: PA10 HID interrupt IN/OUT
- Interface 3: Vendor class + HID-shaped control compatibility
- Interface 4: Standard HID keyboard, endpoint `0x86`

F3/F4/F5 were verified on an actual vehicle as menu, volume up, and volume down respectively. Automated button test code has been removed.
*(Note: After the `v1` tag, the `main` branch connects GP8/GP9/GP10 physical buttons with 25 ms debounce and Interface 4 HID keyboard report generation).*

## 6. Verified Milestones

- Windows device recognition and stereo 48 kHz format
- Audacity timeline recording and continuous PCM capture
- I2S bit alignment, DMA ring, and USB queue stabilization
- Real-time human voice output on vehicle
- Vehicle volume / reverb / mode command reception
- Mitigated 82 Hz low-frequency feedback
- Final vehicle usability confirmed at +3 dB gain

## 7. Out of Scope / Deferred

- Advanced reverb algorithm diffusion/damping improvements
- Official production VID/PID and certifications
- RP2040 Pico W network audio sources

Pico W / WebSocket / UDP ideas were explored only on the `codex/pico-w-websocket-mic` branch and discontinued.

<details>
<summary><b>한국어 (Korean)</b></summary>

# BydPicoMic v1 최종 구성

## 1. 기준점

| 항목 | 값 |
|---|---|
| Branch | `main` |
| Tag | `v1` |
| Board | Waveshare RP2040-Zero |
| Microphone | INMP441, left channel |
| SDK | Raspberry Pi Pico SDK 1.5.1 |
| USB identity | `4661:0002`, `SM`, `BYD-micTS02`, `P3U1ST04` |

`v1`은 PC Audacity 입력과 BYD 차량의 MiniKaraoke 경로에서 실시간 음성, 차량 volume/reverb/mode 연동을 확인한 기준점이다.

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

시스템 clock은 120 MHz다. PIO divider `19.53125`로 I2S sample rate가 정확히 48 kHz가 되며 USB SOF와 장시간 drift하는 문제를 줄였다.

USB endpoint queue는 약 8 ms를 목표로 유지하고, I2S DMA는 최근 16 ms를 보관한다. 데이터가 아직 준비되지 않았을 때 임의의 1 ms zero block을 끼우지 않아 로봇 음성과 주기적인 무음 틈을 방지한다.

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

## 5. USB와 버튼 상태

- Interface 0/1: UAC1 AudioControl/AudioStreaming
- Interface 2: PA10 HID interrupt IN/OUT
- Interface 3: vendor class + HID-shaped control compatibility
- Interface 4: 표준 HID keyboard, endpoint `0x86`

F3/F4/F5가 각각 차량 menu, volume up, volume down으로 동작하는 것은 실차에서 확인했다.
*(참고: `v1` 태그 이후 `main` 브랜치에는 GP8/GP9/GP10 물리 버튼 25 ms 디바운스 및 Interface 4 HID 키보드 리포트 전송 기능이 구현되어 적용되었습니다.)*

</details>
