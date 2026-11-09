# TamaPoke + TamaDigi 멀티게임(부팅 선택/전환) 설계안

작성일: 2026-11-08
대상 보드: Waveshare ESP32-S3-Touch-AMOLED-1.75 (16MB Flash / 8MB PSRAM)
참고 소스:
- https://github.com/socquique/TamaPoke
- https://github.com/joop0810/tamadigi

---

## 0. 결론 (TL;DR)

**가능하다.** 다만 두 게임을 하나의 펌웨어로 합치는 방식이 아니라,

> **16MB 플래시를 게임별 앱 파티션으로 나누고, 맨 앞에 “게임 선택 허브(TamaHub)” 앱을 두는 방식**

이 가장 안전하고 유지보수하기 좋다.

- 첫 부팅: 허브가 뜨고 `1. Pokémon / 2. Digimon / 3. 추가 슬롯`을 선택
- 게임 실행: 선택한 앱 파티션으로 부팅
- 게임 중 전환: 아래쪽 물리 버튼 길게 누르기 → 저장 → 허브로 복귀
- 상태 보존: 두 게임의 NVS 네임스페이스가 이미 분리되어 있어 그대로 유지됨
- 추가 게임: 파티션 슬롯을 하나 더 쓰고 허브에 카드만 추가

예상 난이도: **중간**. 핵심 기능만 하면 주말~1주, 설치 프로그램/CI/문서까지 하면 **2~3주(파트타임)**.

---

## 1. 소스/하드웨어에서 확인한 사실

### 1.1 펌웨어 크기

| 펌웨어 | app.bin 크기 | 현재 파티션 |
|---|---:|---|
| TamaPoke | 968,480 bytes (약 946KB) | 3MB app + 9MB FAT |
| TamaDigi | 1,664,656 bytes (약 1.59MB) | 3MB app + 9MB FAT |

- 두 프로젝트 모두 내부 FATFS를 **사용하지 않는다.** 스프라이트는 microSD(`SD_MMC`)에서 읽는다.
- 따라서 9MB FAT 영역을 게임별 앱 슬롯으로 재활용할 수 있다.
- 단순 합치기(한 펌웨어)면 946KB + 1.59MB + 허브/중복 라이브러리 ≈ **3MB 안팎**. 3MB app 슬롯에서는 여유가 거의 없다.

### 1.2 NVS(설정/게임 저장)

- TamaPoke: `Preferences` 네임스페이스 **`tamapoke`**
- TamaDigi: **`tamadigi`** + 초기화 의도용 **`tpsys`**
- 이미 분리되어 있으므로 앱을 바꿔도 서로의 세이브를 덮어쓰지 않는다. **큰 장점.**
- 단, 현재 NVS 파티션은 약 **20KB**(app3M_fat9M 기준). TamaDigi는 자체적으로 “NVS 공간 부족” 경고까지 넣어 두었다. 두 게임을 한 기기에 같이 두면 **64KB 정도로 키우는 것을 권장**.

### 1.3 SD카드 경로 충돌 (반드시 수정)

두 프로젝트 모두 같은 경로를 쓴다.

```
/mons/pNNN.bin       (일반)
/mons/psNNN.bin      (이로치/색변종)
/mons/thumbs.bin     (도감 썸네일)
/mons/.pack-N        (TamaDigi 팩 마커)
```

- TamaPoke의 `p001.bin`과 TamaDigi의 `p001.bin`은 **완전히 다른 파일**이다.
- 그대로 두면 게임을 바꿀 때 스프라이트가 섞이거나 깨진다.
- 권장: `/tamapoke/mons/...`, `/tamadigi/mons/...`처럼 **게임별 루트 디렉터리**로 분리.
- 수정 범위: `sdmon.cpp`의 경로 생성, `mkdir`, LS/PUT 가드, `tools/pack_*.py`, 웹 설치 프로그램의 팩 업로드 경로.

### 1.4 TamaDigi의 “Ver.1~5 선택”의 정체

- TamaDigi 첫 부팅의 Ver.1~5는 **게임(펌웨어) 선택이 아니라, 디지몬 V-Pet 버전(지역) 선택**이다.
- 그 안에서 다시 스타터(알)를 고른다.
- 따라서 우리가 만들 허브는 그 **위 계층**이다.

```
TamaHub (게임 선택)
  ├─ Pokémon → TamaPoke (첫 실행: 스타터 선택)
  └─ Digimon → TamaDigi (첫 실행: Ver.1~5 → 스타터 선택)
```

### 1.5 물리 버튼

- 보드의 물리 버튼은 **PWR**과 **BOOT** 두 개다.
  - PWR: AXP2101 전원 경로. 4초 길게 누르면 **하드웨어 전원 차단**.
  - BOOT: **GPIO0**, 외부 풀업. 누르면 LOW.
- TamaPoke/TamaDigi 모두 **GPIO0(BOOT)를 전혀 읽지 않는다.**
- TamaDigi는 PWR 롱프레스를 저장 플러시에만 사용하고, 실제 전원은 AXP가 끊는다.
- TamaPoke는 PWR 숏프레스만 사용한다.
- 따라서 **아래쪽 물리 버튼을 BOOT(GPIO0)에 매핑하는 것이 가장 깔끔하다.**
- 주의: 부팅 중 BOOT를 누르고 있으면 **다운로드 모드**로 진입한다. “부팅 시 길게 눌러 메뉴”는 불가능하고, **부팅 완료 후 길게 누르기**로 구현해야 한다.
- 화면 아래의 터치 아이콘(먹이/놀기/빛/청소)은 두 게임 모두 적극적으로 사용 중이다. **그 터치 영역을 게임 전환에 재사용하면 안 된다.** 물리 버튼과 터치 UI를 구분할 것.

### 1.6 기타 위험 요소

- TamaPoke의 SD 마운트:
  ```cpp
  SD_MMC.begin("/sdcard", true, true);  // 마지막 true = 마운트 실패 시 포맷
  ```
  마운트가 일시적으로 실패하면 **SD카드를 포맷해 양쪽 게임 에셋을 날릴 수 있다.** 멀티부트에서는 반드시 `false`로 바꾸고, 실패 시 사용자에게 “SD 없음”을 표시해야 한다.
- TamaDigi의 빌드 스크립트 경고:
  - `merge-bin`으로 0번지부터 합친 이미지를 쓰면 **NVS까지 0xFF로 덮어써 세이브가 삭제된다.**
  - 웹 설치 프로그램은 반드시 **파트별 오프셋**으로 써야 한다.
- Arduino 기본 레이아웃의 `boot_app0.bin`은 `0xE000`에 쓰인다. 우리가 NVS를 64KB로 키우면 **`0xE000`은 NVS 내부**가 되므로 절대 쓰면 안 된다.

---

## 2. 추천 아키텍처: TamaHub + 멀티 앱 파티션

### 2.1 파티션 레이아웃 (16MB)

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x10000,
otadata,  data, ota,     0x19000,  0x2000,
factory,  app,  factory, 0x20000,  0x300000,
ota_0,    app,  ota_0,   0x320000, 0x300000,
ota_1,    app,  ota_1,   0x620000, 0x300000,
ota_2,    app,  ota_2,   0x920000, 0x300000,
```

| 파티션 | 용도 | 크기 |
|---|---|---:|
| `nvs` | 두 게임 + 허브 설정 | 64KB |
| `otadata` | 다음 부팅 앱 선택 | 8KB |
| `factory` | **TamaHub** (게임 선택) | 3MB |
| `ota_0` | **TamaPoke** | 3MB |
| `ota_1` | **TamaDigi** | 3MB |
| `ota_2` | **추가 게임 슬롯** | 3MB |
| 여유 | 약 3.8MB | - |

- TamaPoke 946KB, TamaDigi 1.59MB이므로 3MB 슬롯은 충분하다.
- 추가 게임이 3MB를 넘으면 슬롯 크기를 3.5MB로 키우고 슬롯 수를 줄이면 된다.
- **v1 레이아웃은 한번 정하면 함부로 바꾸지 말 것.** NVS 오프셋이 바뀌면 세이브가 사라진다.

### 2.2 부팅 흐름

```
전원 ON
  └─ 2차 부트로더가 otadata 확인
       ├─ 비어 있음(0xFF) → factory = TamaHub
       └─ 설정됨          → ota_N = 마지막 게임

TamaHub
  ├─ 슬롯별 앱 유효성 검사 (첫 바이트 0xE9 등)
  ├─ 게임 카드 표시 (Pokémon / Digimon / 추가 슬롯)
  └─ 선택 시:
       esp_ota_set_boot_partition(ota_N)
       esp_restart()

게임 앱
  ├─ 정상 초기화 (pet.begin → syncClock → SD)
  └─ loop에서 GPIO0 길게 누르기 감지
       → 저장 + 일시정지 마커
       → otadata 삭제
       → esp_restart()
       → 부트로더가 factory(TamaHub) 부팅
```

핵심 API:

```cpp
// 게임 실행
const esp_partition_t *p =
    esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                             ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
esp_ota_set_boot_partition(p);
esp_restart();

// 허브로 복귀 (factory 부팅)
const esp_partition_t *otadata =
    esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                             ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
esp_partition_erase_range(otadata, 0, otadata->size);
esp_restart();
```

- `esp_ota_set_boot_partition(factory)`도 가능하지만, **otadata를 지우는 방식이 문서화된 동작**(비어 있으면 factory 부팅)이라 더 안전하다.
- otadata 삭제 중 전원이 나가도 “이전 게임” 또는 “허브” 둘 중 하나로 부팅되므로 벽돌이 되지 않는다.

### 2.3 첫 부팅과 이후 부팅 (확정: 마지막 게임 자동 재개)

- **첫 설치 직후**: otadata가 비어 있으므로 무조건 허브가 뜬다. → 게임 선택 가능.
- **이후 전원 ON**: otadata에 마지막 게임이 기록되어 있으므로 그 게임으로 **바로 부팅**된다.
  - 허브를 거치지 않으므로 재개가 빠르다.
  - 전원을 껐다 켜도 마지막 게임이 자동으로 이어진다.
- **허브로 돌아가고 싶을 때**: 게임 중 아래쪽 버튼 2초 롱프레스 → 저장 → 허브.
- 즉, 허브는 “첫 설치”와 “게임 전환” 때만 나온다. 매번 전원 ON 때 나오지 않는다.
- 이 동작은 `esp_reset_reason()` 트릭이 필요 없다. otadata가 마지막 게임을 가리키고 있으면 그대로 부팅하면 된다.

### 2.4 게임 전환 UI

- 게임 중 **BOOT(GPIO0) 2초 이상 누르기**
- 화면 하단에 작은 시트 표시:
  - `게임 선택으로` (저장 후 허브)
  - `취소`
- 3초 카운트다운 후 자동 취소 → 오조작 방지
- 전환 중에는:
  - `pet.save()` / `pet.flushSave()`로 **즉시 저장**
  - 오디오 정지, SD 파일 닫기
  - `esp_restart()`
- 전투/미니게임 중 전환은 두 가지 정책 중 선택:
  - (권장) 전투 중에는 “전투 중에는 전환할 수 없습니다”로 차단
  - 또는 저장 후 전환하되 진행 중인 전투만 포기

### 2.5 상태 보존과 “비활성 게임” 처리 (확정: 일시정지)

두 게임 모두 `lastSeenEpoch`를 기준으로 오프라인 진행(밥/기분/에너지 감소, 나이 증가)을 적용한다.

문제: Pokémon을 켜 둔 2일 동안 Digimon도 2일치 감소하면, 다시 돌아왔을 때 거의 죽어 있을 수 있다.

**확정 정책: 다른 게임을 플레이하는 동안 비활성 게임은 “일시정지”.**
기기를 꺼 둔 시간만 오프라인 진행으로 계산한다.

구현:

1. 게임 A에서 전환 시:
   - `pet.save()` 후 `lastSeenEpoch = now`로 저장.
2. 허브가 게임 B를 실행하기 직전:
   - B의 네임스페이스에 `resumeAt = now` 기록 (RTC 사용).
3. 게임 B `setup()`에서:
   - `pet.begin()` 후, `syncClock()` 전에 `resumeAt`을 읽음.
   - `lastSeenEpoch = max(lastSeenEpoch, resumeAt)` 후 `resumeAt` 삭제.
   - → B는 “A를 플레이한 시간”만큼 오프라인 진행을 건너뜀.
4. 기기 전원을 껐다 켠 경우:
   - `resumeAt`이 없으므로 정상적으로 오프라인 진행 적용.
   - 즉, “다른 게임을 한 시간”은 멈추고, “기기를 꺼 둔 시간”은 흐르게 할 수 있다.

대안: 두 게임을 완전히 병렬로 키우는 “동시 육성” 정책. 현실적이지만 난이도가 높고, 유저가 한쪽을 방치하게 된다. v1은 **일시정지**를 권장.

### 2.6 추가 슬롯

- 파티션은 `ota_2`까지 미리 만들어 두고, 허브에는 “빈 슬롯”으로 표시.
- 새 게임 추가 시:
  1. 앱을 빌드해 `ota_2` 오프셋에 플래시
  2. 허브의 슬롯 테이블에 이름/아이콘 추가
- 허브를 다시 빌드하지 않고 추가하고 싶으면 SD의 `/hub/slots.txt` 같은 텍스트 설정을 읽게 할 수 있다.
  - 예: `2=MyGame|/hub/icons/my.bin`
  - v1에서는 하드코딩해도 충분.

---

## 3. 대안 비교

| 방식 | 설명 | 장점 | 단점 | 판정 |
|---|---|---|---|---|
| **A. 한 펌웨어로 합치기** | TamaPoke + TamaDigi 코드를 하나로 통합 | 재부팅 없이 즉시 전환, UI 통일 | 전역 변수/함수 충돌, 3MB app 한계, 업스트림 추적 불가, 대규모 리팩터링 | **장기 과제** |
| **B. 허브 + 멀티 앱 (추천)** | factory=허브, ota_0/1/2=게임 | 원본 코드 거의 유지, 추가 슬롯 쉬움, 업데이트 독립 | 파티션/설치 프로그램 직접 관리, 전환 시 1~2초 재부팅 | **v1 정답** |
| C. 두 앱 + 각자 메뉴 | ota_0=Poke, ota_1=Digi, 메뉴 코드를 양쪽에 복사 | 앱 3개 불필요, 플래시 절약 | 메뉴 UI 중복, 첫 부팅/복귀 로직 꼬이기 쉬움 | MVP 대안 |
| D. 전환 시 재플래싱 | SD/시리얼로 펌웨어 교체 | 파티션 불필요 | 느림, 플래시 마모, 벽돌 위험 | **비추천** |

A는 장기적으로 “TamaEngine” 같은 공통 엔진으로 리팩터링하면 가능하지만, 지금 두 프로젝트를 합치는 것은 유지보수 지옥이 된다. B가 현실적이다.

---

## 4. 필수 수정 체크리스트

### 4.1 파티션/빌드

- [ ] `partitions.csv` 작성 (위 레이아웃)
- [ ] 허브 앱을 `factory`로 빌드
- [ ] TamaPoke/TamaDigi는 기존 `app3M` 설정으로 빌드해도 됨 (앱 이미지는 OTA 슬롯에 배치 가능)
- [ ] 각 app.bin 크기 < 슬롯 크기(3MB) 자동 검사
- [ ] `boot_app0.bin`을 `0xE000`에 쓰지 않기 (NVS 내부)
- [ ] v1 레이아웃 동결 및 문서화

### 4.2 TamaHub

- [ ] 디스플레이/터치/PMU/RTC 초기화 (TamaPoke `setup()`에서 게임 로직만 제거)
- [ ] 파티션 유효성 검사 (빈 슬롯 표시)
- [ ] 게임 카드 UI, 터치 선택
- [ ] `esp_ota_set_boot_partition` + `esp_restart`
- [ ] `resumeAt` 기록 (RTC)
- [ ] 배터리/시계 표시 (선택)
- [ ] “마지막 게임 자동 실행” 설정 (선택)

### 4.3 TamaPoke 패치

- [ ] GPIO0(BOOT) 2초 롱프레스 감지 + 디바운스
- [ ] 전환 확인 오버레이
- [ ] `pet.save()` 호출 후 otadata 삭제 + 재부팅
- [ ] `resumeAt` 처리 (begin 후, syncClock 전)
- [ ] SD 경로를 `/tamapoke/...`로 변경
- [ ] `SD_MMC.begin(..., true)` → `false`
- [ ] (PWR 롱프레스로 전환하고 싶을 경우) AXP 롱 IRQ 추가 + 하드웨어 4초 차단 해제

### 4.4 TamaDigi 패치

- [ ] GPIO0(BOOT) 2초 롱프레스 감지
- [ ] 전환 확인 오버레이
- [ ] `flushBeforePowerLoss()` / `pet.flushSave()` 후 otadata 삭제 + 재부팅
- [ ] `resumeAt` 처리 (begin 후, syncClock 전)
- [ ] SD 경로를 `/tamadigi/...`로 변경
- [ ] 팩 마커/썸네일 경로도 함께 변경
- [ ] 전투/전송 중 전환 차단

### 4.5 설치 프로그램/CI

- [ ] 파트별 오프셋으로 플래시 (merge-bin 금지)
- [ ] NVS를 건드리지 않는 업데이트 경로
- [ ] `new_install_prompt_erase: true`의 실제 의미 유지 (TamaDigi `build_web.sh` 주석 참고)
- [ ] GitHub Actions에서 3개 앱 빌드 + 크기/오프셋 검사 + Pages 배포
- [ ] 설치 후 첫 부팅이 허브인지 확인

---

## 5. 하드웨어 버튼 가이드

### 확인 방법 (가장 먼저 할 일)

`tools/button_test/button_test.ino`를 플래시한 뒤 케이스의 아래쪽 버튼을 눌러 본다.

- `BOOT / GPIO0` 줄이 초록색 → BOOT를 누르는 구조. 그대로 사용.
- `PWR / TCA9554 P4` 줄이 초록색 → PWR을 누르는 구조. 새 스위치 권장.
- 둘 다 반응 없음 → 버튼이 보드까지 닿지 않음. 구조/배선 수정 필요.

자세한 사용법은 `tools/button_test/README.md` 참고.

### 권장: 아래쪽 버튼 = BOOT(GPIO0)

- 이미 보드에 있고, 두 펌웨어 모두 사용하지 않음.
- 외부 풀업이 있어 별도 회로 불필요.
- `pinMode(0, INPUT_PULLUP)` 후 `digitalRead(0)`.
- 부팅 중 누르고 있으면 다운로드 모드가 되므로, **부팅 완료 후 2초 롱프레스**만 사용.
- 케이스의 아래쪽 버튼이 실제로 BOOT를 누르는지 멀티미터로 확인.

### 대안: 아래쪽 버튼 = PWR

- PWR은 GPIO가 아니라 AXP2101/TCA9554 경로.
- TCA9554 `EXIO4`가 눌림 상태를 읽을 수 있다 (HIGH = pressed).
- 하지만 4초 하드웨어 전원 차단을 소프트웨어가 제어해야 하므로 위험.
- 전환용으로 쓰려면:
  - AXP의 `setPowerKeyPressOffTime`을 늘리거나 비활성화
  - 소프트웨어에서 6~8초 롱프레스 시 `pmu.shutdown()`
  - 정전/행 시 복구가 어려워짐
- **추천하지 않음.** PWR은 전원 전용으로 두는 것이 안전.

### 대안: 새 택트 스위치

- 확장 헤더의 GPIO16/17/18 사용 가능.
- 단, `-G`(GPS) 버전은 GPIO17/18이 GPS UART와 공유되므로 주의.
- GPIO19/20은 USB, GPIO26~37은 플래시/PSRAM이므로 사용 금지.

---

## 6. GitHub 저장소 구성 (제안)

```
tama-multiboot/
├─ hub/                      # Arduino sketch: TamaHub
├─ games/
│  ├─ TamaPoke/              # upstream + 최소 패치
│  └─ TamaDigi/              # upstream + 최소 패치
├─ partitions.csv
├─ tools/
│  ├─ build_all.ps1
│  ├─ make_installer.py
│  └─ check_layout.py
├─ web/                      # 통합 웹 설치 프로그램
├─ docs/
│  ├─ MULTI_GAME_PLAN.md
│  ├─ HARDWARE.md
│  ├─ PATCHES.md
│  ├─ UPSTREAM.md
│  └─ TESTING.md
├─ .github/workflows/build.yml
├─ LICENSE
├─ CREDITS.md
└─ README.md
```

- 업스트림 추적:
  - `games/TamaPoke`와 `games/TamaDigi`는 원본 커밋 SHA를 `UPSTREAM.md`에 기록.
  - 가능하면 `git remote add upstream ...` 후 `git subtree` 또는 패치 파일로 유지.
  - 패치는 “왜 필요한지”를 `PATCHES.md`에 남길 것.
- 라이선스:
  - 코드: MIT (TamaPoke © Quique Tortosa, TamaDigi © Dennis)
  - 스프라이트: Pokémon © Nintendo/Game Freak, PMD SpriteCollab CC BY-NC / Digimon © Bandai/Toei
  - **비상업 팬 프로젝트**로만 공개. 유료 배포/후원 보상 금지.
  - `LICENSE`, `CREDITS.md`를 두 프로젝트에서 그대로 가져오고, 허브 코드의 라이선스도 명시.
- 대용량 에셋:
  - TamaDigi 팩 약 9.5MB, TamaPoke 스프라이트 약 40MB.
  - GitHub 저장소에 그대로 올리면 용량이 크다. **Releases / Git LFS / 생성 스크립트** 사용 권장.

---

## 7. 단계별 로드맵

| 단계 | 내용 | 예상 |
|---|---|---|
| **Phase 0. Proof** | 파티션 테이블 + 3개 앱 수동 플래시 + 시리얼 명령으로 파티션 전환 | 1~2일 |
| **Phase 1. Hub** | 허브 UI, 게임 카드, 실행/복귀, 마지막 게임 기억 | 3~5일 |
| **Phase 2. 패치** | 양쪽 게임 롱프레스, 저장/일시정지, SD 경로 분리 | 3~5일 |
| **Phase 3. 설치** | 통합 웹 설치 프로그램, GitHub Actions, 크기/오프셋 검사 | 2~3일 |
| **Phase 4. 안정화** | 전원 차단/재부팅 반복, NVS 여유, 24~48시간 soak | 1주 |

MVP만 원하면 Phase 0~2를 주말에 할 수 있다. Phase 3부터가 “남에게 배포 가능한” 수준.

---

## 8. 주요 리스크와 대응

| 리스크 | 대응 |
|---|---|
| 파티션/앱 불일치로 부팅 루프 | 허브를 factory에 두고, 최소한의 복구 UI 제공. USB 플래싱 경로 유지 |
| NVS 공간 부족/손상 | NVS 64KB, 두 게임 세이브 백업(EXPORT) 기능, 부팅 시 여유 공간 로그 |
| SD 경로 충돌 | 게임별 루트 디렉터리 분리, 설치 프로그램도 경로 검사 |
| TamaPoke가 SD를 포맷 | `formatIfUnreadable=false`로 변경 |
| 업스트림과 멀어짐 | 패치 최소화, `PATCHES.md`·커밋 SHA 기록 |
| 전환 중 전원 차단 | 저장 후 전환, NVS 체크포인트(TamaDigi) 활용 |
| PWR 4초 하드웨어 차단 | 전환 버튼은 BOOT 사용 |
| 설치 프로그램이 NVS 삭제 | 파트별 오프셋, `boot_app0.bin` 금지, 업데이트 시 NVS 미포함 |
| 플래시 마모 | 전환 시 otadata 2섹터 쓰기 정도는 문제 없음. NVS 빈번 저장은 기존과 동일 |

---

## 9. 확정된 결정 / 남은 확인

### 확정

1. **부팅 시 동작**: 마지막 게임 자동 재개
   - 첫 설치 때만 허브에서 게임 선택.
   - 이후 전원 ON은 마지막 게임으로 바로 부팅.
   - 허브는 게임 중 롱프레스로만 진입.
2. **비활성 게임 정책**: 일시정지
   - 다른 게임을 플레이하는 동안에는 나이/스탯이 줄지 않음.
   - 기기를 꺼 둔 시간만 오프라인 진행.
3. **추가 슬롯**: v1은 4개 슬롯(허브 + 3게임)으로 두고, 실제로는 2개만 사용.
4. **아래쪽 버튼 = BOOT/GPIO0** (보드에서 확인).
   - 전환은 게임 실행 중 2초 롱프레스 → 저장 → 손을 뗀 뒤 재부팅.
   - 부팅 중 누르고 있으면 ROM 다운로드 모드가 되므로, 항상 게임이 뜬 뒤에 사용.

### 남은 확인

1. **GitHub**
   - 저장소 이름/공개 여부
   - 대용량 스프라이트 팩을 LFS/Releases 중 무엇으로 둘지

---

## 10. 구현 현황 (2026-11-09)

1. ✅ 아래쪽 버튼이 BOOT/GPIO0임을 확인 (`tools/button_test`)
2. ✅ `partitions.csv` + TamaHub (카드 UI, 터치/BOOT 선택, 실행/복귀, `resumeAt`, 버전 표시)
3. ✅ TamaPoke(expanded v1.36.2) / TamaDigi / TamaNaruto 멀티부팅 패치
   (롱프레스 + 강제 저장 + 일시정지 + SD 경로)
4. ✅ 통합 웹 설치 프로그램 (4개 앱, 파트별 오프셋, NVS 미포함)
5. ✅ Web Serial SD 업로더 (게임 식별, 쓰기 테스트, 여유 공간, 포맷/비우기)
6. ✅ GitHub Actions 빌드/크기·오프셋 검사

로컬 빌드 검증 완료: Hub 435,008 B · Poke 730,496 B · Digi 1,667,600 B ·
Naruto 1,719,680 B (모두 3MB 슬롯 이내). 실제 기기 플래시/전환 soak 테스트는 남아 있다.
