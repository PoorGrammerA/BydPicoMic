# BPA10 / BYD USB 마이크 호환 프로토콜 조사 노트

## 1. 문서 목적과 범위

이 문서는 RP2040-Zero + INMP441 장치가 BYD 차량의 MiniKaraoke 앱에서 유선 USB 마이크 수신기로 인식되고, 실시간 UAC1 오디오를 전달하기까지 확인한 USB descriptor, 초기 핸드셰이크, HID 전송 방식과 프레임 형식을 정리한다. 현재 구현 기준은 태그 `v1`이다.

문제 발견부터 UAC1 차량 재생, WAV 검증, INMP441 전환과 음질 안정화까지의 시간순 개발 기록은 [DEVELOPMENT_HISTORY.md](DEVELOPMENT_HISTORY.md)를 참고한다. 최종 하드웨어와 DSP 수치는 [V1_FINAL.md](V1_FINAL.md)에 별도로 고정했다.

자료의 출처는 다음 세 가지다.

- 차량에서 수집한 logcat 및 USB/audio dumps
- `MiniKaraoke` APK와 포함 라이브러리의 디컴파일 결과
- 현재 `BydPicoMic` v1 펌웨어에서 실제로 동작 확인한 구현

표기 규칙:

- **확인됨**: 로그, 디컴파일 코드 및 차량 시험 중 둘 이상에서 일치한 내용
- **구현됨**: 현재 RP2040 펌웨어가 처리하는 내용
- **미구현**: 형식은 확인했지만 현재 펌웨어가 상태 저장 또는 응답하지 않는 내용
- **추정**: 파서 동작으로 유추했으며 원본 장치 USB 캡처로 재검증할 필요가 있는 내용

이 값들은 사설 상호운용 시험에서 얻은 것이다. `0x4661:0x0002`는 이 프로젝트에 할당된 VID/PID가 아니므로 제품 배포에는 사용할 수 없다.

## 2. USB 장치 식별 정보

현재 차량 호환 펌웨어의 장치 정보는 다음과 같다.

| 항목 | 값 | 상태/용도 |
|---|---:|---|
| USB 규격 | `0x0200` | USB 2.0 descriptor |
| VID | `0x4661` (decimal 18017) | MiniKaraoke BPA10 식별 |
| PID | `0x0002` | MiniKaraoke BPA10 식별 |
| bcdDevice | `0x0100` | 장치 릴리스 1.00 |
| Manufacturer | `SM` | 앱의 장치 판별에 사용 |
| Product | `BYD-micTS02` | 차량 USB 오디오 허용 목록 통과에 필요 |
| Serial | `P3U1ST04` | Windows의 이전 WAV/mono descriptor 캐시 분리용 |
| Configuration 수 | 1 | 복합 장치 |
| EP0 크기 | 64 bytes | Control endpoint |

차량 로그에서는 이 조합을 연결했을 때 `UsbPA10MicControl:V2.10.3`가 선택되었다. 따라서 제품 문자열에 `TS02`가 들어가더라도 현재 차량에서 실제 UI 제어에 사용된 주 프로토콜은 아래의 **PA10 `0x4D` 프레임**이다.

## 3. USB 인터페이스와 endpoint 구성

### 3.1 현재 펌웨어 구성

| Interface | 클래스 | 역할 | Endpoint |
|---:|---|---|---|
| 0 | Audio / AudioControl | UAC1 topology 및 Feature Unit | EP0 control |
| 1 alt 0 | Audio / AudioStreaming | 스트림 정지 | 없음 |
| 1 alt 1 | Audio / AudioStreaming | 마이크 PCM capture | `0x81` Isochronous IN |
| 2 | HID Generic IN/OUT | PA10 명령 및 비동기 상태 | `0x02` Interrupt OUT, `0x83` Interrupt IN |
| 3 | Vendor specific | HID 모양의 class-control 호환 통로 | `0x04` Interrupt OUT, `0x85` Interrupt IN도 descriptor에 노출 |
| 4 | HID keyboard | 검증된 F3/F4/F5 물리 버튼 호환 통로 | `0x86` Interrupt IN |

Interface 3은 현재 의도적으로 vendor class로 광고한다. Android 커널 HID 드라이버가 이 인터페이스를 독점하지 않게 하면서, 사용자 공간 라이브러리가 interface 3을 대상으로 보내는 HID `SET_REPORT`/`GET_REPORT` 형태의 control transfer는 펌웨어에서 직접 처리한다.

과거 수집 로그 일부에는 interface 3이 class 3(HID)로 표시된다. 이는 당시 probe 펌웨어 descriptor이며, 현재 소스의 vendor-class 구성과 혼동하지 않아야 한다.

### 3.2 UAC1 오디오 형식

| 항목 | 값 |
|---|---|
| USB Audio Class | UAC1 (`bcdADC = 0x0100`) |
| 방향 | 장치 → 차량, microphone capture |
| 표본화율 | 48,000 Hz |
| 비트 깊이 | Signed PCM 16-bit little-endian |
| 채널 | 2채널 stereo |
| 현재 음원 | INMP441 mono sample을 L/R에 복제 |
| Audio endpoint | `0x81` |
| 전송 | Isochronous, no-sync |
| 주기 | 1 ms |
| 현재 최대 packet | 192 bytes (`48 frames × 2 ch × 2 bytes`) |
| Feature Unit ID | 2 |
| Feature Unit 제어 | Mute, Volume |

차량은 AudioStreaming interface의 alt setting을 0에서 1로 바꿔 스트림을 시작한다. INMP441은 PIO/DMA로 계속 capture되며 펌웨어는 준비된 1 ms mono block을 DSP 처리하고 L/R로 복제해 192 bytes의 PCM을 `tud_audio_write()`에 공급한다.

## 4. 통신 경로 개요

```text
차량 MiniKaraoke                         RP2040
────────────────────────────────────────────────────────────────
USB enumeration/descriptors  ────────>  VID/PID/문자열/인터페이스 제공

Interface 2, EP 0x02 OUT     ────────>  PA10 0x4D 명령 수신
Interface 2, EP 0x83 IN      <────────  PA10 응답 및 비동기 상태

Interface 3, EP0 SET_REPORT  ────────>  A5 5A FC 제어 프레임
Interface 3, EP0 GET_REPORT  <────────  A5 5A FC 응답 프레임
Interface 2, EP 0x83 IN      <────────  A5 5A FD 비동기 이벤트(호환 경로)

Interface 1, EP 0x81 IN      <────────  UAC1 PCM 오디오
```

TinyUSB에서 interface 2 OUT은 `tud_hid_set_report_cb(instance=0, OUTPUT)`로 들어오며 `handle_bpa10_command()`가 처리한다. 장치→차량 응답은 `tud_hid_n_report(0, 0, packet, len)`으로 `0x83`에 보낸다.

Interface 3의 class-control 요청은 `tud_vendor_control_xfer_cb()`에서 다음 요청으로 처리한다.

- `bRequest = 0x09`: HID `SET_REPORT`, 차량→장치
- `bRequest = 0x01`: HID `GET_REPORT`, 장치→차량
- `wIndex` 하위 바이트: `3`
- 최대 호환 버퍼: 256 bytes, 일반 응답은 40 또는 64 bytes 이내

## 5. PA10 `0x4D` 프레임

### 5.1 일반 형식

payload가 있는 일반 프레임은 다음 형태다.

```text
Offset  Size  의미
0       1     Report ID = 0x4D ('M')
1       1     Command
2       1     Sub-command 또는 object/effect ID
3       1     Payload length N
4       N     Payload
4+N     1     Checksum
```

논리 프레임 길이는 보통 `N + 5` bytes다. Interrupt endpoint의 실제 읽기 버퍼는 64 bytes이므로, 로그에는 논리 프레임 뒤에 0 padding이 이어질 수 있다.

payload 길이가 0인 일부 단순 명령은 체크섬 없이 4 bytes만 생성된다. 예: `4D 1C 00 00` 연결 상태 조회.

### 5.2 체크섬

체크섬은 payload byte들의 8-bit 합이다.

```c
uint8_t checksum = 0;
for (i = 4; i < frame_length - 1; ++i) {
    checksum += frame[i];
}
```

즉 header의 `0x4D`, command, sub-command, length는 합산하지 않는다.

### 5.3 현재 확인된 주요 명령

| Command | 방향 | 의미 | 현재 펌웨어 |
|---:|---|---|---|
| `0x01` | 차량→장치 / 장치→차량 | Effect MD5 조회/응답 | ID 10 구현됨 |
| `0x02` | 차량→장치 | 설정 쓰기 | 현재 기본 분기에서 무시; 추가 처리 필요 |
| `0x04` | 차량→장치 | 마이크 연결/페어링 관련 | 형식 일부 확인, 미구현 |
| `0x05` | 양방향 | RSSI, battery, 연결 상태 계열 | 연결 parser 확인, 미구현 |
| `0x06` | 양방향 | 수신기 버전 | 응답 구현됨 |
| `0x09` | 양방향 | 주파수/RSSI 설정 및 조회 | 미구현 |
| `0x0A` | 차량→장치 | Effect binary 조각 | 저장 미구현 |
| `0x0B` | 차량→장치 | Effect 저장 시작 | 미구현 |
| `0x0D` | 양방향 | Mute | 간이 응답 구현됨 |
| `0x0F` | 차량→장치 | 마이크 firmware binary 조각 | 미구현 |
| `0x12` | 양방향 | Factory reset | 미구현 |
| `0x16` | 양방향 | 마이크 정보/버전 | 응답 구현됨 |
| `0x1B` | 차량→장치 | OTA 준비 | 미구현 |
| `0x1C` | 양방향 | 연결 상태 조회/보고 | 구현됨 |
| `0x1D` | 장치→차량 | Key press | parser 확인, 송신 미구현 |
| `0x1F` | 차량→장치 | OTA 과정 종료 | 미구현 |
| `0xE0` | 장치→차량 | USB/error 상태 | parser만 확인 |

OTA 및 effect binary 관련 명령은 정상 마이크 동작에 필수인 범위가 아니므로, 해당 명령에 대한 표는 구현 우선순위를 뜻하지 않는다.

## 6. 차량 연결 및 초기 핸드셰이크

현재까지 확인된 대표 순서는 다음과 같다. 비동기 IN 보고 때문에 일부 단계의 순서는 실행마다 앞뒤가 바뀔 수 있다.

### 6.1 Enumeration 및 장치 선택

1. 차량이 device/configuration/string descriptor를 읽는다.
2. AudioControl/AudioStreaming 및 HID data interface를 열거한다.
3. MiniKaraoke가 VID/PID와 인터페이스 구조를 보고 `UsbPA10MicControl`을 선택한다.
4. 제품 문자열 `BYD-micTS02`가 차량의 USB audio allow-list를 통과시킨다.
5. RP2040의 `tud_mount_cb()`가 연결 상태 보고를 pending으로 설정한다.

### 6.2 비동기 연결 상태 보고

RP2040은 다음 논리 프레임을 EP `0x83` IN으로 보고한다.

```text
4D 1C 00 08 02 A1 10 00 00 01 01 01 B6
```

| 위치 | 값 | 의미 |
|---:|---|---|
| 0 | `4D` | report ID |
| 1 | `1C` | connection state command |
| 2 | `00` | sub-command |
| 3 | `08` | payload 8 bytes |
| 4..9 | `02 A1 10 00 00 01` | 테스트용 microphone MAC |
| 10 | `01` | BLE/첫 연결 상태 필드 |
| 11 | `01` | UHF/둘째 연결 상태 필드 |
| 12 | `B6` | payload checksum |

현재 펌웨어는 64-byte interrupt report를 보내고, 위 13-byte 논리 프레임 뒤의 0 padding 영역 일부에 `P3DG` 진단 카운터를 넣는다. MiniKaraoke parser는 논리 길이 이후를 무시한다.

차량이 명시적으로 조회할 때 보내는 짧은 명령은 다음과 같다.

```text
4D 1C 00 00
```

### 6.3 Effect MD5 검사

차량이 effect ID 10의 MD5를 조회한다.

```text
차량 → 장치: 4D 01 0A 02 01 01 02
```

응답 형식:

```text
4D 01 0A 10 [16-byte MD5] CS
```

현재 구현 값:

```text
B4 4F 62 61 DB 98 02 EA 80 B8 5F 32 DE 7F 51 E9
```

이 값은 차량 asset `BPA10/YUAN_PLUS/Dirac/eq.bin`의 MD5다. 일치 값을 보고하면 차량이 effect binary를 다시 업로드하면서 발생하던 초기화 지연과 timeout을 피할 수 있다.

불일치하면 차량은 다음과 같이 저장 요청 후 `0x0A` 조각들을 전송한다.

```text
4D 0B 00 05 53 41 56 45 0A 39
            S  A  V  E  ID CS
```

Effect data frame은 보통 다음 형태다.

```text
4D 0A index_lo index_hi [data ...]
```

마지막 조각은 index 위치에 `ED ED`를 사용하고 실제 data length byte가 뒤따른다. 현재 RP2040은 effect binary 자체를 저장하거나 적용하지 않는다.

### 6.4 초기 UI 설정

MiniKaraoke는 연결 직후 기본값을 전송한다. 수집 로그에서는 volume 10, reverb 10, KTV mode 1이 확인되었다.

```text
Volume 10:
4D 02 03 07 4D 49 43 76 6F 6C 0A 34
            M  I  C  v  o  l

Reverb 10:
4D 02 04 07 52 65 76 65 72 62 0A 70
            R  e  v  e  r  b

KTV effect:
4D 02 05 07 45 66 66 65 63 74 01 4E
            E  f  f  e  c  t
```

P3DG v8부터 `handle_bpa10_command()`가 command `0x02`의 volume/reverb를 처리한다. 차량 UI의 0..10 값을 내부 DSP의 0..15 범위로 반올림 변환하며, `0x4D 01 04/05` 현재값 조회에도 PA10 형식으로 응답한다. P3DG v9에서는 Effect mode `0x02/0x05`도 저장하고 `0x4D 01 06` 조회에 응답한다.

### 6.5 버전과 마이크 정보

수신기 버전 조회:

```text
차량 → 장치: 4D 06 01 02 01 01 02
장치 → 차량: 4D 06 01 0F "RX-1-20250324-1" CS
```

현재 마이크 정보 응답의 payload 구성:

```text
[MAC 6 bytes]
[TX version = "TX-20241109-1"]
[model 16 bytes = "BYD Wireless Mic" + padding]
```

frame header는 `4D 16 length_hi length_lo`이며, 이 명령만 현재 구현에서 2-byte big-endian payload length 형태로 다룬다.

## 7. 차량의 4가지 마이크 모드

차량 UI의 네 모드는 command `0x02`, sub-command `0x05`로 RP2040에 직접 전달된다.

일반 형식:

```text
4D 02 05 07 45 66 66 65 63 74 MM CS
            E  f  f  e  c  t
```

| `MM` | 앱 enum | 차량 표시 | 완성된 패킷 |
|---:|---|---|---|
| `00` | `STUDIO` | Recording Studio | `4D 02 05 07 45 66 66 65 63 74 00 4D` |
| `01` | `KTV` | KTV | `4D 02 05 07 45 66 66 65 63 74 01 4E` |
| `02` | `HALL` | Music Hall | `4D 02 05 07 45 66 66 65 63 74 02 4F` |
| `03` | `ORIGINAL` | Original | `4D 02 05 07 45 66 66 65 63 74 03 50` |

현재 모드 조회 명령:

```text
4D 01 06 02 01 01 02
```

차량 parser가 기대하는 응답은 byte 4..9가 ASCII `Effect`이고 byte 10이 현재 mode인 `0x4D` 프레임이다. P3DG v9은 수신한 `MM`을 `g_pa10_effect_mode`에 저장하고 이 조회에 반환한다. 아직 모드 설정을 받지 않은 초기 조회에는 KTV(`01`)를 호환 기본값으로 응답하되, 내부 상태와 LED는 idle을 유지한다.

RP2040-Zero 온보드 WS2812(GPIO 16)는 수신 상태를 다음과 같이 표시한다. 밝기는 차량 실내를 고려해 제한했다.

| 상태 | LED 색 |
|---|---|
| 아직 모드 미수신(idle) | 흰색 고정 |
| Recording Studio | 하늘색 |
| KTV | 빨강 |
| Music Hall | 핑크 |
| Original | 오렌지 |

P3DG v14부터 실차 로그에서 확인한 PA10 mode 저장 명령도 직접 처리한다.

```text
4D 0B 00 05 53 41 56 45 MM CS
```

`53 41 56 45`는 ASCII `SAVE`, `MM`은 Studio, KTV, Music Hall, Original 순서의 `00..03`이다. `CS`는 기존 PA10 프레임과 동일하게 byte 4부터 mode byte까지 더한 하위 8비트다. 차량은 이 프레임 뒤에 세 개의 `4D 0A` DSP parameter block을 전송하지만, mode 선택 이벤트 자체는 `SAVE` 프레임만으로 확정할 수 있다.

P3DG v14 실차 시험에서 네 mode가 각각 하늘색, 빨강, 핑크, 오렌지로 전환되어 이 수신 경로와 mode mapping이 모두 정상임을 확인했다. 이후 차량 UI를 다시 확인해 mode 3의 정확한 명칭은 Acoustic이 아니라 Original로 정정했다.

P3DG v16부터 mode 값을 RP2040 내부의 간단한 PCM DSP preset에도 연결한다. Studio는 짧고 약한 room ambience, KTV는 짧은 반복 echo, Music Hall은 두 delay line을 교차 feedback한 긴 잔향, Original은 effect를 완전히 우회한 원음이다. 차량의 Reverb 다이얼 값은 Original을 제외한 각 preset의 wet 비율에 곱해진다.

P3DG v16 실차 시험에서 네 mode의 수신, LED 표시, mode별 음향 차이와 Original 원음 bypass까지 정상 동작해 당시 시험용 WAV 재생 펌웨어의 목표를 완료했다.

이 표시는 실제 reverb/EQ DSP 구현과 독립적이며, 색이 변하면 차량의 mode frame이 RP2040까지 도착했다는 뜻이다.

## 8. Interface 3의 `A5 5A FC` 제어 프레임

PA10 `0x4D` 명령과 별개로, 디컴파일된 `libTsService.so`에는 다음 framed control protocol이 있다.

```text
A5 5A FC PL [payload PL bytes] 16
```

| 명령 | 프레임 | 의미 |
|---|---|---|
| Volume set | `A5 5A FC 04 B0 B0 01 VV 16` | `VV` 설정 |
| Reverb set | `A5 5A FC 04 B0 B0 02 RR 16` | `RR` 설정 |
| Volume query | `A5 5A FC 02 C0 08 16` | 현재 volume 조회 |
| Volume reply | `A5 5A FC 03 A0 08 VV 16` | 현재 volume |
| Reverb query | `A5 5A FC 02 C0 07 16` | 현재 reverb 조회 |
| Reverb reply | `A5 5A FC 03 A0 07 RR 16` | 현재 reverb |
| Internal effect set | `A5 5A FC 03 B0 A1 EE 16` | internal effect index 설정 |
| Internal effect query | `A5 5A FC 02 C0 A1 16` | internal effect 조회 |
| Internal effect reply | `A5 5A FC 03 A0 A1 EE 16` | internal effect index |

`PL`은 payload 길이이며, 끝의 `0x16`은 terminator다. 현재 펌웨어는 volume/reverb/internal-effect set/query를 처리하고, 알 수 없는 유효 프레임은 기본적으로 echo한다.

P3DG v9 실차 시험에서는 mode 선택 후에도 `0x4D/0x02/0x05`가 들어오지 않아 WS2812가 idle 흰색을 유지했다. 제품명 `BYD-micTS02`는 Thunder/TS controller의 UF 장치 분기에도 정확히 일치하며, 이 경로는 mode 번호 대신 선택한 effect 파일을 다음 조각으로 업로드한다.

```text
A5 5A 88 12 II II [effect data 16 bytes] 16
```

`II II`는 big-endian 조각 index다. P3DG v11은 APK에 포함된 TS02 effect 파일들의 Adler-32 지문을 스트리밍 계산하여 Studio/KTV/Hall/Acoustic을 판별한다. `0x88`은 `libTsService.so`의 file offset `0x21D72`에서 실제 template `A5 5A 88 12 ... 16`을 추출해 확인했다. 또한 단순 `B0 A1 EE` 경로와 PA10 `0x4D` 경로도 계속 지원한다. 바이트 단위로 동일한 `haiou_eq` Studio/Acoustic 파일 한 쌍은 USB 데이터만으로 구분할 수 없어 지문 표에서 제외했다.

## 9. 장치→차량 `A5 5A FD` 비동기 이벤트

`libTsService.so`의 event loop는 interface 2 HID IN에서 정확히 8 bytes를 읽고 다음 형식을 검사한다.

```text
A5 5A FD TT B4 B5 B6 16
```

확인된 `TT` 값:

| `TT` | 의미 | 주요 값 |
|---:|---|---|
| `00` | 마이크 연결 변경 | `B5 - 1` = mic index, `B6` = connected |
| `01` | 마이크 volume 변경 | `B5 - 1` = mic index, `B6` = volume |
| `02` | reverb 변경 | `B6` = reverb |
| `03` | 전원/battery 변경 | `B4 - 1` = mic, `B5:B6` = quantity |
| `04` | audio effect 변경 | `B4` = 추가 인자, `B6` = effect |
| `05` | function key | `B6` = key |
| `06` | dry/wet ratio | 후속 버전 parser에서 확인 |
| `07` | 초기화 종료 | 후속 버전 parser에서 확인 |
| `08` | function/resing key | 후속 버전 parser에서 확인 |

GPIO 버튼에서 장치가 자체적으로 volume 또는 effect를 바꾼 뒤 차량 UI도 갱신하려면 이 비동기 IN 통로를 사용할 수 있다. 파서에 근거한 후보 프레임은 다음과 같지만, 원본 마이크 USB 캡처로 `B4/B5` 예약값을 최종 검증해야 한다.

```text
Volume changed: A5 5A FD 01 00 01 VV 16
Effect changed: A5 5A FD 04 00 00 EE 16
```

현재 펌웨어의 `g_hid_reply`는 단일 slot이므로 GPIO 이벤트를 구현하기 전 작은 HID IN FIFO로 바꾸는 편이 안전하다. 그렇지 않으면 연결 보고와 명령 응답이 버튼 이벤트로 덮어써질 수 있다.

## 10. PA10 physical key 보고

PA10 SDK parser에는 command `0x1D` key press 경로도 있다. 별도의 HID status polling 구현에서는 다음 bit가 확인되었다.

MiniKaraoke의 `ProcessManager.handleKeyPress()`와 `MiniKaraokeView.onMicKeyEventChange()`를 대조해 PA10 interrupt IN frame과 실제 keycode도 확인했다.

```text
4D 1D KK 06 [mic MAC 6 bytes] CS
```

| `KK` | 기능 |
|---:|---|
| `131` | Microphone power on |
| `132` | Microphone power off |
| `133` | Auto Voice / sound-effect menu (앱에서는 패널 전환) |
| `134` | Volume up |
| `135` | Volume down |

P3DG v17은 GPIO 연결 전 통로 검증을 위해 연결 2초 후부터 `135`, `134`를 2초 간격으로 번갈아 전송한다. 앱이 키를 수신하면 UI volume을 조정하고 정상 `4D 02 03 07 MICvol VV CS` 명령을 다시 보내므로 버튼 이벤트의 왕복 경로까지 시험할 수 있다.

v17 실차 시험에서는 UI 반응이 없었다. P3DG v18은 HID report descriptor와 Android read buffer에 맞춰 논리 11-byte key frame 뒤를 0으로 채운 64-byte report로 전송한다. 연결 상태 report의 `P3DG` tail byte 55에는 마지막 keycode, byte 56..57에는 성공적으로 queue한 key report 수를 little-endian으로 기록해 USB 송신 단계와 앱 parser 단계를 분리 진단한다.

v18 logcat에는 `onMicKeyEventChange(... keycode=135/134)`가 정확히 2초 간격으로 기록되어 RP2040부터 `MicManager`까지의 전송과 parser가 정상임을 확인했다. 그러나 이 차량 앱의 `UsbPA10MicControl`이 등록한 `IMicManagerListener`는 `onMicKeyEventChange()`를 override하지 않아 기본 no-op에서 이벤트가 사라지고 `MICvol` 명령도 돌아오지 않는다.

P3DG v19은 앱 key listener를 우회해 RP2040의 로컬 volume을 2초마다 `9→0`, 이어서 `1→10`으로 직접 설정한다. 동시에 `4D 01 04 07 "MICvol" VV CS` 현재값 report를 차량에 전송한다. 따라서 실제 음량 변화는 앱 UI와 무관하게 검증할 수 있고, UI callback이 활성화된 화면에서는 현재값 동기화 가능성도 함께 시험한다.

v19 실차 시험에서는 실제 음량은 변했지만 UI는 갱신되지 않았다. logcat의 `MicManager: onVolumeValue: mValueCallback is null`은 현재값 report가 Android까지 도착했으나 해당 화면에 UI callback이 연결되지 않았음을 보여준다. P3DG v20은 volume sweep을 제거하고 연결 시 PA10 기본 UI volume 8로 초기화한 뒤, keycode `133` menu event를 3초마다 64-byte report로 전송한다. 연결 상태 report의 진단 tail byte 55는 `133`, byte 56..57은 성공 송신 횟수다.

v20 실차 log에는 `4D 1D 85 06 [MAC] CS`와 `onMicKeyEventChange(... keycode=133)`가 정확히 3초 간격으로 기록됐다. 메뉴 event도 RP2040에서 Android `MicManager`까지 정상 도착하지만, volume key와 마찬가지로 `UsbPA10MicControl` listener가 이를 처리하지 않아 UI는 바뀌지 않았다.

P3DG v21은 마지막 물리 키 통로 확인을 위해 menu 자동 전송을 제거하고, 연결 5초 후부터 power on `131`과 power off `132`를 5초마다 교대로 전송한다. 진단 tail byte 55에는 마지막 keycode, byte 56..57에는 성공 송신 횟수가 기록된다.

v21 실차 log에는 `131, 132, 131, 132`가 정확히 5초 간격으로 기록됐다. 두 power event 모두 RP2040에서 Android `MicManager`까지 정상 전달됐지만 차량 UI나 오디오에는 변화가 없었다. 디컴파일된 범용 key service는 이를 각각 `onPower(..., true/false)` 상태 알림으로 변환하며, PA10 `KaraokeController`에는 `131`로 안내/UI를 여는 일부 처리만 있고 `132`로 volume을 0으로 만드는 처리는 없다. 따라서 이 frame은 차량이 마이크 전원을 제어하는 명령이 아니라, 원본 마이크가 자체 전원 상태를 바꾼 뒤 차량에 알리는 event로 해석하는 것이 타당하다.

P3DG v22 stable은 v17~v21의 모든 주기적 key/volume 시험을 제거했다. 연결 시 UI volume 8로 초기화하고 차량 명령에만 반응하며, idle LED는 흰색 고정이다. 보라색/cyan transport 진단색도 제거해 LED에는 idle 또는 검증된 네 mode만 표시한다. 연결 report tail은 다시 sample-rate control, streaming alternate-setting, audio callback 및 PCM completion 카운터를 제공한다.

이후 정식 마이크 영상과 APK UI 경로를 다시 대조해 별도의 표준 Android key input 통로를 찾았다. Android keycode `131..135`는 F1..F5이고, USB HID usage는 `0x3A..0x3E`다. 일반 USB 키보드로 F3/F4/F5를 누른 실차 시험에서 `byd.intent.minikaraoke_micevent` broadcast가 생성됐으며 `KaraokeReceiver → KaraokeService → KaraokeController → MiniKaraokeView.onMicKeyEventChange()`를 거쳐 menu popup과 volume UI가 정상 변경됐다.

P3DG v23은 기존 raw PA10 HID instance 0과 vendor control interface 3을 보존하고, interface 4/HID instance 1/IN endpoint `0x86`에 report ID가 없는 표준 keyboard descriptor를 추가한다. 연결 5초 후 F5, F4, F3를 차례로 보내며, 각 press 뒤 100 ms 후 all-zero release report를 보낸다. 실차에서 세 입력과 UI 반응이 모두 검증됐다. raw PA10 packet에 report ID를 붙이거나 기존 interface를 keyboard로 바꾸지 않았으므로 `0x4D` handshake와 UAC1 경로는 영향을 받지 않는다.

P3DG v24 stable은 v23의 5초 지연 및 F5/F4/F3 자동 반복 전송을 제거한다. interface 4 keyboard descriptor와 endpoint는 유지하지만 GPIO가 아직 연결되지 않았으므로 평상시에는 keyboard report를 보내지 않는다. 이 버전은 임의의 UI 변경 없이 UAC1, PA10 handshake, 차량 dial/mode 수신, WAV 및 DSP를 계속 동작시키는 안정화 기준점이다.

### 실차 검증 결과 요약

| Keycode | PA10 `0x4D 1D`→`MicManager` | HID keyboard→차량 UI | GPIO 구현 권장 동작 |
|---:|---|---|---|
| `131` / F1 | 확인 | 미시험 | 전원 ON 후보, 실제 동작 확인 후 연결 |
| `132` / F2 | 확인 | 미시험 | 전원 OFF 후보, 실제 동작 확인 후 연결 |
| `133` / F3 | 확인 | menu popup 확인 | F3 press/release 전송 |
| `134` / F4 | 확인 | volume + 및 UI 확인 | F4 press/release 전송 |
| `135` / F5 | 확인 | volume - 및 UI 확인 | F5 press/release 전송 |

PA10 `0x4D 1D` event는 `MicManager`까지 도착하지만 이 앱의 `UsbPA10MicControl` listener가 `onMicKeyEventChange()`를 구현하지 않아 UI로 이어지지 않는다. 반면 표준 HID keyboard F3/F4/F5는 Android system broadcast 경로를 통해 UI까지 전달된다. 두 경로는 같은 숫자 `133..135`를 사용하지만 transport와 callback chain이 서로 다르다.

| Bit | 기능 |
|---:|---|
| `0x02` | TV/phone/menu |
| `0x04` | Volume up |
| `0x08` | Volume down |
| `0x20` | Voice |
| `0x40` | Power |

이 bit 표는 PA10 `0x1D` parser와 별도로 존재하는 범용/다른 마이크 controller의 polling 경로다. 따라서 `Voice`와 `TV/phone/menu`라는 추가 논리 버튼의 존재는 확인됐지만, 현재 실차의 `UsbPA10MicControl`에서 곧바로 같은 기능이 보장되지는 않는다. PA10 경로에서 확인된 별도 keycode는 power on `131`, power off `132`, menu/Auto Voice `133`, volume up `134`, volume down `135`다.

향후 RP2040 GPIO의 menu/volume 버튼은 v23에서 검증된 별도 keyboard interface로 F3/F4/F5 press와 release를 보내는 것이 기본 구현이다. 차량 앱이 이 입력을 받은 뒤 기존 PA10 volume/effect 명령을 RP2040으로 돌려보내므로 UI와 로컬 DSP 상태가 같은 차량 제어 경로를 공유한다. `0x4D 1D` event는 호환성 진단이나 원본 protocol 재현이 필요한 경우에만 병행한다.

GPIO power는 volume 상태 자체를 0으로 덮어쓰지 않고 별도의 mute flag로 PCM을 0 출력하는 편이 안전하다. OFF 중 차량에서 volume 설정이 들어오면 목표 gain만 갱신하고, ON에서 최신 gain을 적용한다. UAC endpoint와 USB 연결은 계속 유지해야 차량의 audio route가 끊기거나 재초기화되지 않는다.

## 11. 현재 펌웨어 구현 상태

### 구현됨

- USB enumeration 및 문자열 identity
- UAC1 48 kHz/16-bit/stereo capture
- interface 2 interrupt OUT/IN
- 연결 상태 비동기 보고 및 조회 응답
- 수신기 버전과 마이크 정보 응답
- effect ID 10 MD5 응답
- mute 간이 응답
- interface 3 HID-shaped SET_REPORT/GET_REPORT
- interface 4 표준 HID keyboard 통로 및 GP8(Vol+)/GP9(Vol-)/GP10(Menu) 물리 버튼(25 ms 디바운스) 전송
- `A5 5A FC` volume/reverb set/query
- PA10 `0x4D 02 03/04` volume/reverb set
- PA10 `0x4D 01 04/05` volume/reverb query
- PA10 및 TS02 effect mode 수신과 네 mode DSP
- INMP441 PIO/DMA 48 kHz capture와 I2S bit alignment
- 100 Hz low cut, 10 kHz high cut, +3 dB preamp, limiter
- 차량 volume/reverb/mode DSP 후 mono-to-stereo UAC1 전송

### 다음 구현 후보

- 연결/응답/버튼 이벤트가 공존할 수 있는 HID IN queue
- GPIO debounce와 F3/F4/F5 keyboard report 연결
- 리버브의 flutter echo/ringing을 줄이는 diffusion 및 damping

## 12. 주의사항과 아직 확인할 부분

- 현재 문서는 특정 차량 software와 MiniKaraoke `UsbPA10MicControl:V2.10.3`에서 확인한 결과다.
- 다른 BYD/덴자 차종이나 앱 버전은 Thunder `A5 5A FC/FD` 경로를 선택할 수 있다.
- 연결 상태 프레임 뒤의 `P3DG` 영역은 이 프로젝트만의 진단 확장이며 원본 프로토콜이 아니다.
- interface 3을 vendor class로 광고하면서 HID class request를 직접 처리하는 방식은 Android 바인딩 문제를 피하기 위한 호환 구현이다.
- Windows UAC Feature Unit volume은 호환 응답용으로 저장되며 실제 PCM gain은 차량 PA10 volume 상태를 따른다.
- 큰 reverb 설정의 금속성 ringing은 현재 두 고정 delay DSP의 알려진 특성이다.
- 원본 장치의 control/interrupt USB trace가 확보되면 key frame, query response padding, ACK timing을 다시 검증해야 한다.

## 13. 근거 파일

프로젝트 내부:

- `src/usb_descriptors.c`
- `src/tusb_config.h`
- `src/main.c`
- `src/inmp441_i2s.c`
- `src/inmp441_i2s.pio`

로컬 APK/JNI 역분석 자료와 원본 차량 로그는 저작권·개인정보 문제로 이 저장소에 배포하지 않는다. 재현 가능한 protocol 결론만 이 문서에 정리한다.
