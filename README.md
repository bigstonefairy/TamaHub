# TamaHub

[![build](https://github.com/bigstonefairy/TamaHub/actions/workflows/build.yml/badge.svg)](https://github.com/bigstonefairy/TamaHub/actions/workflows/build.yml)

설치 페이지: <https://bigstonefairy.github.io/TamaHub/>

Waveshare **ESP32-S3-Touch-AMOLED-1.75** 한 대에서 **TamaPoke(Pokémon)**,
**TamaDigi(Digimon)**, **TamaNaruto(Naruto)** 를 함께 즐기는 멀티게임 빌드입니다.

- 부팅/전환할 때만 **게임 선택 허브(TamaHub)** 가 뜹니다.
- 게임을 고르면 그 게임으로 재부팅하고, **다음 전원 ON부터는 마지막 게임으로 바로** 이어집니다.
- 게임 중 **아래쪽 버튼(BOOT/GPIO0)을 2초 이상 누른 뒤 떼면** 저장하고 허브로 돌아옵니다.
- 세 게임의 세이브는 NVS 네임스페이스(`tamapoke`, `tamadigi`, `tamanaruto`)로
  분리되어 있어 서로 덮어쓰지 않습니다.
- **버전 확인**: TamaHub 화면 상단에 `TAMA HUB v1.0`, 설치 페이지 상단에
  Hub/Pokémon/Digimon/Naruto의 빌드 버전이 표시됩니다.
- 다른 게임을 플레이하는 동안 비활성 게임은 **일시정지**됩니다. 기기를 꺼 둔 시간만 오프라인 진행으로 계산합니다.

```
전원 ON
  └─ otadata 확인
       ├─ 비어 있음 → factory = TamaHub  (첫 설치 / 게임에서 돌아온 직후)
       └─ 게임 지정 → ota_0 = TamaPoke, ota_1 = TamaDigi, ota_2 = TamaNaruto
                      (마지막 게임 자동 재개)

TamaHub v1.0
  ├─ Pokémon 카드  → ota_0  (TamaPoke expanded v1.36.2)
  ├─ Digimon 카드  → ota_1  (TamaDigi v1.0.1)
  └─ Naruto 카드   → ota_2  (TamaNaruto v1.2.0)

게임 중 BOOT 2초 길게 → 저장 → otadata 삭제 → 재부팅 → TamaHub
```

> Pokémon 슬롯은 **ShadowEnemyx TamaPoke expanded v1.36.2** (386마리)입니다.
> 기본 팩(#1–251)과 Gen3 업데이트 팩(#252–386)을 **둘 다** SD에 넣어야 합니다.
> SD 업로드 페이지가 두 팩을 자동으로 합쳐 전송합니다.

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
| TamaNaruto | `0x920000` |

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
  0x620000 build/TamaDigi/TamaDigi.ino.bin \
  0x920000 build/TamaNaruto/TamaNaruto.ino.bin
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

- 한 게임을 플레이하는 동안 나머지 두 게임은 나이/스탯이 줄지 않습니다.
- 기기를 꺼 둔 시간은 세 게임 모두 오프라인 진행으로 적용됩니다.
- 허브에서 게임을 실행할 때 `resumeAt`을 대상 게임의 NVS에 기록하고, 게임은 시작 시 그 시각까지를 건너뜁니다.

알려진 한계: **허브 화면에 머문 채 전원을 꺼 둔 시간**은 일시정지로 처리됩니다.
(허브는 마지막으로 게임이 저장한 시각을 기준으로 resume 지점을 잡기 때문입니다.)
일반적인 사용 흐름(게임 → 2초 → 허브 → 게임)에서는 영향이 없습니다.

---

## 3. SD 카드

세 게임 모두 스프라이트를 microSD에서 읽고, 경로가 겹치지 않도록 게임별 루트로 분리했습니다.

### 웹에서 바로 넣기 (권장)

GitHub Pages 설치 페이지의 **“SD 카드에 스프라이트 넣기”** 섹션에서 USB로 바로
업로드할 수 있습니다. SD 카드를 뽑을 필요가 없습니다.

1. microSD를 보드에 넣습니다.
2. 업로드할 게임을 실행합니다.
   - Pokémon → **TamaPoke** 화면
   - Digimon → **TamaDigi** 화면
   - Naruto → **TamaNaruto** 화면
3. 페이지에서 **보드 연결** → 해당 게임의 **스프라이트 넣기** 버튼.
4. 완료 후 전원을 껐다 켭니다.

- **TamaHub 화면에서는 업로드가 안 됩니다.** 게임이 실행 중일 때만 SD와 시리얼을
  함께 제어할 수 있습니다.
- 연결하면 실행 중인 게임을 자동으로 확인합니다. TamaPoke/TamaDigi/TamaNaruto가
  아니면 연결을 해제하고 안내합니다.
- 연결 시 **SD 상태와 쓰기 테스트**를 자동으로 수행합니다. `SD: 쓰기 실패`면
  카드가 쓰기 금지/불량/가득 찬 상태입니다. PC에서 FAT32 포맷하거나 카드를 교체하세요.
- SD가 인식되지 않으면 **SD 마운트 / 포맷**을 누르세요. 정상 카드는 데이터가
  유지되고, 인식할 수 없는 카드만 FAT32로 포맷됩니다.
- 공간이 부족하면 **스프라이트 비우기**로 현재 게임의 SD 폴더를 비울 수 있습니다.
- Pokémon은 **약 105MB**(기본 + Gen3 업데이트 2개 팩), Digimon 약 10MB,
  Naruto 약 9MB입니다. 중간에 끊기면 USB를 다시 연결하고 같은 버튼을 누르면
  이미 보낸 파일은 건너뜁니다.
- SD를 포맷했거나 카드를 바꿨다면 페이지의 **진행 기록 초기화**를 누르세요.
- 스프라이트 팩은 `web/sd/`에 포함되어 Pages에서 페이지와 같은 오리진으로
  제공됩니다. (CORS 문제 없음)

> 이번 업로더는 `WHOAMI`/`SDSTAT`/`SDTEST`/`SDFORMAT`/`SDCLEAR` 명령을 사용합니다.
> **Pokémon / Digimon / Naruto 펌웨어를 모두 최신 빌드로 업데이트**해야 합니다.
> (게임별 업데이트는 세이브를 유지합니다.)
>
> 예전 빌드는 새로 포맷한 카드에서 `/tamapoke`, `/tamadigi`, `/tamaruto`
> 상위 폴더를 만들지 못해 쓰기 테스트가 실패했습니다. 최신 빌드에서 수정되었습니다.

### 수동 준비

```
/tamapoke/mons/p001.bin
/tamapoke/mons/ps001.bin
/tamapoke/mons/thumbs.bin

/tamadigi/mons/p001.bin
/tamadigi/mons/ps001.bin
/tamadigi/mons/thumbs.bin
/tamadigi/mons/.pack-1 ... .pack-5   (직접 복사 시 없어도 됨)

/tamaruto/mons/p001.bin
/tamaruto/mons/ps001.bin
/tamaruto/mons/thumbs.bin
```

- TamaPoke(expanded): `_repos\TamaPoke-expanded`의 `tools/sdcard/mons` 내용을
  `/tamapoke/mons`로 복사. (기본 251 + Gen3 업데이트 252~386, 총 773개 파일)
- TamaDigi: 업스트림 체크아웃에서 `python tools/unpack_bundle.py`를 실행하면
  `tools/sdcard/mons`가 복원됩니다. 그 내용을 `/tamadigi/mons`로 복사하세요.
  (`web/sprites-ver*.pak` 5개를 한 번에 풀어 `thumbs.bin`까지 만듭니다.)
- TamaNaruto: `python tools/unpack_bundle.py` → `tools/sdcard/mons` 내용을
  `/tamaruto/mons`로 복사. (마을 팩 4개, 67개 파일)
- 예전처럼 `/mons` 하나에 모든 파일이 있던 카드라면, 게임별로 나눠서 복사해야 합니다.
  파일 이름은 같지만 내용이 완전히 다릅니다.
- TamaPoke의 SD 마운트는 이제 **실패해도 포맷하지 않습니다** (`formatIfUnreadable=false`).
- SD 전송용 시리얼 명령(`PUT`, `LS`)은 새 루트만 허용합니다. 예전 클라이언트가
  `mons/xxx.bin`으로 보내도 각 게임 루트로 자동 변환됩니다.

도우미 스크립트:

```powershell
.\tools\prepare_sd.ps1 -Drive E:\ -Game pokemon
.\tools\prepare_sd.ps1 -Drive E:\ -Game digimon
.\tools\prepare_sd.ps1 -Drive E:\ -Game naruto
.\tools\prepare_sd.ps1 -Drive E:\ -Game all
```

`prepare_sd.ps1`은 `_repos\TamaPoke-expanded`, `_repos\TamaDigi-main`,
`_repos\TamaRuto-main`의 팩을 자동으로 풀어 복사합니다.

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
바꿉니다. `main`에 푸시할 때마다 `.github/workflows/build.yml`이 네 앱을 빌드하고
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

- 앱 슬롯은 각 3MB. TamaPoke 약 0.73MB, TamaDigi 약 1.67MB, TamaNaruto 약 1.72MB로 여유가 있습니다.
- `nvs`는 64KB. 세 게임이 함께 써도 TamaDigi의 “NVS 공간 부족” 경고가 뜨지 않습니다.
- **이 표를 바꾸면 세이브가 사라집니다.** `tools/check_layout.py`가 v1 값과 다르면 빌드를 거부합니다.

### TamaNaruto (slot 3)

`ota_2` (`0x920000`)에 TamaNaruto v1.2.0이 들어갑니다. 허브의 세 번째 카드는
`NARUTO`이며 NVS 네임스페이스는 `tamanaruto`, SD 루트는 `/tamaruto/mons`입니다.

### 네 번째 게임 추가

v1 레이아웃에는 **빈 슬롯이 없습니다.** 추가하려면 `partitions.csv`를 바꿔
NVS 뒤에 슬롯을 더 만들어야 하며, 이때 기존 세이브/앱을 백업하고 전체 재설치해야
합니다. `tools/check_layout.py`의 `EXPECTED`도 함께 수정해야 합니다.

---

## 6. 문제 해결

| 증상 | 원인 / 해결 |
|---|---|
| 화면이 검은색 | 부팅 중 BOOT를 눌렀는지 확인. 다운로드 모드면 USB 재연결 후 재설치 |
| 허브로 안 돌아감 | 버튼을 2초 누른 뒤 **완전히 떼야** 재부팅합니다 |
| 게임 카드가 `NOT INSTALLED` | 해당 슬롯에 앱이 없거나 이미지가 손상됨. 웹 설치 프로그램으로 다시 설치 |
| `SLOT IS EMPTY` | Naruto(`ota_2`) 슬롯이 비어 있음. 전체 설치 또는 Naruto 업데이트 |
| 게임에서 `HUB MISSING` | `factory` 슬롯이 비었거나 손상됨. 허브를 `0x20000`에 다시 설치 |
| SD 업로드가 첫 파일에서 멈춤 | Pokémon/Digimon/Naruto 펌웨어를 **모두 최신 빌드로 업데이트**. 페이지가 `WHOAMI`/`SDSTAT`/`SDTEST`로 확인합니다 |
| `게임을 확인할 수 없습니다` | TamaHub 화면이거나 구버전 펌웨어. 게임을 실행하고 세 게임 업데이트를 먼저 실행 |
| `SD: 인식 안 됨` | 카드를 다시 넣고 **SD 마운트 / 포맷**. FAT32가 아니면 포맷이 필요할 수 있음 |
| `SD: 쓰기 실패` | 카드가 쓰기 금지/불량/가득 참. 어댑터 LOCK 확인 → PC에서 FAT32 포맷 또는 카드 교체 |
| Naruto 카드가 `NOT INSTALLED` | **허브 업데이트** + **Naruto 업데이트**를 실행 (`ota_2`) |
| 허브에 Naruto 카드가 없음 | **허브 업데이트**(`0x20000`)를 실행 |
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
games/TamaPoke/       TamaPoke expanded v1.36.2 + 멀티부팅 패치
games/TamaDigi/       TamaDigi v1.0.1 + 멀티부팅 패치
games/TamaNaruto/     TamaNaruto v1.2.0 + 멀티부팅 패치
partitions.csv        동결된 v1 파티션 테이블
tools/                빌드/검증/설치 프로그램/SD 도우미
                      (setup_windows_toolchain.ps1: 한글 경로 우회)
web/                  GitHub Pages 설치 페이지
web/sd/               웹 SD 업로더용 스프라이트 팩 (TPAK)
docs/                 설계, 하드웨어, 패치, 업스트림 기록
CHANGELOG.md          릴리스 노트
.github/workflows/    CI (4개 앱 빌드 + 크기 검사 + Pages)
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
