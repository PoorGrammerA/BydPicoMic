# BPA10 / BYD USB Microphone Compatibility Protocol Investigation Notes

## 1. Document Purpose and Scope

This document details the USB descriptors, initial handshakes, HID transmission methods, and frame formats verified to make the RP2040-Zero + INMP441 device recognized as a wired USB microphone receiver by the BYD MiniKaraoke app, delivering real-time UAC1 audio. The current implementation baseline is tag `v1`.

Final hardware specifications and DSP figures are documented separately in [V1_FINAL.md](V1_FINAL.md).

Data sources:
- Vehicle logcat and USB/audio dumps
- Decompilation results of `MiniKaraoke` APK and embedded libraries
- Actual verified behavior in `BydPicoMic` v1 firmware

Notation rules:
- **Verified**: Confirmed across two or more sources (logs, decompiled code, vehicle testing)
- **Implemented**: Currently handled by RP2040 firmware
- **Unimplemented**: Format confirmed, but firmware does not track state or respond
- **Inferred**: Derived from parser logic; requires re-verification with original hardware USB trace

These values were obtained from private interoperability testing. `0x4661:0x0002` is not an assigned VID/PID for this project and cannot be used for commercial distribution.

## 2. USB Device Identification

Current vehicle-compatible firmware device information:

| Property | Value | Status / Usage |
|---|---:|---|
| USB Spec | `0x0200` | USB 2.0 descriptor |
| VID | `0x4661` (18017) | MiniKaraoke BPA10 identification |
| PID | `0x0002` | MiniKaraoke BPA10 identification |
| bcdDevice | `0x0100` | Device release 1.00 |
| Manufacturer | `SM` | Used by app for device detection |
| Product | `BYD-micTS02` | Required to pass vehicle USB audio allow-list |
| Serial | `P3U1ST04` | Separates from Windows legacy WAV/mono descriptor cache |
| Configurations | 1 | Composite device |
| EP0 Size | 64 bytes | Control endpoint |

In vehicle logs, connecting this combination selected `UsbPA10MicControl:V2.10.3`. Therefore, even though the product string contains `TS02`, the primary control protocol used by the current vehicle UI is the **PA10 `0x4D` frame** detailed below.

## 3. USB Interface and Endpoint Configuration

### 3.1 Current Firmware Layout

| Interface | Class | Role | Endpoint |
|---:|---|---|---|
| 0 | Audio / AudioControl | UAC1 topology & Feature Unit | EP0 control |
| 1 alt 0 | Audio / AudioStreaming | Stream stopped | None |
| 1 alt 1 | Audio / AudioStreaming | Microphone PCM capture | `0x81` Isochronous IN |
| 2 | HID Generic IN/OUT | PA10 commands & async state | `0x02` Interrupt OUT, `0x83` Interrupt IN |
| 3 | Vendor specific | HID-shaped class-control channel | `0x04` Interrupt OUT, `0x85` Interrupt IN exposed in descriptor |
| 4 | HID keyboard | Verified F3/F4/F5 physical button channel | `0x86` Interrupt IN |

Interface 3 is intentionally advertised as a vendor class to prevent the Android kernel HID driver from claiming it exclusively, while control transfers (`SET_REPORT`/`GET_REPORT`) targeting interface 3 from user-space libraries are handled directly in firmware.

### 3.2 UAC1 Audio Format

| Item | Value |
|---|---|
| USB Audio Class | UAC1 (`bcdADC = 0x0100`) |
| Direction | Device → Vehicle, microphone capture |
| Sample Rate | 48,000 Hz |
| Bit Depth | Signed PCM 16-bit little-endian |
| Channels | 2-channel stereo |
| Audio Source | INMP441 mono sample duplicated to L/R |
| Audio Endpoint | `0x81` |
| Transfer Type | Isochronous, no-sync |
| Interval | 1 ms |
| Max Packet | 192 bytes (`48 frames × 2 ch × 2 bytes`) |
| Feature Unit ID | 2 |
| Feature Unit Controls | Mute, Volume |

## 4. Communication Overview

```text
Vehicle MiniKaraoke                       RP2040
────────────────────────────────────────────────────────────────
USB enumeration/descriptors  ────────>  Provide VID/PID/Strings/Interfaces

Interface 2, EP 0x02 OUT     ────────>  Receive PA10 0x4D commands
Interface 2, EP 0x83 IN      <────────  PA10 responses & async state

Interface 3, EP0 SET_REPORT  ────────>  A5 5A FC control frames
Interface 3, EP0 GET_REPORT  <────────  A5 5A FC response frames
Interface 2, EP 0x83 IN      <────────  A5 5A FD async events

Interface 1, EP 0x81 IN      <────────  UAC1 PCM audio
```

## 5. PA10 `0x4D` Frame Format

### 5.1 General Format

```text
Offset  Size  Meaning
0       1     Report ID = 0x4D ('M')
1       1     Command
2       1     Sub-command or object/effect ID
3       1     Payload length N
4       N     Payload
4+N     1     Checksum
```

Logical frame length is `N + 5` bytes. Interrupt endpoints use 64-byte read buffers, so zero-padding may follow the logical frame.

### 5.2 Checksum

Checksum is the 8-bit sum of payload bytes:

```c
uint8_t checksum = 0;
for (i = 4; i < frame_length - 1; ++i) {
    checksum += frame[i];
}
```

### 5.3 Primary Verified Commands

| Command | Direction | Meaning | Current Firmware Status |
|---:|---|---|---|
| `0x01` | Both | Effect MD5 query / reply | Implemented for ID 10 |
| `0x02` | Vehicle → Device | Setting write | Handled for volume/reverb/mode |
| `0x06` | Both | Receiver version | Implemented |
| `0x0D` | Both | Mute | Simple response implemented |
| `0x16` | Both | Microphone info / version | Implemented |
| `0x1C` | Both | Connection state query / report | Implemented |
| `0x1D` | Device → Vehicle | Key press | Parser verified, standard HID keyboard preferred |

## 6. Vehicle Connection & Handshake

1. Vehicle enumerates descriptors; MiniKaraoke selects `UsbPA10MicControl`.
2. Product string `BYD-micTS02` passes the vehicle USB audio allow-list.
3. RP2040 sends connection state frame `4D 1C 00 08 [MAC 6 bytes] 01 01 CS` on EP `0x83` IN.
4. Vehicle queries effect ID 10 MD5 (`4D 01 0A 02 01 01 02`); RP2040 replies with `B4 4F 62 61 DB 98 02 EA 80 B8 5F 32 DE 7F 51 E9` (matching `eq.bin` MD5) to prevent unnecessary 10-second effect binary uploads.
5. Vehicle sends initial UI volume, reverb, and mode commands (`4D 02 03/04/05`).

## 7. Four Vehicle Microphone Modes

| `MM` | App Enum | Display Name | Packet |
|---:|---|---|---|
| `00` | `STUDIO` | Recording Studio | `4D 02 05 07 45 66 66 65 63 74 00 4D` |
| `01` | `KTV` | KTV | `4D 02 05 07 45 66 66 65 63 74 01 4E` |
| `02` | `HALL` | Music Hall | `4D 02 05 07 45 66 66 65 63 74 02 4F` |
| `03` | `ORIGINAL` | Original | `4D 02 05 07 45 66 66 65 63 74 03 50` |

WS2812 Status LED Colors:
- Idle: White
- Recording Studio: Sky Blue
- KTV: Red
- Music Hall: Pink
- Original: Orange

## 8. Current Firmware Implementation Status

### Implemented
- USB enumeration & string identity
- UAC1 48 kHz / 16-bit / stereo capture
- Interface 2 interrupt OUT/IN
- Connection state reporting and responses
- Receiver version and microphone info responses
- Effect ID 10 MD5 response
- Mute response
- Interface 3 HID-shaped SET_REPORT / GET_REPORT
- Interface 4 standard HID keyboard channel & GP8 (Vol+) / GP9 (Vol-) / GP10 (Menu) physical buttons (25 ms debounce)
- `A5 5A FC` volume/reverb set & query
- PA10 `0x4D 02 03/04` volume/reverb set
- PA10 `0x4D 01 04/05` volume/reverb query
- PA10 & TS02 effect mode reception and 4-mode DSP
- INMP441 PIO/DMA 48 kHz capture and I2S 1-bit alignment
- 100 Hz high-pass filter, 10 kHz low-pass filter, +3 dB preamp, limiter
- Mono-to-stereo UAC1 transmission after volume/reverb/mode DSP

<details>
<summary><b>한국어 (Korean)</b></summary>

# BPA10 / BYD USB 마이크 호환 프로토콜 조사 노트

## 1. 문서 목적과 범위

이 문서는 RP2040-Zero + INMP441 장치가 BYD 차량의 MiniKaraoke 앱에서 유선 USB 마이크 수신기로 인식되고, 실시간 UAC1 오디오를 전달하기까지 확인한 USB descriptor, 초기 핸드셰이크, HID 전송 방식과 프레임 형식을 정리한다. 현재 구현 기준은 태그 `v1`이다.

최종 하드웨어와 DSP 수치는 [V1_FINAL.md](V1_FINAL.md)에 별도로 정리되어 있다.

## 2. USB 장치 식별 정보

| 항목 | 값 | 상태/용도 |
|---|---:|---|
| USB 규격 | `0x0200` | USB 2.0 descriptor |
| VID | `0x4661` (decimal 18017) | MiniKaraoke BPA10 식별 |
| PID | `0x0002` | MiniKaraoke BPA10 식별 |
| bcdDevice | `0x0100` | 장치 릴리스 1.00 |
| Manufacturer | `SM` | 앱의 장치 판별에 사용 |
| Product | `BYD-micTS02` | 차량 USB 오디오 허용 목록 통과에 필요 |
| Serial | `P3U1ST04` | Windows의 이전 WAV/mono descriptor 캐시 분리용 |

## 3. USB 인터페이스와 endpoint 구성

| Interface | 클래스 | 역할 | Endpoint |
|---:|---|---|---|
| 0 | Audio / AudioControl | UAC1 topology 및 Feature Unit | EP0 control |
| 1 alt 0 | Audio / AudioStreaming | 스트림 정지 | 없음 |
| 1 alt 1 | Audio / AudioStreaming | 마이크 PCM capture | `0x81` Isochronous IN |
| 2 | HID Generic IN/OUT | PA10 명령 및 비동기 상태 | `0x02` Interrupt OUT, `0x83` Interrupt IN |
| 3 | Vendor specific | HID 모양의 class-control 호환 통로 | `0x04` Interrupt OUT, `0x85` Interrupt IN |
| 4 | HID keyboard | 검증된 F3/F4/F5 물리 버튼 호환 통로 | `0x86` Interrupt IN |

</details>
