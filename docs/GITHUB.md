# GitHub 연결 가이드

프로젝트: `C:\Users\레오\Documents\ESP32`
GitHub Pages에 올리면 웹 설치 프로그램을 휴대폰/다른 PC에서도 열 수 있습니다.

---

## 0. Git 설치

현재 PC에는 Git이 없습니다.

```powershell
winget install --id Git.Git -e --source winget
```

설치 후 **터미널을 닫았다가 새로 엽니다.**

```powershell
git --version
```

처음 한 번만 이름/이메일을 설정합니다. (GitHub 계정과 같게)

```powershell
git config --global user.name "Your Name"
git config --global user.email "you@example.com"
```

---

## 1. GitHub에 빈 저장소 만들기

1. <https://github.com/new> 접속
2. Repository name: 예) `TamaHub-ESP32`
3. **Public** 권장 (Private은 Pages 사용에 유료 플랜이 필요할 수 있음)
4. **README, .gitignore, license는 추가하지 않습니다.** (이미 저장소에 있음)
5. **Create repository**

생성 후 나오는 주소를 사용합니다.

```
https://github.com/<username>/<repo>.git
```

---

## 2. 로컬 저장소 초기화 + 첫 푸시

```powershell
cd C:\Users\레오\Documents\ESP32

git init
git add .
git status
```

`git status`에서 아래 항목이 **보이면 안 됩니다** (`.gitignore`로 제외됨):

- `_repos/` — 업스트림 체크아웃
- `build/` — 빌드 산출물
- `web/dist/` — 생성된 설치 프로그램
- `tools/bin/` — arduino-cli 실행 파일
- `*.log`

문제가 없으면 커밋합니다.

```powershell
git commit -m "TamaHub v1.0: multi-game firmware for ESP32-S3"
git branch -M main
git remote add origin https://github.com/<username>/<repo>.git
git push -u origin main
```

### 인증

- **Git Credential Manager**가 함께 설치되므로, 첫 `push`에서 브라우저 로그인 창이 뜹니다. GitHub 계정으로 로그인하면 끝입니다.
- 창이 안 뜨거나 비밀번호를 물으면 **계정 비밀번호 대신 Personal Access Token**을 넣습니다.
  - GitHub → Settings → Developer settings → Personal access tokens → Tokens (classic) → Generate new token
  - 권한: `repo` (Private 저장소면 필수)
  - 토큰을 비밀번호 자리에 붙여넣기
- SSH를 쓰려면 별도로 키를 만들고 `git@github.com:<username>/<repo>.git`을 remote로 사용합니다.

---

## 3. GitHub Pages 켜기

이 저장소의 워크플로는 `actions/configure-pages@v5`(`enablement: true`)로
첫 실행 때 Pages를 자동으로 켜려고 시도합니다. 그래도 아래를 확인하세요.

1. 저장소 → **Settings** → **Pages**
2. **Build and deployment** → **Source: GitHub Actions**
3. **Actions** 탭에서 `build` 워크플로가 도는지 확인
4. 성공하면 Pages 주소가 나옵니다.

```
https://<username>.github.io/<repo>/
```

이 주소가 웹 설치 프로그램입니다. (HTTPS라서 Web Serial 동작)

### 저장소 소개(선택)

저장소 오른쪽 **About**의 톱니바퀴에서:

- Description: `Waveshare ESP32-S3 Tamagotchi multi-boot: TamaPoke (Pokémon) + TamaDigi (Digimon)`
- Topics: `esp32-s3`, `arduino`, `tamagotchi`, `pokemon`, `digimon`, `ota`, `waveshare`, `amoled`

> Private 저장소는 Pages 배포에 GitHub Pro/Team 이상이 필요할 수 있습니다.
> 무료로 쓰려면 Public으로 두세요. 이 프로젝트는 **비상업 팬 프로젝트**입니다.

---

## 4. 설치/업데이트 흐름

### 처음 설치

1. Pages 주소를 Chrome/Edge에서 엽니다.
2. 보드를 USB로 연결합니다.
3. **전체 설치** → **Erase** → 설치

### 코드 수정 후

```powershell
git add .
git commit -m "설명"
git push
```

- GitHub Actions가 자동으로 세 앱을 빌드하고 파티션/크기/설치 오프셋을 검사합니다.
- 성공하면 Pages의 설치 페이지가 새 빌드로 갱신됩니다.
- 기기에서는 **게임별 업데이트** 버튼으로 해당 슬롯만 다시 쓰면 세이브가 유지됩니다.

### CI가 하는 검사

- `tools/check_layout.py` — v1 파티션 표와 앱 크기
- `tools/check_installer.py` — 매니페스트가 NVS(`0x9000..0x18FFF`) 또는
  `0xE000`을 건드리면 실패
- 세 앱 컴파일 (Hub/Poke/Digi)

---

## 5. 자주 막히는 부분

| 증상 | 해결 |
|---|---|
| `git: command not found` | Git 설치 후 **터미널 새로 열기** |
| `Authentication failed` | 비밀번호 대신 Personal Access Token 사용 |
| `src refspec main does not match any` | 커밋 전이거나 브랜치 이름이 `master`. `git branch -M main` |
| `failed to push some refs` | 원격에 이미 커밋이 있음. `git pull --rebase origin main` 후 다시 push |
| Actions에서 빌드 실패 | Actions 로그의 첫 `error:` 확인. 로컬에서 `.\tools\build_all.ps1 -Installer`로 재현 |
| Pages 404 | Settings → Pages의 Source가 **GitHub Actions**인지, 워크플로가 성공했는지 확인 |
| `Get Pages site failed` | Settings → Pages → Source: **GitHub Actions**로 설정한 뒤, Actions에서 **Re-run failed jobs** |
| Private 저장소에서 Pages 안 됨 | Public으로 전환하거나 유료 플랜 사용 |

---

## 6. 라이선스 주의

- **코드**: MIT (`LICENSE`)
- **Pokémon/Digimon 스프라이트**: 각 권리자 소유, PMD 스프라이트는 CC BY-NC
- **비상업** 팬 프로젝트입니다. 기기 판매, 후원 보상, 광고 수익화에 사용하지 마세요.
- 스프라이트 팩은 저장소에 커밋하지 말고 SD 카드/릴리스로만 배포하세요.
