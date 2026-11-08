# 실기 플래시 가이드

프로젝트 위치: `C:\Users\레오\Documents\ESP32`
보드: Waveshare ESP32-S3-Touch-AMOLED-1.75 (16MB Flash / 8MB PSRAM)

> **가장 중요한 규칙**
> 이 레이아웃에서는 **Arduino IDE의 Upload 버튼을 쓰면 안 됩니다.**
> ESP32 Arduino 코어는 업로드할 때 앱을 `0x10000`, `boot_app0.bin`을 `0xE000`에
> 쓰는데, 둘 다 이 빌드에서는 NVS(`0x9000..0x18FFF`)를 덮어씁니다.
> Arduino IDE는 **컴파일(Verify) 용도로만** 사용하세요.

---

## 0. 준비물

- 보드와 **데이터 전송이 되는 USB-C 케이블** (충전 전용 케이블은 COM 포트가 안 잡힘)
- Windows PC의 **Chrome 또는 Edge 데스크톱**
- microSD 카드 (FAT32) — 스프라이트를 넣을 경우
- 이 저장소의 `build\` 폴더 또는 `web\dist\` 폴더

COM 포트 확인:

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

예: `COM5`. Device Manager의 **포트(COM & LPT)** 에서도 확인할 수 있습니다.

---

## 1. 방법 A — 웹 설치 프로그램 (권장)

### 1-1. 설치 페이지 열기

**GitHub Pages를 아직 안 만들었다면** 로컬 서버로 엽니다.

```powershell
cd C:\Users\레오\Documents\ESP32
py -m http.server 8000 --directory web/dist
```

브라우저에서 <http://localhost:8000> 을 엽니다.
(`web\`가 아니라 반드시 `web\dist\`를 서빙해야 합니다.)

GitHub Pages를 만든 뒤에는 `https://<사용자>.github.io/<저장소>/` 를 열면 됩니다.

### 1-2. 설치

1. Arduino IDE, 시리얼 모니터, PuTTY 등 **COM 포트를 잡고 있는 프로그램을 모두 닫습니다.**
2. 보드를 USB로 연결합니다.
3. 페이지에서 **전체 설치** 버튼을 누릅니다.
4. 시리얼 포트를 선택합니다.
5. **Erase**를 선택합니다. (첫 설치 필수 — 기존 파티션/세이브가 지워집니다)
6. 설치가 끝날 때까지 **USB를 뽑지 않습니다.**
7. 자동 재부팅 후 **TamaHub** 화면이 뜨면 성공입니다.

### 1-3. 포트가 안 보이거나 연결 실패 시

보드를 다운로드 모드로 넣고 다시 시도합니다.

- **아래쪽 BOOT 버튼을 누른 채로 USB를 연결**하고, 2초 뒤 손을 뗍니다.
- 또는 BOOT를 누른 채 위쪽 PWR(또는 RESET)을 짧게 눌렀다가 뗍니다.
- 다운로드 모드에서는 화면이 검은색인 것이 정상입니다.

Web Serial은 **HTTPS 또는 localhost**에서만 동작합니다. `file://`로 열면 안 됩니다.

---

## 2. 방법 B — esptool 수동 플래시

Arduino 코어에 포함된 esptool을 사용합니다. (v5 기준, 명령이 `write-flash`입니다)

```powershell
cd C:\Users\레오\Documents\ESP32

$esp = Get-ChildItem "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\esptool_py" `
       -Recurse -Filter esptool.exe |
       Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
& $esp version
```

### 2-1. 최초 1회: 전체 지우기

```powershell
& $esp --chip esp32s3 --port COM5 erase-flash
```

`COM5`는 실제 포트로 바꾸세요. 이 단계에서 **모든 세이브가 지워집니다.**

### 2-2. 5개 이미지 쓰기

```powershell
& $esp --chip esp32s3 --port COM5 --baud 921600 write-flash `
  --flash-mode dio --flash-freq 80m --flash-size 16MB `
  0x0      build\hub\hub.ino.bootloader.bin `
  0x8000   build\hub\hub.ino.partitions.bin `
  0x20000  build\hub\hub.ino.bin `
  0x320000 build\TamaPoke\TamaPoke.ino.bin `
  0x620000 build\TamaDigi\TamaDigi.ino.bin
```

연결이 불안정하면 `--baud 921600`을 `--baud 115200`으로 낮춥니다.

### 2-3. 게임만 업데이트

NVS와 다른 게임은 건드리지 않고 해당 슬롯만 다시 씁니다.

```powershell
# TamaPoke만
& $esp --chip esp32s3 --port COM5 --baud 921600 write-flash `
  --flash-mode dio --flash-freq 80m --flash-size 16MB `
  0x320000 build\TamaPoke\TamaPoke.ino.bin

# TamaDigi만
& $esp --chip esp32s3 --port COM5 --baud 921600 write-flash `
  --flash-mode dio --flash-freq 80m --flash-size 16MB `
  0x620000 build\TamaDigi\TamaDigi.ino.bin
```

### 2-4. 전체 주소표

| 파트 | 주소 | 파일 |
|---|---|---|
| bootloader | `0x0` | `build\hub\hub.ino.bootloader.bin` |
| partition table | `0x8000` | `build\hub\hub.ino.partitions.bin` |
| TamaHub | `0x20000` | `build\hub\hub.ino.bin` |
| TamaPoke | `0x320000` | `build\TamaPoke\TamaPoke.ino.bin` |
| TamaDigi | `0x620000` | `build\TamaDigi\TamaDigi.ino.bin` |
| (예약) | `0x920000` | 추가 게임 슬롯 |

**절대 쓰지 않는 것**

- `boot_app0.bin` (`0xE000` — NVS 내부)
- `*.merged.bin` (offset 0 — NVS/파티션 전체 덮어씀)
- `0x10000` (기본 Arduino 앱 위치)

---

## 3. 첫 부팅

1. USB를 뽑았다가 다시 연결합니다. **BOOT를 누르지 마세요.**
2. TamaHub가 뜨고 카드 3개가 보입니다.
   - `POKEMON` / `DIGIMON`은 `READY`
   - `EXTRA SLOT`은 `NOT INSTALLED`
3. 카드를 터치하면 해당 게임으로 재부팅합니다.
4. 한 번 게임을 고른 뒤에는 전원을 껐다 켜도 **마지막 게임으로 바로** 부팅합니다.
5. 다른 게임으로 바꾸려면 게임 중 **아래쪽 BOOT를 2초 이상 누른 뒤 손을 뗍니다.**
   - `SAVING` → `SAVED / release the button` → 허브

부팅 중 BOOT를 누르고 있으면 ROM 다운로드 모드로 들어가므로, 전환은 항상
게임이 뜬 뒤에 하세요.

---

## 4. SD 카드 준비

카드는 **FAT32**로 포맷합니다. 스프라이트가 없어도 게임은 실행되지만
도감/그림이 비어 보입니다.

### 웹 업로드 (권장)

설치 페이지의 **“SD 카드에 스프라이트 넣기”**에서 USB로 바로 넣을 수 있습니다.

1. microSD를 보드에 넣습니다.
2. 업로드할 게임을 실행합니다.
   - Pokémon → **TamaPoke** 화면
   - Digimon → **TamaDigi** 화면
3. 페이지에서 **보드 연결** → **Pokémon/Digimon 스프라이트 넣기**.
4. 완료 후 전원을 껐다 켭니다.

- **TamaHub 화면에서는 동작하지 않습니다.**
- 연결하면 게임(TamaPoke/TamaDigi)을 자동 확인합니다. 구버전 펌웨어는
  확인 명령이 없으므로 **두 게임을 모두 업데이트**하세요.
- SD가 인식되지 않으면 **SD 마운트 / 포맷**을 누르세요. 정상 카드는 데이터가
  유지되고, 인식할 수 없는 카드만 FAT32로 포맷됩니다.
- Pokémon 약 40MB / Digimon 약 10MB. 중간에 끊기면 같은 버튼으로 이어서 전송합니다.
- SD를 포맷/교체했다면 **진행 기록 초기화**를 누르세요.
- 팩은 페이지와 같은 오리진(`sd/…`)에서 제공되므로 별도 서버가 필요 없습니다.

> 예전 TamaDigi를 이미 플래시했다면 **Digimon 업데이트**를 한 번 실행하세요.
> (업로드 재시도 시 파일이 덧붙는 문제 수정)

### PC에서 복사

```powershell
cd C:\Users\레오\Documents\ESP32
.\tools\prepare_sd.ps1 -Drive E:\ -Game both
```

`_repos\TamaPoke-main`, `_repos\TamaDigi-main`이 있으면 그대로 사용합니다.
TamaDigi는 `tools\unpack_bundle.py`를 자동 실행해 팩을 풉니다.

### 수동

```
E:\tamapoke\mons\p001.bin ...
E:\tamapoke\mons\thumbs.bin

E:\tamadigi\mons\p001.bin ...
E:\tamadigi\mons\thumbs.bin
E:\tamadigi\mons\.pack-1 ... .pack-5   (직접 복사 시 없어도 됨)
```

- TamaPoke: 업스트림 `tools/sdcard/mons` 내용
- TamaDigi: 업스트림에서 `python tools/unpack_bundle.py` 실행 후 `tools/sdcard/mons`
- 두 게임의 파일 이름은 같지만 내용이 완전히 다르므로 **폴더를 섞지 마세요.**

---

## 5. 문제 해결

| 증상 | 해결 |
|---|---|
| `Failed to connect` | BOOT 누른 채 USB 연결(다운로드 모드), 데이터 케이블 확인, `--baud 115200` |
| `Permission denied` / 포트 사용 중 | Arduino IDE·시리얼 모니터·PuTTY 종료 |
| 화면이 검은색 | 부팅 중 BOOT를 눌렀는지 확인. USB 다시 연결 |
| 웹 설치 페이지에서 포트가 안 보임 | Chrome/Edge인지, `https://` 또는 `localhost`인지 확인 |
| 첫 파일에서 멈춤 | **양쪽 펌웨어를 최신 빌드로 업데이트**했는지 확인. 페이지가 `WHOAMI`로 게임을 확인하고 `SDSTAT`로 카드 상태를 확인합니다 |
| `게임을 확인할 수 없습니다` | TamaHub 화면이거나 구버전 펌웨어. 게임을 실행하고 Pokémon/Digimon 업데이트를 먼저 실행 |
| `SD: 인식 안 됨` | 카드를 다시 넣고 **SD 마운트 / 포맷**. FAT32가 아니면 포맷이 필요할 수 있음 |
| 카드가 `NOT INSTALLED` | 전체 설치를 했는지, 올바른 슬롯에 썼는지 확인 |
| 세이브가 사라짐 | `erase-flash` 또는 Erase 전체 설치를 했는지 확인 |
| 스프라이트가 안 보임 | `/tamapoke/mons`, `/tamadigi/mons` 경로 확인 |
| 부팅 루프 | 시리얼 115200 로그 확인 → `erase-flash` 후 전체 재설치 |

실기 검증 항목은 [`TESTING.md`](TESTING.md)를 순서대로 따라가세요.
