# TamaNav — 폰 연동 턴바이턴 표시

TamaNav는 **화면 표시 장치**입니다. 기판에 GPS도 지자기도 없고, 지도도 저장하지
않습니다. 경로 계산은 폰의 내비 앱이 하고, TamaNav는 그 결과 중 **회전 아이콘,
다음 조작까지의 거리, 남은 시간/ETA**만 BLE로 받아 크게 그립니다.

```
[Naver / Kakao / Google Maps]  →  [폰 동반 앱]  →BLE→  [TamaNav]
        경로/안내 계산              알림 파싱           화살표 + 숫자
```

이 구조 덕분에 기기에는 한글 폰트, 도로명 파싱, 지도 타일이 전혀 필요 없습니다.

---

## 1. 펌웨어

| 항목 | 값 |
|---|---|
| 버전 | 1.0.0 |
| 기본 슬롯 | `ota_4` (`0xA20000`) |
| 크기 | 약 0.72 MB (2 MB 슬롯) |
| BLE 이름 | `TamaNav` |
| NVS | `tamanav` (`bright`) |

화면은 두 가지입니다.

- **대기 화면**: `TamaNav / TURN-BY-TURN`, `PAIR YOUR PHONE`, BLE 이름, 배터리.
- **안내 화면**: 회전 화살표(화면 중앙), 거리(큰 숫자), 남은 시간, ETA, 출처
  (`NAVER`/`KAKAO`/`GOOGLE`/`PHONE`), 링크 점(초록=연결, 노랑=데이터 지연,
  빨강=끊김), 배터리.

동작 규칙:

- 패킷이 **15초** 이상 오지 않으면 점이 노랑으로 바뀌고 `NO LINK`가 뜹니다.
  값은 지우지 않습니다(터널에서 잠깐 끊겨도 마지막 안내 유지).
- 경로 이탈 플래그가 오면 화살표와 거리가 주황색이 되고 `OFF ROUTE`가 뜹니다.
- 도착 플래그가 오면 체크 깃발과 `ARRIVED`가 뜹니다.
- 연결되면 기기가 `HELLO`와 `STATUS`를 한 번 보내고, 그 뒤 **30초마다** `STATUS`를
  알립니다(battery, brightness, uptime).
- BOOT 짧게: 밝기 4단계 순환 (60/120/200/255, NVS 저장)
- BOOT 2초: TamaHub로 복귀

---

## 2. BLE 프로토콜

모든 패킷은 기본 ATT MTU(23)에 들어갑니다. MTU 협상이 필요 없습니다.

| 역할 | UUID |
|---|---|
| 서비스 | `4e415600-0000-1000-8000-00805f9b34fb` |
| RX (폰 → 기기, write / write-no-response) | `4e415601-…` |
| TX (기기 → 폰, notify + read) | `4e415602-…` |

`4e4156`는 ASCII `NAV`입니다.

### 2.1 안내 패킷 (폰 → 기기, 16바이트)

리틀엔디언. 16바이트보다 길면 뒤는 무시하고, 10바이트까지만 와도 동작합니다
(그 경우 ETA는 기기가 RTC + 남은 시간으로 계산).

| 옥텟 | 형 | 필드 |
|---:|---|---|
| 0 | u8 | `type` = `0x01` |
| 1 | u8 | `icon` (아래 표) |
| 2–3 | u16 | `dist_m` — 다음 조작까지 미터, `0xFFFF` = 모름 |
| 4–5 | u16 | `remain_s` — 목적지까지 초, `0xFFFF` = 모름 |
| 6 | u8 | `flags` — bit0 도착, bit1 이탈, bit2 속도 유효, bit3 ETA 유효 |
| 7 | u8 | `speed_kmh`, `0xFF` = 모름 |
| 8 | u8 | `source` — 0 모름, 1 NAVER, 2 KAKAO, 3 GOOGLE, 4 기타 |
| 9 | u8 | `seq` — 0–255 순환, 폰이 끊긴 쓰기를 확인하는 용도 |
| 10–11 | u16 | `eta_min` — 도착 시각(자정부터 분), `0xFFFF` = 모름 |
| 12–13 | u16 | `total_m` — 남은 총 거리 |
| 14–15 | u16 | `trip_s` — 남은 총 시간 |

`type`만 다른 1바이트 패킷:

| 값 | 의미 |
|---|---|
| `0x02` | `CLEAR` — 안내를 지우고 대기 화면으로 |
| `0x03` | `PING` — 기기가 `STATUS`로 응답 |

### 2.2 아이콘

| 값 | 이름 | 화면 |
|---:|---|---|
| 0 | `NONE` | 점선 직진 |
| 1 | `STRAIGHT` | 직진 |
| 2 | `SLIGHT_LEFT` | 완만한 좌회전 |
| 3 | `LEFT` | 좌회전 |
| 4 | `SHARP_LEFT` | 급좌회전 |
| 5 | `SLIGHT_RIGHT` | 완만한 우회전 |
| 6 | `RIGHT` | 우회전 |
| 7 | `SHARP_RIGHT` | 급우회전 |
| 8 | `UTURN_LEFT` | 좌측 유턴 |
| 9 | `UTURN_RIGHT` | 우측 유턴 |
| 10 | `ROUNDABOUT` | 회전교차로 |
| 11 | `MERGE_LEFT` | 좌측 합류 |
| 12 | `MERGE_RIGHT` | 우측 합류 |
| 13 | `EXIT_LEFT` | 좌측 출구 |
| 14 | `EXIT_RIGHT` | 우측 출구 |
| 15 | `FERRY` | 페리 |
| 16 | `ARRIVE` | 도착 |
| 17 | `OFF_ROUTE` | 경로 이탈 |
| 18 | `RECALC` | 재탐색 |

### 2.3 기기 → 폰

`HELLO` (8바이트, 연결 직후 1회):

| 옥텟 | 필드 |
|---:|---|
| 0 | `type` = `0x80` |
| 1 | `proto` = 1 |
| 2–4 | 펌웨어 major / minor / patch |
| 5 | `screen` = 0 (466 px 원형) |
| 6 | 예약 |
| 7 | XOR 0–6 |

`STATUS` (8바이트, 연결 직후 + 30초마다 + `PING` 응답):

| 옥텟 | 필드 |
|---:|---|
| 0 | `type` = `0x81` |
| 1 | 배터리 %, 255 = 모름 |
| 2 | flags — bit0 USB, bit1 충전 중 |
| 3 | 밝기 |
| 4 | 가동 시간(시간, 순환) |
| 5–6 | 예약 |
| 7 | XOR 0–6 |

---

## 3. 폰 쪽 구현

### 3.1 가장 빠른 방법: Tasker + AutoNotification (코드 없음)

1. Tasker에 AutoNotification 플러그인 설치.
2. 프로필: `AutoNotification Intercept` → 앱 `네이버 지도` / `카카오맵` / `Google Maps`.
3. `%antext`에서 `(\d+)\s*m` 같은 정규식으로 거리, `(\d+)\s*분`으로 시간을 뽑습니다.
4. `BLE Write` 액션(또는 `BLE Scanner`의 write)으로 16바이트를 `…5601`에 씁니다.

앱마다 알림 문구가 다르므로 **앱별로 필터를 따로** 만드는 편이 안정적입니다.

### 3.2 권장: Android 동반 앱 (Kotlin)

`NotificationListenerService`로 내비 알림을 읽고, 문구를 아이콘/거리/시간으로
바꿔 BLE로 씁니다. 필요한 것은 다음뿐입니다.

```kotlin
// AndroidManifest.xml
<service android:name=".NavListener"
         android:permission="android.permission.BIND_NOTIFICATION_LISTENER_SERVICE"
         android:exported="true">
  <intent-filter>
    <action android:name="android.service.notification.NotificationListenerService" />
  </intent-filter>
</service>
```

```kotlin
override fun onNotificationPosted(sbn: StatusBarNotification) {
    val pkg = sbn.packageName              // com.nhn.android.nmap / com.kakao.taxi ... / com.google.android.apps.maps
    val text = sbn.notification.extras
        .getCharSequence(Notification.EXTRA_TEXT)?.toString() ?: return
    val (icon, dist) = parseTurn(text)     // "300m 앞에서 우회전" -> (RIGHT, 300)
    val remain = parseRemain(text)         // "15분" -> 900
    send(NavPacket(icon, dist, remain, ...)) // 16 bytes, write to RX
}
```

주의할 점:

- Android 12+는 `BLUETOOTH_SCAN` / `BLUETOOTH_CONNECT` 런타임 권한이 필요합니다.
- GATT write는 **write without response**가 가장 빠릅니다. 1초에 한 번이면 충분합니다.
- 알림 문구는 앱 업데이트로 바뀝니다. 파싱 실패 시 **직진 + 거리/시간만** 보내도
  화면은 정상 동작합니다.
- Google Maps는 알림 문구가 가장 빈약합니다. 정확한 턴 정보가 필요하면 Google
  Navigation SDK(유료, 자체 앱 필요)를 쓰거나 **Naver/Kakao를 기본**으로 두세요.
- iOS는 다른 앱의 알림을 읽을 수 없습니다. iOS를 지원하려면 자체 내비 앱이
  필요하므로, 현재 TamaNav는 **Android 전용**입니다.

### 3.3 연결 순서

1. 기기에서 TamaNav 실행 → `PAIR YOUR PHONE`.
2. 폰에서 `TamaNav`를 스캔해 연결.
3. TX(`…5602`)를 구독(notify)하면 `HELLO`/`STATUS`가 옵니다.
4. 안내가 시작되면 RX(`…5601`)에 16바이트 패킷을 1초 간격으로 씁니다.
5. 안내가 끝나면 `CLEAR`(0x02) 한 바이트를 보내 대기 화면으로 돌립니다.

---

## 4. 시리얼로 화면 확인하기 (폰 없이)

115200 baud. 화면과 프로토콜을 폰 없이 검증할 수 있습니다.

```
DEMO           3초마다 아이콘/거리/시간이 바뀌는 데모 (토글)
ICON=6         아이콘 직접 지정 (0–18)
DIST=350       다음 조작까지 미터
REMAIN=900     목적지까지 초
SPD=42         속도 km/h
SRC=1          출처 (1 NAVER, 2 KAKAO, 3 GOOGLE, 4 PHONE)
ARRIVE         도착 화면
OFFROUTE       경로 이탈 화면
CLR            대기 화면
STATUS         현재 상태 출력
BRIGHT=200     밝기
```

`DEMO`는 BLE 연결 없이도 동작하므로 첫 설치 직후 화면 확인에 사용하세요.

---

## 5. 한계

- 지도, 도로명, 남은 경로 모양은 표시하지 않습니다.
- 폰과 BLE가 끊기면 마지막 안내를 유지하다가 `NO LINK`로 표시합니다.
- 폰이 절전 모드로 내비 알림을 죽이면 갱신이 멈춥니다. 동반 앱에 배터리 최적화
  예외를 걸어 두세요.
- 회전교차로는 몇 번째 출구인지 표시하지 않습니다(아이콘만).
- 실기 검증 항목은 [`TESTING.md`](TESTING.md) §9를 참고하세요.
