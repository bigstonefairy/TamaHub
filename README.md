# TamaHub

[![build](https://github.com/bigstonefairy/TamaHub/actions/workflows/build.yml/badge.svg)](https://github.com/bigstonefairy/TamaHub/actions/workflows/build.yml)

설치 페이지: <https://bigstonefairy.github.io/TamaHub/>

Waveshare **ESP32-S3-Touch-AMOLED-1.75** 한 대에서 **TamaPoke(Pokémon)** 와
**TamaDigi(Digimon)** 를 함께 즐기는 멀티게임 빌드입니다.

- 부팅/전환할 때만 **게임 선택 허브(TamaHub)** 가 뜹니다.
- 게임을 고르면 그 게임으로 재부팅하고, **다음 전원 ON부터는 마지막 게임으로 바로** 이어집니다.
- 게임 중 **아래쪽 버튼(BOOT/GPIO0)을 2초 이상 누른 뒤 떼면** 저장하고 허브로 돌아옵니다.
- 두 게임의 세이브는 NVS 네임스페이스(`tamapoke`, `tamadigi`)로 이미 분리되어 있어 서로 덮어쓰지 않습니다.
- 다른 게임을 플레이하는 동안 비활성 게임은 **일시정지**됩니다. 기기를 꺼 둔 시간만 오프라인 진행으로 계산합니다.

```
전원 ON
  └─ otadata 확인
       ├─ 비어 있음 → factory = TamaHub  (첫 설치 / 게임에서 돌아온 직후)
       └─ 게임 지정 → ota_0 = TamaPoke, ota_1 = TamaDigi  (마지막 게임 자동 재개)

TamaHub
  ├─ Pokémon 카드  → ota_0
  ├─ Digimon 카드  → ota_1
  └─ Extra Slot    → ota_2 (v1에서는 비어 있음)

게임 중 BOOT 2초 길게 → 저장 → otadata 삭제 → 재부팅 → TamaHub
```

---

## 1. 설치

> 처음 플래시부터 문제 해결까지: [`docs/FLASHING.md`](docs/FLASHING.md)

### 가장 쉬운 방법: 웹 설치 프로그램

GitHub Pages에 배포되는 `web/dist/index.html` 을 **Chrome 또는 Edge 데스크톱**에서 엽니다.
(Web Serial은 HTTPS 또는 localhost에서만 동작합니다.)

1. 기기를 USB로 연결합니다.
2. **전체 설치** 버튼을 누릅니다.
3. 처음 설치라면 **Erase** 를 선택합니다. (파티션 배치가 기존 펌웨어와 다르므로 필수)
4. 설치가 끝나면 자동으로 재부팅되고 TamaHub가 뜹니다.

GitHub Pages 없이 로컬에서 시험하려면 `web/dist`를 정적 서버로 열면 됩니다.
(Web Serial은 `localhost`에서도 동작합니다.)

```powershell
py -m http.server 8000 --directory web/dist
# 브라우저에서 http://localhost:8000
```

전체 설치가 쓰는 주소:

| 파트 | 주소 |
|---|---|
| bootloader | `0x0` |
| partition table | `0x8000` |
| TamaHub | `0x20000` |
| TamaPoke | `0x320000` |
| TamaDigi | `0x620000` |

> **`boot_app0.bin` 은 쓰지 않습니다.** 기본 Arduino 레이아웃에서는 `0xE000`에 쓰지만,
> 이 빌드에서는 그 주소가 64KB NVS 내부입니다. 설치 프로그램은 파트별 오프셋만 기록합니다.

### 수동 설치 (esptool)

```bash
# 최초 1회: 칩 전체 지우기 (세이브도 지워집니다)
esptool.py --chip esp32s3 -p COM5 erase_flash

esptool.py --chip esp32s3 -p COM5 -b 921600 write_flash \
  0x0      build/hub/hub.ino.bootloader.bin \
  0x8000   build/hub/hub.ino.partitions.bin \
  0x20000  build/hub/hub.ino.bin \
  0x320000 build/TamaPoke/TamaPoke.ino.bin \
  0x620000 build/TamaDigi/TamaDigi.ino.bin
```

게임만 업데이트할 때는 해당 앱만 씁니다. NVS와 다른 게임은 그대로 남습니다.

> Windows에서 `build_all.ps1`을 실행하면 산출물이 `build\`가 아니라
> `C:\Users\Public\tamahub-build\`에 생깁니다(한글 경로 우회). 위 경로를 그대로
> 바꿔 쓰세요.
>
> Arduino 코어에 포함된 esptool을 직접 쓸 수도 있습니다:
> `%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\5.3.1\esptool.exe`
> (ASCII 스테이징을 쓰면 `C:\Users\Public\Arduino15\...`).

```bash
esptool.py --chip esp32s3 -p COM5 -b 921600 write_flash \
  0x320000 build/TamaPoke/TamaPoke.ino.bin
```

> **Arduino IDE의 Upload 버튼은 쓰지 마세요.** ESP32 Arduino 코어는 업로드할 때
> 앱을 항상 `0x10000`, `boot_app0.bin`을 항상 `0xE000`에 씁니다. 이 레이아웃에서는
> 둘 다 NVS를 덮어씁니다. Arduino IDE는 **컴파일 용도로만** 사용하세요.

---

## 2. 사용법

### 게임 선택

- 카드를 **터치**하면 바로 실행됩니다.
- 아래쪽 **BOOT** 버튼: 짧게 누르면 선택 이동, **1.2초 이상 누른 뒤 떼면** 실행.

### 게임 전환

1. 게임 중 아래쪽 버튼을 **2초 이상** 누릅니다.
2. `SAVING` → `SAVED` 화면이 뜹니다.
3. **손을 뗍니다.** (떼기 전에는 재부팅하지 않습니다 — GPIO0이 LOW인 채로 리셋되면 다운로드 모드로 들어가기 때문)
4. TamaHub로 돌아옵니다.

> 부팅 중에 BOOT를 누르고 있으면 다운로드 모드가 됩니다. 반드시 게임이 켜진 뒤에 누르세요.

### 일시정지 정책

- Pokémon을 플레이하는 동안 Digimon은 나이/스탯이 줄지 않습니다.
- 기기를 꺼 둔 시간은 두 게임 모두 오프라인 진행으로 적용됩니다.
- 허브에서 게임을 실행할 때 `resumeAt`을 대상 게임의 NVS에 기록하고, 게임은 시작 시 그 시각까지를 건너뜁니다.

알려진 한계: **허브 화면에 머문 채 전원을 꺼 둔 시간**은 일시정지로 처리됩니다.
(허브는 마지막으로 게임이 저장한 시각을 기준으로 resume 지점을 잡기 때문입니다.)
일반적인 사용 흐름(게임 → 2초 → 허브 → 게임)에서는 영향이 없습니다.

---

## 3. SD 카드

두 게임 모두 스프라이트를 microSD에서 읽고, 경로가 겹치지 않도록 게임별 루트로 분리했습니다.

```
/tamapoke/mons/p001.bin
/tamapoke/mons/ps001.bin
/tamapoke/mons/thumbs.bin

/tamadigi/mons/p001.bin
/tamadigi/mons/ps001.bin
/tamadigi/mons/thumbs.bin
/tamadigi/mons/.pack-1 ... .pack-5   (게임이 자동 생성, 없어도 됨)
```

- TamaPoke: 업스트림 `tools/sdcard/mons` 내용을 `/tamapoke/mons`로 복사.
- TamaDigi: 업스트림 체크아웃에서 `python tools/unpack_bundle.py`를 실행하면
  `tools/sdcard/mons`가 복원됩니다. 그 내용을 `/tamadigi/mons`로 복사하세요.
  (`web/sprites-ver*.pak` 5개를 한 번에 풀어 `thumbs.bin`까지 만듭니다.)
- 예전처럼 `/mons` 하나에 모든 파일이 있던 카드라면, 게임별로 나눠서 복사해야 합니다.
  파일 이름은 같지만 내용이 완전히 다릅니다.
- TamaPoke의 SD 마운트는 이제 **실패해도 포맷하지 않습니다** (`formatIfUnreadable=false`).
- SD 전송용 시리얼 명령(`PUT`, `LS`)은 새 루트만 허용합니다. 예전 클라이언트가
  `mons/xxx.bin`으로 보내도 각 게임 루트로 자동 변환됩니다.

도우미 스크립트:

```powershell
.\tools\prepare_sd.ps1 -Drive E:\ -Game pokemon
.\tools\prepare_sd.ps1 -Drive E:\ -Game digimon
```

`prepare_sd.ps1`은 `_repos\TamaDigi-main`이 있으면 `unpack_bundle.py`를 자동으로
실행한 뒤 복사합니다.

---

## 4. 직접 빌드

### 필요한 것

- Arduino CLI 1.x (저장소에 `tools/bin/arduino-cli.exe`를 둘 수 있음)
- ESP32 Arduino 코어 **3.3.11**
- 라이브러리
  - `GFX Library for Arduino` **1.6.7**
  - `SensorLib` **0.4.1**
  - `XPowersLib` **0.3.3**
  - `U8g2` **2.36.19** (TamaPoke의 CJK 글꼴 데이터)

```powershell
.\tools\bin\arduino-cli.exe core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
.\tools\bin\arduino-cli.exe core install esp32:esp32@3.3.11 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
.\tools\bin\arduino-cli.exe lib install "GFX Library for Arduino@1.6.7" "SensorLib@0.4.1" "XPowersLib@0.3.3" "U8g2@2.36.19"
```

### 한 번에 빌드

```powershell
.\tools\build_all.ps1 -Installer
```

> PowerShell 실행 정책으로 막히면:
> `powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_all.ps1 -Installer`

- 허브: `PartitionScheme=custom` + `hub/partitions.csv`
- 게임: 업스트림과 동일한 `PartitionScheme=app3M_fat9M_16MB`
- 빌드 후 `tools/check_layout.py`로 파티션/크기 검증
- `-Installer`를 주면 `web/dist`에 설치 프로그램 생성 + `tools/check_installer.py`로
  NVS(`0x9000..0x18FFF`)·`0xE000` 침범 여부 검사

### Windows에서 사용자 폴더 이름이 한글일 때

`C:\Users\레오\...` 같은 경로는 arduino-cli가 링커에 넘기는 `-L` 경로를
`C:\Users\????\...`로 깨뜨려 `cannot find -lpthread` 또는 `.elf` 생성 실패가 납니다.
`build_all.ps1`은 이를 자동으로 감지해 ASCII 경로로 한 번 스테이징합니다.

```powershell
# 최초 1회: ASCII 작업 디렉터리 준비 (ESP32-S3 툴체인 약 1.8GB 복사)
. .\tools\setup_windows_toolchain.ps1
```

- 저장소: `C:\Users\Public\tamahub` (정션, 코드는 원래 위치 그대로)
- Arduino 데이터: `C:\Users\Public\Arduino15` (**정션 아님**, 실제 복사본)
  - 정션으로 두면 arduino-cli가 심볼릭 링크를 따라가다 한글 경로를 다시 만나
    `Platform 'esp32:esp32' not found`가 됩니다.
- 라이브러리: `C:\Users\Public\ArduinoUser\libraries`
- 빌드 출력: `C:\Users\Public\tamahub-build`

이후에는 평소처럼 `.\tools\build_all.ps1 -Installer`만 실행하면 됩니다.
Arduino IDE로 컴파일만 할 때도 동일한 문제가 생기면, IDE의 스케치북/데이터
경로를 ASCII 위치로 바꾸거나 위 스크립트로 만든 경로를 사용하세요.

### Arduino IDE에서 열기

1. Arduino IDE 2.x 설치.
2. **Boards Manager**에서 ESP32 보드 패키지 3.3.11 설치.
   - 추가 URL: `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. **Library Manager**에서 위 라이브러리 네 개 설치.
4. `hub/hub.ino` 또는 `games/TamaPoke/TamaPoke.ino`를 엽니다.
5. 보드 설정:
   - Board: **ESP32S3 Dev Module**
   - Flash Size: **16MB**
   - PSRAM: **OPI PSRAM**
   - USB CDC On Boot: **Enabled**
   - Partition Scheme:
     - 허브 → **Custom** (`hub/partitions.csv` 사용)
     - 게임 → **16M Flash (3MB APP/9.9MB FATFS)**
6. **Verify(컴파일)** 까지만 사용하세요. 업로드는 위 웹 설치 프로그램 또는 esptool을 사용합니다.

### GitHub에 올리고 Pages로 배포

> 인증, Private 저장소, 문제 해결 포함 상세 절차:
> [`docs/GITHUB.md`](docs/GITHUB.md)

Git이 없다면 먼저 설치합니다.

```powershell
winget install --id Git.Git
```

저장소를 만들고 첫 커밋을 올립니다. (`_repos/`, `build/`, `web/dist/`,
`tools/bin/`은 `.gitignore`로 제외됩니다. 스프라이트는 SD 카드에만 둡니다.)

```powershell
git init
git add .
git commit -m "TamaHub: multi-game boot selector for TamaPoke + TamaDigi"
git branch -M main
git remote add origin https://github.com/<사용자>/<저장소>.git
git push -u origin main
```

GitHub에서 **Settings → Pages → Build and deployment → Source: GitHub Actions**로
바꿉니다. `main`에 푸시할 때마다 `.github/workflows/build.yml`이 세 앱을 빌드하고
`web/dist`를 Pages에 배포합니다. 배포가 끝나면 아래 주소가 설치 페이지가 됩니다.

```
https://<사용자>.github.io/<저장소>/
```

> CI는 매번 파티션 테이블과 앱 크기를 검사하고, 설치 매니페스트가 NVS
> (`0x9000..0x18FFF`)나 `0xE000`을 건드리면 실패합니다.

---

## 5. 파티션 레이아웃 (v1, 동결)

```csv
nvs,      data, nvs,     0x9000,   0x10000,
otadata,  data, ota,     0x19000,  0x2000,
factory,  app,  factory, 0x20000,  0x300000,
ota_0,    app,  ota_0,   0x320000, 0x300000,
ota_1,    app,  ota_1,   0x620000, 0x300000,
ota_2,    app,  ota_2,   0x920000, 0x300000,
```

- 앱 슬롯은 각 3MB. TamaPoke 약 0.85MB, TamaDigi 약 1.67MB로 여유가 있습니다.
- `nvs`는 64KB. 두 게임이 함께 써도 TamaDigi의 “NVS 공간 부족” 경고가 뜨지 않습니다.
- **이 표를 바꾸면 세이브가 사라집니다.** `tools/check_layout.py`가 v1 값과 다르면 빌드를 거부합니다.

### 세 번째 게임 추가

1. 새 앱을 `ota_2` 오프셋(`0x920000`)에 플래시.
2. `hub/hub.ino`의 `slots[]`에서 `EXTRA SLOT` 항목의 `ns`(NVS 네임스페이스), 이름, 색을 채움.
3. 허브만 다시 빌드해서 `0x20000`에 플래시.

`ota_2` 슬롯이 비어 있으면 허브는 자동으로 `NOT INSTALLED`로 표시합니다.

---

## 6. 문제 해결

| 증상 | 원인 / 해결 |
|---|---|
| 화면이 검은색 | 부팅 중 BOOT를 눌렀는지 확인. 다운로드 모드면 USB 재연결 후 재설치 |
| 허브로 안 돌아감 | 버튼을 2초 누른 뒤 **완전히 떼야** 재부팅합니다 |
| 게임 카드가 `NOT INSTALLED` | 해당 슬롯에 앱이 없거나 이미지가 손상됨. 웹 설치 프로그램으로 다시 설치 |
| `SLOT IS EMPTY` | 빈 `ota_2`(Extra) 카드를 실행함. 정상 동작이며 허브로 돌아옵니다 |
| 게임에서 `HUB MISSING` | `factory` 슬롯이 비었거나 손상됨. 허브를 `0x20000`에 다시 설치 |
| 세이브가 사라짐 | 전체 설치(Erase)를 했는지 확인. 게임별 업데이트는 NVS를 건드리지 않음 |
| SD 스프라이트가 안 보임 | `/tamapoke/mons`, `/tamadigi/mons` 경로 확인. 게임별로 파일이 다름 |
| 부팅 루프 | 시리얼 115200으로 로그 확인. 필요하면 `erase_flash` 후 전체 재설치 |

시리얼 명령:

- 허브: `1` / `2` / `3` 즉시 실행, `SLOTS` 상태 출력
- 게임: 업스트림 명령 유지 (`PUT`, `LS`, …)

---

## 7. 저장소 구조

```
hub/                  TamaHub 스케치 (factory)
games/TamaPoke/       업스트림 + 최소 패치
games/TamaDigi/       업스트림 + 최소 패치
partitions.csv        동결된 v1 파티션 테이블
tools/                빌드/검증/설치 프로그램/SD 도우미
                      (setup_windows_toolchain.ps1: 한글 경로 우회)
web/                  GitHub Pages 설치 페이지
docs/                 설계, 하드웨어, 패치, 업스트림 기록
CHANGELOG.md          릴리스 노트
.github/workflows/    CI (3개 앱 빌드 + 크기 검사 + Pages)
```

패치 목록과 이유는 [`docs/PATCHES.md`](docs/PATCHES.md),
업스트림 커밋은 [`docs/UPSTREAM.md`](docs/UPSTREAM.md),
실기 테스트 항목은 [`docs/TESTING.md`](docs/TESTING.md)에 정리했습니다.

---

## 8. 라이선스 / 크레딧

- 코드: MIT. 원저작자와 포크 기여자는 [`CREDITS.md`](CREDITS.md) 참고.
- Pokémon 스프라이트: © Nintendo / Game Freak / The Pokémon Company.
  PMD 스프라이트는 SpriteCollab의 CC BY-NC 조건을 따릅니다.
- Digimon: © Bandai / Toei.
- **비상업 팬 프로젝트**입니다. 유료 배포, 후원 보상, 광고 수익화에 사용하지 마세요.
