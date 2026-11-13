// TamaPoke - tamagotchi pixel art inspirado en la gen 1
// para Waveshare ESP32-S3-Touch-AMOLED-1.75
//
// Librerias (Library Manager o repo de Waveshare):
//   - "GFX Library for Arduino" (moononournation), con soporte CO5300 QSPI
//   - "SensorLib" (Lewis He), driver tactil CST9217
//
// Placa: ESP32S3 Dev Module | Flash 16MB | PSRAM: OPI PSRAM | USB CDC On Boot: Enabled
//
// Los sprites y la tabla de especies se generan con tools/sprites.py (emit).

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <time.h>
#include <esp_sleep.h>
#include "Arduino_GFX_Library.h"
#include "TouchDrvCSTXXX.hpp"
#include "pin_config.h"
#include "species.h"
#include "dex.h"
#include "pet.h"
#include "sdmon.h"
#include "rtcbat.h"
#include "i18n.h"
#include "audio.h"
#include "battle.h"
#include "time_utils.h"
#include "dayphase.h"
#include "imu.h"
#include "multiboot.h"
#include <U8g2lib.h>  // datos de la fuente coreana (la pinta Arduino_GFX)

// Version del firmware. Subir este numero en cada release (y manifest.json para
// el instalador web). Se muestra en la pantalla de ajustes y por serie al arrancar.
// Multi-game build: this copy always ships the public Gen-3 profile (386
// Pokemon).  Without an explicit profile the fork refuses to compile.
#ifndef TAMAPOKE_GEN3_RELEASE
#define TAMAPOKE_GEN3_RELEASE
#endif

// Version shown in settings and tagged into the image for TamaHub.  Keep it a
// single literal: the installer reads this same #define, so page, hub and
// settings always agree.
#define FW_VERSION "0.1.0"

#if defined(TAMAPOKE_LOCAL_TEST) && defined(TAMAPOKE_GEN3_RELEASE)
#error "TAMAPOKE_LOCAL_TEST and TAMAPOKE_GEN3_RELEASE are mutually exclusive"
#endif
#if DEX_COUNT > 251 && !defined(TAMAPOKE_LOCAL_TEST) && !defined(TAMAPOKE_GEN3_RELEASE)
#error "Gen-3 source requires an explicit local-test or release build profile"
#endif

// TamaHub scans the image for this tag, so the app can live in any ota slot.
// Keep the string in .rodata and mark it used: nothing else references it.
__attribute__((used)) const char kHubAppId[] = "TAMAHUB_APP:IdlePoke";
__attribute__((used)) const char kHubAppVer[] = "TAMAHUB_VER:" FW_VERSION;

#define HELP_PAGE_COUNT 3
#define HELP_LINE_COUNT 6

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(
  bus, LCD_RESET, 0 /*rotation*/, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
// Framebuffer completo en PSRAM: dibujamos todo y hacemos flush() (sin parpadeo)
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);

TouchDrvCST92xx touch;
Pet pet;

// sprite animado de la SD para la especie actual (si existe el archivo)
SdMon mon;          // sprite B/N (respaldo y minijuego si no hay PMD)
PmdMon pmd;         // sprite PMD multi-accion (pantalla principal)
PmdMon evoPmd;      // forma anterior, solo durante el parpadeo de evolucion
PmdMon wildPmd;     // rival salvaje en la pantalla de combate
int16_t monFor = -2;
bool monShinyFor = false;

// comportamiento del bicho en pantalla
struct {
  uint8_t mode = 0;     // 0 idle, 1 paseo, 2 gesto one-shot
  uint8_t act = PMD_IDLE;
  uint32_t t0 = 0;      // inicio de la animacion en curso
  uint32_t until = 0;   // fin del estado actual
  float x = 233, targetX = 233;
} beh;
#define PET_GROUND 304  // linea de suelo de la mascota
PmdMon galleryPmd;  // sprite grande de la vista detalle de la galeria (PMD/TPK2, legal)

// galeria pokedex
bool galleryOpen = false;
bool galleryDirty = false;
int galleryPage = 0;        // 10 paginas de 16
int16_t galleryDetail = 0;  // dex en vista detalle, 0 = rejilla
uint8_t galleryFilter = 0;  // 0 todos, 1 criados, 2 capturados
uint8_t galleryRegion = 0;  // 0 all, 1 Kanto, 2 Johto, 3 Hoenn

bool screenOff = false;       // pulsacion corta del boton PWR
bool cardOpen = false;        // ficha del bicho (deslizar vertical)
bool kbOpen = false;          // teclado para renombrar al bicho
char nameBuf[12] = "";
uint8_t nameLen = 0;
#define CARD_COUNT 9
uint8_t cardPage = 0;         // 0 perfil ... 7 expedicion, 8 pasos/trail
uint8_t boxPage = 0;
uint8_t boxSort = 0;          // 0 dex, 1 tipo, 2 criados primero
bool expeditionTrainChoiceOpen = false;
ExpeditionSite expeditionSiteChoice = EXP_SITE_COAST;
bool clockOpen = false;       // pantalla de ajuste de hora (deslizar abajo)
int clockH = 12, clockM = 0;  // hora en edicion
bool powerSave = false;       // ahorro opcional: off por defecto
bool easyMode = false;        // unused in the idle fork, kept for old saves
uint8_t gameSpeed = 1;        // idle evolution speed: 1x / 2x / 4x / 8x

// ---------- idle engine ----------
// One form lives for one hour of "game" time.  The speed multiplier scales
// only that hour; the wall clock and the day/night scene stay real.
#define IDLE_EVO_SEC 3600UL
#define IDLE_MAX_EVOLVES_PER_BOOT 12
static uint32_t idleAccum = 0;        // scaled seconds toward the next form
static uint32_t idleAnchorEpoch = 0;  // wall clock at the last save
static uint32_t idleLastSecond = 0;   // millis of the last idle tick
static uint32_t idleLastEpoch = 0;    // RTC second of the last idle tick
static uint32_t idleLastSave = 0;
static uint16_t idleEvoTotal = 0;
static char idleToast[28] = "";
static uint32_t idleToastUntil = 0;
static bool idleBooting = true;
static uint32_t releaseArmUntil = 0;

bool helpOpen = false;
uint8_t helpPage = 0;
bool uiDirty = true;
bool cardDirty = true;
bool clockDirty = true;
bool helpDirty = true;
bool keyboardDirty = true;
bool gameMenuDirty = true;
bool battleDirty = true;
bool starterDirty = true;

int8_t watermarkDrawnSlot = -1;

// escena de bano: espuma sobre el bicho y limpieza al reventar
uint32_t bathUntil = 0;
bool bathPending = false;
struct { int16_t x, y; uint8_t r, ph; } bubbles[14];
uint32_t feedMenuUntil = 0;   // selector de comida abierto hasta este millis
uint32_t nextAmbientSoundAt = 0;

// minijuego "toques": mantener la pokeball en el aire
bool gameOpen = false;
bool gameMenuOpen = false;
uint8_t gameMode = 0;  // 0 ball, 1 catch, 2 memo
uint32_t gameOverUntil = 0;
float ballX, ballY, ballVX, ballVY, gamePetX;
uint8_t gameScore, gameMisses;
float hitX, hitY;             // ultimo golpe (anillo de impacto)
uint32_t hitTime = 0;
uint32_t ballLastHitAt = 0;
bool gameNewHi = false;
uint8_t gameGain = 0;
uint32_t catchUntil = 0, catchTargetUntil = 0;
int16_t catchX = 0, catchY = 0;
uint8_t catchIcon = 0;
uint8_t memoSeq[14] = { 0 };
uint8_t memoLen = 0, memoShow = 0, memoInput = 0, memoRounds = 0;
uint32_t memoNextAt = 0;
bool memoShowing = false;
int8_t memoActivePad = -1, memoFlashPad = -1, memoHintPad = -1;
bool memoFlashGood = false;
uint32_t memoFlashUntil = 0, memoFailUntil = 0, memoTurnUntil = 0;
int16_t cleanX[4] = { 0 }, cleanY[4] = { 0 };
bool cleanAlive[4] = { false };
uint32_t cleanUntil = 0, cleanSpawnAt = 0;
uint8_t cleanActive = 0;
uint8_t typeEnemy = TYPE_GRASS;
uint8_t typeChoice[3] = { TYPE_FIRE, TYPE_WATER, TYPE_GRASS };
uint8_t typeCorrect = 0;
uint32_t typeUntil = 0;

// saco de entrenamiento (entrena la fuerza)
bool sackOpen = false;
uint32_t sackUntil = 0, sackOverUntil = 0;
uint16_t sackHits = 0;
float sackShake = 0;
uint8_t sackGain = 0;
bool sackNewHi = false;

bool battleOpen = false;
bool battleResolved = false;
int16_t battleDex = 0;
uint8_t battleLevel = 1;
BattleStats battlePlayer = {};
BattleStats battleEnemy = {};
BattleResult battleResult = {};
BattleRuntime battleRun = {};
BattleTurnResult battleTurn = {};
BattleAction battleLastAction = BATTLE_ATTACK;
BattleReward battleReward = {};
char battleMsg[28] = "";
uint32_t battleAttackMenuUntil = 0;
bool battleCatchOffered = false;
bool battleCatchTried = false;
bool battleCatchDone = false;
bool battleCatchSuccess = false;
bool battleRespectCatch = false;
uint8_t battleCatchChance = 0;
bool battleLowHpWarned = false;

#define WILD_COOLDOWN_MS (20UL * 60UL * 1000UL)
#define WILD_PROMPT_MS 20000UL
#define WILD_CHECK_MS 60000UL
uint32_t wildPromptUntil = 0;
uint32_t nextWildEligible = 0;
uint32_t lastWildCheck = 0;
int16_t wildPromptDex = 0;
uint8_t wildPromptLevel = 1;
bool wildPromptShiny = false;
bool battleShiny = false;

#define PET_EVENT_COOLDOWN_MS (15UL * 60UL * 1000UL)
#define PET_EVENT_PROMPT_MS 18000UL
#define PET_EVENT_CHECK_MS 60000UL
uint32_t petEventUntil = 0;
uint32_t nextPetEventEligible = 0;
uint32_t lastPetEventCheck = 0;
uint8_t petEventType = PET_EVENT_BERRY;
uint32_t petEventFeedbackUntil = 0;
char petEventMsg[18] = "";
uint32_t morningUntil = 0;
uint32_t eggWiggleUntil = 0;

int sceneHour();
uint8_t currentDayPhase();
bool mainScreenReadyForShake();
void maybeOfferMorning(uint32_t now);
uint32_t pmdActTotalMs(const PmdAct &a);
bool watermarkNeedsRedraw();
void flushStampedFrame();
void startBattleWith(int16_t forcedDex, uint8_t forcedLevel, int8_t forcedShiny = -1);
void startBattle();

// las 9 especies con sprite propio en flash (respaldo sin SD): dex -> indice
int flashIdxForDex(int16_t dex) {
  static const int8_t IDX[10] = { -1, 3, 4, 5, 0, 1, 2, 6, 7, 8 };
  return (dex >= 1 && dex <= 9) ? IDX[dex] : -1;
}

#define CX 233  // centro de la pantalla redonda
#define CY 233
#define PET_CY 202  // centro vertical del sprite

// ---------- texto: tamano y cursor (fuente clasica / coreana) ----------
// La 5x7 de Arduino_GFX ancla el cursor en la ESQUINA SUPERIOR y mide 8 px de
// alto a escala 1; la fuente U8g2 del coreano ancla en la LINEA BASE. setCur()
// compensa el ascenso para que la Y siga significando "arriba" con las dos.
uint8_t gTextSize = 1;
int gFontAscent = 0;  // px del borde superior a la linea base (0 = 5x7 clasica)
bool gCjkFont = false;

// La unifont coreana mide 16 px a escala 1, lo mismo que la 5x7 a escala 2, asi
// que la escala pedida se divide por 2 para que los tamanos cuadren.
#define CJK_FONT_KO u8g2_font_unifont_t_korean2
#define CJK_SIZE_DIV 2

void setSize(uint8_t n) {
  // gTextSize guarda la escala REALMENTE aplicada: setCur() multiplica el
  // ascenso por ella y tiene que cuadrar con lo que se pinta.
  gTextSize = gCjkFont ? (n >= CJK_SIZE_DIV ? n / CJK_SIZE_DIV : 1) : n;
  gfx->setTextSize(gTextSize);
}

void setCur(int x, int y) { gfx->setCursor(x, y + gFontAscent * gTextSize); }

// Fija la fuente del idioma activo. Es el UNICO sitio que la toca.
void applyLangFont() {
  gCjkFont = LANG_IS_CJK(gLang);
  gTextSize = 1;
  gfx->setTextSize(1);
  if (!gCjkFont) {
    gfx->setFont();          // 5x7 clasica: la Y ya es el borde superior
    gfx->setUTF8Print(false);
    gFontAscent = 0;
    return;
  }
  gfx->setFont((const uint8_t *)CJK_FONT_KO);
  gfx->setUTF8Print(true);   // las cadenas coreanas son UTF-8 multibyte
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds("A", 0, 0, &x1, &y1, &w, &h);
  gFontAscent = -y1;         // y1 negativo: subir desde la linea base
}

// Ancho del texto en la fuente activa. Con la clasica devuelve exactamente la
// misma cuenta que hacia el codigo (strlen * 6 * escala); con la coreana mide.
uint16_t textW(const char *s, uint8_t size) {
  if (!gCjkFont) return (uint16_t)strlen(s) * 6 * size;
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

// Alto real de una linea. La 5x7 ocupa 8 px por escala; la unifont coreana
// ocupa 16 px por escala real (setSize divide por 2), asi que la cuenta no
// puede ser 8*n: en coreano setSize(3) se queda en 16 px, no en 24.
int textH(uint8_t size) {
  if (!gCjkFont) return 8 * size;
  const uint8_t real = size >= CJK_SIZE_DIV ? size / CJK_SIZE_DIV : 1;
  return 16 * real;
}

int centerX(const char *s, uint8_t size) { return CX - textW(s, size) / 2; }

// La unifont a escala reducida deja trazos de 1 px; con el coreano se repinta
// desplazado 1 px (pseudo-negrita) para que no se vea tenue.
void printT(char c) { gfx->print(c); }

void printT(const char *s) {
  if (gCjkFont) {
    int16_t x = gfx->getCursorX(), y = gfx->getCursorY();
    gfx->print(s);
    gfx->setCursor(x + 1, y);
  }
  gfx->print(s);
}

static const uint16_t INK_K = 0x18C4;  // spriteColor('k')

void loadPowerSave() {
  Preferences p;
  p.begin("idlepoke", true);
  powerSave = p.getBool("psave", false);
  p.end();
}

void setPowerSave(bool on) {
  powerSave = on;
  Preferences p;
  p.begin("idlepoke", false);
  p.putBool("psave", powerSave);
  p.end();
}

// Idle fork: the only gameplay option is the evolution speed.  It lives in the
// same NVS namespace as the save, but in its own keys, so changing it never
// touches the creature.
void loadGameOptions() {
  Preferences p;
  p.begin("idlepoke", true);
  gameSpeed = p.getUChar("gspd", 1);
  p.end();
  if (gameSpeed != 1 && gameSpeed != 2 && gameSpeed != 4 && gameSpeed != 8) gameSpeed = 1;
  easyMode = false;
  pet.easyMode = false;
  pet.setGameSpeed(1);  // age/hatch stay real-time; only IDLE_EVO_SEC is scaled
}

void setGameSpeed(uint8_t speed) {
  if (speed != 1 && speed != 2 && speed != 4 && speed != 8) speed = 1;
  gameSpeed = speed;
  Preferences p;
  p.begin("idlepoke", false);
  p.putUChar("gspd", gameSpeed);
  p.end();
}

// botones de icono siguiendo el arco inferior de la pantalla redonda
// (los exteriores van mas altos para no salirse del circulo)
struct Btn {
  int16_t cx, cy;
  const char *const *icon;
};
Btn buttons[4] = {
  { 140, 390, SPR_ICON_FOOD },   // comer
  { 202, 404, SPR_ICON_PLAY },   // jugar
  { 264, 404, SPR_ICON_LIGHT },  // luz
  { 326, 390, SPR_ICON_CLEAN },  // bano
};
#define BTN_HALF 26  // boton de 52x52
#define BTN_HIT 36   // radio tactil (un poco mas generoso)

// grietas del huevo (pixeles 'k' sobre el sprite)
static const uint8_t CRACK1[][2] = { {15,8},{16,9},{15,10} };
static const uint8_t CRACK2[][2] = { {11,13},{12,14},{11,15},{20,12},{19,13},{20,14} };
// estrellas del modo noche
static const uint16_t STARS[][2] = { {120,140},{330,120},{370,210},{95,230},{280,90},{160,95} };

bool wasPressed = false;
// eleccion de inicial (primera partida): primero region, despues 3 starters.
static const int16_t STARTER_DEX[3][3] = {
  { 1, 4, 7 },       // Kanto
  { 152, 155, 158 }, // Johto
  { 252, 255, 258 }, // Hoenn
};
static const StrId STARTER_REGION_LABELS[3] = {
  S_RANK_KANTO, S_RANK_JOHTO, S_RANK_HOENN
};
uint8_t starterRegion = 0xFF;  // se elige en cada arranque si el starter sigue pendiente
#define STARTER_ROW_Y 110
#define STARTER_ROW_H 70
#define STARTER_ROW_GAP 8
// boton-CTA de evolucion (centrado, mitad de pantalla)
#define EVO_BTN_W 256
#define EVO_BTN_H 64
#define EVO_BTN_X (CX - EVO_BTN_W / 2)
#define EVO_BTN_Y 172
// boton-CTA de despedida (mas ancho: lleva el nombre + frase)
#define FAR_BTN_W 408
#define FAR_BTN_H 58
#define FAR_BTN_X (CX - FAR_BTN_W / 2)
#define FAR_BTN_Y 176
// el CST9217 avisa por el pin INT cuando hay datos tactiles; lo usamos para no
// leer el bus I2C mientras el chip esta dormido (esa lectura se colgaba ~1s)
volatile bool gTouchIrq = false;
void IRAM_ATTR touchIsr() { gTouchIrq = true; }
uint32_t lastRender = 0;
uint32_t lastLoopStart = 0;
uint32_t perfRenderLastMs = 0, perfRenderMaxMs = 0;
uint32_t perfLoopLastMs = 0, perfLoopMaxMs = 0;
uint32_t perfRenderCount = 0, perfRenderSkipCount = 0;
// proteccion del AMOLED: atenuado por inactividad
uint32_t lastInteract = 0;
uint8_t dimStage = 0;        // 0 despierto, 1 atenuado (90s), 2 casi apagado (5min)
bool swallowGesture = false; // el toque que despierta no acciona nada
uint32_t ignoreTouchUntil = 0;
uint32_t holdStart = 0;     // pulsacion larga sobre el bicho
uint32_t confirmUntil = 0;  // dialogo "soltar?" activo hasta este millis
uint8_t choiceKind = 0;     // 0 ninguno, 1 evolucion, 2 despedida, 3 objetivo de evolucion
uint32_t choiceUntil = 0;   // se cierra solo a este millis
int16_t tX0, tY0, tXl, tYl; // gesto en curso (inicio y ultima posicion)
uint32_t tStart = 0;
bool holdFired = false;

void markUiDirty() {
  uiDirty = true;
  starterDirty = true;
  cardDirty = true;
  clockDirty = true;
  helpDirty = true;
  keyboardDirty = true;
  gameMenuDirty = true;
  battleDirty = true;
  galleryDirty = true;
}

void lockTouchBrief(uint16_t ms = 160) {
  uint32_t until = millis() + ms;
  if (until > ignoreTouchUntil) ignoreTouchUntil = until;
  wasPressed = false;
  swallowGesture = true;
}

const char *screenName() {
  if (pet.awaitingStarter()) return "starter";
  if (galleryOpen) return galleryDetail ? "gallery-detail" : "gallery-grid";
  if (gameOpen) return "game";
  if (sackOpen) return "sack";
  if (battleOpen) return battleResolved ? "battle-result" : "battle";
  if (kbOpen) return "keyboard";
  if (helpOpen) return "help";
  if (clockOpen) return "settings";
  if (cardOpen) return "card";
  if (gameMenuOpen) return "game-menu";
  return screenOff ? "screen-off" : "main";
}

// Pulsacion larga para soltar al bicho: vale en CUALQUIER punto de la pantalla
// principal. Antes habia que empezar el gesto sobre el sprite (inPetZone) y una
// lista de pantallas excluidas escrita a mano, asi que el gesto quedaba casi
// escondido y ademas se quedaba corto en cuanto se anadia una pantalla nueva.
// screenName() solo devuelve "main" cuando ningun panel, juego ni aviso manda
// en la pantalla, asi que preguntar por el no se puede quedar desfasado.
bool canHoldToRelease() {
  if (strcmp(screenName(), "main") != 0) return false;
  if (feedMenuUntil || confirmUntil || choiceKind || bathUntil || wildPromptUntil || petEventUntil) return false;
  return !pet.isEgg() && !pet.ceremony && !pet.awaitingStarter();
}

bool staticScreenClean() {
  if (watermarkNeedsRedraw()) return false;
  if (pet.awaitingStarter()) return !starterDirty;
  if (galleryOpen && !galleryDetail) return !galleryDirty;
  if (battleOpen && battleResolved) return !battleDirty;
  if (kbOpen) return !keyboardDirty;
  if (helpOpen) return !helpDirty;
  if (clockOpen) return !clockDirty;
  if (cardOpen) return !cardDirty;
  if (gameMenuOpen) return !gameMenuDirty;
  return false;
}

void physicalBack();   // BOOT short press

void setup() {
  Serial.setRxBufferSize(8192);  // la transferencia a SD llega en bloques de 2 KB
  Serial.begin(115200);
  // CRITICO: sin esto, Serial.print BLOQUEA el juego cuando no hay un
  // monitor serie abierto en el host (el bufer TX del USB CDC se llena
  // y nadie lo vacia) -> con timeout 0 los mensajes se descartan
  Serial.setTxTimeoutMs(0);
  Serial.printf("IdlePoke fw v%s\n", FW_VERSION);
  Serial.printf("hub tag: %s %s\n", kHubAppId, kHubAppVer);  // keep tags in the image
  mboot::begin();  // lower case button (BOOT/GPIO0) opens the game menu
  loadLang();  // idioma guardado (ingles por defecto)
  Wire.begin(IIC_SDA, IIC_SCL);
  // CST9217 (tactil), AXP2101 (PMU) y PCF85063 (RTC) comparten este bus I2C.
  // Red de seguridad para PMU/RTC (SensorLib NO respeta este timeout en el
  // tactil; el cuelgue del tactil dormido se resuelve gateando por INT, ver
  // handleTouch).
  Wire.setTimeOut(50);

  // CRITICO: encender la alimentacion del panel (BLDO1=OLED VDD 3.3V) ANTES de
  // inicializar el display. Si el PMU se reseteo (drenaje total), este rail
  // queda OFF y la pantalla se ve negra aunque el resto de la placa funcione.
  pmuEnablePanel();

  // QSPI a 80MHz (por defecto 40): el flush del framebuffer es el cuello de
  // botella del fps (~56ms a 40MHz). Si el panel mostrara basura, bajar a 40M.
  if (!gfx->begin(80000000)) Serial.println("gfx->begin() fallo");
  panel->setBrightness(180);
  applyLangFont();  // fuente del idioma guardado (clasica salvo coreano)

  touch.setPins(TP_RESET, TP_INT);
  bool touchOk = false;
  for (int i = 0; i < 3 && !touchOk; i++) {  // a veces falla al primer intento
    touchOk = touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL);
    if (!touchOk) delay(150);
  }
  if (!touchOk) Serial.println("CST9217 no detectado");
  // begin() deja el chip en modo comando (lee la identidad y no sale);
  // hace falta un reset por hardware para que vuelva a reportar toques
  touch.reset();
  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  touch.setMirrorXY(true, true);  // el panel esta montado girado 180 grados
  // INT activo-bajo: salta cuando hay datos. Gatea las lecturas I2C (ver loop)
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TP_INT), touchIsr, FALLING);

  pet.begin();
  Serial.printf("SAVE %s spec=%d lv=%u wins=%u losses=%u streak=%u\n",
                pet.saveLoadedFromNvs ? "loaded" : "created",
                pet.speciesId, pet.level(), pet.battleWins, pet.battleLosses, pet.battleStreak);
  sdBegin();
  thumbs.load();

  // reloj real: aplica el tiempo que estuvo apagado
  rtcBegin();
  batBegin();
  pwrSetup();
  uint32_t e = rtcEpoch();
  if (e == 0) {
    rtcSetEpoch(1767225600UL);  // RTC virgen: semilla (la hora absoluta da igual,
    e = rtcEpoch();             // solo importan las diferencias)
    Serial.println("RTC sin hora: sembrado, sin progresion offline esta vez");
  }
  loadGameOptions();  // before syncClock/idleBoot: speed scales the offline time
  pet.syncClock(e);
  idleBoot(e);
  idleProcess(true);  // fast-forward any evolutions earned while powered off
  idleBooting = false;
  ensureMon();
  loadPowerSave();
  imuBegin();

  audioBegin();  // ES8311 + I2S + amplificador (suena un jingle de arranque)

  lastInteract = millis();
}

// carga/descarga el sprite de SD cuando cambia la especie
void ensureMon() {
  if (pet.speciesId == monFor && monShinyFor == pet.shiny && !sdDirty) return;
  sdDirty = false;
  monFor = pet.speciesId;
  monShinyFor = pet.shiny;
  mon.unload();
  pmd.unload();
  beh.x = beh.targetX = 233;
  beh.mode = 0;
  beh.until = 0;
  if (pet.speciesId >= 1 && pet.speciesId <= DEX_COUNT) {
    pmd.load(pet.speciesId, pet.shiny);          // principal: PMD
    if (!pmd.loaded) mon.load(pet.speciesId, pet.shiny);  // respaldo: B/N
  }
}

// ------------------------------------------------------------ idle engine ----
// The whole game loop of this fork: one form per hour, random branch, new egg
// after the final form.  Everything is stored in the "idlepoke" namespace so
// the real TamaPoke save is never touched.

void idleSave(uint32_t nowEpoch) {
  Preferences p;
  p.begin("idlepoke", false);
  p.putUInt("iacc", idleAccum);
  p.putUInt("ianch", nowEpoch ? nowEpoch : idleAnchorEpoch);
  p.putUShort("ievo", idleEvoTotal);
  p.end();
  if (nowEpoch) idleAnchorEpoch = nowEpoch;
  idleLastSave = millis();
}

void idleBoot(uint32_t nowEpoch) {
  Preferences p;
  p.begin("idlepoke", true);
  idleAccum = p.getUInt("iacc", 0);
  idleAnchorEpoch = p.getUInt("ianch", 0);
  idleEvoTotal = p.getUShort("ievo", 0);
  p.end();

  if (!idleAnchorEpoch) idleAnchorEpoch = nowEpoch;
  if (nowEpoch && idleAnchorEpoch && nowEpoch > idleAnchorEpoch) {
    uint32_t away = nowEpoch - idleAnchorEpoch;
    if (away > 7UL * 86400UL) away = 7UL * 86400UL;  // agreed offline cap
    if (!pet.isEgg() && !pet.ceremony) {
      idleAccum += away * (uint32_t)gameSpeed;
      Serial.printf("idle offline: %lu s x%u\n", (unsigned long)away, gameSpeed);
    } else if (pet.isEgg()) {
      idleAccum = 0;
    }
    idleAnchorEpoch = nowEpoch;
  }
  idleLastSecond = millis();
  idleLastEpoch = nowEpoch;
  idleLastSave = millis();
}

// The settings screen may move the RTC; drop the tick baseline so the jump is
// not credited (or charged) to the evolution timer.
void idleResetClock() {
  idleLastEpoch = 0;
  idleLastSecond = millis();
}

uint32_t idleRemainingSec() {
  return idleAccum >= IDLE_EVO_SEC ? 0 : IDLE_EVO_SEC - idleAccum;
}

uint16_t idlePermille() {
  uint32_t v = idleAccum * 1000UL / IDLE_EVO_SEC;
  return v > 1000UL ? 1000 : (uint16_t)v;
}

uint8_t nextSpeed(uint8_t s) {
  return s == 1 ? 2 : s == 2 ? 4 : s == 4 ? 8 : 1;
}

// Applies as many due steps as allowed.  `chain` is used at boot: a long
// absence may cross several forms, so the player sees the final result.  While
// running, one evolution at a time so the 5 s FX can play out.
void idleProcess(bool chain) {
  if (pet.isEgg()) {
    idleAccum = 0;
    return;
  }
  if (pet.ceremony) return;

  uint8_t guard = 0;
  uint8_t limit = chain ? IDLE_MAX_EVOLVES_PER_BOOT : 1;
  while (idleAccum >= IDLE_EVO_SEC && guard++ < limit) {
    if (!chain && pet.evolving()) break;  // let the 5 s FX finish first
    idleAccum -= IDLE_EVO_SEC;

    uint8_t branches = pet.idleEvolutionOptionCount();
    if (branches == 0) {
      // Final form: one hour later the cycle restarts with a fresh egg.
      pet.newEgg();
      idleAccum = 0;
      snprintf(idleToast, sizeof(idleToast), "NEW EGG");
      idleToastUntil = millis() + 4000;
      break;
    }

    int16_t old = pet.speciesId;
    int16_t target = pet.idleEvolutionOption((uint8_t)random(branches));
    if (!pet.idleForceEvolveTo(target)) {
      idleAccum += IDLE_EVO_SEC;  // should not happen; keep the time
      break;
    }
    evoPmd.load(old, pet.shiny);
    idleEvoTotal++;
    snprintf(idleToast, sizeof(idleToast), "EVOLVED!");
    idleToastUntil = millis() + 4500;
    if (!chain) break;
  }
  if (pet.isEgg()) idleAccum = 0;
}

void idleTick(uint32_t now) {
  if (now - idleLastSecond < 1000UL) return;

  uint32_t e = rtcEpoch();
  uint32_t dt;
  if (e && idleLastEpoch && e > idleLastEpoch) dt = e - idleLastEpoch;
  else dt = (now - idleLastSecond) / 1000UL;
  if (dt == 0) dt = 1;
  if (dt > 86400UL) dt = 86400UL;
  idleLastSecond = now;
  if (e) idleLastEpoch = e;

  if (!pet.isEgg() && !pet.ceremony && !pet.evolving()) {
    idleAccum += dt * (uint32_t)gameSpeed;
    idleProcess(false);
  }

  if (now - idleLastSave >= 60000UL) idleSave(e ? e : idleAnchorEpoch);
}


bool mainScreenReadyForAmbientSound() {
  if (audioMode() != SOUND_FULL || screenOff || dimStage > 0) return false;
  if (pet.awaitingStarter() || pet.isEgg() || pet.sleeping || pet.ceremony) return false;
  if (battleOpen || gameOpen || gameMenuOpen || sackOpen || cardOpen || galleryOpen || kbOpen || clockOpen || helpOpen) return false;
  if (feedMenuUntil || confirmUntil || choiceKind || bathUntil || wildPromptUntil || petEventUntil) return false;
  if (pet.evolving() || pet.wantEvolveButton() || pet.canRunawayNow() || pet.wantFarewellButton()) return false;
  return true;
}

void maybePlayAmbientSound(uint32_t now) {
  if (!mainScreenReadyForAmbientSound()) {
    nextAmbientSoundAt = now + 9000;
    return;
  }
  if (nextAmbientSoundAt == 0) nextAmbientSoundAt = now + 8000 + random(8000);
  if (deadlineActive(now, nextAmbientSoundAt)) return;
  uint8_t r = (uint8_t)random(3);
  sfxPlay(r == 0 ? SFX_HEART : (r == 1 ? SFX_EVENT_SPARKLE : SFX_MENU));
  nextAmbientSoundAt = now + 8000 + random(8000);
}

uint16_t renderIntervalMs() {
  if (screenOff) return 5000;
  if (battleOpen) return battleResolved ? 320 : 190;
  if (gameOpen || sackOpen) return 115;
  if (galleryOpen || cardOpen || kbOpen || clockOpen || helpOpen || gameMenuOpen) return powerSave ? 650 : 320;
  if (!powerSave) return 100;
  if (dimStage >= 2) return 650;
  if (dimStage >= 1) return 350;
  return 180;
}

bool lightSleepAllowed(uint32_t now) {
  if (!powerSave || usbPresent() || audioBusy() || Serial.available()) return false;
  if (!screenOff && dimStage == 0) return false;
  if (!screenOff && now - lastInteract < 1500UL) return false;
  if (wasPressed || gTouchIrq || deadlineActive(now, ignoreTouchUntil)) return false;
  if (gameOpen || sackOpen || battleOpen || bathUntil) return false;
  if (pet.awaitingStarter() || feedMenuUntil || confirmUntil || choiceKind || wildPromptUntil || petEventUntil) return false;
  if (pet.evolving() || pet.ceremony || pet.eating() || pet.showHeart()) return false;
  if (galleryOpen || cardOpen || kbOpen || clockOpen || helpOpen || gameMenuOpen) return false;
  return true;
}

uint16_t lightSleepMs(uint32_t now) {
  if (!lightSleepAllowed(now)) return 0;
  uint16_t maxMs = screenOff ? 750 : (dimStage >= 2 ? 300 : 180);
  // Die Software-Schritterkennung braucht auch bei dunklem Display regelmaessige
  // Rohwerte. Kurze Light-Sleep-Intervalle erhalten sie ohne Dauer-Rendering.
  if (imuOk() && maxMs > 70) maxMs = 70;
  uint16_t ri = renderIntervalMs();
  uint32_t sinceRender = now - lastRender;
  if (sinceRender < ri) {
    uint32_t untilRender = ri - sinceRender;
    if (untilRender < maxMs) maxMs = untilRender;
  }
  if (maxMs < 40) return 0;
  return maxMs;
}

void maybeLightSleep(uint32_t now) {
  uint16_t ms = lightSleepMs(now);
  if (!ms) return;

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)TP_INT, 0);
  esp_light_sleep_start();

  if (digitalRead(TP_INT) == LOW || esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    gTouchIrq = true;
  }
}

void loop() {
  uint32_t now = millis();
  uint32_t loopStart = now;
  bool petChanged = pet.update(now);
  if (petChanged && cardOpen && cardPage != 7) cardDirty = true;

  handleTouch();
  handleSerial();
  idleTick(now);

  // Lower physical button (BOOT/GPIO0): hold 2 s to save and return to TamaHub.
  static mboot::Hold mbHold;
  if (mbHold.reached()) {
    panel->setBrightness(180);
    screenOff = false;
    mboot::screenMessage(gfx, "SAVING", "please wait", 0x07E0);
    uint32_t e = rtcEpoch();
    if (e) pet.lastSeenEpoch = e;
    idleSave(e);
    pet.saveNow();
    mboot::screenMessage(gfx, "SAVED", "release the button", 0x07E0);
    mboot::waitRelease();
    if (!mboot::hubLooksValid()) {
      Serial.println("multiboot: factory slot is empty, staying in game");
      mboot::screenMessage(gfx, "HUB MISSING", "reinstall from a PC", 0xFD20);
      delay(2500);
    } else {
      mboot::returnToHub();
    }
  }

  // Short press = Back.  The 2 s hold above still saves and returns to TamaHub.
  if (mbHold.shortPress()) physicalBack();

  ensureMon();
  setPowerCacheInterval(powerSave ? 10000UL : 2000UL);

  // The idle fork has no wild battles, pet events, expeditions or step goals.
  // The screen is the game; only the clock and the evolution timer advance.

  // Die Expeditionskarte bleibt sonst als statischer Screen stehen. Ein
  // sekundenweises Dirty-Render ist nur aktiv, waehrend ihr Countdown sichtbar ist.
  static uint32_t lastExpeditionCardTick = 0;
  if (cardOpen && cardPage == 7 && now - lastExpeditionCardTick >= 1000UL) {
    lastExpeditionCardTick = now;
    cardDirty = true;
  }

  // pulsacion corta del PWR: pantalla on/off
  static uint32_t lastPwr = 0;
  if (now - lastPwr > 250) {
    lastPwr = now;
    if (pwrShortPressed()) {
      bool wasOff = screenOff;
      screenOff = !screenOff;
      if (!screenOff) {
        lastInteract = now;
        if (wasOff) {
          ignoreTouchUntil = now + 900;
          swallowGesture = true;
          wasPressed = false;
        }
      }
    }
  }

  updateBrightness(now);

  // vuelca el autoguardado periodico SOLO con la pantalla atenuada/apagada o
  // durmiendo: la escritura a NVS congela ~1s ambos cores (caché de flash off),
  // y aqui no hay animacion que se corte ni dedo esperando respuesta. Con 90s
  // de inactividad la pantalla ya atenua, asi que se vuelca enseguida; el uso
  // activo persiste igual por los guardados de cada accion (comer/jugar/...).
  if (pet.savePending() && (screenOff || dimStage >= 1 || pet.sleeping)) {
    pet.flushSave();
  }

  // anota la hora real cada 30 s (se persiste en cada save del juego)
  static uint32_t lastClock = 0;
  uint32_t clockPollMs = powerSave ? 60000UL : 30000UL;
  if (now - lastClock > clockPollMs) {
    lastClock = now;
    uint32_t e = rtcEpoch();
    if (e) pet.lastSeenEpoch = e;
  }

  static bool expeditionWasActive = false;
  bool expeditionActiveNow = pet.expeditionActive(pet.lastSeenEpoch);
  if (expeditionWasActive && !expeditionActiveNow && pet.expeditionEndEpoch) {
    sfxPlay(SFX_EXPEDITION_FOUND);
    if (cardOpen && cardPage == 7) cardDirty = true;
  }
  expeditionWasActive = expeditionActiveNow;

  // latido de salud cada 5 min (para el soak test; se descarta si no hay monitor)
  static uint32_t lastHealth = 0;
  uint32_t healthMs = powerSave ? 600000UL : 300000UL;
  if (now - lastHealth > healthMs) {
    lastHealth = now;
    Serial.printf("HEALTH up=%lus heap=%u min=%u rMax=%lums lMax=%lums screen=%s\n",
                  (unsigned long)(now / 1000), ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                  (unsigned long)perfRenderMaxMs, (unsigned long)perfLoopMaxMs, screenName());
    perfRenderMaxMs = 0;
    perfLoopMaxMs = 0;
  }

  // Juego/combate usan intervalos conservadores para que el redibujado no pise
  // el envio DMA del frame anterior; las pantallas estaticas se saltan si no
  // estan "dirty".
  if (now - lastRender >= renderIntervalMs()) {
    lastRender = now;
    uint32_t rt0 = millis();
    render();
    perfRenderLastMs = millis() - rt0;
    if (perfRenderLastMs > perfRenderMaxMs) perfRenderMaxMs = perfRenderLastMs;
    perfRenderCount++;
  }

  maybeLightSleep(millis());
  perfLoopLastMs = millis() - loopStart;
  if (perfLoopLastMs > perfLoopMaxMs) perfLoopMaxMs = perfLoopLastMs;
  lastLoopStart = loopStart;
}

// brillo segun sueno + inactividad (proteccion del AMOLED)
void updateBrightness(uint32_t now) {
  // los eventos visibles despiertan la pantalla solos
  if (pet.evolving() || pet.ceremony || pet.eating() || pet.showHeart()) {
    lastInteract = now;
  }
  uint32_t idle = now - lastInteract;
  dimStage = (idle > 300000) ? 2 : (idle > 90000) ? 1 : 0;
  uint8_t target = pet.sleeping ? 25 : (usbPresent() ? 180 : 145);
  if (dimStage == 1) target = pet.sleeping ? 10 : 60;
  else if (dimStage == 2) target = 8;
  if (screenOff) target = 0;
  static uint8_t current = 255;
  if (target != current) {
    current = target;
    panel->setBrightness(target);
  }
}

// ---------- consola serie (provision de SD + depuracion) ----------

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  if (sdSerialCommand(line)) return;

  // Browser save backup protocol: SAVEGET -> "SAVE <bytes>" + hex data + DONE;
  // SAVEPUT <bytes> -> OK + raw data -> DONE and reboot. The Pet checksum
  // rejects altered or incomplete snapshots before NVS is changed.
  if (line == "SAVEGET") {
    uint8_t backup[Pet::BACKUP_MAX_BYTES];
    size_t size = pet.exportBackup(backup, sizeof(backup));
    if (!size) { Serial.println("ERR"); return; }
    // Normal game logs must never block when no terminal is open, hence setup
    // uses a zero TX timeout. A backup is requested by a connected browser,
    // so temporarily wait for its complete (hex) response instead of dropping it.
    Serial.setTxTimeoutMs(1500);
    Serial.printf("SAVE %u\n", (unsigned)size);
    Serial.print("SAVEHEX ");
    static const char HEX_CHARS[] = "0123456789ABCDEF";
    for (size_t i = 0; i < size; ++i) { Serial.print(HEX_CHARS[backup[i] >> 4]); Serial.print(HEX_CHARS[backup[i] & 15]); }
    Serial.println();
    Serial.println("DONE");
    Serial.flush();
    Serial.setTxTimeoutMs(0);
    return;
  }
  if (line.startsWith("SAVEPUT ")) {
    uint32_t size = (uint32_t)line.substring(8).toInt();
    if (size == 0 || size > Pet::BACKUP_MAX_BYTES) { Serial.println("ERR"); return; }
    uint8_t backup[Pet::BACKUP_MAX_BYTES];
    Serial.println("OK");
    Serial.setTimeout(6000);
    size_t got = Serial.readBytes(backup, size);
    Serial.setTimeout(1000);
    if (got != size || !pet.importBackup(backup, size)) { Serial.println("ERR"); return; }
    Serial.println("DONE");
    delay(150);
    ESP.restart();
    return;
  }

  if (line == "PERF") {
    Serial.printf("screen=%s render=%lums max=%lums count=%lu skip=%lu loop=%lums loopMax=%lums interval=%u dirty ui=%d card=%d clock=%d help=%d kb=%d menu=%d battle=%d gallery=%d\n",
                  screenName(),
                  (unsigned long)perfRenderLastMs, (unsigned long)perfRenderMaxMs,
                  (unsigned long)perfRenderCount, (unsigned long)perfRenderSkipCount,
                  (unsigned long)perfLoopLastMs, (unsigned long)perfLoopMaxMs,
                  renderIntervalMs(), uiDirty, cardDirty, clockDirty, helpDirty,
                  keyboardDirty, gameMenuDirty, battleDirty, galleryDirty);
    perfRenderMaxMs = 0;
    perfLoopMaxMs = 0;
    Serial.println("DONE");
  } else if (line == "HATCH") {
    pet.eggTap(); pet.eggTap(); pet.eggTap();
    Serial.println("DONE");
  } else if (line.startsWith("SPEC ")) {
    int n = line.substring(5).toInt();
    if (n >= 1 && n <= DEX_COUNT) {
      pet.prevSpeciesId = pet.speciesId;
      pet.speciesId = n;
      Serial.printf("especie #%d %s\n", n, DEX_TBL[n].name);
    }
    Serial.println("DONE");
  } else if (line.startsWith("LVL ")) {
    pet.ageMinutes = (uint32_t)line.substring(4).toInt() * MINUTES_PER_LEVEL;
    Serial.println("DONE");
  } else if (line.startsWith("TIME ")) {
    uint32_t e = (uint32_t)line.substring(5).toInt();
    rtcSetEpoch(e);
    pet.setClock(e);
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line.startsWith("RTCSET ")) {  // solo RTC (simular apagados en pruebas)
    rtcSetEpoch((uint32_t)line.substring(7).toInt());
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line == "TIME") {
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line == "GAL") {
    galleryOpen = !galleryOpen;
    galleryDetail = 0;
    galleryDirty = true;
    if (!galleryOpen) galleryPmd.unload();
    Serial.println("DONE");
  } else if (line == "EGGS") {
    // simula 20 tiradas de huevo (no cambia el estado del juego)
    for (int i = 0; i < 20; i++) {
      int16_t d = pet.pickEggSpecies();
      Serial.printf("%d:%s(r%u) ", d, DEX_TBL[d].name, DEX_TBL[d].rarity);
    }
    Serial.println();
    Serial.println("DONE");
  } else if (line == "SHINY") {  // alterna shiny del actual (pruebas)
    pet.shiny = !pet.shiny;
    Serial.printf("shiny=%d\n", pet.shiny);
    Serial.println("DONE");
  } else if (line.startsWith("NICK ")) {
    pet.rename(line.substring(5).c_str());
    Serial.printf("nick=%s\n", pet.nick);
    Serial.println("DONE");
  } else if (line == "CAREDAY") {  // simula un dia nuevo cuidado (pruebas)
    pet.setClock(pet.lastSeenEpoch + 86400);
    pet.caress();
    Serial.printf("streak=%u bond=%u medals=0x%X\n", pet.streak, pet.bond, pet.medals);
    Serial.println("DONE");
  } else if (line == "BYE") {
    pet.startFarewell();
    Serial.println("DONE");
  } else if (line == "RUN") {
    pet.startRunaway();
    Serial.println("DONE");
  } else if (line == "BEEP") {
    sfxPlay(SFX_HATCH);  // prueba de audio
    Serial.println("DONE");
  } else if (line == "ABANDON") {
    pet.dbgRunawayReady();  // fuerza el estado "lista para escaparse" (test del boton)
    Serial.println("DONE");
  } else if (line == "WIPE") {
    pet.factoryReset();     // borra NVS y reinicia -> partida nueva (eleccion de inicial)
    Serial.println("DONE");
    delay(100);
    ESP.restart();
  } else if (line == "REG") {
    Serial.printf("pokedex %u/%u:", pet.registeredCount(), DEX_COUNT);
    for (int i = 1; i <= DEX_COUNT; i++)
      if (pet.isRegistered(i)) Serial.printf(" %d", i);
    Serial.println();
    Serial.println("DONE");
#ifdef TAMAPOKE_LOCAL_TEST
  } else if (line.startsWith("TESTMON ")) {
    int split = line.indexOf(' ', 8);
    int n = line.substring(8, split < 0 ? line.length() : split).toInt();
    int shinyFlag = split < 0 ? 0 : line.substring(split + 1).toInt();
    if (n >= 1 && n <= DEX_COUNT) {
      pet.prevSpeciesId = pet.speciesId;
      pet.speciesId = n;
      pet.shiny = shinyFlag != 0;
      pet.sleeping = false;
      pet.ceremony = CER_NONE;
      markUiDirty();
      Serial.printf("testmon #%d %s shiny=%d\n", n, DEX_TBL[n].name, pet.shiny);
    }
    Serial.println("DONE");
  } else if (line.startsWith("TESTEVO ")) {
    int split = line.indexOf(' ', 8);
    int source = line.substring(8, split < 0 ? line.length() : split).toInt();
    int target = split < 0 ? 0 : line.substring(split + 1).toInt();
    if (source >= 1 && source <= DEX_COUNT) {
      pet.speciesId = source;
      pet.prevSpeciesId = -1;
      pet.sleeping = false;
      pet.ceremony = CER_NONE;
      pet.resetEvolutionDeferral();
      pet.careMistakes = 0;
      uint8_t requiredLevel = pet.evolutionRequiredLevelFor(target);
      pet.ageMinutes = (uint32_t)(requiredLevel > 0 ? requiredLevel - 1 : 0) * MINUTES_PER_LEVEL;
      pet.fullness = pet.joy = pet.energy = pet.hygiene = 80;
      pet.bond = 60;
      pet.trAtk = pet.trDef = pet.trSpe = 0;
      uint32_t testEpoch = 1767225600UL + 12UL * 3600UL;
      if (target == 197) testEpoch += 9UL * 3600UL;
      pet.setClock(testEpoch);
      if (source == 236) {
        if (target == 106) pet.trAtk = 80;
        else if (target == 107) pet.trDef = 80;
        else pet.trAtk = pet.trDef = 40;
      }
      markUiDirty();
      Serial.printf("testevo #%d -> #%d level=%u bond=%u\n", source, target,
                    pet.level(), pet.bond);
    }
    Serial.println("DONE");
  } else if (line == "CAUGHT") {
    Serial.printf("caught %u/%u:", pet.caughtCount(), DEX_COUNT);
    for (int i = 1; i <= DEX_COUNT; i++)
      if (pet.isCaught(i)) Serial.printf(" %d", i);
    Serial.println();
    Serial.println("DONE");
  } else if (line.startsWith("CAUGHT ")) {
    int n = line.substring(7).toInt();
    if (n < 1 || n > DEX_COUNT) n = pet.speciesId;
    if (n >= 1 && n <= DEX_COUNT) {
      pet.registerCaught((int16_t)n);
      Serial.printf("caught #%d %s\n", n, DEX_TBL[n].name);
    }
    Serial.println("DONE");
  } else if (line.startsWith("BATTLE ")) {
    int split = line.indexOf(' ', 7);
    int n = line.substring(7, split < 0 ? line.length() : split).toInt();
    int8_t shinyOverride = -1;
    if (split >= 0) shinyOverride = line.substring(split + 1).toInt() ? 1 : 0;
    startBattleWith((int16_t)n, pet.level(), shinyOverride);
    Serial.printf("battle #%d %s shiny=%d caught=%d\n", battleDex,
                  (battleDex >= 1 && battleDex <= DEX_COUNT) ? DEX_TBL[battleDex].name : "?",
                  battleShiny, pet.isCaught(battleDex));
    Serial.println("DONE");
  } else if (line == "BATTLE") {
    startBattle();
    Serial.printf("battle #%d %s caught=%d\n", battleDex,
                  (battleDex >= 1 && battleDex <= DEX_COUNT) ? DEX_TBL[battleDex].name : "?",
                  pet.isCaught(battleDex));
    Serial.println("DONE");
#endif
  } else if (line == "SAVEINFO") {
    Serial.printf("fw=%s save=%s createdBoot=%d spec=%d level=%u egg=%d starter=%d age=%lu\n",
                  FW_VERSION, pet.saveLoadedFromNvs ? "loaded" : "created",
                  pet.saveCreatedThisBoot, pet.speciesId, pet.level(), pet.isEgg(),
                  pet.awaitingStarter(), (unsigned long)pet.ageMinutes);
    Serial.printf("battle wins=%u losses=%u streak=%u best=%u nick=%s\n",
                  pet.battleWins, pet.battleLosses, pet.battleStreak,
                  pet.bestBattleStreak, pet.nick);
    Serial.printf("records ball=%u catch=%u memo=%u sack=%u\n",
                  pet.gameHi, pet.catchHi, pet.memoHi, pet.strHi);
    Serial.println("DONE");
  } else if (line == "HEALTH") {
    Serial.printf("up=%lus heap=%u min=%u sd=%d mon=%d\n",
                  (unsigned long)(millis() / 1000), ESP.getFreeHeap(),
                  ESP.getMinFreeHeap(), sdReady, pmd.loaded || mon.loaded);
    Serial.println("DONE");
  } else if (line == "STATS") {
    Serial.printf("spec=%d nv=%u com=%u fel=%u ene=%u lim=%u desc=%u sd=%d mon=%d bat=%d usb=%d rtc=%u\n",
                  pet.speciesId, pet.level(), pet.fullness, pet.joy, pet.energy,
                  pet.hygiene, pet.careMistakes, sdReady, mon.loaded,
                  batPercent(), usbPresent(), rtcEpoch());
    Serial.printf("peso=%u fue=%u def=%u vel=%u genes=%u/%u/%u tr=%u/%u/%u baya=%d\n",
                  pet.weight, pet.atkStat(), pet.defStat(), pet.speStat(),
                  pet.geneAtk, pet.geneDef, pet.geneSpe,
                  pet.trAtk, pet.trDef, pet.trSpe, pet.berryKnown);
    Serial.printf("shiny=%d streak=%u/%u bond=%u medals=0x%X(%u) nick=%s\n",
                  pet.shiny, pet.streak, pet.bestStreak, pet.bond, pet.medals,
                  pet.totalMedals, pet.nick);
    Serial.printf("steps=%lu/%lu trail=%u shinyLuck=%u/4096 catchBonus=%u\n",
                  (unsigned long)pet.stepsToday, (unsigned long)pet.stepsTotal,
                  pet.stepTrailRank(), pet.stepShinyChancePer4096(), pet.stepCatchBonus());
    Serial.println("DONE");
  } else if (line == "IMU") {
    Serial.printf("imu=%d addr=0x%02X mag=%.2f gyro=%.0f rawPed=%lu softPed=%lu phase=%u hour=%d\n",
                  imuOk(), imuAddr(), imuLastMagG(), imuLastGyroDps(),
                  (unsigned long)imuPedometer(), (unsigned long)imuSoftwarePedometer(),
                  currentDayPhase(), sceneHour());
    Serial.println("DONE");
  } else if (line == "SHAKE") {
    bool okShake = pet.applyShake();
    Serial.printf("shake=%d joy=%u\n", okShake, pet.joy);
    Serial.println("DONE");
  } else if (line.startsWith("WALK ")) {
    uint16_t n = (uint16_t)line.substring(5).toInt();
    uint8_t gained = pet.applyWalk(n, rtcEpoch());
    Serial.printf("walk=%u today=%lu total=%lu joy=%u bond=%u\n", gained,
                  (unsigned long)pet.stepsToday, (unsigned long)pet.stepsTotal,
                  pet.joy, pet.bond);
    Serial.println("DONE");
  } else if (line == "STEPS") {
    Serial.printf("steps today=%lu total=%lu rank=%u shiny=%u/4096 catch+%u usb=%d imu=%d raw=%lu soft=%lu\n",
                  (unsigned long)pet.stepsToday, (unsigned long)pet.stepsTotal,
                  pet.stepTrailRank(), pet.stepShinyChancePer4096(), pet.stepCatchBonus(),
                  usbPresent(), imuOk(), (unsigned long)imuPedometer(),
                  (unsigned long)imuSoftwarePedometer());
    Serial.println("DONE");
  }
}

// ---------- entrada tactil ----------

bool inPetZone(int16_t x, int16_t y) {
  return x > 105 && x < 361 && y > 225 && y < 408;
}

bool inProfilePetPortrait(int16_t x, int16_t y) {
  int dx = x - CX, dy = y - 206;
  return dx * dx + dy * dy <= 76 * 76;
}

// el toque se resuelve al LEVANTAR el dedo para distinguir tap de deslizar
void openClock();  // idle settings, opened by a long press or a right swipe

void handleTouch() {
  static uint32_t lastPoll = 0;
  uint32_t now = millis();
  if (deadlineActive(now, ignoreTouchUntil)) {
    gTouchIrq = false;
    wasPressed = false;
    return;
  }
  if (now - lastPoll < (powerSave && screenOff ? 80UL : 20UL)) return;  // 50 Hz activo; menos si pantalla apagada
  lastPoll = millis();
  // solo tocamos el bus si el chip aviso por INT o si el dedo sigue abajo (hay
  // que detectar el levantamiento). Leer el CST9217 dormido se colgaba ~1s y
  // congelaba el loop entero; SensorLib no respeta el timeout de Wire.
  if (!gTouchIrq && !wasPressed) return;
  gTouchIrq = false;
  int16_t x, y;
  bool pressed = touch.getPoint(&x, &y, 1) > 0;

  // Pantalla apagada por PWR: no despertar por cualquier roce accidental.
  // PWR corto sigue despertando al instante; touch requiere mantener pulsado.
  if (screenOff) {
    if (pressed && !wasPressed) {
      tX0 = tXl = x;
      tY0 = tYl = y;
      tStart = now;
      holdFired = false;
      wasPressed = true;
    } else if (pressed) {
      tXl = x;
      tYl = y;
      if (!holdFired && now - tStart >= 700UL &&
          abs(tXl - tX0) < 42 && abs(tYl - tY0) < 42) {
        screenOff = false;
        dimStage = 0;
        lastInteract = now;
        holdFired = true;
        swallowGesture = true;
        markUiDirty();
        lockTouchBrief(320);
        sfxPlay(SFX_TAP);
      }
    } else {
      wasPressed = false;
      holdFired = false;
    }
    return;
  }

  // saco de entrenamiento: cada toque cuenta al instante (aporrear rapido)
  if (sackOpen) {
    if (pressed && !wasPressed) {
      lastInteract = millis();
      if (y < 72) sackOpen = false;  // tocar arriba = abandonar
      else sackTap();
    }
    wasPressed = pressed;
    return;
  }

  if (pressed && !wasPressed) {  // empieza el gesto
    tX0 = tXl = x;
    tY0 = tYl = y;
    tStart = millis();
    holdFired = false;
    swallowGesture = (dimStage > 0) || screenOff;  // si estaba a oscuras, solo despierta
    screenOff = false;
    lastInteract = millis();
    if (!swallowGesture && gameOpen && gameMode == 0) {
      gameTap(x, y);         // ball: cuenta al tocar, no al soltar
      swallowGesture = true; // evita doble tap al levantar el dedo
    } else if (!swallowGesture && gameOpen && gameMode == 1) {
      catchTap(x, y);        // juego de reflejos: cuenta al tocar, no al soltar
      swallowGesture = true; // evita doble tap al levantar el dedo
    }
  } else if (pressed) {  // sigue apoyado
    tXl = x;
    tYl = y;
    // Long press anywhere on the main screen opens the idle settings.  A
    // 1.2 s hold is deliberate enough not to collide with a tap, but it also
    // survives the small drift a finger has while staying still (the old
    // 30 px limit rejected plenty of honest holds).  It works even on the
    // gesture that wakes a dimmed screen: nothing else was meant by it.
    if (!holdFired && millis() - tStart > 1200 &&
        abs(tXl - tX0) < 60 && abs(tYl - tY0) < 60 &&
        !clockOpen && !galleryOpen && !helpOpen && !kbOpen && !cardOpen) {
      swallowGesture = true;   // do not also treat the release as a tap
      openClock();
      holdFired = true;
    }
  } else if (wasPressed) {  // levanta el dedo: resolver gesto
    lastInteract = millis();
    int dx = tXl - tX0, dy = tYl - tY0;
    uint32_t dt = millis() - tStart;
    if (!holdFired && !swallowGesture) {
      if (abs(dx) > 80 && abs(dy) < 70 && dt < 800) onSwipe(dx > 0 ? 1 : -1);
      else if (abs(dy) > 80 && abs(dx) < 70 && dt < 800) onSwipeV(dy > 0 ? 1 : -1);
      else if (dt < 1500 && abs(dx) < 40 && abs(dy) < 40) onTap(tX0, tY0);
    }
  }
  wasPressed = pressed;
}

// deslizar vertical: abre/cierra la ficha del bicho
void openClock();  // prototipo
int16_t boxDexAt(uint16_t index);
uint8_t boxPageCount();
int sceneHour();
uint8_t currentDayPhase();
StrId dayPhaseTextId(uint8_t phase);
bool mainScreenReadyForShake();
void maybeOfferMorning(uint32_t now);
uint32_t pmdActTotalMs(const PmdAct &a);
void startCleanGame();
void startTypeGame();
void cleanTap(int16_t x, int16_t y);
void typeTap(int16_t x, int16_t y);
void expeditionCardTap(int16_t x, int16_t y);
uint32_t expeditionNowEpoch();
bool expeditionHudVisible();
bool inExpeditionHudHit(int16_t x, int16_t y);
void openExpeditionCard();
void drawExpeditionHud();
bool inStepsHudHit(int16_t x, int16_t y);
void openStepsCard();
void drawStepsHud();

void onSwipeV(int dir) {
  if (helpOpen) { helpOpen = false; clockOpen = true; clockDirty = true; lockTouchBrief(); sfxPlay(SFX_TAP); return; }
  if (clockOpen) { clockOpen = false; markUiDirty(); lockTouchBrief(); sfxPlay(SFX_TAP); return; }
  if (galleryOpen || kbOpen) return;
  if (dir > 0) openClock();  // down: same as a right swipe
}

// BOOT short press.  Unlike onSwipeV(1) this must not open the clock on the
// main screen, and it must never abandon a battle or a minigame -- losing a
// fight to a button that is supposed to mean "back" would be worse than the
// button doing nothing.  So it only unwinds the overlays that are pure UI.
void physicalBack() {
  if (kbOpen) {                    // cancel rename, keep the old nickname
    kbOpen = false;
    cardDirty = true;
    lockTouchBrief();
    sfxPlay(SFX_TAP);
    return;
  }
  if (helpOpen) {                  // help was opened from the clock screen
    helpOpen = false;
    clockOpen = true;
    clockDirty = true;
    lockTouchBrief();
    sfxPlay(SFX_TAP);
    return;
  }
  if (clockOpen) {
    clockOpen = false;
    markUiDirty();
    lockTouchBrief();
    sfxPlay(SFX_TAP);
    return;
  }
  if (galleryOpen) {
    if (galleryDetail) {
      galleryDetail = 0;
      galleryPmd.unload();
    } else {
      galleryOpen = false;
      galleryPmd.unload();
    }
    galleryDirty = true;
    lockTouchBrief();
    sfxPlay(SFX_TAP);
    return;
  }
  if (gameMenuOpen) {
    gameMenuOpen = false;
    markUiDirty();
    lockTouchBrief();
    sfxPlay(SFX_TAP);
    return;
  }
  if (cardOpen) {
    cardOpen = false;
    expeditionTrainChoiceOpen = false;
    markUiDirty();
    lockTouchBrief();
    sfxPlay(SFX_TAP);
    return;
  }
  // battleOpen, gameOpen, sackOpen, ceremonies and the main screen: no-op.
}

// deslizar: dir +1 = hacia la derecha.  Left opens the dex, right the idle
// settings; inside the dex the same gestures page and exit.
void onSwipe(int dir) {
  if (helpOpen) {
    if (dir < 0 && helpPage + 1 < HELP_PAGE_COUNT) { helpPage++; helpDirty = true; }
    else if (dir > 0 && helpPage > 0) { helpPage--; helpDirty = true; }
    sfxPlay(SFX_MENU);
    return;
  }
  if (clockOpen) return;
  if (galleryOpen) {
    if (galleryDetail) {  // detail: back to the grid
      galleryDetail = 0;
      galleryPmd.unload();
      galleryDirty = true;
      lockTouchBrief();
      return;
    }
    int np = galleryPage - dir;
    int maxPage = galleryPageCount() - 1;
    if (np < 0) {  // right swipe from the first page exits
      galleryOpen = false;
      galleryPmd.unload();
      markUiDirty();
      lockTouchBrief();
      sfxPlay(SFX_TAP);
      return;
    }
    if (np > maxPage) np = maxPage;
    if (np != galleryPage) {
      galleryPage = np;
      galleryDirty = true;
      sfxPlay(SFX_MENU);
    }
    return;
  }
  if (pet.ceremony) return;
  if (dir > 0) {  // right: settings
    openClock();
    return;
  }
  // left: pokedex
  galleryOpen = true;
  galleryPage = 0;
  galleryDetail = 0;
  galleryDirty = true;
  lockTouchBrief();
  sfxPlay(SFX_MENU);
}

struct GameMenuTile {
  int16_t x, y, w, h;
  uint8_t game;
};

static const GameMenuTile GAME_MENU_TILES[5] = {
  { 88, 156, 138, 58, 0 },
  { 240, 156, 138, 58, 1 },
  { 88, 226, 138, 58, 2 },
  { 240, 226, 138, 58, 3 },
  { 94, 296, 278, 62, 4 },
};

int8_t gameMenuHit(int16_t x, int16_t y) {
  for (uint8_t i = 0; i < 5; i++) {
    const GameMenuTile &t = GAME_MENU_TILES[i];
    if (x >= t.x && x <= t.x + t.w && y >= t.y && y <= t.y + t.h) return i;
  }
  return -1;
}

void startGameMenuChoice(uint8_t idx) {
  switch (GAME_MENU_TILES[idx].game) {
    case 0: startGame(); break;
    case 1: startCatchGame(); break;
    case 2: startMemoGame(); break;
    case 3: startCleanGame(); break;
    case 4: startTypeGame(); break;
  }
}

void onTap(int16_t x, int16_t y) {
  // Serial.printf("TOUCH %d %d\n", x, y);  // diagnostico (silenciado: satura el log)
  if (pet.awaitingStarter()) {  // primera partida: region y despues inicial
    if (starterRegion == 0xFF) {
      for (uint8_t i = 0; i < 3; i++) {
        int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
        if (x >= 70 && x <= 396 && y >= ry && y <= ry + STARTER_ROW_H) {
          starterRegion = i;
          starterDirty = true;
          lockTouchBrief();
          sfxPlay(SFX_TAP);
          break;
        }
      }
      return;
    }
    for (int i = 0; i < 3; i++) {
      int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
      if (x >= 70 && x <= 396 && y >= ry && y <= ry + STARTER_ROW_H) {
        pet.chooseStarter(STARTER_DEX[starterRegion][i]);
        starterRegion = 0xFF;
        markUiDirty();
        lockTouchBrief();
        sfxPlay(SFX_TAP);
        break;
      }
    }
    return;
  }
  if (galleryOpen) {
    galleryTap(x, y);
    return;
  }
  if (kbOpen) {
    keyboardTap(x, y);
    return;
  }
  if (helpOpen) {
    helpTap(x, y);
    return;
  }
  if (clockOpen) {
    clockTap(x, y);
    return;
  }
  // Idle main screen: only the egg and the creature react to a tap.  Feeding,
  // minigames, battles and the old care buttons are gone on purpose.
  if (pet.isEgg()) {
    pet.eggTap();
    sfxPlay(SFX_TAP);
    return;
  }
  if (inPetZone(x, y)) {
    uint8_t r = pet.interactPet(currentDayPhase() == 2);
    if (audioMode() == SOUND_FULL) speciesChirpPlay(pet.speciesId, true);
    if (r) {
      snprintf(idleToast, sizeof(idleToast), "%s", T((r & PET_INTERACT_BOND) ? S_BOND_GAIN : S_HAPPY_FB));
      idleToastUntil = millis() + 1600;
      sfxPlay((r & PET_INTERACT_BOND) ? SFX_HEART : SFX_TAP);
    }
  }
  return;
  if (pet.ceremony) return;  // durante la despedida no hay botones
  if (cardOpen) {
    if (cardPage == 0 && inProfilePetPortrait(x, y)) {
      speciesChirpPlay(pet.speciesId, true);
    }
    else if (cardPage == 0 && y < 84) openKeyboard();  // tocar el nombre = renombrar
    else if (cardPage == 0 && y >= 366 && y <= 398) {
      uint8_t count = pet.unlockedCollectionFrameCount();
      uint8_t next = pet.collectionFrame;
      if (x >= 62 && x <= 146) next = pet.collectionFrame == 0 ? count - 1 : pet.collectionFrame - 1;
      else if (x >= 320 && x <= 404) next = (uint8_t)((pet.collectionFrame + 1) % count);
      else return;
      if (pet.setCollectionFrame(next)) {
        cardDirty = true;
        sfxPlay(SFX_MENU);
        lockTouchBrief();
      }
    }
    else if (cardPage == 3) {
      if (x >= 302 && x <= 408 && y >= 62 && y <= 92) {
        boxSort = (boxSort + 1) % 3;
        boxPage = 0;
        cardDirty = true;
        sfxPlay(SFX_TAP);
      } else if (x >= 76 && x <= 170 && y >= 348 && y <= 386) {
        if (boxPage > 0) { boxPage--; cardDirty = true; }
        sfxPlay(SFX_TAP);
      } else if (x >= 296 && x <= 390 && y >= 348 && y <= 386) {
        uint8_t pages = boxPageCount();
        if (boxPage + 1 < pages) { boxPage++; cardDirty = true; }
        sfxPlay(SFX_TAP);
      } else {
        int row = (y - 122) / 42;
        if (row >= 0 && row < 5 && x >= 58 && x <= 408 && y >= 122 + row * 42 && y <= 156 + row * 42) {
          int16_t dex = boxDexAt((uint16_t)boxPage * 5 + row);
          if (dex > 0) {
            cardOpen = false;
            galleryOpen = true;
            galleryDetail = dex;
            galleryDirty = true;
            lockTouchBrief();
            galleryPmd.load(dex, pet.isShinyRegistered(dex));
            sfxPlay(SFX_TAP);
            speciesChirpPlay(dex);
          }
        } else if (y >= 400) {
          cardOpen = false;
          markUiDirty();
          lockTouchBrief();
        }
      }
    } else if (cardPage == 4 && y >= 286 && y <= 336 && x >= 82 && x <= 384) {
      cardOpen = false;
      markUiDirty();
      lockTouchBrief();
      startBattle();
    } else if (cardPage == 4 && y >= 330 && y <= 388 && x >= 82 && x <= 384) {
      cardOpen = false;            // boton ENTRENAR FUERZA
      markUiDirty();
      lockTouchBrief();
      startSack();
    } else if (cardPage == 7) {
      expeditionCardTap(x, y);
    } else if (y >= 400) {
      cardOpen = false;
      markUiDirty();
      lockTouchBrief();
      sfxPlay(SFX_TAP);
    }
    return;
  }
  if (gameOpen) {
    gameTap(x, y);
    return;
  }
  if (battleOpen) {
    battleTap(x, y);
    return;
  }
  if (petEventUntil && deadlineActive(millis(), petEventUntil) && inPetEventHit(x, y)) {
    acceptPetEvent();
    return;
  }
  if (gameMenuOpen) {
    int8_t hit = gameMenuHit(x, y);
    if (hit >= 0) { lockTouchBrief(140); startGameMenuChoice((uint8_t)hit); }
    else { gameMenuOpen = false; markUiDirty(); lockTouchBrief(); sfxPlay(SFX_TAP); }
    return;
  }
  if (wildPromptUntil) {
    if (deadlineActive(millis(), wildPromptUntil)) {
      bool fight = (x >= 93 && x <= 373 && y >= 226 && y <= 270);
      bool later = (x >= 93 && x <= 373 && y >= 278 && y <= 322);
      if (fight) {
        startBattleWith(wildPromptDex, wildPromptLevel, wildPromptShiny ? 1 : 0);
      } else if (later) {
        wildPromptUntil = 0;
        scheduleNextWild(millis());
        sfxPlay(SFX_TAP);
      }
    } else {
      wildPromptUntil = 0;
    }
    return;
  }
  if (inExpeditionHudHit(x, y)) {
    openExpeditionCard();
    return;
  }
  if (inStepsHudHit(x, y)) {
    openStepsCard();
    return;
  }
  if (choiceKind) {          // dialogo de decision o selector de objetivo evolutivo
    if (choiceKind == 3) {
      uint8_t options = pet.evolutionOptionCount();
      for (uint8_t i = 0; i < options; i++) {
        int y = 158 + i * 45;
        if (x >= 82 && x <= 394 && y <= 194 + i * 45 && y >= 158 + i * 45) {
          int16_t target = pet.evolutionOption(i);
          int16_t old = pet.speciesId;
          pet.evolveTo(target);
          evoPmd.load(old, pet.shiny);
          choiceKind = 0;
          return;
        }
      }
      if (y >= 392 && y <= 444) choiceKind = 0;
      return;
    }
    bool b1 = (x >= 93 && x <= 373 && y >= 206 && y <= 258);  // accion
    bool b2 = (x >= 93 && x <= 373 && y >= 268 && y <= 320);  // mantener / quedaros
    if (choiceKind == 1) {                 // evolucion
      if (b1) {
        if (pet.evolutionOptionCount() > 1) {
          choiceKind = 3;
          choiceUntil = millis() + 12000;
          return;
        }
        int16_t old = pet.speciesId;
        pet.evolveTo(pet.evolutionOption(0));
        evoPmd.load(old, pet.shiny);
      }
      else if (b2) pet.declineEvolve();
    } else if (choiceKind == 2) {          // despedida
      if (b1) pet.startFarewell();
      else if (b2) pet.declineFarewell();
    }
    choiceKind = 0;
    return;
  }
  if (confirmUntil) {        // dialogo "soltar?": SI / NO
    if (deadlineActive(millis(), confirmUntil) && x >= 118 && x <= 218 && y >= 252 && y <= 304) {
      pet.release();
    }
    confirmUntil = 0;
    return;
  }
  if (feedMenuUntil) {       // selector de comida
    if (deadlineActive(millis(), feedMenuUntil) && y >= 288 && y <= 352 && x >= 101 && x <= 365) {
      int item = (x - 101) / 66;
      if (item == 3) pet.feedCandy();
      else pet.feedBerry(item);
      sfxPlay(SFX_EAT);
    }
    feedMenuUntil = 0;
    return;
  }
  if (pet.isEgg()) {
    pet.eggTap();
    sfxPlay(SFX_TAP);
    return;
  }
  // boton de evolucion: abre el dialogo evolucionar/mantener
  if (pet.wantEvolveButton() && x >= EVO_BTN_X && x <= EVO_BTN_X + EVO_BTN_W &&
      y >= EVO_BTN_Y && y <= EVO_BTN_Y + EVO_BTN_H) {
    if (pet.canEvolveNow()) {
      choiceKind = 1; choiceUntil = millis() + 12000;
    } else {
      StrId blocked = pet.evolutionNeedsMoreBond() ? S_EVO_NEED_BOND : S_EVO_BLOCKED;
      snprintf(petEventMsg, sizeof(petEventMsg), "%s", T(blocked));
      petEventFeedbackUntil = millis() + 1800;
      sfxPlay(SFX_DENY);
    }
    return;
  }
  // botones de final (mismo recuadro): escapada directa; despedida abre dialogo
  if (x >= FAR_BTN_X && x <= FAR_BTN_X + FAR_BTN_W &&
      y >= FAR_BTN_Y && y <= FAR_BTN_Y + FAR_BTN_H) {
    if (pet.canRunawayNow()) { pet.startRunaway(); return; }
    if (pet.wantFarewellButton()) { choiceKind = 2; choiceUntil = millis() + 12000; return; }
  }
  for (int i = 0; i < 4; i++) {
    int dx = x - buttons[i].cx, dy = y - buttons[i].cy;
    if (dx * dx + dy * dy <= BTN_HIT * BTN_HIT) {
      Serial.printf("BTN %d\n", i);
      sfxPlay(SFX_TAP);
      if (i == 0) {
        if (!pet.sleeping) {
          feedMenuUntil = millis() + 6000;
          sfxPlay(SFX_MENU);
        }
      } else if (i == 1) {
        if (!pet.sleeping) {
          gameMenuOpen = true;
          gameMenuDirty = true;
          lockTouchBrief();
          sfxPlay(SFX_MENU);
        }
      } else if (i == 2) {
        pet.toggleLight();
      } else {
        startBath();
      }
      return;
    }
  }
  // tocar al bicho = caricia
  if (inPetZone(x, y)) {
    Serial.println("PET");
    uint8_t r = pet.interactPet(currentDayPhase() == 2);
    // Der Spezies-Chirp ist direkte Rueckmeldung fuer jeden Pet-Tap. Er darf
    // nicht von der zehnminuetigen Pflege-Belohnung abhaengen.
    if (!pet.sleeping && audioMode() == SOUND_FULL) speciesChirpPlay(pet.speciesId, true);
    if (r == PET_INTERACT_NONE) return;
    StrId msg = S_WAIT;
    if (r & PET_INTERACT_BOND) msg = S_BOND_GAIN;
    else if (r & PET_INTERACT_JOY) msg = S_HAPPY_FB;
    snprintf(petEventMsg, sizeof(petEventMsg), "%s", T(msg));
    petEventFeedbackUntil = millis() + 1600;
    if (!pet.sleeping && audioMode() != SOUND_FULL)
      sfxPlay((r & PET_INTERACT_BOND) ? SFX_HEART : SFX_TAP);
  }
}

// ---------- render ----------

bool gNight = false;  // noche real (por hora) o durmiendo: lo fija render()
uint16_t inkColor() { return gNight ? UI_INK_NIGHT : UI_INK; }

// ---------- escena de fondo: bioma del tipo + hora real del RTC ----------

#define C565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define HORIZON 232  // linea donde el cielo se encuentra con el suelo

uint16_t lerp565(uint16_t a, uint16_t b, int i, int n) {
  if (n <= 0) return a;
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)((((ar + (br - ar) * i / n) << 11)) |
                    (((ag + (bg - ag) * i / n) << 5)) | (ab + (bb - ab) * i / n));
}

static uint8_t watermarkSlot() {
  return (uint8_t)((millis() / 20000UL) & 3U);
}

bool watermarkNeedsRedraw() { return false; }

static void drawWatermark(uint8_t) {}

void flushStampedFrame() { gfx->flush(); }

int sceneHour() {
  return sceneHourFromEpoch(pet.lastSeenEpoch);
}

uint8_t currentDayPhase() {
  return dayPhaseFromHour(sceneHour());
}

StrId dayPhaseTextId(uint8_t phase) {
  switch (phase) {
    case 0: return S_MORNING;
    case 2: return S_EVENING;
    case 3: return S_NIGHT;
    default: return S_DAY;
  }
}

// suelo de cada bioma de dia (de noche se mezcla hacia el azul nocturno)
static const uint16_t BIOME_SOIL[6] = {
  C565(0x7e, 0xc0, 0x7f),  // 0 pradera
  C565(0xdc, 0xca, 0x94),  // 1 playa (arena)
  C565(0x4f, 0x8a, 0x55),  // 2 bosque
  C565(0x8a, 0x55, 0x44),  // 3 volcan
  C565(0xa8, 0x90, 0x6a),  // 4 montana
  C565(0xe6, 0xee, 0xf5),  // 5 nieve
};

void drawClouds(uint32_t now, uint16_t col) {
  for (int k = 0; k < 2; k++) {
    int cx = (int)((now / 50 + k * 250) % 560) - 40;
    int cy = 70 + k * 34;
    gfx->fillCircle(cx, cy, 16, col);
    gfx->fillCircle(cx + 18, cy + 3, 13, col);
    gfx->fillCircle(cx - 15, cy + 4, 12, col);
  }
}

void drawScene(uint8_t biome, uint32_t now, bool night) {
  int h = sceneHour();
  uint16_t top, bot;
  if (night)            { top = C565(0x0c, 0x12, 0x24); bot = C565(0x1e, 0x26, 0x46); }
  else if (h < 8)       { top = C565(0xd1, 0x6a, 0x86); bot = C565(0xf3, 0xb8, 0x7c); }  // amanecer
  else if (h < 18)      { top = C565(0x8f, 0xc8, 0xea); bot = C565(0xdc, 0xee, 0xe6); }  // dia
  else                  { top = C565(0xc7, 0x5a, 0x4a); bot = C565(0xf0, 0xae, 0x64); }  // atardecer

  // cielo en bandas
  for (int y = 0; y < HORIZON; y += 8)
    gfx->fillRect(0, y, 466, 8, lerp565(top, bot, y, HORIZON));

  // sol o luna (dentro del circulo y a la derecha: la fecha ocupa el centro)
  if (night) {
    gfx->fillCircle(370, 70, 18, C565(0xe8, 0xee, 0xf5));
    gfx->fillCircle(378, 64, 16, lerp565(top, bot, 78, HORIZON));  // creciente
    for (auto &st : STARS) gfx->fillRect(st[0], st[1], 4, 4, UI_WHITE);
  } else if (h < 18) {
    gfx->fillCircle(372, 74, 18, h < 8 ? C565(0xff, 0xd9, 0x8a) : C565(0xff, 0xe7, 0x9f));
    drawClouds(now, C565(0xff, 0xff, 0xff));
  } else {
    gfx->fillCircle(233, HORIZON - 6, 34, C565(0xff, 0xf1, 0xc8));  // sol poniente
  }

  // mar de la playa: una franja de agua sobre la arena
  uint16_t soil = BIOME_SOIL[biome < 6 ? biome : 0];
  if (night) soil = lerp565(soil, C565(0x16, 0x1c, 0x30), 9, 16);
  if (biome == 1) {
    uint16_t sea = night ? C565(0x1c, 0x34, 0x52) : C565(0x4f, 0x96, 0xc4);
    gfx->fillRect(0, HORIZON - 26, 466, 26, sea);
    for (int i = 0; i < 3; i++) {
      int wy = HORIZON - 22 + i * 7;
      uint16_t fc = night ? C565(0x3a, 0x58, 0x78) : C565(0xbf, 0xe6, 0xf5);
      gfx->fillRect(60 + ((now / 60 + i * 30) % 60), wy, 26, 2, fc);
      gfx->fillRect(300 - ((now / 60 + i * 20) % 60), wy, 26, 2, fc);
    }
  }

  // suelo
  gfx->fillRect(0, HORIZON, 466, 466 - HORIZON, soil);
  uint16_t hill = lerp565(soil, night ? C565(0x0c, 0x12, 0x24) : C565(0xff, 0xff, 0xff), 3, 16);
  gfx->fillRoundRect(-60, HORIZON - 14, 586, 60, 30, hill);

  // detalles del bioma
  uint16_t dk = lerp565(soil, C565(0x10, 0x18, 0x20), night ? 11 : 7, 16);
  if (biome == 2) {  // bosque: coniferas en silueta
    for (int tx : { 60, 150, 360, 416 }) {
      gfx->fillTriangle(tx, HORIZON - 46, tx - 16, HORIZON, tx + 16, HORIZON, dk);
      gfx->fillTriangle(tx, HORIZON - 60, tx - 12, HORIZON - 28, tx + 12, HORIZON - 28, dk);
    }
  } else if (biome == 3) {  // volcan: rocas y brasas
    gfx->fillTriangle(70, HORIZON, 40, HORIZON + 30, 100, HORIZON + 30, dk);
    gfx->fillTriangle(400, HORIZON + 4, 372, HORIZON + 30, 430, HORIZON + 30, dk);
    if (!night)
      for (int e = 0; e < 4; e++)
        gfx->fillRect(120 + e * 70, HORIZON + 8 + (e % 2) * 6, 4, 4, C565(0xff, 0x9b, 0x3a));
  } else if (biome == 4) {  // montana: cumbres al fondo
    gfx->fillTriangle(140, HORIZON - 50, 60, HORIZON, 220, HORIZON, dk);
    gfx->fillTriangle(330, HORIZON - 38, 250, HORIZON, 410, HORIZON, dk);
  } else if (biome == 5 && !night) {  // nieve: copos cayendo
    for (int f = 0; f < 10; f++) {
      int fx = (f * 53 + now / 40) % 466;
      int fy = (f * 90 + now / 18) % HORIZON;
      gfx->fillRect(fx, fy, 3, 3, UI_WHITE);
    }
  } else if (biome == 0) {  // pradera: matas de hierba
    for (int gx : { 80, 175, 300, 395 })
      for (int b = -1; b <= 1; b++)
        gfx->fillRect(gx + b * 5, HORIZON + 6, 2, 8 + (b == 0 ? 4 : 0), dk);
  }
}

// primera partida: elige inicial entre Bulbasaur / Charmander / Squirtle
void renderStarterRegionSelect() {
  if (!starterDirty) { perfRenderSkipCount++; return; }
  starterDirty = false;
  gfx->fillScreen(UI_BG_DAY);
  const char *t = T(S_CHOOSE_REGION);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(CX - textW(t, 2) / 2, 68);
  printT(t);
  for (uint8_t i = 0; i < 3; i++) {
    int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
    gfx->fillRoundRect(70, ry, 326, STARTER_ROW_H, 14, lerp565(UI_BAR_OK, UI_WHITE, 6, 8));
    gfx->drawRoundRect(70, ry, 326, STARTER_ROW_H, 14, UI_BAR_OK);
    gfx->setTextColor(UI_INK);
    setSize(3);
    const char *label = T(STARTER_REGION_LABELS[i]);
    setCur(CX - textW(label, 3) / 2, ry + 24);
    printT(label);
  }
  flushStampedFrame();
}

void renderStarterSelect() {
  if (!starterDirty) { perfRenderSkipCount++; return; }
  starterDirty = false;
  gfx->fillScreen(UI_BG_DAY);
  const char *t = T(S_CHOOSE_STARTER);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(CX - textW(t, 2) / 2, 68);
  printT(t);
  for (int i = 0; i < 3; i++) {
    int16_t d = STARTER_DEX[starterRegion][i];
    const DexEntry &de = DEX_TBL[d];
    int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
    gfx->fillRoundRect(70, ry, 326, STARTER_ROW_H, 14, lerp565(de.accent, UI_WHITE, 6, 8));
    gfx->drawRoundRect(70, ry, 326, STARTER_ROW_H, 14, de.accent);
    const uint8_t *th = thumbs.get(d);     // miniatura del inicial (si la SD esta lista)
    if (th) drawThumb(th, 76, ry - 5, 3, false);
    gfx->setTextColor(UI_INK);
    setSize(3);
    setCur(178, ry + 24);
    printT(dexName(d));
  }
  flushStampedFrame();
}

void render() {
  if (watermarkNeedsRedraw()) {
    starterDirty = true;
    cardDirty = true;
    clockDirty = true;
    helpDirty = true;
    keyboardDirty = true;
    gameMenuDirty = true;
    battleDirty = true;
    galleryDirty = true;
  }
  if (staticScreenClean()) { perfRenderSkipCount++; return; }
  if (pet.awaitingStarter()) {  // primera partida: region y starter (prioridad total)
    if (starterRegion == 0xFF) renderStarterRegionSelect();
    else renderStarterSelect();
    return;
  }
  if (galleryOpen) {
    renderGallery();
    return;
  }
  if (gameOpen) {
    renderGame();
    return;
  }
  if (sackOpen) {
    renderSack();
    return;
  }
  if (battleOpen) {
    renderBattle();
    return;
  }
  if (kbOpen) {
    renderKeyboard();
    return;
  }
  if (helpOpen) {
    renderHelp();
    return;
  }
  if (clockOpen) {
    renderClock();
    return;
  }
  drawIdleMain();
  return;
  if (cardOpen) {
    renderCard();
    return;
  }
  int h = sceneHour();
  gNight = isVisualNight(h, pet.sleeping);
  // drawScene cubre los 466x466 completos: sin fillScreen(NEGRO) previo para
  // que un flush DMA solapado nunca capture negro a medias (anti-parpadeo)
  drawScene(pet.isEgg() ? 0 : DEX_TBL[pet.speciesId].biome, millis(), gNight);

  if (pet.ceremony) {
    const DexEntry &d = DEX_TBL[pet.speciesId];
    const char *msg = (pet.ceremony == CER_FAREWELL) ? T(S_FAREWELL)
                      : (pet.ceremony == CER_RUNAWAY) ? T(S_RUNAWAY)
                                                      : T(S_GOODBYE);
    drawHeader(dexName(pet.speciesId), d.accent, msg);
    drawCeremony();
    flushStampedFrame();
    return;
  }

  if (pet.isEgg()) {
    drawHeader(T(S_EGG_HDR), inkColor(), eggMsg());
    int s = 5, x = CX - 16 * s, y = PET_CY - 16 * s;
    if (deadlineActive(millis(), eggWiggleUntil))
      x += ((millis() / 40) % 2) ? 6 : -6;
    drawMap(SPR_EGG, SPRITE_H, x, y, s, false);
    if (pet.eggCracks() >= 1)
      for (auto &c : CRACK1) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
    if (pet.eggCracks() >= 2)
      for (auto &c : CRACK2) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
    if (pet.eggRarity() >= R_RARO) {
      const char *rar = (pet.eggRarity() == R_LEGENDARIO) ? T(S_EGG_LEGEND) : T(S_EGG_RARE);
      gfx->setTextColor(pet.eggRarity() == R_LEGENDARIO ? UI_BAR_WARN : 0x4C98);
      setSize(2);
      setCur(CX - textW(rar, 2) / 2, 316);
      printT(rar);
    }
    char reg[24];
    snprintf(reg, sizeof(reg), T(S_POKEDEX_FMT), pet.registeredCount(), DEX_COUNT);
    gfx->fillRect(0, 312, 466, 154, gNight ? UI_BG_NIGHT : UI_BG_DAY);
    gfx->setTextColor(inkColor());
    setSize(2);
    setCur(CX - textW(reg, 2) / 2, 348);
    printT(reg);
    drawStepsHud();
  } else {
    const DexEntry &d = DEX_TBL[pet.speciesId];
    char name[28];
    const char *base = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
    snprintf(name, sizeof(name), T(S_NAME_FMT), pet.shiny ? "*" : "", base, pet.level());
    drawHeader(name, gNight ? UI_INK_NIGHT : d.accent, nullptr);
    drawMainClock();
    // Status moved down: the clock now owns the old status slot, and the text
    // sits just above the pet without touching the sprite.
    const char *status = statusMsg();
    gfx->setTextColor(inkColor());
    setSize(2);
    setCur(CX - textW(status, 2) / 2, 116);
    printT(status);
    drawStreakBadge();
    drawStepsHud();
    drawPet();
    drawBath();
    drawPoops();
    drawExpeditionHud();
    // panel inferior: base limpia para barras y botones sobre el paisaje
    gfx->fillRect(0, 312, 466, 154, gNight ? UI_BG_NIGHT : UI_BG_DAY);
    drawBars();
    drawButtons();
    drawCelebration();
    drawPetEvent();
    if (gameMenuOpen) drawGameMenu();
    if (pet.wantEvolveButton()) drawEvolveButton();        // CTA rojo: evolucionar
    else if (pet.canRunawayNow()) drawRunawayButton();     // CTA sombrio: escapada (abandono)
    else if (pet.wantFarewellButton()) drawFarewellButton();  // CTA dorado: despedida
  }

  if (pet.sleeping) {
    gfx->setTextColor(UI_INK_NIGHT);
    setSize(3);
    setCur(320, 130);
    printT("Zz");
  }

  if (morningUntil) {
    if (deadlineReached(millis(), morningUntil)) morningUntil = 0;
    else {
      const char *hello = T(S_GOOD_MORNING);
      gfx->fillRoundRect(90, 78, 286, 44, 14, UI_WHITE);
      gfx->drawRoundRect(90, 78, 286, 44, 14, inkColor());
      gfx->setTextColor(inkColor());
      setSize(2);
      setCur(CX - textW(hello, 2) / 2, 92);
      printT(hello);
    }
  }

  if (wildPromptUntil) {
    if (deadlineReached(millis(), wildPromptUntil)) wildPromptUntil = 0;
    else drawWildPrompt();
  }

  // selector de comida
  if (feedMenuUntil) {
    if (deadlineReached(millis(), feedMenuUntil)) {
      feedMenuUntil = 0;
    } else {
      gfx->fillRoundRect(101, 288, 264, 64, 14, UI_WHITE);
      gfx->drawRoundRect(101, 288, 264, 64, 14, inkColor());
      drawMap(SPR_ICON_FOOD, 16, 110, 296, 3, false);
      drawMap(SPR_ICON_BERRY_B, 16, 176, 296, 3, false);
      drawMap(SPR_ICON_BERRY_G, 16, 242, 296, 3, false);
      drawMap(SPR_ICON_CANDY, 16, 308, 296, 3, false);
    }
  }

  // dialogo "soltar?" (pulsacion larga sobre el bicho)
  if (confirmUntil) {
    if (deadlineReached(millis(), confirmUntil)) {
      confirmUntil = 0;
    } else {
      gfx->fillRoundRect(94, 168, 278, 152, 16, UI_WHITE);
      gfx->drawRoundRect(94, 168, 278, 152, 16, UI_INK);
      char q[40];
      snprintf(q, sizeof(q), T(S_RELEASE_FMT), dexName(pet.speciesId));
      gfx->setTextColor(UI_INK);
      setSize(2);
      setCur(CX - textW(q, 2) / 2, 196);
      printT(q);
      gfx->fillRoundRect(118, 252, 100, 52, 12, UI_BAR_OK);
      gfx->setTextColor(UI_WHITE);
      setCur(118 + (100 - textW(T(S_YES), 2)) / 2, 270);
      printT(T(S_YES));
      gfx->fillRoundRect(248, 252, 100, 52, 12, UI_BAR_BAD);
      setCur(248 + (100 - textW(T(S_NO), 2)) / 2, 270);
      printT(T(S_NO));
    }
  }

  // dialogo de decision (evolucionar/mantener, despedirse/quedaros)
  if (choiceKind) {
    if (deadlineReached(millis(), choiceUntil)) choiceKind = 0;
    else drawChoiceDialog();
  }

  flushStampedFrame();
}

// ---------- minijuego: toques con la pokeball ----------

void drawGameMenu() {
  gameMenuDirty = false;
  gfx->fillRoundRect(78, 112, 310, 266, 18, UI_WHITE);
  gfx->drawRoundRect(78, 112, 310, 266, 18, UI_INK);
  gfx->setTextColor(UI_INK);
  setSize(3);
  const char *title = T(S_PLAY);
  setCur(CX - textW(title, 3) / 2, 124);
  printT(title);
  const char *labels[5] = { T(S_GAME_BALL), T(S_GAME_CATCH), T(S_GAME_MEMO), T(S_GAME_CLEAN), T(S_GAME_TYPE) };
  uint16_t cols[5] = { UI_BAR_BAD, UI_BAR_WARN, 0x4C98, UI_BAR_OK, 0xF3B7 };
  for (int i = 0; i < 5; i++) {
    const GameMenuTile &t = GAME_MENU_TILES[i];
    gfx->fillRoundRect(t.x, t.y, t.w, t.h, 14, cols[i]);
    gfx->drawRoundRect(t.x, t.y, t.w, t.h, 14, UI_INK);
    gfx->setTextColor(i == 1 ? UI_INK : UI_BG_DAY);
    setSize(2);
    setCur(t.x + (t.w - textW(labels[i], 2)) / 2, t.y + (t.h - 16) / 2);
    printT(labels[i]);
  }
}

void startGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameMenuOpen = false;
  gameOpen = true;
  gameMode = 0;
  gameOverUntil = 0;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  gameGain = 0;
  hitTime = 0;
  ballLastHitAt = 0;
  gamePetX = 233;
  respawnBall();
  sfxPlay(SFX_GAME_START);
}

void spawnCatchTarget() {
  catchX = 86 + random(294);
  catchY = 118 + random(206);
  catchIcon = random(3);
  uint32_t life = 980;
  uint32_t speedup = (uint32_t)gameScore * 35;
  if (speedup > 530) speedup = 530;
  life -= speedup;
  catchTargetUntil = millis() + life;
}

void startCatchGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameMenuOpen = false;
  gameOpen = true;
  gameMode = 1;
  gameOverUntil = 0;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  gameGain = 0;
  catchUntil = millis() + 20000;
  spawnCatchTarget();
  sfxPlay(SFX_GAME_START);
}

void startMemoRound() {
  if (memoLen < 14) memoSeq[memoLen++] = random(4);
  memoShow = 0;
  memoInput = 0;
  memoActivePad = -1;
  memoHintPad = -1;
  memoShowing = true;
  memoNextAt = millis() + 350;
  memoTurnUntil = 0;
}

void startMemoGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameMenuOpen = false;
  gameOpen = true;
  gameMode = 2;
  gameOverUntil = 0;
  gameScore = 0;
  gameNewHi = false;
  gameGain = 0;
  memoLen = 0;
  memoRounds = 0;
  memoFlashPad = -1;
  memoHintPad = -1;
  memoFlashUntil = memoFailUntil = memoTurnUntil = 0;
  startMemoRound();
  sfxPlay(SFX_GAME_START);
}

void spawnCleanSpot() {
  for (uint8_t i = 0; i < 4; i++) {
    if (cleanAlive[i]) continue;
    cleanX[i] = 88 + random(290);
    cleanY[i] = 122 + random(224);
    cleanAlive[i] = true;
    cleanActive++;
    return;
  }
}

void startCleanGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameMenuOpen = false;
  gameOpen = true;
  gameMode = 3;
  gameOverUntil = 0;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  gameGain = 0;
  cleanActive = 0;
  for (uint8_t i = 0; i < 4; i++) cleanAlive[i] = false;
  cleanUntil = millis() + 18000;
  cleanSpawnAt = millis();
  spawnCleanSpot();
  sfxPlay(SFX_GAME_START);
}

static const uint8_t TYPE_DEF_POOL[] = {
  TYPE_GRASS, TYPE_FIRE, TYPE_WATER, TYPE_ELECTRIC, TYPE_ROCK, TYPE_GROUND,
  TYPE_FLYING, TYPE_POISON, TYPE_PSYCHIC, TYPE_GHOST, TYPE_DRAGON, TYPE_ICE
};
static const uint8_t TYPE_COUNTER_POOL[] = {
  TYPE_FIRE, TYPE_WATER, TYPE_GRASS, TYPE_GROUND, TYPE_WATER, TYPE_WATER,
  TYPE_ELECTRIC, TYPE_PSYCHIC, TYPE_BUG, TYPE_GHOST, TYPE_ICE, TYPE_FIRE
};
static const uint8_t TYPE_OPTION_POOL[] = {
  TYPE_NORMAL, TYPE_FIRE, TYPE_WATER, TYPE_ELECTRIC, TYPE_GRASS, TYPE_ICE,
  TYPE_FIGHTING, TYPE_POISON, TYPE_GROUND, TYPE_FLYING, TYPE_PSYCHIC, TYPE_BUG,
  TYPE_ROCK, TYPE_GHOST, TYPE_DRAGON
};

void nextTypeQuestion() {
  uint8_t q = (uint8_t)random(sizeof(TYPE_DEF_POOL));
  typeEnemy = TYPE_DEF_POOL[q];
  uint8_t correct = TYPE_COUNTER_POOL[q];
  typeCorrect = (uint8_t)random(3);
  for (uint8_t i = 0; i < 3; i++) typeChoice[i] = TYPE_NONE;
  typeChoice[typeCorrect] = correct;
  for (uint8_t i = 0; i < 3; i++) {
    if (i == typeCorrect) continue;
    uint8_t cand;
    do {
      cand = TYPE_OPTION_POOL[random(sizeof(TYPE_OPTION_POOL))];
    } while (cand == correct || cand == typeChoice[0] || cand == typeChoice[1] || cand == typeChoice[2] ||
             battleTypeEffectPct(cand, typeEnemy, TYPE_NONE) > 100);
    typeChoice[i] = cand;
  }
  typeUntil = millis() + 4200;
}

void startTypeGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameMenuOpen = false;
  gameOpen = true;
  gameMode = 4;
  gameOverUntil = 0;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  gameGain = 0;
  nextTypeQuestion();
  sfxPlay(SFX_GAME_START);
}

void respawnBall() {
  ballX = 112 + random(242);
  ballY = 82;
  float sp = 3.35f + gameScore * 0.14f;
  if (sp > 8.4f) sp = 8.4f;
  ballVX = random(2) ? sp : -sp;
  ballVX += ((int)random(9) - 4) * 0.28f;
  ballVY = 2.05f;
}

int ballHitRadius() {
  if (gameScore >= 20) return 38;
  if (gameScore >= 8) return 44;
  return 50;
}

void gameTap(int16_t x, int16_t y) {
  if (gameMode == 1) {
    catchTap(x, y);
    return;
  }
  if (gameMode == 2) {
    memoTap(x, y);
    return;
  }
  if (gameMode == 3) {
    cleanTap(x, y);
    return;
  }
  if (gameMode == 4) {
    typeTap(x, y);
    return;
  }
  if (gameOverUntil) return;
  if (y < 72) {  // tocar la cabecera = abandonar sin premio
    gameOpen = false;
    sfxPlay(SFX_TAP);
    return;
  }
  float dx = ballX - x, dy = ballY - y;
  int hitRadius = ballHitRadius();
  if (dx * dx + dy * dy < hitRadius * hitRadius) {  // toque a la bola!
    uint32_t now = millis();
    if (now - ballLastHitAt < 260 || ballVY < -0.25f) return;
    gameScore++;
    sfxPlay(SFX_MINIGAME_OK);
    float lift = 4.65f + (gameScore > 16 ? 1.7f : gameScore * 0.11f);
    ballVY = -lift;
    float drift = 0.38f + (gameScore >= 6 ? 0.08f : 0.0f) + (gameScore >= 14 ? 0.10f : 0.0f);
    float chaos = ((int)random(17) - 8) * 0.26f;
    ballVX += dx * drift + chaos;
    if (random(100) < 28) ballVX = -ballVX * (0.72f + random(45) * 0.01f);
    if (ballVX > 11.5f) ballVX = 11.5f;
    if (ballVX < -11.5f) ballVX = -11.5f;
    hitX = ballX;
    hitY = ballY;
    hitTime = now;
    ballLastHitAt = now;
  }
}

void finishCatchGame() {
  gameNewHi = (gameScore > pet.catchHi);
  gameGain = pet.applyCatchResult(gameScore);
  sfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
  gameOverUntil = millis() + 4000;
}

void catchTap(int16_t x, int16_t y) {
  if (gameOverUntil) return;
  if (y < 72) { gameOpen = false; sfxPlay(SFX_TAP); return; }
  int dx = x - catchX, dy = y - catchY;
  if (dx * dx + dy * dy <= 52 * 52) {
    gameScore++;
    hitX = catchX;
    hitY = catchY;
    hitTime = millis();
    sfxPlay(SFX_MINIGAME_OK);
    spawnCatchTarget();
  } else if (++gameMisses >= 3) {
    finishCatchGame();
  } else {
    sfxPlay(SFX_MINIGAME_BAD);
  }
}

void finishMemoGame() {
  gameScore = memoRounds;
  gameNewHi = (memoRounds > pet.memoHi);
  gameGain = pet.applyMemoResult(memoRounds);
  sfxPlay(gameNewHi && memoRounds > 0 ? SFX_MEDAL : SFX_LEVEL);
  gameOverUntil = millis() + 4000;
}

int memoPadAt(int16_t x, int16_t y) {
  const int16_t px[4] = { 142, 324, 142, 324 };
  const int16_t py[4] = { 164, 164, 318, 318 };
  for (int i = 0; i < 4; i++) {
    int dx = x - px[i], dy = y - py[i];
    if (dx * dx + dy * dy <= 54 * 54) return i;
  }
  return -1;
}

void memoPadSound(uint8_t pad) {
  if (pad < 4) sfxPlay((uint8_t)(SFX_MEMO_PAD_0 + pad));
}

void memoTap(int16_t x, int16_t y) {
  if (gameOverUntil) return;
  if (y < 72) { gameOpen = false; sfxPlay(SFX_TAP); return; }
  if (memoShowing || memoFailUntil || deadlineActive(millis(), memoTurnUntil)) return;
  int pad = memoPadAt(x, y);
  if (pad < 0) return;
  if (pad != memoSeq[memoInput]) {
    memoFlashPad = pad;
    memoFlashGood = false;
    memoFlashUntil = millis() + 620;
    memoHintPad = memoSeq[memoInput];
    memoFailUntil = memoFlashUntil;
    sfxPlay(SFX_MINIGAME_BAD);
    return;
  }
  memoFlashPad = pad;
  memoFlashGood = true;
  memoFlashUntil = millis() + 180;
  memoPadSound((uint8_t)pad);
  memoInput++;
  if (memoInput >= memoLen) {
    memoRounds++;
    if (memoLen >= 14) finishMemoGame();
    else startMemoRound();
  }
}

void finishCleanGame() {
  gameNewHi = (gameScore > pet.cleanHi);
  gameGain = pet.applyCleanResult(gameScore);
  sfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
  gameOverUntil = millis() + 4000;
}

void cleanTap(int16_t x, int16_t y) {
  if (gameOverUntil) return;
  if (y < 72) { gameOpen = false; sfxPlay(SFX_TAP); return; }
  for (uint8_t i = 0; i < 4; i++) {
    if (!cleanAlive[i]) continue;
    int dx = x - cleanX[i], dy = y - cleanY[i];
    if (dx * dx + dy * dy <= 38 * 38) {
      cleanAlive[i] = false;
      if (cleanActive) cleanActive--;
      gameScore++;
      hitX = cleanX[i];
      hitY = cleanY[i];
      hitTime = millis();
      sfxPlay(SFX_MINIGAME_OK);
      return;
    }
  }
  if (++gameMisses >= 3) finishCleanGame();
  else sfxPlay(SFX_MINIGAME_BAD);
}

void finishTypeGame() {
  gameNewHi = (gameScore > pet.typeHi);
  gameGain = pet.applyTypeResult(gameScore);
  sfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
  gameOverUntil = millis() + 4000;
}

void typeTap(int16_t x, int16_t y) {
  if (gameOverUntil) return;
  if (y < 72) { gameOpen = false; sfxPlay(SFX_TAP); return; }
  int idx = -1;
  for (int i = 0; i < 3; i++) {
    int by = 210 + i * 60;
    if (x >= 70 && x <= 396 && y >= by - 8 && y <= by + 56) idx = i;
  }
  if (idx < 0) return;
  if ((uint8_t)idx == typeCorrect) {
    gameScore++;
    sfxPlay(SFX_MINIGAME_OK);
    nextTypeQuestion();
  } else {
    if (++gameMisses >= 3) finishTypeGame();
    else sfxPlay(SFX_MINIGAME_BAD);
  }
}

void stepGame() {
  float grav = 1.14f + gameScore * 0.046f;
  if (gameScore >= 5) grav += 0.14f;
  if (gameScore >= 12) grav += 0.18f;
  if (grav > 2.10f) grav = 2.10f;
  ballVX += sinf((millis() + gameScore * 97) * 0.018f) * 0.11f;
  if (random(100) < 7) ballVX += ((int)random(7) - 3) * 0.22f;
  ballVY += grav;
  ballX += ballVX;
  ballY += ballVY;
  // rebote en la pared circular
  float dx = ballX - CX, dy = ballY - CY;
  float d = sqrtf(dx * dx + dy * dy);
  if (d > 205) {
    float nx = dx / d, ny = dy / d;
    float dot = ballVX * nx + ballVY * ny;
    if (dot > 0) {
      ballVX = (ballVX - 2 * dot * nx) * 1.05f;
      ballVY = (ballVY - 2 * dot * ny) * 0.88f;
      ballVX += ((int)random(9) - 4) * 0.18f;
      sfxPlay(SFX_BALL_BOUNCE);
    }
    ballX = CX + nx * 205;
    ballY = CY + ny * 205;
  }
  if (ballY > 384) {  // al suelo
    if (++gameMisses >= 3) {
      gameNewHi = (gameScore > pet.gameHi);
      pet.playResult(gameScore);  // actualiza el record y da felicidad
      sfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
      gameOverUntil = millis() + 4000;
    } else {
      respawnBall();
      sfxPlay(SFX_BALL_MISS);
    }
  }
  // el bicho la sigue por abajo
  float chase = (ballX - gamePetX) * 0.12f;
  if (chase > 7) chase = 7;
  if (chase < -7) chase = -7;
  gamePetX += chase;
}

// ---------- saco de entrenamiento (entrena la fuerza) ----------

void startSack() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  sackOpen = true;
  sackUntil = millis() + 10000;
  sackOverUntil = 0;
  sackHits = 0;
  sackShake = 0;
  sackNewHi = false;
  sfxPlay(SFX_GAME_START);
}

void sackTap() {
  if (deadlineReached(millis(), sackUntil)) return;  // ya termino el tiempo
  sackHits++;
  sackShake = 16;  // sacude el saco
  if ((sackHits & 1) == 1) sfxPlay(SFX_PLAY);
}

void drawGameScene();  // prototipo (definida mas abajo)
const char *battleTypeName(uint8_t type);
uint16_t battleTypeColor(uint8_t type);

void renderSack() {
  uint32_t now = millis();
  drawGameScene();  // fondo del habitat
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  // pantalla de resultado
  if (sackOverUntil) {
    if (deadlineReached(now, sackOverUntil)) { sackOpen = false; return; }
    char b[20];
    snprintf(b, sizeof(b), T(S_HITS_FMT), sackHits);
    gfx->setTextColor(ink);
    setSize(4);
    setCur(CX - textW(b, 4) / 2, 150);
    printT(b);
    char g[18];
    snprintf(g, sizeof(g), T(S_STR_GAIN_FMT), sackGain);
    gfx->setTextColor(UI_BAR_BAD);
    setSize(3);
    setCur(CX - textW(g, 3) / 2, 210);
    printT(g);
    setSize(2);
    if (sackNewHi && sackHits > 0) {
      gfx->setTextColor(UI_BAR_WARN);
      setCur(CX - textW(T(S_NEW_RECORD), 2) / 2, 256);
      printT(T(S_NEW_RECORD));
    } else {
      char r[18];
      snprintf(r, sizeof(r), T(S_RECORD_FMT), pet.strHi);
      gfx->setTextColor(ink);
      setCur(CX - textW(r, 2) / 2, 256);
      printT(r);
    }
    flushStampedFrame();
    return;
  }

  // se acabaron los 10 s: aplicar entrenamiento
  if (deadlineReached(now, sackUntil)) {
    sackNewHi = (sackHits > pet.strHi);
    sackGain = pet.trainStrength(sackHits);
    sfxPlay(sackNewHi ? SFX_MEDAL : SFX_PLAY);
    sackOverUntil = now + 3500;
    flushStampedFrame();
    return;
  }

  // aporreo activo
  sackShake *= 0.84f;
  int off = (int)(sackShake * sinf(now * 0.05f));
  int sx = CX + off, top = 86, sy = 150;
  gfx->fillRect(CX - 3, 56, 6, top - 56, ink);          // gancho/cuerda
  gfx->fillRect(sx - 4, top - 30, 8, 34, ink);          // cadena
  gfx->fillRoundRect(sx - 42, top, 84, 150, 26, C565(0xb5, 0x3a, 0x3a));  // saco
  gfx->fillRoundRect(sx - 42, top, 84, 22, 18, C565(0x7e, 0x28, 0x28));   // tapa
  gfx->drawRoundRect(sx - 42, top, 84, 150, 26, ink);
  gfx->fillRect(sx - 42, top + 70, 84, 4, C565(0x7e, 0x28, 0x28));        // costura

  // contador de golpes
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", sackHits);
  gfx->setTextColor(ink);
  setSize(6);
  setCur(CX - textW(buf, 6) / 2, 268);
  printT(buf);

  setSize(2);
  setCur(CX - textW(T(S_HIT_FAST), 2) / 2, 322);
  printT(T(S_HIT_FAST));

  // barra de tiempo
  uint32_t left = sackUntil - now;
  int bw = 280, fw = (int)((uint32_t)bw * left / 10000);
  gfx->fillRoundRect(CX - bw / 2, 350, bw, 16, 5, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, 350, fw, 16, 5, UI_BAR_OK);

  flushStampedFrame();
}

// fondo del minijuego: hatibat del bicho (cielo por hora + suelo del bioma)
void drawGameScene() {
  int hh = sceneHour();
  bool night = hh < 6 || hh >= 20;
  uint16_t top, bot;
  if (night)       { top = C565(0x0c, 0x12, 0x24); bot = C565(0x1e, 0x26, 0x46); }
  else if (hh < 8) { top = C565(0xd1, 0x6a, 0x86); bot = C565(0xf3, 0xb8, 0x7c); }
  else if (hh < 18){ top = C565(0x8f, 0xc8, 0xea); bot = C565(0xdc, 0xee, 0xe6); }
  else             { top = C565(0xc7, 0x5a, 0x4a); bot = C565(0xf0, 0xae, 0x64); }
  int hor = 376;
  for (int y = 0; y < hor; y += 8)
    gfx->fillRect(0, y, 466, 8, lerp565(top, bot, y, hor));
  if (night)
    for (auto &st : STARS) gfx->fillRect(st[0], st[1], 4, 4, UI_WHITE);
  uint8_t bio = pet.isEgg() ? 0 : DEX_TBL[pet.speciesId].biome;
  uint16_t soil = BIOME_SOIL[bio < 6 ? bio : 0];
  if (night) soil = lerp565(soil, C565(0x16, 0x1c, 0x30), 9, 16);
  gfx->fillRect(0, hor, 466, 466 - hor, soil);
}

void drawGameResult(const char *recordFmt, uint16_t record, StrId gainFmt) {
  drawGameScene();
  if (deadlineReached(millis(), gameOverUntil)) {
    gameOpen = false;
    return;
  }
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  char buf[22];
  snprintf(buf, sizeof(buf), T(S_SCORE_FMT), gameScore);
  gfx->setTextColor(ink);
  setSize(4);
  setCur(CX - textW(buf, 4) / 2, 148);
  printT(buf);
  char gain[18];
  snprintf(gain, sizeof(gain), T(gainFmt), gameGain);
  gfx->setTextColor(gainFmt == S_DEF_GAIN_FMT ? 0x4C98 : (gainFmt == S_HYG_GAIN_FMT ? UI_BAR_OK : UI_BAR_WARN));
  setSize(3);
  setCur(CX - textW(gain, 3) / 2, 204);
  printT(gain);
  setSize(2);
  if (gameNewHi && gameScore > 0) {
    gfx->setTextColor(UI_BAR_WARN);
    setCur(CX - textW(T(S_NEW_RECORD), 2) / 2, 256);
    printT(T(S_NEW_RECORD));
  } else {
    char rec[20];
    snprintf(rec, sizeof(rec), recordFmt, record);
    gfx->setTextColor(ink);
    setCur(CX - textW(rec, 2) / 2, 256);
    printT(rec);
  }
  flushStampedFrame();
}

void renderCatchGame() {
  uint32_t now = millis();
  if (gameOverUntil) {
    drawGameResult(T(S_RECORD_FMT), pet.catchHi, S_SPD_GAIN_FMT);
    return;
  }
  if (deadlineReached(now, catchUntil) || gameMisses >= 3) {
    finishCatchGame();
    return;
  }
  if (deadlineReached(now, catchTargetUntil)) {
    if (++gameMisses >= 3) {
      finishCatchGame();
      return;
    }
    spawnCatchTarget();
  }
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  gfx->setTextColor(ink);
  setSize(3);
  setCur(CX - textW(T(S_CATCH_TITLE), 3) / 2, 32);
  printT(T(S_CATCH_TITLE));
  char score[16], rec[16];
  snprintf(score, sizeof(score), T(S_SCORE_FMT), gameScore);
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.catchHi);
  setSize(2);
  setCur(50, 78);
  printT(score);
  setCur(294, 78);
  printT(rec);
  for (int i = 0; i < 3; i++) {
    if (i < 3 - gameMisses) gfx->fillCircle(180 + i * 28, 104, 6, UI_BAR_BAD);
    else gfx->drawCircle(180 + i * 28, 104, 6, UI_TRACK);
  }
  const char *const *icon = catchIcon == 0 ? SPR_ICON_FOOD : (catchIcon == 1 ? SPR_ICON_BERRY_B : SPR_ICON_BERRY_G);
  gfx->fillCircle(catchX, catchY, 34, UI_WHITE);
  gfx->drawCircle(catchX, catchY, 36, UI_BAR_WARN);
  drawMap(icon, 16, catchX - 24, catchY - 24, 3, false);
  int bw = 280;
  int fw = (int)((uint32_t)bw * (catchUntil - now) / 20000);
  if (fw < 0) fw = 0;
  gfx->fillRoundRect(CX - bw / 2, 362, bw, 16, 5, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, 362, fw, 16, 5, UI_BAR_OK);
  uint32_t ht = millis() - hitTime;
  if (hitTime && ht < 220) gfx->drawCircle((int)hitX, (int)hitY, 42 + ht / 8, UI_BAR_WARN);
  flushStampedFrame();
}

void stepMemoGame() {
  uint32_t now = millis();
  if (memoFailUntil) {
    if (deadlineReached(now, memoFailUntil)) {
      memoFailUntil = 0;
      memoHintPad = -1;
      finishMemoGame();
    }
    return;
  }
  if (!memoShowing || deadlineActive(now, memoNextAt)) return;
  if (memoActivePad >= 0) {
    memoActivePad = -1;
    memoShow++;
    if (memoShow >= memoLen) {
      memoShowing = false;
      memoInput = 0;
      memoTurnUntil = now + 520;
    } else {
      memoNextAt = now + 150;
    }
    return;
  }
  memoActivePad = memoSeq[memoShow];
  memoPadSound((uint8_t)memoActivePad);
  memoNextAt = now + 480;
}

void renderMemoGame() {
  if (gameOverUntil) {
    drawGameResult(T(S_RECORD_FMT), pet.memoHi, S_DEF_GAIN_FMT);
    return;
  }
  stepMemoGame();
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  char roundBuf[18], rec[16];
  snprintf(roundBuf, sizeof(roundBuf), T(S_ROUND_FMT), memoRounds + 1);
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.memoHi);
  gfx->setTextColor(ink);
  setSize(3);
  setCur(CX - textW(T(S_GAME_MEMO), 3) / 2, 34);
  printT(T(S_GAME_MEMO));
  setSize(2);
  setCur(60, 82);
  printT(roundBuf);
  setCur(310, 82);
  printT(rec);
  const int16_t px[4] = { 142, 324, 142, 324 };
  const int16_t py[4] = { 164, 164, 318, 318 };
  const uint16_t col[4] = { UI_BAR_BAD, UI_BAR_WARN, 0x4C98, UI_BAR_OK };
  int active = memoShowing ? memoActivePad : (memoFailUntil ? memoHintPad : -1);
  for (int i = 0; i < 4; i++) {
    uint16_t fill = i == active ? lerp565(col[i], UI_WHITE, 5, 8) : col[i];
    gfx->fillCircle(px[i], py[i], 48, fill);
    gfx->drawCircle(px[i], py[i], 52, ink);
    if (i == active) {
      int pulse = 56 + (int)((millis() / 70) % 5);
      gfx->drawCircle(px[i], py[i], pulse, col[i]);
    }
    if (i == memoFlashPad && deadlineActive(millis(), memoFlashUntil)) {
      gfx->drawCircle(px[i], py[i], 60, memoFlashGood ? UI_BAR_OK : UI_BAR_BAD);
      gfx->drawCircle(px[i], py[i], 64, memoFlashGood ? UI_BAR_OK : UI_BAR_BAD);
    }
  }
  char phase[28];
  if (memoFailUntil) snprintf(phase, sizeof(phase), "%s", T(S_MEMO_WRONG));
  else if (memoShowing) snprintf(phase, sizeof(phase), "%s", T(S_MEMO_WATCH));
  else snprintf(phase, sizeof(phase), T(S_MEMO_TURN_FMT), memoInput + 1, memoLen);
  // Freier Streifen zwischen den oberen und unteren Pads: dadurch bleibt die
  // Anleitung auf dem runden Display klar lesbar und liegt nicht auf Farben.
  gfx->fillRoundRect(78, 230, 310, 24, 7, night ? UI_BG_NIGHT : UI_BG_DAY);
  gfx->drawRoundRect(78, 230, 310, 24, 7, ink);
  gfx->setTextColor(memoFailUntil ? UI_BAR_BAD : (memoShowing ? UI_BAR_WARN : UI_BAR_OK));
  setSize(2);
  setCur(CX - textW(phase, 2) / 2, 234);
  printT(phase);
  flushStampedFrame();
}

void renderCleanGame() {
  uint32_t now = millis();
  if (gameOverUntil) {
    drawGameResult(T(S_RECORD_FMT), pet.cleanHi, S_HYG_GAIN_FMT);
    return;
  }
  if (deadlineReached(now, cleanUntil) || gameMisses >= 3) {
    finishCleanGame();
    return;
  }
  if (deadlineReached(now, cleanSpawnAt)) {
    if (cleanActive < 4) spawnCleanSpot();
    cleanSpawnAt = now + 720 - (gameScore > 12 ? 260 : gameScore * 20);
  }
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  gfx->setTextColor(ink);
  setSize(3);
  setCur(CX - textW(T(S_CLEAN_TITLE), 3) / 2, 32);
  printT(T(S_CLEAN_TITLE));
  char score[16], rec[16];
  snprintf(score, sizeof(score), T(S_SCORE_FMT), gameScore);
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.cleanHi);
  setSize(2);
  setCur(50, 78);
  printT(score);
  setCur(294, 78);
  printT(rec);
  for (int i = 0; i < 3; i++) {
    if (i < 3 - gameMisses) gfx->fillCircle(180 + i * 28, 104, 6, UI_BAR_BAD);
    else gfx->drawCircle(180 + i * 28, 104, 6, UI_TRACK);
  }
  for (uint8_t i = 0; i < 4; i++) {
    if (!cleanAlive[i]) continue;
    gfx->fillCircle(cleanX[i], cleanY[i], 26, C565(0x8a, 0x66, 0x45));
    gfx->drawCircle(cleanX[i], cleanY[i], 28, UI_INK);
    gfx->fillCircle(cleanX[i] - 8, cleanY[i] - 8, 5, C565(0x62, 0x45, 0x2e));
    gfx->fillCircle(cleanX[i] + 10, cleanY[i] + 4, 6, C565(0x62, 0x45, 0x2e));
  }
  int bw = 280;
  int fw = (int)((uint32_t)bw * (cleanUntil - now) / 18000);
  if (fw < 0) fw = 0;
  gfx->fillRoundRect(CX - bw / 2, 362, bw, 16, 5, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, 362, fw, 16, 5, UI_BAR_OK);
  uint32_t ht = millis() - hitTime;
  if (hitTime && ht < 220) gfx->drawCircle((int)hitX, (int)hitY, 42 + ht / 8, UI_BAR_OK);
  flushStampedFrame();
}

void renderTypeGame() {
  uint32_t now = millis();
  if (gameOverUntil) {
    drawGameResult(T(S_RECORD_FMT), pet.typeHi, S_ATK_GAIN_FMT);
    return;
  }
  if (deadlineReached(now, typeUntil)) {
    if (++gameMisses >= 3) {
      finishTypeGame();
      return;
    }
    sfxPlay(SFX_MINIGAME_BAD);
    nextTypeQuestion();
  }
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  gfx->setTextColor(ink);
  setSize(3);
  setCur(CX - textW(T(S_TYPE_TITLE), 3) / 2, 32);
  printT(T(S_TYPE_TITLE));
  char score[16], rec[16];
  snprintf(score, sizeof(score), T(S_SCORE_FMT), gameScore);
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.typeHi);
  setSize(2);
  setCur(50, 78);
  printT(score);
  setCur(294, 78);
  printT(rec);
  for (int i = 0; i < 3; i++) {
    if (i < 3 - gameMisses) gfx->fillCircle(180 + i * 28, 104, 6, UI_BAR_BAD);
    else gfx->drawCircle(180 + i * 28, 104, 6, UI_TRACK);
  }

  const char *enemy = battleTypeName(typeEnemy);
  gfx->fillRoundRect(118, 126, 230, 54, 14, lerp565(battleTypeColor(typeEnemy), UI_WHITE, 4, 8));
  gfx->drawRoundRect(118, 126, 230, 54, 14, ink);
  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(CX - textW(enemy, 3) / 2, 143);
  printT(enemy);

  for (int i = 0; i < 3; i++) {
    int bx = 88;
    int by = 210 + i * 60;
    const char *label = battleTypeName(typeChoice[i]);
    gfx->fillRoundRect(bx, by, 290, 48, 12, lerp565(battleTypeColor(typeChoice[i]), UI_WHITE, 5, 8));
    gfx->drawRoundRect(bx, by, 290, 48, 12, ink);
    gfx->setTextColor(UI_INK);
    setSize(2);
    setCur(bx + (290 - textW(label, 2)) / 2, by + 17);
    printT(label);
  }
  int bw = 280;
  int fw = (int)((uint32_t)bw * (typeUntil - now) / 4200);
  if (fw < 0) fw = 0;
  gfx->fillRoundRect(CX - bw / 2, 392, bw, 14, 5, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, 392, fw, 14, 5, UI_BAR_OK);
  flushStampedFrame();
}

void renderGame() {
  // sin fillScreen(NEGRO): drawGameScene cubre los 466x466 completos. Si el
  // DMA del flush anterior aun lee el buffer, vera contenido valido (no negro
  // a medio pintar), que era el parpadeo a 25 fps.
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  if (gameMode == 1) {
    renderCatchGame();
    return;
  }
  if (gameMode == 2) {
    renderMemoGame();
    return;
  }
  if (gameMode == 3) {
    renderCleanGame();
    return;
  }
  if (gameMode == 4) {
    renderTypeGame();
    return;
  }

  if (gameOverUntil) {
    drawGameScene();
    if (deadlineReached(millis(), gameOverUntil)) {
      gameOpen = false;
      return;
    }
    char buf[22];
    snprintf(buf, sizeof(buf), T(S_SCORE_FMT), gameScore);
    gfx->setTextColor(ink);
    setSize(4);
    setCur(CX - textW(buf, 4) / 2, 160);
    printT(buf);
    setSize(2);
    if (gameNewHi && gameScore > 0) {
      gfx->setTextColor(UI_BAR_WARN);
      setCur(CX - textW(T(S_NEW_RECORD), 2) / 2, 214);
      printT(T(S_NEW_RECORD));
    } else {
      char rec[20];
      snprintf(rec, sizeof(rec), T(S_RECORD_FMT), pet.gameHi);
      gfx->setTextColor(ink);
      setCur(CX - textW(rec, 2) / 2, 214);
      printT(rec);
    }
    const char *msg = gameScore >= 10 ? T(S_GREAT_JOY) : T(S_PLUS_JOY);
    gfx->setTextColor(ink);
    setCur(CX - textW(msg, 2) / 2, 250);
    printT(msg);
    flushStampedFrame();
    return;
  }

  drawGameScene();
  stepGame();

  // marcador, record y vidas
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", gameScore);
  gfx->setTextColor(ink);
  setSize(4);
  setCur(CX - textW(buf, 4) / 2, 30);
  printT(buf);
  char rec[12];
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.gameHi);
  setSize(2);
  setCur(CX - textW(rec, 2) / 2, 76);
  printT(rec);
  for (int i = 0; i < 3; i++) {
    if (i < 3 - gameMisses) gfx->fillCircle(180 + i * 28, 104, 6, UI_BAR_BAD);
    else gfx->drawCircle(180 + i * 28, 104, 6, UI_TRACK);
  }

  if (pmd.loaded) {
    uint8_t act = (ballX > gamePetX + 4) ? PMD_WALKR : (ballX < gamePetX - 4) ? PMD_WALKL : PMD_IDLE;
    if (!pmd.has(act)) act = PMD_IDLE;
    drawPmdAct(act, (int)gamePetX, 394, millis(), true, false, 3);
  } else if (mon.loaded) {
    int s = (mon.h * 2 > 130) ? 1 : 2;
    int w = mon.w * s, h = mon.h * s;
    uint16_t fm = mon.frameMs ? mon.frameMs : 100;
    uint16_t fi = (millis() / fm) % mon.frames;
    const uint8_t *fr = mon.data + (uint32_t)fi * mon.w * mon.h;
    int px = (int)gamePetX - w / 2, py = 394 - h;
    for (int r = 0; r < mon.h; r++)
      for (int c = 0; c < mon.w; c++) {
        uint8_t idx = fr[r * mon.w + c];
        if (idx == 0xFF) continue;
        gfx->fillRect(px + c * s, py + r * s, s, s, mon.pal[idx]);
      }
  }

  // anillo de impacto que se expande y desvanece (feedback suave del golpe)
  uint32_t ht = millis() - hitTime;
  if (hitTime && ht < 260) {
    int rad = 22 + (int)(ht / 6);
    gfx->drawCircle((int)hitX, (int)hitY, rad, C565(0xff, 0xe7, 0x9f));
    gfx->drawCircle((int)hitX, (int)hitY, rad - 2, C565(0xff, 0xd9, 0x8a));
  }

  // la pokeball
  drawMap(SPR_ICON_PLAY, 16, (int)ballX - 24, (int)ballY - 24, 3, false);

  flushStampedFrame();
}

// ---------- combate salvaje manual ----------

uint16_t battleHpFor(const BattleStats &stats) {
  return stats.hp ? stats.hp : (uint16_t)(30 + stats.level * 5 + stats.def);
}

BattleStats petBattleStats() {
  BattleStats stats = {};
  stats.level = pet.level();
  stats.atk = pet.atkStat();
  stats.def = pet.defStat();
  stats.spe = pet.speStat();
  stats.hp = 0;
  if (!pet.isEgg() && pet.speciesId >= 1 && pet.speciesId <= DEX_COUNT) {
    stats.type1 = DEX_TBL[pet.speciesId].type1;
    stats.type2 = DEX_TBL[pet.speciesId].type2;
  }
  return stats;
}

void scheduleNextWild(uint32_t now) {
  nextWildEligible = now + WILD_COOLDOWN_MS;
}

bool mainScreenReadyForWild() {
  if (screenOff || pet.awaitingStarter() || pet.isEgg() || pet.sleeping || pet.ceremony) return false;
  if (battleOpen || gameOpen || gameMenuOpen || sackOpen || cardOpen || galleryOpen || kbOpen || clockOpen || helpOpen) return false;
  if (feedMenuUntil || confirmUntil || choiceKind || bathUntil || petEventUntil) return false;
  if (pet.evolving() || pet.wantEvolveButton() || pet.canRunawayNow() || pet.wantFarewellButton()) return false;
  return true;
}

void maybeOfferWildEncounter(uint32_t now) {
  if (nextWildEligible == 0) {
    scheduleNextWild(now);
    return;
  }
  if (wildPromptUntil) return;
  if (now - lastWildCheck < WILD_CHECK_MS) return;
  lastWildCheck = now;
  if (deadlineActive(now, nextWildEligible)) return;
  if (!mainScreenReadyForWild()) return;
  int16_t cand = pickWildSpecies((uint8_t)random(100), currentDayPhase());
  uint8_t phase = currentDayPhase();
  uint8_t chance = (phase == 3) ? 4 : (phase == 0 ? 7 : 8);
  if (phase == 3 && cand >= 1 && cand <= DEX_COUNT) {
    const DexEntry &d = DEX_TBL[cand];
    if (d.type1 == TYPE_GHOST || d.type2 == TYPE_GHOST ||
        d.type1 == TYPE_POISON || d.type2 == TYPE_POISON) chance = 8;
  }
  if ((uint8_t)random(100) >= chance) return;

  wildPromptDex = cand;
  wildPromptLevel = wildLevelFor(pet.level(), (uint8_t)random(100));
  wildPromptShiny = (random(4096) < pet.stepShinyChancePer4096());
  wildPromptUntil = now + WILD_PROMPT_MS;
  scheduleNextWild(now);
  sfxPlay(SFX_MENU);
}

void scheduleNextPetEvent(uint32_t now) {
  nextPetEventEligible = now + PET_EVENT_COOLDOWN_MS;
}

bool mainScreenReadyForPetEvent() {
  if (screenOff || pet.awaitingStarter() || pet.isEgg() || pet.sleeping || pet.ceremony) return false;
  if (battleOpen || gameOpen || gameMenuOpen || sackOpen || cardOpen || galleryOpen || kbOpen || clockOpen || helpOpen) return false;
  if (feedMenuUntil || confirmUntil || choiceKind || bathUntil || wildPromptUntil) return false;
  if (pet.evolving() || pet.wantEvolveButton() || pet.canRunawayNow() || pet.wantFarewellButton()) return false;
  return true;
}

void maybeOfferPetEvent(uint32_t now) {
  if (nextPetEventEligible == 0) {
    scheduleNextPetEvent(now);
    return;
  }
  if (petEventUntil) {
    if (deadlineReached(now, petEventUntil)) petEventUntil = 0;
    return;
  }
  if (now - lastPetEventCheck < PET_EVENT_CHECK_MS) return;
  lastPetEventCheck = now;
  if (deadlineActive(now, nextPetEventEligible)) return;
  if (!mainScreenReadyForPetEvent()) return;
  uint8_t phase = currentDayPhase();
  uint8_t chance = (phase == 0) ? 14 : (phase == 3 ? 8 : 10);
  if ((uint8_t)random(100) >= chance) return;

  petEventType = (uint8_t)random(3);
  petEventUntil = now + PET_EVENT_PROMPT_MS;
  scheduleNextPetEvent(now);
  sfxPlay(SFX_EVENT_SPARKLE);
}

bool inPetEventHit(int16_t x, int16_t y) {
  int16_t ex = 366, ey = 286;
  int dx = x - ex, dy = y - ey;
  return dx * dx + dy * dy <= 44 * 44;
}

void acceptPetEvent() {
  uint8_t type = petEventType;
  petEventUntil = 0;
  scheduleNextPetEvent(millis());
  if (!pet.applyPetEvent(type)) return;
  StrId msg = S_EVENT_FOUND;
  if (type == PET_EVENT_HEART) msg = S_EVENT_PET;
  else if (type == PET_EVENT_SPARKLE) msg = S_EVENT_LUCKY;
  snprintf(petEventMsg, sizeof(petEventMsg), "%s", T(msg));
  petEventFeedbackUntil = millis() + 1800;
  sfxPlay(type == PET_EVENT_BERRY ? SFX_EAT : (type == PET_EVENT_SPARKLE ? SFX_EVENT_SPARKLE : SFX_HEART));
}

bool mainScreenReadyForShake() {
  if (screenOff || pet.awaitingStarter() || pet.isEgg() || pet.sleeping || pet.ceremony) return false;
  if (battleOpen || gameOpen || gameMenuOpen || sackOpen || galleryOpen || kbOpen || clockOpen || helpOpen) return false;
  if (feedMenuUntil || confirmUntil || choiceKind || bathUntil) return false;
  if (pet.evolving()) return false;
  return true;
}

void maybeOfferMorning(uint32_t now) {
  if (morningUntil) {
    if (deadlineReached(now, morningUntil)) morningUntil = 0;
    return;
  }
  if (screenOff || pet.awaitingStarter() || pet.ceremony) return;
  if (battleOpen || gameOpen || gameMenuOpen || sackOpen || cardOpen || galleryOpen || kbOpen || clockOpen || helpOpen) return;
  if (feedMenuUntil || confirmUntil || choiceKind || bathUntil || wildPromptUntil || petEventUntil) return;
  if (currentDayPhase() != 0) return;
  if (!pet.takeMorningGreeting()) return;
  morningUntil = now + 2500UL;
  if (!pet.sleeping && !pet.isEgg() && audioMode() == SOUND_FULL)
    speciesChirpPlay(pet.speciesId, true);
  else if (!pet.sleeping && !pet.isEgg())
    sfxPlay(SFX_HEART);
}

void closeBattle() {
  battleOpen = false;
  battleResolved = false;
  battleAttackMenuUntil = 0;
  battleCatchOffered = false;
  battleCatchTried = false;
  battleCatchDone = false;
  battleCatchSuccess = false;
  battleRespectCatch = false;
  battleCatchChance = 0;
  wildPmd.unload();
  markUiDirty();
  lockTouchBrief();
}

void startBattleWith(int16_t forcedDex, uint8_t forcedLevel, int8_t forcedShiny) {
  if (!canStartWildBattle(pet.isEgg(), pet.sleeping, pet.ceremony)) return;
  wildPromptUntil = 0;
  scheduleNextWild(millis());
  if (forcedDex >= 1 && forcedDex <= DEX_COUNT) {
    battleDex = forcedDex;
    battleLevel = forcedLevel ? forcedLevel : wildLevelFor(pet.level(), (uint8_t)random(100));
  } else {
    uint8_t speciesRoll = random(100);
    uint8_t levelRoll = random(100);
    battleDex = pickWildSpecies(speciesRoll, currentDayPhase());
    battleLevel = wildLevelFor(pet.level(), levelRoll);
  }
  battleShiny = forcedShiny < 0 ? (random(4096) < pet.stepShinyChancePer4096()) : forcedShiny != 0;
  battlePlayer = petBattleStats();
  battleEnemy = wildBattleStats(battleDex, battleLevel);
  battleEnemy.hp = 0;
  battleResult = {};
  battleRun = beginBattleRuntime(battlePlayer, battleEnemy);
  battleTurn = {};
  battleReward = {};
  battleMsg[0] = 0;
  battleAttackMenuUntil = 0;
  battleLowHpWarned = false;
  battleCatchOffered = false;
  battleCatchTried = false;
  battleCatchDone = false;
  battleCatchSuccess = false;
  battleRespectCatch = false;
  battleCatchChance = 0;
  battleResolved = false;
  battleOpen = true;
  battleDirty = true;
  wildPmd.unload();
  wildPmd.load(battleDex, battleShiny);
  sfxPlay(SFX_TAP);
  speciesChirpPlay(battleDex);
}

void startBattle() {
  startBattleWith(0, 0, -1);
}

void finishBattle() {
  if (battleResolved) return;
  battleResolved = true;
  battleDirty = true;
  // Modo facil: la derrota no cuenta. Se marca como victoria ANTES de repartir
  // premios, para que pantalla, vinculo, racha y medallas vean un combate
  // ganado y no uno perdido.
  if (easyMode) battleTurn.playerWon = true;
  if (battleTurn.playerWon) {
    bool closeWin = battleRun.playerHp <= battleRun.playerMaxHp / 3;
    battleReward = pet.applyBattleWin(battleDex, closeWin);
    battleCatchOffered = true;
    battleRespectCatch = false;
    battleCatchChance = pet.catchChanceForWild(battleDex, battleLevel, battlePlayer.level, closeWin);
    if (easyMode) battleCatchChance = 100;  // la captura tambien esta garantizada
    sfxPlay(SFX_BATTLE_WIN);
  } else {
    battleReward = {};
    pet.applyBattleLoss();
    bool closeLoss = battleRun.enemyHp > 0 && battleRun.enemyHp * 100UL <= battleRun.enemyMaxHp * 30UL;
    battleCatchChance = closeLoss ? pet.respectCatchChanceForWild(battleDex, battleLevel, battlePlayer.level) : 0;
    battleCatchOffered = battleCatchChance > 0;
    battleRespectCatch = battleCatchOffered;
    sfxPlay(SFX_BATTLE_LOSS);
  }
}

void performBattleAction(BattleAction action) {
  if (battleResolved) return;
  battleAttackMenuUntil = 0;
  battleLastAction = action;
  battleTurn = stepBattle(battleRun, action, (uint8_t)random(100));
  // Easy mode: any attack is a guaranteed one-hit KO, even if the roll dodged
  // or the enemy would have moved first. Resolve it before the message so the
  // damage figure, the empty HP bar and the win banner all agree.
  const bool attacking = action == BATTLE_ATTACK || action == BATTLE_ATTACK_QUICK ||
                         action == BATTLE_ATTACK_HEAVY;
  if (easyMode && attacking && !battleTurn.battleEnded) {
    uint16_t left = battleRun.enemyHp;
    battleRun.enemyHp = 0;
    battleTurn.playerDamage = (uint16_t)(battleTurn.playerDamage + left);
    battleTurn.enemyDodged = false;
    battleTurn.enemyDamage = 0;
    battleTurn.battleEnded = true;
    battleTurn.playerWon = true;
  }
  if (battleTurn.restFailed) {
    snprintf(battleMsg, sizeof(battleMsg), T(S_NO_REST));
  } else if (battleTurn.counterReady) {
    snprintf(battleMsg, sizeof(battleMsg), T(S_COUNTER_READY));
  } else if (battleTurn.playerRested) {
    char healMsg[18];
    snprintf(healMsg, sizeof(healMsg), T(S_RESTED_FMT), battleTurn.playerHeal);
    snprintf(battleMsg, sizeof(battleMsg), "%s %s", healMsg, T(S_GUARD));
  } else if (battleTurn.playerDamage > 0) {
    if (battleTurn.playerTypePct > 100) snprintf(battleMsg, sizeof(battleMsg), "%s %u", T(S_EFFECTIVE), battleTurn.playerDamage);
    else if (battleTurn.playerTypePct < 100) snprintf(battleMsg, sizeof(battleMsg), "%s %u", T(S_NOT_EFFECTIVE), battleTurn.playerDamage);
    else snprintf(battleMsg, sizeof(battleMsg), T(S_HIT_FMT), battleTurn.playerDamage);
  } else if (battleTurn.enemyDodged) {
    snprintf(battleMsg, sizeof(battleMsg), T(S_ENEMY_DODGED));
  } else if (battleTurn.playerDodged) {
    snprintf(battleMsg, sizeof(battleMsg), T(S_DODGED));
  } else {
    snprintf(battleMsg, sizeof(battleMsg), T(S_MISSED));
  }
  if (battleTurn.battleEnded) {
    finishBattle();
    return;
  }
  if (!battleLowHpWarned && battleRun.playerHp > 0 && battleRun.playerHp <= battleRun.playerMaxHp * 3 / 10) {
    battleLowHpWarned = true;
    sfxPlay(SFX_LOW_HP);
  }
  if (battleTurn.restFailed) sfxPlay(SFX_DENY);
  else if (battleTurn.counterReady) sfxPlay(SFX_COUNTER);
  else if (battleTurn.playerRested) sfxPlay(SFX_REST);
  else if (action == BATTLE_ATTACK_QUICK) sfxPlay(SFX_ATTACK_QUICK);
  else if (action == BATTLE_ATTACK_HEAVY) sfxPlay(SFX_ATTACK_HEAVY);
  else if (battleTurn.playerDamage > 0 && battleTurn.playerTypePct > 100) sfxPlay(SFX_EFFECTIVE);
  else if (battleTurn.playerDamage > 0 && battleTurn.playerTypePct < 100) sfxPlay(SFX_WEAK_HIT);
  else if (battleTurn.enemyDamage > 0) sfxPlay(SFX_ENEMY_HIT);
  else sfxPlay(battleTurn.playerDamage > 0 ? SFX_PLAY : SFX_TAP);
}

void battleTap(int16_t x, int16_t y) {
  if (battleResolved) {
    if (battleCatchOffered && !battleCatchDone) {
      if (x >= 76 && x <= 224 && y >= 392 && y <= 448) {
        bool closeWin = battleRun.playerHp <= battleRun.playerMaxHp / 3;
        battleCatchTried = true;
        battleCatchDone = true;
        if (easyMode) {
          // Modo facil: la captura no se sortea. registerCaught() es lo mismo
          // que hace tryCatchWild() al acertar (dex, diario, premios y save).
          pet.registerCaught(battleDex, battleShiny);
          battleCatchSuccess = true;
          battleCatchChance = 100;
        } else if (battleRespectCatch) {
          battleCatchSuccess = pet.tryRespectCatchWild(battleDex, battleLevel, battlePlayer.level,
                                                       (uint8_t)random(100), battleShiny);
          battleCatchChance = pet.respectCatchChanceForWild(battleDex, battleLevel, battlePlayer.level);
        } else {
          battleCatchSuccess = pet.tryCatchWild(battleDex, battleLevel, battlePlayer.level, closeWin,
                                                (uint8_t)random(100), battleShiny);
          battleCatchChance = pet.catchChanceForWild(battleDex, battleLevel, battlePlayer.level, closeWin);
        }
        sfxPlay(battleCatchSuccess ? SFX_CATCH_OK : SFX_CATCH_FAIL);
        galleryDirty = true;
        battleDirty = true;
        return;
      }
      if (x >= 242 && x <= 390 && y >= 392 && y <= 448) {
        battleCatchDone = true;
        battleCatchTried = false;
        battleDirty = true;
        sfxPlay(SFX_TAP);
        return;
      }
      return;
    }
    if (x >= 118 && x <= 348 && y >= 392 && y <= 454) closeBattle();
    return;
  }
  if (battleAttackMenuUntil) {
    if (x >= 66 && x <= 232 && y >= 292 && y <= 352) {
      performBattleAction(BATTLE_ATTACK_QUICK);
      return;
    }
    if (x >= 234 && x <= 400 && y >= 292 && y <= 352) {
      performBattleAction(BATTLE_ATTACK_HEAVY);
      return;
    }
    battleAttackMenuUntil = 0;
    battleDirty = true;
    sfxPlay(SFX_TAP);
    return;
  }
  if (x >= 184 && x <= 282 && y >= 100 && y <= 136) {
    closeBattle();
    sfxPlay(SFX_TAP);
    return;
  }
  if (x >= 46 && x <= 174 && y >= 344 && y <= 428) {
    battleAttackMenuUntil = 1;
    sfxPlay(SFX_TAP);
  } else if (x >= 169 && x <= 297 && y >= 344 && y <= 428) {
    performBattleAction(BATTLE_DODGE);
  } else if (x >= 292 && x <= 420 && y >= 344 && y <= 428) {
    performBattleAction(BATTLE_REST);
  }
}

void drawBattleHpBar(int x, int y, uint16_t cur, uint16_t maxHp, uint16_t color) {
  if (maxHp == 0) maxHp = 1;
  int w = 150;
  int fw = (int)((uint32_t)cur * w / maxHp);
  if (fw > w) fw = w;
  gfx->fillRoundRect(x, y, w, 14, 4, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(x, y, fw, 14, 4, color);
}

void battleRewardText(char *buf, size_t len) {
  if (battleReward.amount == 0) { buf[0] = 0; return; }
  StrId fmt = S_SPD_GAIN_FMT;
  if (battleReward.stat == BATTLE_REWARD_ATK) fmt = S_ATK_GAIN_FMT;
  else if (battleReward.stat == BATTLE_REWARD_DEF) fmt = S_DEF_GAIN_FMT;
  snprintf(buf, len, T(fmt), battleReward.amount);
}

void drawBattleButtonLabel(int x, int y, int w, const char *label) {
  setSize(2);  // medir con la escala con la que se va a pintar
  int px = textW(label, 2);
  if (px <= w - 8) {
    setSize(2);
    setCur(x + (w - px) / 2, y);
    printT(label);
  } else {
    setSize(1);
    setCur(x + (w - textW(label, 1)) / 2, y + 3);
    printT(label);
    setSize(2);
  }
}

const char *battleTypeName(uint8_t type) {
  static const char *const TYPE_NAMES[LANG_COUNT][19] = {
    { "", "NORMAL", "FIRE", "WATER", "ELEC", "GRASS", "ICE", "FIGHT", "POISON", "GROUND", "FLY", "PSY", "BUG", "ROCK", "GHOST", "DRAGON", "DARK", "STEEL", "FAIRY" },
    { "", "노말", "불꽃", "물", "전기", "풀", "얼음", "격투", "독", "땅", "비행", "에스퍼", "벌레", "바위", "고스트", "드래곤", "악", "강철", "페어리" },
  };
  if (type < 19) return TYPE_NAMES[gLang][type];
  switch (type) {
    case TYPE_NORMAL: return "NORMAL";
    case TYPE_FIRE: return "FIRE";
    case TYPE_WATER: return "WATER";
    case TYPE_ELECTRIC: return "ELEC";
    case TYPE_GRASS: return "GRASS";
    case TYPE_ICE: return "ICE";
    case TYPE_FIGHTING: return "FIGHT";
    case TYPE_POISON: return "POISON";
    case TYPE_GROUND: return "GROUND";
    case TYPE_FLYING: return "FLY";
    case TYPE_PSYCHIC: return "PSY";
    case TYPE_BUG: return "BUG";
    case TYPE_ROCK: return "ROCK";
    case TYPE_GHOST: return "GHOST";
    case TYPE_DRAGON: return "DRAGON";
    case TYPE_DARK: return "DARK";
    case TYPE_STEEL: return "STEEL";
    case TYPE_FAIRY: return "FAIRY";
  }
  return "";
}

uint16_t battleTypeColor(uint8_t type) {
  switch (type) {
    case TYPE_FIRE: return 0xEA87;
    case TYPE_WATER: return 0x4C98;
    case TYPE_ELECTRIC: return 0xBCA1;
    case TYPE_GRASS: return 0x3C49;
    case TYPE_ICE: return 0x5D99;
    case TYPE_FIGHTING: return 0xA2A5;
    case TYPE_POISON: return 0x8A73;
    case TYPE_GROUND: return 0xB447;
    case TYPE_FLYING: return 0x8D7F;
    case TYPE_PSYCHIC: return 0xD28F;
    case TYPE_BUG: return 0x7CC4;
    case TYPE_ROCK: return 0x9407;
    case TYPE_GHOST: return 0x6B33;
    case TYPE_DRAGON: return 0x5A5F;
    case TYPE_DARK: return 0x5ACB;
    case TYPE_STEEL: return 0xA534;
    case TYPE_FAIRY: return 0xF3B7;
    default: return 0x8C4D;
  }
}

void typeText(char *buf, size_t len, const DexEntry &d) {
  if (d.type2 == TYPE_NONE) snprintf(buf, len, "%s", battleTypeName(d.type1));
  else snprintf(buf, len, "%s %s", battleTypeName(d.type1), battleTypeName(d.type2));
}

int typeChipWidth(uint8_t type) {
  if (!gCjkFont) return (int)strlen(battleTypeName(type)) * 6 + 14;
  setSize(1);  // el chip siempre pinta el tipo a escala 1
  return (int)textW(battleTypeName(type), 1) + 14;
}

void drawTypeChip(int x, int y, uint8_t type) {
  if (type == TYPE_NONE) return;
  const char *label = battleTypeName(type);
  int w = typeChipWidth(type);
  // La unifont coreana mide 16 px de alto y no cabe en la capsula de 16 px de
  // la 5x7: en coreano la capsula crece 4 px y el texto sube 2.
  int ch = gCjkFont ? 20 : 16;
  gfx->fillRoundRect(x, y, w, ch, 5, lerp565(battleTypeColor(type), UI_WHITE, 5, 8));
  gfx->drawRoundRect(x, y, w, ch, 5, UI_INK);
  setSize(1);
  gfx->setTextColor(UI_INK);
  setCur(x + 7, y + (gCjkFont ? 3 : 5));
  printT(label);
}

int drawTypeChips(int x, int y, const DexEntry &d, bool alignRight) {
  int w1 = typeChipWidth(d.type1);
  int w2 = d.type2 == TYPE_NONE ? 0 : typeChipWidth(d.type2);
  int total = w1 + (w2 ? 4 + w2 : 0);
  int sx = alignRight ? x - total : x;
  drawTypeChip(sx, y, d.type1);
  if (d.type2 != TYPE_NONE) drawTypeChip(sx + w1 + 4, y, d.type2);
  setSize(2);
  return sx;
}

void drawWildPrompt() {
  gfx->fillRoundRect(82, 156, 302, 178, 18, UI_WHITE);
  gfx->drawRoundRect(82, 156, 302, 178, 18, UI_INK);
  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(CX - textW(T(S_WILD_Q), 3) / 2, 176);
  printT(T(S_WILD_Q));
  char name[28];
  snprintf(name, sizeof(name), "%s%s Lv.%u", wildPromptShiny ? "*" : "",
           dexName(wildPromptDex), wildPromptLevel);
  setSize(2);
  setCur(CX - textW(name, 2) / 2, 206);
  printT(name);

  gfx->fillRoundRect(93, 226, 280, 44, 12, UI_BAR_BAD);
  gfx->fillRoundRect(93, 278, 280, 44, 12, UI_TRACK);
  gfx->setTextColor(UI_WHITE);
  setCur(CX - textW(T(S_FIGHT), 2) / 2, 240);
  printT(T(S_FIGHT));
  gfx->setTextColor(UI_BG_DAY);
  setCur(CX - textW(T(S_LATER), 2) / 2, 292);
  printT(T(S_LATER));
}

void drawPetEvent() {
  uint32_t now = millis();
  if (petEventUntil && deadlineReached(now, petEventUntil)) petEventUntil = 0;
  if (!petEventUntil && (!petEventFeedbackUntil || deadlineReached(now, petEventFeedbackUntil))) return;

  int16_t ex = 366, ey = 286;
  if (petEventUntil) {
    gfx->fillCircle(ex, ey, 32, UI_WHITE);
    gfx->drawCircle(ex, ey, 34, UI_BAR_WARN);
    if (petEventType == PET_EVENT_BERRY) {
      drawMap(SPR_ICON_BERRY_G, 16, ex - 24, ey - 24, 3, false);
    } else if (petEventType == PET_EVENT_HEART) {
      drawMap(SPR_HEART, 32, ex - 32, ey - 32, 2, false);
    } else {
      gfx->setTextColor(UI_BAR_WARN);
      setSize(4);
      setCur(ex - 12, ey - 18);
      printT("*");
      gfx->fillCircle(ex - 14, ey + 12, 4, UI_WHITE);
      gfx->fillCircle(ex + 16, ey + 10, 4, UI_WHITE);
    }
  }
  if (petEventFeedbackUntil && deadlineActive(now, petEventFeedbackUntil)) {
    gfx->setTextColor(UI_BAR_WARN);
    setSize(2);
    setCur(CX - textW(petEventMsg, 2) / 2, 292);
    printT(petEventMsg);
  }
}

void renderBattle() {
  if (battleResolved) battleDirty = false;
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  const DexEntry &mine = DEX_TBL[pet.speciesId];
  const DexEntry &wild = DEX_TBL[battleDex];

  gfx->setTextColor(ink);
  setSize(3);
  setCur(CX - textW(T(S_WILD_BATTLE), 3) / 2, 34);
  printT(T(S_WILD_BATTLE));

  char left[24], right[24];
  snprintf(left, sizeof(left), "%s Lv.%u", pet.nick[0] ? pet.nick : dexName(pet.speciesId), battlePlayer.level);
  snprintf(right, sizeof(right), "%s%s Lv.%u", battleShiny ? "*" : "",
           dexName(battleDex), battleLevel);
  setSize(2);
  setCur(28, 82);
  printT(left);
  setSize(2);
  int rightTextSize = textW(right, 2) <= 144 ? 2 : 1;
  setSize(rightTextSize);
  int rightTextW = textW(right, rightTextSize);
  int rightX = 408 - rightTextW;
  if (rightX < 246) rightX = 246;
  setSize(rightTextSize);
  setCur(rightX, rightTextSize == 2 ? 82 : 88);
  printT(right);
  setSize(2);

  uint16_t playerMax = battleRun.playerMaxHp;
  uint16_t enemyMax = battleRun.enemyMaxHp;
  uint16_t playerCur = battleRun.playerHp;
  uint16_t enemyCur = battleRun.enemyHp;
  drawBattleHpBar(28, 110, playerCur, playerMax, UI_BAR_OK);
  drawBattleHpBar(288, 110, enemyCur, enemyMax, UI_BAR_BAD);
  drawTypeChips(28, 130, mine, false);
  int wildChipLeft = drawTypeChips(438, 130, wild, true);

  if (!battleResolved) {
    gfx->fillRoundRect(188, 102, 90, 32, 9, UI_TRACK);
    gfx->setTextColor(UI_BG_DAY);
    setSize(2);
    setCur(188 + (90 - textW(T(S_RUN_BATTLE), 2)) / 2, 111);
    printT(T(S_RUN_BATTLE));
  }
  // Links neben den Typ-Chips des Gegners (Gift/Flug usw.). Oben rechts
  // neben dem Namen liegt der Marker ausserhalb des runden Displays.
  if (pet.isCaught(battleDex))
    drawCaughtBattleMarker(wildChipLeft - 36, 122);

  if (pmd.loaded) drawPmdAct(PMD_IDLE, 142, 286, millis(), true, false, 3);
  else {
    const uint8_t *th = thumbs.get(pet.speciesId);
    if (th) drawThumb(th, 94, 166, 3, false);
  }
  if (wildPmd.loaded) drawPmdActM(wildPmd, PMD_IDLE, 328, 286, millis(), true, false, 3);
  else {
    const uint8_t *th = thumbs.get(battleDex);
    if (th) drawThumb(th, 280, 166, 3, false);
  }

  if (battleResolved) {
    const char *res = battleTurn.playerWon ? T(S_WIN) : T(S_LOSS);
    gfx->setTextColor(battleTurn.playerWon ? UI_BAR_OK : UI_BAR_BAD);
    setSize(4);
    setCur(CX - textW(res, 4) / 2, 300);
    printT(res);
    char rounds[20], damage[28];
    snprintf(rounds, sizeof(rounds), T(S_ROUNDS_FMT), battleRun.round);
    snprintf(damage, sizeof(damage), T(S_DAMAGE_FMT), battleRun.playerDamageTotal, battleRun.enemyDamageTotal);
    gfx->setTextColor(ink);
    setSize(2);
    setCur(CX - textW(rounds, 2) / 2, 334);
    printT(rounds);
    setCur(CX - textW(damage, 2) / 2, 356);
    printT(damage);
    if (battleTurn.playerWon) {
      char reward[20];
      battleRewardText(reward, sizeof(reward));
      if (reward[0]) {
        gfx->setTextColor(UI_BAR_WARN);
        setCur(CX - textW(reward, 2) / 2, 378);
        printT(reward);
      }
    } else if (battleRespectCatch && battleCatchOffered && !battleCatchDone) {
      gfx->setTextColor(UI_BAR_WARN);
      setSize(2);
      setCur(CX - textW(T(S_CLOSE_CHANCE), 2) / 2, 378);
      printT(T(S_CLOSE_CHANCE));
    }
    if (battleCatchOffered && !battleCatchDone) {
      gfx->fillRoundRect(76, 396, 148, 52, 14, UI_BAR_OK);
      gfx->fillRoundRect(242, 396, 148, 52, 14, UI_TRACK);
      gfx->setTextColor(UI_BG_DAY);
      drawBattleButtonLabel(76, 414, 148, T(S_CATCH_WILD));
      drawBattleButtonLabel(242, 414, 148, T(S_LEAVE_WILD));
    } else {
      if (battleCatchDone && battleCatchTried) {
        const char *catchMsg = battleCatchSuccess ? T(S_CAUGHT_OK) : T(S_ESCAPED);
        gfx->setTextColor(battleCatchSuccess ? UI_BAR_OK : UI_BAR_BAD);
        setSize(2);
        setCur(CX - textW(catchMsg, 2) / 2, 378);
        printT(catchMsg);
      }
      gfx->fillRoundRect(118, 396, 230, 52, 14, UI_BAR_OK);
      gfx->setTextColor(UI_BG_DAY);
      setSize(3);
      setCur(CX - textW(T(S_OK), 3) / 2, 413);
      printT(T(S_OK));
    }
  } else {
    char roundBuf[14];
    snprintf(roundBuf, sizeof(roundBuf), T(S_ROUND_SHORT_FMT), battleRun.round + 1);
    gfx->setTextColor(ink);
    setSize(2);
    setCur(32, 318);
    printT(roundBuf);
    if (battleMsg[0]) {
      setCur(CX - textW(battleMsg, 2) / 2, 318);
      printT(battleMsg);
      if (battleTurn.enemyDamage > 0) {
        char eb[16];
        snprintf(eb, sizeof(eb), "-%u", battleTurn.enemyDamage);
        gfx->setTextColor(UI_BAR_BAD);
        setCur(CX - textW(eb, 2) / 2, 340);
        printT(eb);
      }
    }

    if (battleAttackMenuUntil) {
      gfx->fillRoundRect(74, 298, 150, 46, 12, UI_BAR_BAD);
      gfx->fillRoundRect(242, 298, 150, 46, 12, UI_BAR_WARN);
      gfx->setTextColor(UI_BG_DAY);
      drawBattleButtonLabel(74, 312, 150, T(S_QUICK_ATTACK));
      drawBattleButtonLabel(242, 312, 150, T(S_HEAVY_ATTACK));
    }

    gfx->fillRoundRect(58, 358, 108, 58, 13, UI_BAR_BAD);
    gfx->fillRoundRect(179, 358, 108, 58, 13, 0x4C98);
    gfx->fillRoundRect(300, 358, 108, 58, 13, UI_BAR_OK);
    gfx->setTextColor(UI_BG_DAY);
    drawBattleButtonLabel(58, 380, 108, T(S_ATTACK));
    drawBattleButtonLabel(179, 380, 108, T(S_DODGE));
    char restLabel[18];
    snprintf(restLabel, sizeof(restLabel), "%s %u", T(S_REST), battleRun.restUsesLeft);
    drawBattleButtonLabel(300, 380, 108, restLabel);
  }

  flushStampedFrame();
}

// ---------- ficha del bicho (deslizar vertical) ----------

void drawCardStat(int y, const char *label, uint16_t val, uint16_t maxBar, uint16_t color) {
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(96, y);
  printT(label);
  char num[8];
  snprintf(num, sizeof(num), "%u", val);
  setCur(330, y);
  printT(num);
  int bw = 160;
  int fw = (int)val * bw / maxBar;
  if (fw > bw) fw = bw;
  gfx->fillRoundRect(150, y + 2, bw, 11, 3, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(150, y + 2, fw, 11, 3, color);
}

// ---------- ajuste de hora en pantalla (deslizar abajo) ----------
// El usuario pone su hora LOCAL a ojo; el firmware la usa tal cual, asi que
// no hay que gestionar zona horaria. Preserva el dia (no rompe racha/edad).

void openClock() {
  uint32_t e = pet.lastSeenEpoch ? pet.lastSeenEpoch : rtcEpoch();
  clockH = (e / 3600) % 24;
  clockM = (e / 60) % 60;
  clockOpen = true;
  clockDirty = true;
  lockTouchBrief();
  sfxPlay(SFX_MENU);
}

void applyClock() {
  uint32_t base = pet.lastSeenEpoch ? pet.lastSeenEpoch : rtcEpoch();
  uint32_t e = (base / 86400) * 86400 + (uint32_t)clockH * 3600 + (uint32_t)clockM * 60;
  rtcSetEpoch(e);
  pet.setClock(e);
  idleResetClock();
  idleAnchorEpoch = e;
  clockOpen = false;
  markUiDirty();
  lockTouchBrief();
}

void drawClockBtn(int x, int y, const char *l) {
  gfx->fillRoundRect(x, y, 58, 58, 12, UI_WHITE);
  gfx->drawRoundRect(x, y, 58, 58, 12, UI_INK);
  gfx->setTextColor(UI_INK);
  setSize(4);
  setCur(x + (58 - textW(l, 4)) / 2, y + 15);
  printT(l);
}

// Pildoras de ajustes. La fila de arriba es la nueva (modo facil + velocidad);
// la de abajo conserva sonido, ahorro e idioma donde estaban.
#define EZ_PILL_Y 244
#define EASY_PILL_X 24
#define EASY_PILL_W 200
#define SPEED_PILL_X 236
#define SPEED_PILL_W 196
#define LANG_PILL_Y 284
#define LANG_PILL_H 30
#define LANG_PILL_X 336          // pildora de idioma (cicla EN/KO al tocar)
#define LANG_PILL_W 96
#define SOUND_PILL_X 24
#define SOUND_PILL_W 116
#define PSAVE_PILL_X 150
#define PSAVE_PILL_W 176

const char *soundModeLabel() {
  switch (audioMode()) {
    case SOUND_FULL: return T(S_SND_FULL);
    case SOUND_MED: return T(S_SND_MED);
    case SOUND_LOW: return T(S_SND_LOW);
    default: return T(S_SND_OFF);
  }
}

uint8_t nextSoundMode() {
  switch (audioMode()) {
    case SOUND_FULL: return SOUND_MED;
    case SOUND_MED: return SOUND_LOW;
    case SOUND_LOW: return SOUND_OFF;
    default: return SOUND_FULL;
  }
}
static const char *const LANG_CODES[LANG_COUNT] = { "EN", "KO" };

const char *powerSaveLabel() {
  return powerSave ? T(S_PSAVE_ON) : T(S_PSAVE_OFF);
}

void drawStatusLine(int y, const char *label, const char *value, uint16_t valueColor) {
  setSize(1);
  gfx->setTextColor(UI_TRACK);
  setCur(94, y);
  printT(label);
  gfx->setTextColor(valueColor);
  setCur(172, y);
  printT(value);
}

static const char *const HELP_WORD[LANG_COUNT] = { "HELP", "HELP" };
static const char *const HELP_OK[LANG_COUNT] = { "OK", "OK" };

// La ayuda se queda en ingles (fuera del alcance de la traduccion); al no
// depender del idioma ahorra las otras cinco copias en flash.
static const char *const HELP_TITLES[HELP_PAGE_COUNT] = { "IDLE", "GESTURES", "DEX" };

static const char *const HELP_LINES[HELP_PAGE_COUNT][HELP_LINE_COUNT] = {
    { "One form lives one hour.", "Then it evolves.", "Branches are random.", "Final form: new egg.", "Speed 1/2/4/8.", "Offline counts up to 7 days." },
    { "Swipe left: Pokedex.", "Swipe right: settings.", "Hold screen: settings.", "Tap pet: reaction.", "Short PWR: screen off.", "Hold BOOT 2s: hub." },
    { "Evolve to register.", "Random branches fill it.", "Shiny is random.", "Tap a cell for detail.", "Swipe to page.", "Short BOOT: back." },
  };

void renderHelp() {
  helpDirty = false;
  gfx->fillScreen(UI_BG_DAY);
  if (helpPage >= HELP_PAGE_COUNT) helpPage = 0;

  gfx->setTextColor(UI_INK);
  setSize(3);
  const char *h = HELP_WORD[gLang];
  setCur(CX - textW(h, 3) / 2, 36);
  printT(h);

  setSize(2);
  gfx->setTextColor(UI_BAR_BAD);
  const char *title = HELP_TITLES[helpPage];
  setCur(CX - textW(title, 2) / 2, 76);
  printT(title);

  gfx->setTextColor(UI_INK);
  for (uint8_t i = 0; i < HELP_LINE_COUNT; i++) {
    const char *line = HELP_LINES[helpPage][i];
    setCur(CX - textW(line, 2) / 2, 116 + i * 34);
    printT(line);
  }

  char pg[12];
  snprintf(pg, sizeof(pg), "%u/%u", helpPage + 1, HELP_PAGE_COUNT);
  gfx->setTextColor(UI_TRACK);
  setCur(CX - textW(pg, 2) / 2, 340);
  printT(pg);

  if (helpPage > 0) {
    gfx->fillRoundRect(48, 398, 82, 42, 12, UI_WHITE);
    gfx->drawRoundRect(48, 398, 82, 42, 12, UI_INK);
    gfx->setTextColor(UI_INK);
    setSize(2);
    setCur(72, 412);
    printT("<<");
  }
  gfx->fillRoundRect(154, 398, 158, 42, 12, UI_BAR_OK);
  gfx->setTextColor(UI_BG_DAY);
  setSize(2);
  const char *ok = HELP_OK[gLang];
  setCur(154 + (158 - textW(ok, 2)) / 2, 412);
  printT(ok);
  if (helpPage + 1 < HELP_PAGE_COUNT) {
    gfx->fillRoundRect(336, 398, 82, 42, 12, UI_WHITE);
    gfx->drawRoundRect(336, 398, 82, 42, 12, UI_INK);
    gfx->setTextColor(UI_INK);
    setCur(360, 412);
    printT(">>");
  }
  flushStampedFrame();
}

void openHelp() {
  helpPage = 0;
  helpOpen = true;
  clockOpen = false;
  helpDirty = true;
  lockTouchBrief();
  sfxPlay(SFX_MENU);
}

void helpTap(int16_t x, int16_t y) {
  if (y >= 392 && y <= 448) {
    if (x >= 48 && x <= 130 && helpPage > 0) {
      helpPage--;
      helpDirty = true;
      sfxPlay(SFX_MENU);
      return;
    }
    if (x >= 336 && x <= 418 && helpPage + 1 < HELP_PAGE_COUNT) {
      helpPage++;
      helpDirty = true;
      sfxPlay(SFX_MENU);
      return;
    }
    if (x >= 154 && x <= 312) {
      helpOpen = false;
      clockOpen = true;
      clockDirty = true;
      lockTouchBrief();
      sfxPlay(SFX_TAP);
      return;
    }
  }
  if (x > CX && helpPage + 1 < HELP_PAGE_COUNT) { helpPage++; helpDirty = true; sfxPlay(SFX_MENU); return; }
  if (x < CX && helpPage > 0) { helpPage--; helpDirty = true; sfxPlay(SFX_MENU); return; }
}

void renderClock() {
  clockDirty = false;
  gfx->fillScreen(UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(CX - textW("SETTINGS", 3) / 2, 40);
  printT("SETTINGS");

  char t[8];
  snprintf(t, sizeof(t), "%02d:%02d", clockH, clockM);
  setSize(7);
  setCur(CX - 105, 96);
  printT(t);

  // "-H/+H/-M/+M" sustituye a las etiquetas HOUR/MIN: libera la franja que
  // ahora ocupa la fila del modo facil y la velocidad, y sigue diciendo cual
  // boton mueve cada campo.
  drawClockBtn(104, 170, "-H");  // hora -
  drawClockBtn(170, 170, "+H");  // hora +
  drawClockBtn(252, 170, "-M");  // min -
  drawClockBtn(318, 170, "+M");  // min +

  // evolution speed: 1x -> 2x -> 4x -> 8x
  char sp[18];
  snprintf(sp, sizeof(sp), T(S_SPEED_FMT), (unsigned)gameSpeed);
  gfx->fillRoundRect(SPEED_PILL_X, EZ_PILL_Y, SPEED_PILL_W, LANG_PILL_H, 8, gameSpeed > 1 ? UI_BAR_WARN : UI_WHITE);
  gfx->drawRoundRect(SPEED_PILL_X, EZ_PILL_Y, SPEED_PILL_W, LANG_PILL_H, 8, UI_INK);
  gfx->setTextColor(gameSpeed > 1 ? UI_BG_DAY : UI_INK);
  setSize(2);
  setCur(SPEED_PILL_X + (SPEED_PILL_W - textW(sp, 2)) / 2, EZ_PILL_Y + 7);
  printT(sp);

  // release: first tap arms, second tap within 4 s replaces the creature with
  // a fresh egg.  The real TamaPoke save is in another namespace, so this only
  // resets the idle run.
  bool armed = releaseArmUntil && deadlineActive(millis(), releaseArmUntil);
  const char *rl = armed ? "RELEASE?" : "RELEASE";
  gfx->fillRoundRect(EASY_PILL_X, EZ_PILL_Y, EASY_PILL_W, LANG_PILL_H, 8, armed ? UI_BAR_BAD : UI_WHITE);
  gfx->drawRoundRect(EASY_PILL_X, EZ_PILL_Y, EASY_PILL_W, LANG_PILL_H, 8, armed ? UI_BAR_BAD : UI_INK);
  gfx->setTextColor(armed ? UI_WHITE : UI_BAR_BAD);
  setSize(2);
  setCur(EASY_PILL_X + (EASY_PILL_W - textW(rl, 2)) / 2, EZ_PILL_Y + 7);
  printT(rl);

  // selector de sonido: mucho / medio / poco / apagado
  uint8_t sndMode = audioMode();
  const char *sl = soundModeLabel();
  gfx->fillRoundRect(SOUND_PILL_X, LANG_PILL_Y, SOUND_PILL_W, LANG_PILL_H, 8, sndMode ? UI_BAR_OK : UI_WHITE);
  gfx->drawRoundRect(SOUND_PILL_X, LANG_PILL_Y, SOUND_PILL_W, LANG_PILL_H, 8, UI_INK);
  gfx->setTextColor(sndMode ? UI_BG_DAY : UI_INK);
  setSize(2);
  setCur(SOUND_PILL_X + (SOUND_PILL_W - textW(sl, 2)) / 2, LANG_PILL_Y + 7);
  printT(sl);

  // selector de ahorro opcional: apagado por defecto, persistente
  const char *pl = powerSaveLabel();
  gfx->fillRoundRect(PSAVE_PILL_X, LANG_PILL_Y, PSAVE_PILL_W, LANG_PILL_H, 8, powerSave ? UI_BAR_WARN : UI_WHITE);
  gfx->drawRoundRect(PSAVE_PILL_X, LANG_PILL_Y, PSAVE_PILL_W, LANG_PILL_H, 8, UI_INK);
  gfx->setTextColor(powerSave ? UI_BG_DAY : UI_INK);
  setSize(2);
  setCur(PSAVE_PILL_X + (PSAVE_PILL_W - textW(pl, 2)) / 2, LANG_PILL_Y + 7);
  printT(pl);

  // selector de idioma: una pildora que cicla English / 한국어 al tocar
  gfx->fillRoundRect(LANG_PILL_X, LANG_PILL_Y, LANG_PILL_W, LANG_PILL_H, 8, UI_WHITE);
  gfx->drawRoundRect(LANG_PILL_X, LANG_PILL_Y, LANG_PILL_W, LANG_PILL_H, 8, UI_INK);
  char lp[10];
  snprintf(lp, sizeof(lp), "%s >", LANG_CODES[gLang]);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(LANG_PILL_X + (LANG_PILL_W - textW(lp, 2)) / 2, LANG_PILL_Y + 7);
  printT(lp);

  // Round-safe info block: two short centred lines instead of the old
  // left-aligned debug rows (they ran into the bezel at the bottom corners).
  char info1[40];
  if (pet.isEgg()) snprintf(info1, sizeof(info1), "EGG");
  else snprintf(info1, sizeof(info1), "#%03d %s", pet.speciesId, dexName(pet.speciesId));
  gfx->setTextColor(UI_INK);
  setSize(1);
  setCur(CX - textW(info1, 1) / 2, 330);
  printT(info1);

  char info2[44];
  snprintf(info2, sizeof(info2), "SAVE %s  SD %s  v%s",
           pet.saveLoadedFromNvs ? "OK" : "NEW", sdReady ? "OK" : "NO", FW_VERSION);
  gfx->setTextColor(UI_TRACK);
  setCur(CX - textW(info2, 1) / 2, 344);
  printT(info2);

  gfx->fillRoundRect(120, 378, 112, 44, 13, UI_WHITE);
  gfx->drawRoundRect(120, 378, 112, 44, 13, UI_INK);
  gfx->setTextColor(UI_INK);
  setSize(2);
  const char *hw = HELP_WORD[gLang];
  setCur(120 + (112 - textW(hw, 2)) / 2, 391);
  printT(hw);

  const char *ok = T(S_OK);
  gfx->fillRoundRect(242, 378, 112, 44, 13, UI_BAR_OK);
  gfx->setTextColor(UI_BG_DAY);
  setSize(3);
  setCur(242 + (112 - textW(ok, 3)) / 2, 388);
  printT(ok);
  flushStampedFrame();
}

void clockTap(int16_t x, int16_t y) {
  if (y >= 170 && y <= 228) {  // fila de botones +/-
    if (x >= 104 && x < 162) clockH = (clockH + 23) % 24;
    else if (x >= 170 && x < 228) clockH = (clockH + 1) % 24;
    else if (x >= 252 && x < 310) clockM = (clockM + 59) % 60;
    else if (x >= 318 && x < 376) clockM = (clockM + 1) % 60;
    clockDirty = true;
    return;
  }
  if (y >= EZ_PILL_Y && y <= EZ_PILL_Y + LANG_PILL_H) {
    if (x >= EASY_PILL_X && x < EASY_PILL_X + EASY_PILL_W) {
      if (releaseArmUntil && deadlineActive(millis(), releaseArmUntil)) {
        pet.newEgg();
        idleAccum = 0;
        releaseArmUntil = 0;
        idleToast[0] = 0;
        markUiDirty();
        clockDirty = true;
        sfxPlay(SFX_HATCH);
      } else {
        releaseArmUntil = millis() + 4000;
        clockDirty = true;
        sfxPlay(SFX_DENY);
      }
      return;
    }
    if (x >= SPEED_PILL_X && x < SPEED_PILL_X + SPEED_PILL_W) {
      setGameSpeed(nextSpeed(gameSpeed));
      clockDirty = true;
      sfxPlay(SFX_MENU);
      return;
    }
  }
  if (y >= LANG_PILL_Y && y <= LANG_PILL_Y + LANG_PILL_H) {
    if (x >= SOUND_PILL_X && x < SOUND_PILL_X + SOUND_PILL_W) {
      audioSetMode(nextSoundMode());
      clockDirty = true;
      if (audioEnabled()) sfxPlay(SFX_LEVEL);  // confirma el nuevo modo si no esta apagado
      return;
    }
    if (x >= PSAVE_PILL_X && x < PSAVE_PILL_X + PSAVE_PILL_W) {
      setPowerSave(!powerSave);
      clockDirty = true;
      sfxPlay(powerSave ? SFX_MENU : SFX_TAP);
      return;
    }
    if (x >= LANG_PILL_X && x < LANG_PILL_X + LANG_PILL_W) {  // cicla idioma
      setLang((Lang)((gLang + 1) % LANG_COUNT));
      applyLangFont();
      clockDirty = true;
      sfxPlay(SFX_TAP);
      return;
    }
  }
  if (y >= 378 && y <= 422 && x >= 120 && x <= 232) { openHelp(); return; }
  if (y >= 378 && y <= 422 && x >= 242 && x <= 354) { applyClock(); return; }
}

// llama + numero de racha arriba a la izquierda
void drawStreakBadge() {
  if (pet.streak < 1) return;
  int x = 26, y = 16;
  gfx->fillTriangle(x + 8, y, x + 1, y + 17, x + 15, y + 17, UI_BAR_BAD);
  gfx->fillTriangle(x + 8, y + 7, x + 4, y + 17, x + 12, y + 17, UI_BAR_WARN);
  char s[6];
  snprintf(s, sizeof(s), "%u", pet.streak);
  gfx->setTextColor(inkColor());
  setSize(2);
  setCur(x + 22, y + 2);
  printT(s);
}

// banner temporal: medalla nueva o hito de racha
void drawCelebration() {
  const char *l1 = nullptr, *l2 = nullptr;
  char buf[20];
  if (pet.showMedal()) {
    for (int i = 0; i < MED_COUNT; i++)
      if (pet.newMedal & (1 << i)) { l2 = medalName(i); break; }
    l1 = T(S_MEDAL_BANNER);
  } else if (pet.showDexReward()) {
    l1 = T(S_NEW_FRAME);
    l2 = T((StrId)(S_RANK_TRAINER + pet.collectionRank()));
  } else if (pet.showMilestone()) {
    snprintf(buf, sizeof(buf), T(S_STREAK_DAYS_FMT), pet.streak);
    l1 = T(S_GREAT);
    l2 = buf;
  } else if (pet.showStepReward()) {
    l1 = T(S_STEP_REWARD);
    if (pet.lastStepReward() == STEP_REWARD_TRAIL_RANK) {
      snprintf(buf, sizeof(buf), T(S_TRAIL_RANK_FMT), pet.stepTrailRank());
      l2 = buf;
    } else if (pet.lastStepReward() == STEP_REWARD_ENERGY) {
      l2 = T(S_ITEM_ENERGY);
    } else if (pet.lastStepReward() == STEP_REWARD_TRAIN) {
      l2 = T(S_ITEM_TRAIN);
    } else {
      l2 = T(S_ITEM_SNACK);
    }
  }
  if (!l1) return;
  gfx->fillRoundRect(73, 150, 320, 96, 16, UI_BAR_WARN);
  gfx->drawRoundRect(73, 150, 320, 96, 16, UI_INK);
  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(CX - textW(l1, 3) / 2, 176);
  printT(l1);
  setSize(2);
  setCur(CX - textW(l2, 2) / 2, 212);
  printT(l2);
}

uint16_t collectionFrameColor(uint8_t frame) {
  static const uint16_t COLORS[] = {
    UI_TRACK, UI_BAR_WARN, UI_BAR_OK, 0x4C98, UI_BAR_BAD, 0xF3B7, 0x7B5C,
    0x7BEF, 0xD452, 0xFD20, 0x051D,
  };
  return COLORS[frame < sizeof(COLORS) / sizeof(COLORS[0]) ? frame : 0];
}

void drawCaughtBattleMarker(int x, int y) {
  // Gleicher 16er-Pokeball wie im Minispiel, 2x neben den Typ-Chips (32 px).
  drawMap(SPR_ICON_PLAY, 16, x, y, 2, false);
}

static void drawFrameCorner(int x, int y, int sx, int sy, uint16_t color, uint8_t len) {
  gfx->drawLine(x, y, x + sx * len, y, color);
  gfx->drawLine(x, y, x, y + sy * len, color);
}

void drawCollectionFrame(uint8_t frame) {
  // Rahmen liegen hinter dem Portrait: Der Sprite bleibt voll sichtbar und
  // die frueheren ueberlagernden Kreise koennen nicht wieder auftauchen.
  if (frame >= pet.unlockedCollectionFrameCount()) frame = 0;
  const int left = 108, right = 358, top = 94, bottom = 302;
  uint16_t color = collectionFrameColor(frame);
  uint8_t len = 22 + frame * 3;
  drawFrameCorner(left, top, 1, 1, color, len);
  drawFrameCorner(right, top, -1, 1, color, len);
  drawFrameCorner(left, bottom, 1, -1, color, len);
  drawFrameCorner(right, bottom, -1, -1, color, len);

  if (frame >= 1) {
    uint16_t accent = frame == 4 ? UI_BAR_WARN : color;
    gfx->fillCircle(left + len + 7, top, 3, accent);
    gfx->fillCircle(right - len - 7, top, 3, accent);
    gfx->fillCircle(left + len + 7, bottom, 3, accent);
    gfx->fillCircle(right - len - 7, bottom, 3, accent);
  }
  if (frame >= 2) {
    uint8_t inner = len - 6;
    drawFrameCorner(left + 6, top + 6, 1, 1, color, inner);
    drawFrameCorner(right - 6, top + 6, -1, 1, color, inner);
    drawFrameCorner(left + 6, bottom - 6, 1, -1, color, inner);
    drawFrameCorner(right - 6, bottom - 6, -1, -1, color, inner);
  }
  if (frame >= 3) {
    gfx->fillRect(CX - 3, top - 3, 6, 6, color);
    gfx->fillRect(CX - 3, bottom - 3, 6, 6, color);
  }
  if (frame >= 4) {
    gfx->drawLine(left + 2, top + len + 10, left + 10, top + len + 2, UI_BAR_WARN);
    gfx->drawLine(right - 2, top + len + 10, right - 10, top + len + 2, UI_BAR_WARN);
    gfx->drawLine(left + 2, bottom - len - 10, left + 10, bottom - len - 2, UI_BAR_WARN);
    gfx->drawLine(right - 2, bottom - len - 10, right - 10, bottom - len - 2, UI_BAR_WARN);
  }
  if (frame >= 5) {
    gfx->fillCircle(CX, top - 12, 3, UI_BAR_WARN);
    gfx->fillCircle(CX, bottom + 12, 3, UI_BAR_WARN);
    gfx->fillCircle(left - 12, CY, 3, UI_BAR_WARN);
    gfx->fillCircle(right + 12, CY, 3, UI_BAR_WARN);
  }
}

// medallas en la ficha: badge con etiqueta, color si conseguida
void drawMedalBadge(int x, int y, int i) {
  bool got = pet.hasMedal(1 << i);
  gfx->fillRoundRect(x, y, 100, 24, 6, got ? UI_BAR_OK : UI_TRACK);
  if (!got) gfx->drawRoundRect(x, y, 100, 24, 6, UI_TRACK);
  gfx->setTextColor(got ? UI_BG_DAY : 0x9492);
  setSize(2);
  setCur(x + (100 - textW(medalLabel(i), 2)) / 2, y + 5);
  printT(medalLabel(i));
}

// pagina 0: perfil (retrato grande, identidad, racha, vinculo, baya)
void renderCardProfile() {
  const DexEntry &d = DEX_TBL[pet.speciesId];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  char head[40];
  snprintf(head, sizeof(head), T(S_NAME_FMT), pet.shiny ? "*" : "", nm, pet.level());
  gfx->setTextColor(d.accent);
  // auto-encoge: a tamano 3 los nombres largos no caben en la franja estrecha de
  // arriba de la pantalla redonda, asi que se cortaban por el borde
  setSize(3);
  int hts = (textW(head, 3) <= 198) ? 3 : 2;
  setSize(hts);
  setCur(CX - textW(head, hts) / 2, hts == 3 ? 34 : 40);
  printT(head);
  if (pet.nick[0]) {  // especie real bajo el apodo
    gfx->setTextColor(UI_TRACK);
    setSize(2);
    char sp[32];
    snprintf(sp, sizeof(sp), "(%s)", dexName(pet.speciesId));
    setCur(CX - textW(sp, 2) / 2, 64);
    printT(sp);
  }

  drawCollectionFrame(pet.collectionFrame);
  if (pmd.loaded) drawPmdAct(PMD_IDLE, CX, 206, millis(), true, false, 4);

  // racha con llama
  int sx = 138, sy = 224;
  gfx->fillTriangle(sx + 8, sy, sx + 1, sy + 18, sx + 15, sy + 18, UI_BAR_BAD);
  gfx->fillTriangle(sx + 8, sy + 7, sx + 4, sy + 18, sx + 12, sy + 18, UI_BAR_WARN);
  char rl[30];
  snprintf(rl, sizeof(rl), T(S_STREAK_FMT), pet.streak, pet.bestStreak);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(sx + 24, sy + 2);
  printT(rl);

  drawCardStat(258, T(S_VIN), pet.bond, 100, C565(0xd4, 0x52, 0x7e));

  const char *berry = !pet.berryKnown ? T(S_BERRY_UNK)
                      : pet.lovesBerry(0) ? T(S_BERRY_RED)
                      : pet.lovesBerry(1) ? T(S_BERRY_BLUE)
                                          : T(S_BERRY_GREEN);
  char info[40];
  snprintf(info, sizeof(info), T(S_INFO_FMT), berry,
           (unsigned long)(pet.ageMinutes / 1440));
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(CX - textW(info, 2) / 2, 296);
  printT(info);

  gfx->setTextColor(UI_TRACK);
  setCur(CX - textW(T(S_RENAME_HINT), 2) / 2, 326);
  printT(T(S_RENAME_HINT));

  const char *rank = T((StrId)(S_RANK_TRAINER + pet.collectionRank()));
  char known[22], frame[22];
  snprintf(known, sizeof(known), T(S_KNOWN_FMT), pet.knownDexCount(), DEX_COUNT);
  snprintf(frame, sizeof(frame), T(S_FRAME_FMT), pet.collectionFrame + 1, pet.unlockedCollectionFrameCount());
  gfx->setTextColor(UI_INK);
  setSize(1);
  setCur(CX - textW(T(S_COLLECTION), 1) / 2, 344);
  printT(T(S_COLLECTION));
  gfx->setTextColor(collectionFrameColor(pet.collectionFrame));
  setCur(CX - textW(rank, 1) / 2, 356);
  printT(rank);
  gfx->setTextColor(UI_TRACK);
  setCur(CX - textW(known, 1) / 2, 368);
  printT(known);

  gfx->fillRoundRect(74, 374, 52, 18, 6, UI_WHITE);
  gfx->drawRoundRect(74, 374, 52, 18, 6, UI_INK);
  gfx->fillRoundRect(340, 374, 52, 18, 6, UI_WHITE);
  gfx->drawRoundRect(340, 374, 52, 18, 6, UI_INK);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(94, 376);
  printT("<");
  setCur(360, 376);
  printT(">");
  setSize(1);
  setCur(CX - textW(frame, 1) / 2, 380);
  printT(frame);
  setSize(2);
}

StrId personalityNameId(PetPersonality p) {
  switch (p) {
    case PERS_PLAYFUL: return S_PERS_PLAYFUL;
    case PERS_BRAVE: return S_PERS_BRAVE;
    case PERS_CALM: return S_PERS_CALM;
    case PERS_LAZY: return S_PERS_LAZY;
    default: return S_PERS_BALANCED;
  }
}

StrId personalityHintId(PetPersonality p) {
  switch (p) {
    case PERS_PLAYFUL: return S_PERS_PLAYFUL_HINT;
    case PERS_BRAVE: return S_PERS_BRAVE_HINT;
    case PERS_CALM: return S_PERS_CALM_HINT;
    case PERS_LAZY: return S_PERS_LAZY_HINT;
    default: return S_PERS_BALANCED_HINT;
  }
}

uint16_t personalityColor(PetPersonality p) {
  switch (p) {
    case PERS_PLAYFUL: return UI_BAR_WARN;
    case PERS_BRAVE: return UI_BAR_BAD;
    case PERS_CALM: return 0x4C98;
    case PERS_LAZY: return 0xB3C8;
    default: return UI_BAR_OK;
  }
}

void drawPersonalityRecord(int x, int y, const char *label, uint16_t val, uint16_t color) {
  gfx->fillRoundRect(x, y, 118, 34, 8, UI_WHITE);
  gfx->drawRoundRect(x, y, 118, 34, 8, color);
  gfx->setTextColor(color);
  setSize(1);
  setCur(x + 10, y + 6);
  printT(label);
  char num[8];
  snprintf(num, sizeof(num), "%u", val);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(x + 118 - 12 - textW(num, 2), y + 14);
  printT(num);
}

// pagina 1: personalidad calculada + records largos, sin tocar balance
void renderCardPersonality() {
  PetPersonality pers = pet.personality();
  const char *title = T(S_PERSONALITY);
  const char *name = T(personalityNameId(pers));
  const char *hint = T(personalityHintId(pers));
  uint16_t col = personalityColor(pers);

  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(CX - textW(title, 3) / 2, 44);
  printT(title);

  gfx->fillRoundRect(62, 86, 342, 70, 16, col);
  gfx->setTextColor(UI_BG_DAY);
  setSize(3);
  int nts = (textW(name, 3) <= 180) ? 3 : 2;
  setSize(nts);
  setCur(CX - textW(name, nts) / 2, nts == 3 ? 104 : 111);
  printT(name);
  setSize(2);
  setCur(CX - textW(hint, 2) / 2, 136);
  printT(hint);

  drawCardStat(182, T(S_VIN), pet.bond, 100, C565(0xd4, 0x52, 0x7e));
  drawCardStat(220, T(S_BAR_JOY), pet.joy, 100, UI_BAR_WARN);

  char age[20];
  snprintf(age, sizeof(age), T(S_AGE_DAYS_FMT), (unsigned long)(pet.ageMinutes / 1440));
  gfx->setTextColor(UI_TRACK);
  setSize(2);
  setCur(CX - textW(age, 2) / 2, 258);
  printT(age);

  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(CX - textW(T(S_RECORDS), 2) / 2, 270);
  printT(T(S_RECORDS));
  drawPersonalityRecord(52, 294, T(S_GAME_BALL), pet.gameHi, UI_BAR_OK);
  drawPersonalityRecord(178, 294, T(S_GAME_CATCH), pet.catchHi, UI_BAR_WARN);
  drawPersonalityRecord(304, 294, T(S_GAME_MEMO), pet.memoHi, 0x4C98);
  drawPersonalityRecord(52, 334, T(S_GAME_CLEAN), pet.cleanHi, UI_BAR_OK);
  drawPersonalityRecord(178, 334, T(S_GAME_TYPE), pet.typeHi, 0xF3B7);
  drawPersonalityRecord(304, 334, T(S_BATTLE), pet.bestBattleStreak, UI_BAR_BAD);
}

StrId dailyGoalLabelId(uint8_t goalType) {
  switch (goalType) {
    case DAILY_GOAL_CARE: return S_GOAL_CARE;
    case DAILY_GOAL_PLAY: return S_GOAL_PLAY;
    case DAILY_GOAL_BATTLE: return S_GOAL_BATTLE;
    case DAILY_GOAL_CATCH: return S_GOAL_CATCH;
    case DAILY_GOAL_MEMO: return S_GOAL_MEMO;
    default: return S_GOAL_CARE;
  }
}

uint16_t dailyGoalColor(uint8_t goalType) {
  switch (goalType) {
    case DAILY_GOAL_CARE: return C565(0xd4, 0x52, 0x7e);
    case DAILY_GOAL_PLAY: return UI_BAR_WARN;
    case DAILY_GOAL_BATTLE: return UI_BAR_BAD;
    case DAILY_GOAL_CATCH: return UI_BAR_OK;
    case DAILY_GOAL_MEMO: return 0x4C98;
    default: return UI_INK;
  }
}

void drawDailyGoalRow(int y, uint8_t idx) {
  uint8_t type = pet.dailyGoalType[idx];
  uint8_t target = pet.dailyGoalTarget(type);
  uint8_t progress = pet.dailyGoalProgress[idx] > target ? target : pet.dailyGoalProgress[idx];
  bool done = pet.dailyGoalComplete(idx);
  uint16_t col = dailyGoalColor(type);
  gfx->fillRoundRect(58, y, 350, 52, 12, done ? col : UI_WHITE);
  gfx->drawRoundRect(58, y, 350, 52, 12, col);
  setSize(2);
  gfx->setTextColor(done ? UI_BG_DAY : UI_INK);
  setCur(82, y + 18);
  printT(T(dailyGoalLabelId(type)));

  char prog[12];
  snprintf(prog, sizeof(prog), "%u/%u", progress, target);
  setCur(286, y + 18);
  printT(done ? T(S_DONE) : prog);
  if (done) {
    gfx->fillCircle(374, y + 26, 12, UI_BG_DAY);
    gfx->setTextColor(col);
    setCur(368, y + 18);
    printT("v");
  }
}

// pagina 2: objetivos diarios
void renderCardDaily() {
  pet.ensureDailyGoals();
  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(CX - textW(T(S_DAILY), 3) / 2, 44);
  printT(T(S_DAILY));
  const char *phase = T(dayPhaseTextId(currentDayPhase()));
  gfx->setTextColor(UI_TRACK);
  setSize(2);
  setCur(CX - textW(phase, 2) / 2, 74);
  printT(phase);

  uint8_t done = 0;
  for (uint8_t i = 0; i < DAILY_GOAL_COUNT; i++) {
    if (pet.dailyGoalComplete(i)) done++;
    drawDailyGoalRow(104 + i * 70, i);
  }

  char bonus[24];
  snprintf(bonus, sizeof(bonus), "%s %u/%u", T(S_REWARD), done, DAILY_GOAL_COUNT);
  gfx->setTextColor(done == DAILY_GOAL_COUNT ? UI_BAR_OK : UI_TRACK);
  setSize(2);
  setCur(CX - textW(bonus, 2) / 2, 324);
  printT(bonus);
}

#define BOX_ROWS 5

bool boxComesBefore(int16_t a, int16_t b) {
  if (boxSort == 1) {
    const DexEntry &da = DEX_TBL[a];
    const DexEntry &db = DEX_TBL[b];
    if (da.type1 != db.type1) return da.type1 < db.type1;
    if (da.type2 != db.type2) return da.type2 < db.type2;
  } else if (boxSort == 2) {
    bool ra = pet.isRegistered(a);
    bool rb = pet.isRegistered(b);
    if (ra != rb) return ra;
  }
  return a < b;
}

uint16_t boxBuildList(int16_t *out) {
  uint16_t n = 0;
  for (int16_t dex = 1; dex <= DEX_COUNT; dex++)
    if (pet.isCaught(dex)) out[n++] = dex;
  for (uint16_t i = 1; i < n; i++) {
    int16_t v = out[i];
    int j = i - 1;
    while (j >= 0 && boxComesBefore(v, out[j])) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = v;
  }
  return n;
}

uint8_t boxPageCount() {
  uint16_t count = pet.caughtCount();
  uint8_t pages = (count + BOX_ROWS - 1) / BOX_ROWS;
  return pages > 0 ? pages : 1;
}

int16_t boxDexAt(uint16_t index) {
  int16_t list[DEX_COUNT];
  uint16_t n = boxBuildList(list);
  return index < n ? list[index] : 0;
}

const char *boxSortLabel() {
  if (boxSort == 1) return T(S_SORT_TYPE);
  if (boxSort == 2) return T(S_SORT_RAISED);
  return T(S_SORT_DEX);
}

void renderCardBox() {
  uint8_t pages = boxPageCount();
  if (boxPage >= pages) boxPage = pages - 1;

  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(CX - textW(T(S_BOX), 3) / 2, 42);
  printT(T(S_BOX));

  const char *sort = boxSortLabel();
  gfx->fillRoundRect(302, 62, 106, 28, 9, UI_WHITE);
  gfx->drawRoundRect(302, 62, 106, 28, 9, UI_INK);
  gfx->setTextColor(UI_INK);
  setSize(1);
  setCur(302 + (106 - textW(sort, 1)) / 2, 73);
  printT(sort);

  char caught[24], known[24], goal[22];
  snprintf(caught, sizeof(caught), T(S_CAUGHT_COUNT_FMT), pet.caughtCount(), DEX_COUNT);
  snprintf(known, sizeof(known), T(S_KNOWN_FMT), pet.knownDexCount(), DEX_COUNT);
  snprintf(goal, sizeof(goal), T(S_DEX_GOAL_FMT), pet.nextDexGoal());
  setSize(2);
  gfx->setTextColor(UI_INK);
  setCur(72, 70);
  printT(caught);
  gfx->setTextColor(UI_TRACK);
  setCur(72, 92);
  printT(known);
  setCur(258, 92);
  printT(goal);

  if (pet.caughtCount() == 0) {
    gfx->fillRoundRect(82, 178, 302, 72, 16, UI_WHITE);
    gfx->drawRoundRect(82, 178, 302, 72, 16, UI_TRACK);
    gfx->setTextColor(UI_TRACK);
    setSize(2);
    setCur(CX - textW(T(S_NO_CATCHES), 2) / 2, 207);
    printT(T(S_NO_CATCHES));
    return;
  }

  int16_t boxList[DEX_COUNT];
  uint16_t boxCount = boxBuildList(boxList);
  uint16_t first = (uint16_t)boxPage * BOX_ROWS;
  for (uint8_t i = 0; i < BOX_ROWS; i++) {
    uint16_t index = first + i;
    int16_t dex = index < boxCount ? boxList[index] : 0;
    if (dex <= 0) break;
    const DexEntry &d = DEX_TBL[dex];
    int y = 122 + i * 42;
    bool raised = pet.isRegistered(dex);
    gfx->fillRoundRect(58, y, 350, 34, 9, UI_WHITE);
    gfx->drawRoundRect(58, y, 350, 34, 9, d.accent);
    char name[24];
    snprintf(name, sizeof(name), "#%03d %s", dex, dexName(dex));
    setSize(2);
    int ts = textW(name, 2) <= 192 ? 2 : 1;
    setSize(ts);
    gfx->setTextColor(UI_INK);
    setCur(72, y + (ts == 2 ? 7 : 5));
    printT(name);
    char types[22];
    typeText(types, sizeof(types), d);
    setSize(1);
    gfx->setTextColor(battleTypeColor(d.type1));
    setCur(72, y + 24);
    printT(types);
    if (raised) {
      gfx->setTextColor(UI_BAR_OK);
      setCur(330, y + 15);
      printT(T(S_RAISED_MARK));
    }
  }
  gfx->fillRoundRect(76, 348, 94, 38, 11, boxPage > 0 ? UI_TRACK : C565(0xe4, 0xe8, 0xee));
  gfx->fillRoundRect(296, 348, 94, 38, 11, boxPage + 1 < pages ? UI_TRACK : C565(0xe4, 0xe8, 0xee));
  gfx->setTextColor(UI_BG_DAY);
  setSize(3);
  setCur(111, 357);
  printT("<");
  setCur(331, 357);
  printT(">");
  char pg[12];
  snprintf(pg, sizeof(pg), T(S_PAGE_FMT), boxPage + 1, pages);
  gfx->setTextColor(UI_TRACK);
  setSize(2);
  setCur(CX - textW(pg, 2) / 2, 360);
  printT(pg);
}

// pagina 3: combate (4 barras + botones)
void renderCardStats() {
  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(CX - textW(T(S_BATTLE), 3) / 2, 48);
  printT(T(S_BATTLE));

  drawCardStat(112, T(S_STAT_ATK), pet.atkStat(), 260, UI_BAR_BAD);
  drawCardStat(150, T(S_STAT_DEF), pet.defStat(), 260, 0x4C98);
  drawCardStat(188, T(S_STAT_SPE), pet.speStat(), 260, UI_BAR_WARN);
  drawCardStat(226, T(S_STAT_WGT), pet.weight, 100, 0xB3C8);

  char wl[20], bs[18], bb[16];
  snprintf(wl, sizeof(wl), T(S_WL_FMT), pet.battleWins, pet.battleLosses);
  snprintf(bs, sizeof(bs), T(S_BSTREAK_FMT), pet.battleStreak);
  snprintf(bb, sizeof(bb), T(S_BBEST_FMT), pet.bestBattleStreak);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(74, 266);
  printT(wl);
  setCur(210, 266);
  printT(bs);
  setCur(334, 266);
  printT(bb);

  gfx->fillRoundRect(96, 294, 274, 36, 11, 0x4C98);
  gfx->setTextColor(UI_BG_DAY);
  setSize(2);
  setCur(CX - textW(T(S_WILD_BATTLE), 2) / 2, 305);
  printT(T(S_WILD_BATTLE));

  // boton: saco de entrenamiento de fuerza
  gfx->fillRoundRect(96, 338, 274, 36, 11, UI_BAR_BAD);
  gfx->setTextColor(UI_BG_DAY);
  setSize(2);
  setCur(CX - textW(T(S_TRAIN_STR), 2) / 2, 349);
  printT(T(S_TRAIN_STR));
}

// pagina 2: medallas con etiqueta descriptiva
void renderCardMedals() {
  int got = 0;
  for (int i = 0; i < MED_COUNT; i++)
    if (pet.hasMedal(1 << i)) got++;
  char head[20];
  snprintf(head, sizeof(head), T(S_MEDALS_FMT), got, MED_COUNT);
  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(CX - textW(head, 3) / 2, 48);
  printT(head);

  for (int i = 0; i < MED_COUNT; i++) {
    int x = 28 + (i % 2) * 206, y = 104 + (i / 2) * 54;
    bool g = pet.hasMedal(1 << i);
    gfx->fillRoundRect(x, y, 196, 44, 10, g ? UI_BAR_OK : UI_TRACK);
    if (g) {  // marca de conseguida
      gfx->fillCircle(x + 22, y + 22, 11, UI_BG_DAY);
      gfx->setTextColor(UI_BAR_OK);
      setSize(2);
      setCur(x + 16, y + 13);
      printT("v");
    }
    gfx->setTextColor(g ? UI_BG_DAY : 0x8410);
    setSize(2);
    setCur(x + 44, y + 14);
    printT(medalDesc(i));
  }
}

// pagina 3: progreso (nivel, evolucion, descuidos) — saca a la luz mecanicas
// que antes eran invisibles (cuanto falta para subir/evolucionar y por que)
void renderCardProgress() {
  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(CX - textW(T(S_PROGRESS), 3) / 2, 44);
  printT(T(S_PROGRESS));

  // nivel grande
  char lv[10];
  snprintf(lv, sizeof(lv), T(S_LVL_FMT), pet.level());
  setSize(5);
  setCur(CX - textW(lv, 5) / 2, 86);
  printT(lv);

  // barra de progreso al siguiente nivel (1 nivel = 60 min de juego)
  uint8_t into = pet.ageMinutes % MINUTES_PER_LEVEL;
  int bx = 93, bw = 280, by = 158, bh = 22;
  gfx->fillRoundRect(bx, by, bw, bh, 6, UI_TRACK);
  int fw = (bw - 4) * into / MINUTES_PER_LEVEL;
  if (fw > 0) gfx->fillRoundRect(bx + 2, by + 2, fw, bh - 4, 5, UI_BAR_OK);
  char nx[26];
  snprintf(nx, sizeof(nx), T(S_NEXT_LVL_FMT), MINUTES_PER_LEVEL - into, pet.level() + 1);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(CX - textW(nx, 2) / 2, by + 32);
  printT(nx);

  // estado de evolucion
  gfx->setTextColor(UI_TRACK);
  setCur(CX - textW(T(S_EVO_LABEL), 2) / 2, 230);
  printT(T(S_EVO_LABEL));
  char evoBuf[28];
  const char *evo;
  uint16_t evoCol = UI_INK;
  if (!pet.hasEvolutionPath(pet.speciesId)) {
    evo = T(S_FINAL_FORM);
  } else {
    int needed = pet.evolutionRequiredLevel();
    if (pet.evolutionUnlocked()) {
      if (pet.canEvolveNow()) { evo = T(S_EVO_READY); evoCol = UI_BAR_OK; }
      else { evo = T(S_EVO_BLOCKED); evoCol = UI_BAR_BAD; }
    } else if (pet.level() >= needed) {
      evo = T(pet.evolutionNeedsMoreBond() ? S_EVO_NEED_BOND : S_EVO_BLOCKED);
      evoCol = UI_BAR_BAD;
    } else {
      snprintf(evoBuf, sizeof(evoBuf), T(S_EVO_IN_FMT), needed - pet.level());
      evo = evoBuf;
    }
  }
  gfx->setTextColor(evoCol);
  setCur(CX - textW(evo, 2) / 2, 256);
  printT(evo);

  // descuidos (retrasan la evolucion)
  char ms[24];
  snprintf(ms, sizeof(ms), T(S_MISTAKES_FMT), pet.careMistakes);
  gfx->setTextColor(pet.careMistakes > 0 ? UI_BAR_BAD : UI_INK);
  setCur(CX - textW(ms, 2) / 2, 312);
  printT(ms);
}

StrId stepRewardTextId(uint8_t index) {
  if (index == 0) return S_ITEM_SNACK;
  if (index == 1) return S_ITEM_ENERGY;
  return S_ITEM_TRAIN;
}

void renderCardSteps() {
  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(CX - textW(T(S_STEPS), 3) / 2, 44);
  printT(T(S_STEPS));

  char today[28], total[28];
  snprintf(today, sizeof(today), T(S_STEPS_TODAY_FMT), (unsigned long)pet.stepsToday);
  snprintf(total, sizeof(total), T(S_STEPS_TOTAL_FMT), (unsigned long)pet.stepsTotal);
  gfx->fillRoundRect(42, 76, 184, 58, 14, UI_WHITE);
  gfx->drawRoundRect(42, 76, 184, 58, 14, UI_BAR_OK);
  gfx->fillRoundRect(240, 76, 184, 58, 14, UI_WHITE);
  gfx->drawRoundRect(240, 76, 184, 58, 14, UI_BAR_WARN);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(42 + (184 - textW(today, 2)) / 2, 99);
  printT(today);
  setCur(240 + (184 - textW(total, 2)) / 2, 99);
  printT(total);

  char rank[24], shiny[24], catchBuf[24];
  snprintf(rank, sizeof(rank), T(S_TRAIL_RANK_FMT), pet.stepTrailRank());
  uint16_t units = pet.stepShinyChancePer4096();
  uint16_t denominator = units ? (uint16_t)((4096U + units / 2U) / units) : 0;
  snprintf(shiny, sizeof(shiny), T(S_SHINY_LUCK_FMT), denominator);
  snprintf(catchBuf, sizeof(catchBuf), T(S_CATCH_BONUS_FMT), pet.stepCatchBonus());
  gfx->setTextColor(UI_BAR_WARN);
  setCur(CX - textW(rank, 2) / 2, 158);
  printT(rank);
  gfx->setTextColor(UI_TRACK);
  setSize(1);
  setCur(76, 184);
  printT(shiny);
  setCur(300, 184);
  printT(catchBuf);

  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(CX - textW(T(S_DAILY), 2) / 2, 208);
  printT(T(S_DAILY));
  for (uint8_t i = 0; i < STEP_DAILY_GOAL_COUNT; i++) {
    uint32_t goal = pet.stepGoal(i);
    uint32_t progress = pet.stepsToday < goal ? pet.stepsToday : goal;
    bool done = pet.stepGoalComplete(i);
    int y = 224 + i * 48;
    uint16_t col = i == 0 ? UI_BAR_OK : (i == 1 ? UI_BAR_WARN : UI_BAR_BAD);
    gfx->fillRoundRect(48, y, 370, 38, 10, done ? col : UI_WHITE);
    gfx->drawRoundRect(48, y, 370, 38, 10, col);
    gfx->setTextColor(done ? UI_BG_DAY : UI_INK);
    setSize(1);
    char goalText[18], prog[18];
    snprintf(goalText, sizeof(goalText), T(S_STEP_GOAL_FMT), (unsigned long)goal);
    snprintf(prog, sizeof(prog), "%lu/%lu", (unsigned long)progress, (unsigned long)goal);
    const int ty = y + (38 - textH(1)) / 2;
    setCur(66, ty);
    printT(goalText);
    setCur(232, ty);
    printT(T(stepRewardTextId(i)));
    // alineado a la derecha: en coreano "5000/5000걸음" es mucho mas ancho que
    // "5000/5000" y a x fija se salia del recuadro por la derecha.
    const char *tail = done ? T(S_DONE) : prog;
    setCur(410 - textW(tail, 1), ty);
    printT(tail);
  }
}

StrId expeditionItemText(ExpeditionItem item) {
  switch (item) {
    case EXP_ITEM_SNACK: return S_ITEM_SNACK;
    case EXP_ITEM_ENERGY: return S_ITEM_ENERGY;
    case EXP_ITEM_CARE: return S_ITEM_CARE;
    case EXP_ITEM_TRAIN: return S_ITEM_TRAIN;
    default: return S_WAIT;
  }
}

StrId expeditionSiteText(ExpeditionSite site) {
  if (site == EXP_SITE_VOLCANO) return S_EXP_VOLCANO;
  if (site == EXP_SITE_FOREST) return S_EXP_FOREST;
  return S_EXP_COAST;
}

uint16_t expeditionSiteColor(ExpeditionSite site) {
  return site == EXP_SITE_VOLCANO ? UI_BAR_BAD : site == EXP_SITE_FOREST ? UI_BAR_OK : 0x4C98;
}

uint16_t expeditionItemColor(ExpeditionItem item) {
  switch (item) {
    case EXP_ITEM_SNACK: return UI_BAR_WARN;
    case EXP_ITEM_ENERGY: return 0x4C98;
    case EXP_ITEM_CARE: return UI_BAR_OK;
    case EXP_ITEM_TRAIN: return UI_BAR_BAD;
    default: return UI_TRACK;
  }
}

uint32_t expeditionNowEpoch() {
  uint32_t nowEpoch = rtcEpoch();
  return nowEpoch ? nowEpoch : pet.lastSeenEpoch;
}

static const int EXP_HUD_X = 310;
static const int EXP_HUD_Y = 106;
static const int EXP_HUD_W = 112;
static const int EXP_HUD_H = 34;

// The step counter sits in the top band, horizontally centred. It has no fill
// any more: just the level-coloured outline and the text, so it stops being the
// brightest thing on the screen. The expedition hint keeps the right side free.
static const int STEP_HUD_X = CX - 126 / 2;  // centered: 170
static const int STEP_HUD_Y = 24;
static const int STEP_HUD_W = 126;
static const int STEP_HUD_H = 22;

bool inStepsHudHit(int16_t x, int16_t y) {
  return x >= STEP_HUD_X && x <= STEP_HUD_X + STEP_HUD_W &&
         y >= STEP_HUD_Y && y <= STEP_HUD_Y + STEP_HUD_H;
}

void openStepsCard() {
  cardOpen = true;
  cardPage = 8;
  expeditionTrainChoiceOpen = false;
  cardDirty = true;
  lockTouchBrief();
  sfxPlay(SFX_MENU);
}

void drawStepsHud() {
  uint16_t color = pet.stepsToday >= 5000UL ? UI_BAR_WARN :
                   pet.stepsToday >= 500UL ? UI_BAR_OK : UI_TRACK;
  char label[24];
  snprintf(label, sizeof(label), T(S_STEPS_FMT), (unsigned long)pet.stepsToday);
  // No white fill: the pill was the brightest thing on the screen. Keep only
  // the level-coloured outline so the counter still reads on the scenery.
  gfx->drawRoundRect(STEP_HUD_X, STEP_HUD_Y, STEP_HUD_W, STEP_HUD_H, 8, color);
  gfx->setTextColor(inkColor());
  setSize(1);
  setCur(STEP_HUD_X + (STEP_HUD_W - textW(label, 1)) / 2,
         STEP_HUD_Y + (STEP_HUD_H - textH(1)) / 2);
  printT(label);
}

bool expeditionHudVisible() {
  if (pet.isEgg() || pet.ceremony || cardOpen || gameOpen || sackOpen || battleOpen ||
      galleryOpen || kbOpen || clockOpen || helpOpen) return false;
  if (gameMenuOpen || feedMenuUntil || confirmUntil || choiceKind || bathUntil ||
      petEventUntil || wildPromptUntil) return false;
  return pet.expeditionHudState(expeditionNowEpoch()) != EXP_HUD_HIDDEN;
}

bool inExpeditionHudHit(int16_t x, int16_t y) {
  return expeditionHudVisible() && x >= EXP_HUD_X && x <= EXP_HUD_X + EXP_HUD_W &&
         y >= EXP_HUD_Y && y <= EXP_HUD_Y + EXP_HUD_H;
}

void openExpeditionCard() {
  cardOpen = true;
  cardPage = 7;
  expeditionTrainChoiceOpen = false;
  cardDirty = true;
  lockTouchBrief();
  sfxPlay(SFX_MENU);
}

void drawExpeditionHud() {
  if (!expeditionHudVisible()) return;

  uint32_t nowEpoch = expeditionNowEpoch();
  ExpeditionHudState state = pet.expeditionHudState(nowEpoch);
  uint16_t color = state == EXP_HUD_READY ? UI_BAR_OK :
                   state == EXP_HUD_BAG ? UI_BAR_WARN : 0x4C98;
  char label[18];
  if (state == EXP_HUD_ACTIVE) {
    uint32_t left = (pet.expeditionEndEpoch - nowEpoch + 59UL) / 60UL;
    snprintf(label, sizeof(label), "%s %lum", T(expeditionSiteText((ExpeditionSite)pet.expeditionSite)), (unsigned long)left);
  } else if (state == EXP_HUD_READY) {
    snprintf(label, sizeof(label), "%s", T(S_EXP_READY));
  } else {
    snprintf(label, sizeof(label), "%s x%u", T(S_EXP_HUD_BAG), pet.expeditionItemCount());
  }

  gfx->fillRoundRect(EXP_HUD_X, EXP_HUD_Y, EXP_HUD_W, EXP_HUD_H, 8, UI_WHITE);
  gfx->drawRoundRect(EXP_HUD_X, EXP_HUD_Y, EXP_HUD_W, EXP_HUD_H, 8, color);
  gfx->fillCircle(EXP_HUD_X + 17, EXP_HUD_Y + 17, 9, color);
  gfx->setTextColor(UI_WHITE);
  setSize(1);
  setCur(EXP_HUD_X + 14, EXP_HUD_Y + 12);
  if (state == EXP_HUD_READY) printT("!");
  else if (state == EXP_HUD_BAG) printT("+");
  else printT(">");
  gfx->setTextColor(UI_INK);
  setSize(1);
  setCur(EXP_HUD_X + 31, EXP_HUD_Y + 13);
  printT(label);
}

void drawExpeditionItem(int x, int y, ExpeditionItem item) {
  const int w = 172, h = 54;
  uint8_t count = pet.itemCounts[item];
  uint16_t col = expeditionItemColor(item);
  gfx->fillRoundRect(x, y, w, h, 9, count ? UI_WHITE : C565(0xe4, 0xe8, 0xee));
  gfx->drawRoundRect(x, y, w, h, 9, count ? col : UI_TRACK);
  gfx->fillCircle(x + 22, y + 27, 12, count ? col : UI_TRACK);
  if (item == EXP_ITEM_ENERGY) gfx->fillRect(x + 20, y + 17, 5, 20, UI_WHITE);
  else if (item == EXP_ITEM_CARE) gfx->fillCircle(x + 22, y + 22, 4, UI_WHITE);
  else if (item == EXP_ITEM_TRAIN) gfx->fillRect(x + 16, y + 25, 12, 4, UI_WHITE);

  const char *label = T(expeditionItemText(item));
  setSize(1);
  gfx->setTextColor(count ? UI_INK : UI_TRACK);
  setCur(x + 42, y + 16);
  printT(label);
  char amount[6];
  snprintf(amount, sizeof(amount), "x%u", count);
  setSize(2);
  setCur(x + 136, y + 29);
  printT(amount);
}

void renderExpeditionTrainChoice() {
  gfx->fillRoundRect(58, 118, 350, 190, 14, UI_WHITE);
  gfx->drawRoundRect(58, 118, 350, 190, 14, UI_INK);
  const char *title = T(S_ITEM_TRAIN);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(CX - textW(title, 2) / 2, 136);
  printT(title);

  const StrId labels[3] = { S_TRAIN_ATK, S_TRAIN_DEF, S_TRAIN_SPE };
  const uint8_t values[3] = { pet.trAtk, pet.trDef, pet.trSpe };
  const uint16_t cols[3] = { UI_BAR_BAD, 0x4C98, UI_BAR_WARN };
  for (uint8_t i = 0; i < 3; i++) {
    int x = 74 + i * 108;
    bool usable = values[i] < 100;
    gfx->fillRoundRect(x, 172, 102, 66, 9, usable ? cols[i] : UI_TRACK);
    gfx->setTextColor(UI_BG_DAY);
    setSize(2);
    const char *label = T(labels[i]);
    setCur(x + (102 - textW(label, 2)) / 2, 184);
    printT(label);
    if (usable) {
      setSize(1);
      setCur(x + 37, 210);
      printT("+2");
    } else {
      const char *maxed = T(S_ITEM_MAXED);
      setSize(1);
      setCur(x + (102 - textW(maxed, 1)) / 2, 210);
      printT(maxed);
    }
  }
  gfx->setTextColor(UI_TRACK);
  setSize(2);
  setCur(CX - textW(T(S_BACK), 2) / 2, 268);
  printT(T(S_BACK));
}

void renderCardExpedition() {
  uint32_t nowEpoch = expeditionNowEpoch();
  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(CX - textW(T(S_EXPEDITION), 3) / 2, 42);
  printT(T(S_EXPEDITION));

  if (pet.expeditionReady(nowEpoch)) {
    char found[34];
    snprintf(found, sizeof(found), T(S_FOUND_ITEM_FMT), T(expeditionItemText((ExpeditionItem)pet.expeditionRewardItem)));
    gfx->setTextColor(UI_BAR_OK);
    setSize(2);
    setCur(CX - textW(found, 2) / 2, 78);
    printT(found);
    gfx->setTextColor(expeditionSiteColor((ExpeditionSite)pet.expeditionSite));
    setSize(1);
    setCur(CX - textW(T(expeditionSiteText((ExpeditionSite)pet.expeditionSite)), 1) / 2, 92);
    printT(T(expeditionSiteText((ExpeditionSite)pet.expeditionSite)));
    gfx->fillRoundRect(98, 98, 270, 48, 11, UI_BAR_OK);
    gfx->setTextColor(UI_BG_DAY);
    setSize(2);
    setCur(CX - textW(T(S_EXP_CLAIM), 2) / 2, 111);
    printT(T(S_EXP_CLAIM));
  } else if (pet.expeditionActive(nowEpoch)) {
    uint32_t left = (pet.expeditionEndEpoch - nowEpoch + 59UL) / 60UL;
    char back[28];
    snprintf(back, sizeof(back), T(S_EXP_IN_FMT), (unsigned)left);
    gfx->setTextColor(0x4C98);
    setSize(3);
    setCur(CX - textW(back, 3) / 2, 86);
    printT(back);
    gfx->setTextColor(UI_TRACK);
    setSize(1);
    setCur(CX - 90, 116);
    printT(T(S_WAIT));
  } else {
    const ExpeditionSite sites[3] = { EXP_SITE_COAST, EXP_SITE_VOLCANO, EXP_SITE_FOREST };
    const int siteXs[3] = { 50, 180, 310 };
    for (uint8_t i = 0; i < 3; i++) {
      bool selected = expeditionSiteChoice == sites[i];
      uint16_t col = expeditionSiteColor(sites[i]);
      gfx->fillRoundRect(siteXs[i], 60, 106, 28, 8, selected ? col : UI_WHITE);
      gfx->drawRoundRect(siteXs[i], 60, 106, 28, 8, col);
      gfx->setTextColor(selected ? UI_BG_DAY : UI_INK); setSize(1);
      const char *name = T(expeditionSiteText(sites[i]));
      setCur(siteXs[i] + (106 - textW(name, 1)) / 2, 70); printT(name);
    }
    char seals[20]; snprintf(seals, sizeof(seals), T(S_EXP_SEALS_FMT), pet.hoennSealCount());
    gfx->setTextColor(pet.hoennSealCount() == 3 ? UI_BAR_WARN : UI_TRACK); setSize(1);
    setCur(CX - textW(seals, 1) / 2, 90); printT(seals);
    const uint8_t minutes[3] = { 15, 30, 60 };
    const StrId labels[3] = { S_EXP_15, S_EXP_30, S_EXP_60 };
    const int xs[3] = { 50, 180, 310 };
    for (uint8_t i = 0; i < 3; i++) {
      uint8_t cost = Pet::expeditionEnergyCost(minutes[i]);
      bool available = pet.canStartExpedition(minutes[i], nowEpoch);
      uint16_t col = i == 0 ? UI_BAR_OK : i == 1 ? 0x4C98 : UI_BAR_BAD;
      gfx->fillRoundRect(xs[i], 100, 106, 54, 9, available ? col : UI_TRACK);
      gfx->setTextColor(UI_BG_DAY);
      setSize(2);
      const char *label = T(labels[i]);
      setCur(xs[i] + (106 - textW(label, 2)) / 2, 110);
      printT(label);
      char costText[12];
      snprintf(costText, sizeof(costText), "-%u ENE", cost);
      setSize(1);
      setCur(xs[i] + (106 - textW(costText, 1)) / 2, 136);
      printT(costText);
      char goalText[24]; snprintf(goalText, sizeof(goalText), T(S_EXP_WALK_PROGRESS), 0, Pet::expeditionStepGoal(minutes[i]));
      setCur(xs[i] + (106 - textW(goalText, 1)) / 2, 146); printT(goalText);
    }
    if (pet.expeditionInventoryFull()) {
      gfx->setTextColor(UI_BAR_BAD);
      setSize(1);
      setCur(CX - textW(T(S_INV_FULL), 1) / 2, 164);
      printT(T(S_INV_FULL));
    } else if (pet.energy < 12) {
      char need[24];
      snprintf(need, sizeof(need), T(S_NEED_ENE_FMT), 12);
      gfx->setTextColor(UI_BAR_BAD);
      setSize(1);
      setCur(CX - textW(need, 1) / 2, 164);
      printT(need);
    }
  }

  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(CX - textW(T(S_INVENTORY), 2) / 2, 184);
  printT(T(S_INVENTORY));
  if (pet.expeditionEndEpoch && pet.expeditionMinutes) {
    char progress[48];
    if (pet.expeditionWalkBonusEarned()) snprintf(progress, sizeof(progress), "%s", T(S_EXP_WALK_EARNED));
    else snprintf(progress, sizeof(progress), T(S_EXP_WALK_PROGRESS), pet.expeditionSteps, Pet::expeditionStepGoal(pet.expeditionMinutes));
    setSize(1); gfx->setTextColor(pet.expeditionWalkBonusEarned() ? UI_BAR_OK : UI_INK);
    setCur(CX - textW(progress, 1) / 2, 160); printT(progress);
  }
  drawExpeditionItem(50, 200, EXP_ITEM_SNACK);
  drawExpeditionItem(244, 200, EXP_ITEM_ENERGY);
  drawExpeditionItem(50, 264, EXP_ITEM_CARE);
  drawExpeditionItem(244, 264, EXP_ITEM_TRAIN);
  if (expeditionTrainChoiceOpen) renderExpeditionTrainChoice();
}

int8_t expeditionItemAt(int16_t x, int16_t y) {
  if ((y < 200 || y > 254) && (y < 264 || y > 318)) return -1;
  bool left = x >= 50 && x <= 222;
  bool right = x >= 244 && x <= 416;
  if (!left && !right) return -1;
  uint8_t row = y >= 264 ? 1 : 0;
  return row * 2 + (right ? 1 : 0);
}

void expeditionCardTap(int16_t x, int16_t y) {
  if (expeditionTrainChoiceOpen) {
    if (y >= 172 && y <= 238 && x >= 74 && x <= 398) {
      int8_t stat = (x - 74) / 108;
      if (stat > TRAIN_STAT_SPE || !pet.useExpeditionItem(EXP_ITEM_TRAIN, stat)) sfxPlay(SFX_DENY);
      else sfxPlay(SFX_ITEM_USE);
      expeditionTrainChoiceOpen = false;
      cardDirty = true;
      lockTouchBrief();
      return;
    }
    expeditionTrainChoiceOpen = false;
    cardDirty = true;
    sfxPlay(SFX_TAP);
    return;
  }
  if (y >= 396) {
    cardOpen = false;
    markUiDirty();
    lockTouchBrief();
    sfxPlay(SFX_TAP);
    return;
  }

  uint32_t nowEpoch = expeditionNowEpoch();
  if (pet.expeditionReady(nowEpoch)) {
    if (x >= 98 && x <= 368 && y >= 98 && y <= 146) {
      ExpeditionItem item = pet.claimExpedition(nowEpoch);
      if (item != EXP_ITEM_NONE) sfxPlay(SFX_EXPEDITION_CLAIM);
      else sfxPlay(SFX_DENY);
      cardDirty = true;
      lockTouchBrief();
    }
    return;
  }
  if (!pet.expeditionActive(nowEpoch) && y >= 60 && y <= 88) {
    int idx = x >= 50 && x <= 156 ? 0 : x >= 180 && x <= 286 ? 1 : x >= 310 && x <= 416 ? 2 : -1;
    if (idx >= 0) { expeditionSiteChoice = (ExpeditionSite)idx; cardDirty = true; sfxPlay(SFX_TAP); lockTouchBrief(); }
    return;
  }
  if (!pet.expeditionActive(nowEpoch) && y >= 100 && y <= 154) {
    const uint8_t minutes[3] = { 15, 30, 60 };
    int idx = x >= 50 && x <= 156 ? 0 : x >= 180 && x <= 286 ? 1 : x >= 310 && x <= 416 ? 2 : -1;
    if (idx >= 0 && pet.startExpedition(minutes[idx], nowEpoch, (uint8_t)random(100), (uint8_t)random(4), expeditionSiteChoice, (uint8_t)random(100))) {
      sfxPlay(SFX_EXPEDITION_START);
      cardDirty = true;
      lockTouchBrief();
    } else {
      sfxPlay(SFX_DENY);
    }
    return;
  }

  int8_t item = expeditionItemAt(x, y);
  if (item < 0) return;
  ExpeditionItem selected = (ExpeditionItem)item;
  if (pet.itemCounts[selected] == 0) {
    sfxPlay(SFX_DENY);
    return;
  }
  if (selected == EXP_ITEM_TRAIN) {
    expeditionTrainChoiceOpen = true;
    cardDirty = true;
    sfxPlay(SFX_MENU);
  } else if (pet.useExpeditionItem(selected)) {
    cardDirty = true;
    sfxPlay(SFX_ITEM_USE);
    lockTouchBrief();
  }
}

void renderCard() {
  cardDirty = false;
  gfx->fillScreen(UI_BG_DAY);
  if (cardPage == 0) renderCardProfile();
  else if (cardPage == 1) renderCardPersonality();
  else if (cardPage == 2) renderCardDaily();
  else if (cardPage == 3) renderCardBox();
  else if (cardPage == 4) renderCardStats();
  else if (cardPage == 5) renderCardMedals();
  else if (cardPage == 6) renderCardProgress();
  else if (cardPage == 7) renderCardExpedition();
  else renderCardSteps();

  // indicador de paginas + ayuda
  int dotsX = CX - ((CARD_COUNT - 1) * 24) / 2;
  for (int i = 0; i < CARD_COUNT; i++) {
    if (i == cardPage) gfx->fillCircle(dotsX + i * 24, 400, 5, UI_INK);
    else gfx->drawCircle(dotsX + i * 24, 400, 4, UI_INK);
  }
  gfx->setTextColor(UI_TRACK);
  setSize(2);
  setCur(CX - textW(T(S_BACK), 2) / 2, 420);
  printT(T(S_BACK));
  flushStampedFrame();
}

// ---------- teclado para renombrar ----------

static const char KB_KEYS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-";  // 28 + DEL + OK = 30
#define KB_COLS 6
#define KB_X 40
#define KB_Y 150
#define KB_W 64
#define KB_H 52

void openKeyboard() {
  kbOpen = true;
  keyboardDirty = true;
  lockTouchBrief();
  strncpy(nameBuf, pet.nick, sizeof(nameBuf) - 1);
  nameBuf[sizeof(nameBuf) - 1] = 0;
  nameLen = strlen(nameBuf);
  sfxPlay(SFX_MENU);
}

void renderKeyboard() {
  keyboardDirty = false;
  gfx->fillScreen(UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(CX - textW(T(S_NAME), 2) / 2, 56);
  printT(T(S_NAME));
  // buffer actual
  gfx->fillRoundRect(83, 84, 300, 40, 8, UI_WHITE);
  gfx->drawRoundRect(83, 84, 300, 40, 8, UI_INK);
  setSize(3);
  setCur(95, 94);
  printT(nameLen ? nameBuf : "_");

  for (int i = 0; i < 30; i++) {
    int x = KB_X + (i % KB_COLS) * KB_W, y = KB_Y + (i / KB_COLS) * KB_H;
    bool special = (i >= 28);
    gfx->fillRoundRect(x, y, KB_W - 6, KB_H - 6, 6, special ? UI_BAR_WARN : UI_WHITE);
    gfx->drawRoundRect(x, y, KB_W - 6, KB_H - 6, 6, UI_INK);
    gfx->setTextColor(UI_INK);
    setSize(2);
    if (i < 28) {
      setCur(x + KB_W / 2 - 9, y + KB_H / 2 - 10);
      printT(KB_KEYS[i]);
    } else {
      const char *lab = (i == 28) ? "<-" : "OK";
      setCur(x + KB_W / 2 - 15, y + KB_H / 2 - 10);
      printT(lab);
    }
  }
  flushStampedFrame();
}

void keyboardTap(int16_t x, int16_t y) {
  int col = (x - KB_X) / KB_W, row = (y - KB_Y) / KB_H;
  if (col < 0 || col >= KB_COLS || row < 0 || row >= 5) return;
  int i = row * KB_COLS + col;
  if (i >= 30) return;
  sfxPlay(SFX_TAP);
  if (i == 28) {  // borrar
    if (nameLen) nameBuf[--nameLen] = 0;
    keyboardDirty = true;
  } else if (i == 29) {  // OK
    pet.rename(nameBuf);
    kbOpen = false;
    cardDirty = true;
    lockTouchBrief();
  } else if (nameLen < sizeof(nameBuf) - 1) {
    nameBuf[nameLen++] = KB_KEYS[i];
    nameBuf[nameLen] = 0;
    keyboardDirty = true;
  }
}

// ---------- galeria pokedex ----------

#define GAL_X 73
#define GAL_Y 116
#define GAL_CELL 74

bool galleryDexVisible(int16_t dex) {
  if (dex < 1 || dex > DEX_COUNT) return false;
  if (galleryRegion == 1 && dex > 151) return false;
  if (galleryRegion == 2 && (dex < 152 || dex > 251)) return false;
  if (galleryRegion == 3 && dex < 252) return false;
  if (galleryFilter == 1) return pet.isRegistered(dex);
  if (galleryFilter == 2) return pet.isCaught(dex);
  if (galleryFilter == 3) return pet.isShinyRegistered(dex);
  return true;
}

uint16_t galleryFilteredCount() {
  if (galleryFilter == 0) return DEX_COUNT;
  uint16_t count = 0;
  for (int16_t dex = 1; dex <= DEX_COUNT; dex++)
    if (galleryDexVisible(dex)) count++;
  return count;
}

int galleryPageCount() {
  uint16_t count = galleryFilteredCount();
  int pages = (count + 15) / 16;
  return pages > 0 ? pages : 1;
}

int16_t galleryDexAt(uint16_t index) {
  for (int16_t dex = 1; dex <= DEX_COUNT; dex++) {
    if (!galleryDexVisible(dex)) continue;
    if (index == 0) return dex;
    index--;
  }
  return 0;
}

// dibuja una miniatura centrada en su celda; sil=true la pinta en tinta
void drawThumb(const uint8_t *b, int x, int y, int s, bool sil) {
  uint8_t w = b[0], h = b[1], n = b[2];
  const uint8_t *pal = b + 3;
  const uint8_t *d = pal + n * 2;
  int ox = x + (GAL_CELL - w * s) / 2;
  int oy = y + (GAL_CELL - h * s) / 2;
  for (int r = 0; r < h; r++) {
    for (int c = 0; c < w; c++) {
      uint8_t idx = d[r * w + c];
      if (idx == 0xFF) continue;
      uint16_t col = sil ? INK_K : (uint16_t)(pal[idx * 2] | (pal[idx * 2 + 1] << 8));
      gfx->fillRect(ox + c * s, oy + r * s, s, s, col);
    }
  }
}

void renderGallery() {
  if (galleryDetail) {  // vista detalle: se redibuja siempre (animada)
    gfx->fillScreen(UI_BG_DAY);
    const DexEntry &d = DEX_TBL[galleryDetail];
    bool reg = pet.isRegistered(galleryDetail);
    bool caught = pet.isCaught(galleryDetail);
    bool known = reg || caught;
    char head[24];
    snprintf(head, sizeof(head), "N.%03d %s%s", galleryDetail,
             pet.isShinyRegistered(galleryDetail) ? "*" : "", known ? dexName(galleryDetail) : "???");
    gfx->setTextColor(known ? d.accent : UI_INK);
    setSize(3);
    int gts = (textW(head, 3) <= 234) ? 3 : 2;  // auto-encoge nombres largos (no caben a t3)
    setSize(gts);
    setCur(CX - textW(head, gts) / 2, gts == 3 ? 56 : 60);
    printT(head);
    if (known) {
      char types[24];
      typeText(types, sizeof(types), d);
      gfx->setTextColor(battleTypeColor(d.type1));
      setSize(2);
      setCur(CX - textW(types, 2) / 2, 94);
      printT(types);
    }
    if (galleryPmd.loaded) {
      // animado y a color si se conoce; silueta estatica si no (estilo "?")
      drawPmdActM(galleryPmd, PMD_IDLE, CX, 300, known ? millis() : 0, true, !known, 6);
    } else {
      const uint8_t *t = thumbs.get(galleryDetail);
      if (t) drawThumb(t, CX - GAL_CELL, 135, 4, !known);
    }
    if (reg) {
      const char *mark = T(S_RAISED_MARK);
      gfx->setTextColor(UI_BAR_OK);
      setSize(2);
      setCur(CX - textW(mark, 2) / 2, caught ? 354 : 366);
      printT(mark);
    }
    if (caught) {
      const char *mark = T(S_CAUGHT_MARK);
      gfx->setTextColor(UI_BAR_WARN);
      setSize(2);
      setCur(CX - textW(mark, 2) / 2, reg ? 376 : 366);
      printT(mark);
    }
    gfx->setTextColor(UI_INK);
    setSize(2);
    setCur(CX - textW(T(S_DETAIL_BACK), 2) / 2, 408);
    printT(T(S_DETAIL_BACK));
    flushStampedFrame();
    return;
  }

  if (!galleryDirty) return;  // la rejilla es estatica
  galleryDirty = false;

  gfx->fillScreen(UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(CX - 7 * 9, 28);
  printT("POKEDEX");

  char head[28];
  snprintf(head, sizeof(head), T(S_RAISED_CAUGHT_FMT), pet.registeredCount(), pet.caughtCount());
  setSize(2);
  setCur(CX - textW(head, 2) / 2, 54);
  printT(head);

  const char *regions[4] = { T(S_FILTER_ALL), "KANTO", "JOHTO", "HOENN" };
  const char *filters[4] = { T(S_FILTER_ALL), T(S_RAISED_MARK), T(S_CAUGHT_MARK), "SHINY" };
  for (int i = 0; i < 4; i++) {
    int fx = 73 + i * 80;
    uint16_t fill = (galleryRegion == i) ? UI_INK : UI_WHITE;
    uint16_t text = (galleryRegion == i) ? UI_BG_DAY : UI_INK;
    gfx->fillRoundRect(fx, 68, 76, 16, 5, fill);
    gfx->drawRoundRect(fx, 68, 76, 16, 5, UI_INK);
    gfx->setTextColor(text); setSize(1);
    setCur(fx + (76 - textW(regions[i], 1)) / 2, 73); printT(regions[i]);
    fill = (galleryFilter == i) ? UI_INK : UI_WHITE;
    text = (galleryFilter == i) ? UI_BG_DAY : UI_INK;
    gfx->fillRoundRect(fx, 88, 76, 16, 5, fill);
    gfx->drawRoundRect(fx, 88, 76, 16, 5, UI_INK);
    gfx->setTextColor(text); setSize(1);
    setCur(fx + (76 - textW(filters[i], 1)) / 2, 93); printT(filters[i]);
  }
/*
  for (int i = 0; i < 3; i++) {
    int fx = 74 + i * 106;
    uint16_t fill = (galleryFilter == i) ? UI_INK : UI_WHITE;
    uint16_t text = (galleryFilter == i) ? UI_BG_DAY : UI_INK;
    gfx->fillRoundRect(fx, 74, 96, 18, 6, fill);
    gfx->drawRoundRect(fx, 74, 96, 18, 6, UI_INK);
    gfx->setTextColor(text);
    setSize(1);
    setCur(fx + (96 - textW(filters[i], 1)) / 2, 80);
    printT(filters[i]);
  }
*/

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      int16_t dex = galleryDexAt(galleryPage * 16 + r * 4 + c);
      if (dex <= 0) continue;
      int x = GAL_X + c * GAL_CELL, y = GAL_Y + r * GAL_CELL;
      const uint8_t *t = thumbs.get(dex);
      if (t) {
        bool reg = pet.isRegistered(dex);
        bool caught = pet.isCaught(dex);
        bool known = reg || caught;
        drawThumb(t, x, y, 2, !known);
        if (pet.isShinyRegistered(dex)) {
          gfx->setTextColor(UI_BAR_WARN);
          setSize(2);
          setCur(x + 62, y + 4);
          printT("*");
        } else if (caught && !reg) {
          gfx->setTextColor(UI_BAR_WARN);
          setSize(1);
          setCur(x + 60, y + 6);
          printT("C");
        }
      } else {
        char num[6];
        snprintf(num, sizeof(num), "%d", dex);
        gfx->setTextColor(UI_TRACK);
        setSize(2);
        setCur(x + 24, y + 32);
        printT(num);
      }
    }
  }
  // puntos de pagina
  int pages = galleryPageCount();
  int dotX = CX - (pages - 1) * 7;
  for (int i = 0; i < pages; i++) {
    if (i == galleryPage) gfx->fillCircle(dotX + i * 14, 436, 4, UI_INK);
    else gfx->drawCircle(dotX + i * 14, 436, 3, UI_INK);
  }
  flushStampedFrame();
}

void galleryTap(int16_t x, int16_t y) {
  if (galleryDetail) {  // volver a la rejilla
    galleryDetail = 0;
    galleryPmd.unload();
    galleryDirty = true;
    sfxPlay(SFX_TAP);
    return;
  }
  if (y < 46) {  // tocar la cabecera = salir
    galleryOpen = false;
    galleryPmd.unload();
    sfxPlay(SFX_TAP);
    return;
  }
  if (y >= 68 && y < GAL_Y) {
    int f = (x - 73) / 80;
    if (f >= 0 && f < 4 && x >= 73 + f * 80 && x <= 149 + f * 80) {
      if (y < 86) galleryRegion = (uint8_t)f;
      else {
      galleryFilter = (uint8_t)f;
      }
      galleryPage = 0;
      galleryDirty = true;
      sfxPlay(SFX_TAP);
      return;
    }
  }
  if (x < GAL_X || y < GAL_Y) return;
  int c = (x - GAL_X) / GAL_CELL, r = (y - GAL_Y) / GAL_CELL;
  if (c < 0 || c > 3 || r < 0 || r > 3) return;
  int16_t dex = galleryDexAt(galleryPage * 16 + r * 4 + c);
  if (dex <= 0) return;
  galleryDetail = dex;
  galleryPmd.load(dex, pet.isShinyRegistered(dex));
  sfxPlay(SFX_MENU);
  if (pet.isRegistered(dex) || pet.isCaught(dex)) speciesChirpPlay(dex);
}

void drawBattery() {
  int pc = batPercent();
  if (pc < 0) return;  // sin bateria conectada
  int x = CX - 14, y = 12, w = 24, h = 11;
  bool charging = batCharging();
  uint16_t col = charging ? UI_BAR_OK
                 : (pc >= 40) ? inkColor()
                 : (pc >= 15) ? UI_BAR_WARN
                              : UI_BAR_BAD;
  gfx->drawRoundRect(x, y, w, h, 2, col);
  gfx->fillRect(x + w, y + 3, 3, 5, col);  // borne
  if (charging) {
    // rayo de carga (zigzag) en vez de la barra de nivel
    uint16_t bolt = C565(0xff, 0xd9, 0x4a);
    int bx = x + w / 2;
    gfx->fillTriangle(bx + 3, y + 1, bx - 4, y + 6, bx + 1, y + 6, bolt);
    gfx->fillTriangle(bx - 1, y + 5, bx + 4, y + 5, bx - 3, y + 10, bolt);
  } else {
    int fw = (w - 4) * pc / 100;
    if (fw > 0) gfx->fillRect(x + 2, y + 2, fw, h - 4, col);
  }
}

void drawHeader(const char *name, uint16_t nameColor, const char *msg) {
  drawBattery();
  gfx->setTextColor(nameColor);
  setSize(3);
  setCur(CX - textW(name, 3) / 2, 52);
  printT(name);
  if (!msg || !msg[0]) return;  // the main screen draws its status below the clock
  gfx->setTextColor(inkColor());
  setSize(2);
  setCur(CX - textW(msg, 2) / 2, 90);
  printT(msg);
}

// Main-screen clock: mirrors TamaClock because both read the same PCF85063.
// The RTC is only polled every few seconds; the minutes tick locally in
// between so a 10 fps render does not hammer the I2C bus.
uint32_t mainClockEpoch() {
  static uint32_t baseEpoch = 0;
  static uint32_t baseMillis = 0;
  const uint32_t now = millis();
  if (!baseEpoch || now - baseMillis >= 5000UL) {
    const uint32_t e = rtcEpoch();
    if (e) {
      baseEpoch = e;
      baseMillis = now;
    }
  }
  if (!baseEpoch) return 0;
  return baseEpoch + (now - baseMillis) / 1000UL;
}

void drawMainClock() {
  uint32_t e = mainClockEpoch();
  char hm[8] = "--:--";
  if (e) {
    time_t tt = (time_t)e;
    struct tm t;
    gmtime_r(&tt, &t);
    snprintf(hm, sizeof(hm), "%02d:%02d", t.tm_hour, t.tm_min);
  }
  gfx->setTextColor(inkColor());
  setSize(4);
  setCur(CX - textW(hm, 4) / 2, 80);
  printT(hm);
}

// animacion de la ceremonia (10s): despedida = reverencia con corazones y se
// aleja caminando; escapada = se asusta y sale corriendo. Sustituye al idle.
void drawCeremony() {
  if (!pmd.loaded) { drawPet(); return; }  // respaldo si no hay sprite PMD
  uint32_t now = millis();
  float t = pet.ceremonyT();               // 0..1 a lo largo de los 10s
  bool panic = (pet.ceremony == CER_RUNAWAY);
  int x = CX, y = PET_GROUND;
  uint8_t act = PMD_IDLE;

  if (panic) {
    // final triste: penumbra azulada + lluvia
    for (int i = 0; i < 46; i++) {
      int rx = (i * 47 + now / 3) % 466;
      int ry = (i * 91 + now / 2) % 470;
      gfx->drawLine(rx, ry, rx - 3, ry + 12, C565(0x6a, 0x84, 0xb0));
    }
    bool fade = false;
    if (t < 0.30f) {                       // cabizbajo, temblando
      act = pmd.has(PMD_HURT) ? PMD_HURT : PMD_IDLE;
      x = CX + (int)(4 * sinf(now * 0.04f));
    } else {                               // se aleja despacio y se desvanece
      act = pmd.has(PMD_WALKL) ? PMD_WALKL : PMD_IDLE;
      x = CX - (int)(((t - 0.30f) / 0.70f) * (CX + 120));
      fade = (t > 0.6f) && ((now / 160) % 2 == 0);  // parpadea hacia la silueta
    }
    drawPmdAct(act, x, y, now, true, fade, 5);  // fade=silueta: se difumina al irse
    // lagrima cayendo del bicho
    if (t < 0.55f) {
      int ty = y - 150 + (int)((now / 6) % 40);
      gfx->fillRect(x + 6, ty, 3, 6, C565(0x9a, 0xc4, 0xe8));
    }
    return;
  }

  // despedida epica: halo dorado pulsante + chispas y corazones que ascienden
  int gcy = PET_GROUND - 96;
  for (int k = 0; k < 4; k++) {
    int r = 60 + k * 34 + (int)(10 * sinf(now * 0.02f));
    gfx->drawCircle(CX, gcy, r, C565(0xff, 0xdf, 0x8a));
  }
  for (int i = 0; i < 16; i++) {
    int px = (i * 71 + 28) % 466;
    int py = 410 - (int)((now / 8 + i * 70) % 360);   // suben y reaparecen abajo
    if (py < 30) continue;
    if (i % 4 == 0) drawMap(SPR_HEART, 32, px - 8, py - 8, 1, false);  // corazoncito
    else gfx->fillRect(px, py, 4, 4, (i % 2) ? C565(0xff, 0xe7, 0x9f) : C565(0xff, 0x9a, 0xc0));
  }

  if (t < 0.45f) {                         // reverencia / pose de despedida
    act = pmd.has(PMD_POSE) ? PMD_POSE : (pmd.has(PMD_NOD) ? PMD_NOD : PMD_IDLE);
  } else {                                 // se aleja por la derecha
    act = pmd.has(PMD_WALKR) ? PMD_WALKR : PMD_IDLE;
    x = CX + (int)(((t - 0.45f) / 0.55f) * (CX + 140));
  }
  drawPmdAct(act, x, y, now, true, false, 5);
  if (pet.showHeart())                     // corazon grande siguiendo al bicho
    drawMap(SPR_HEART, 32, x + 50, y - 190, 2, false);
}

// dialogo de decision (2 botones apilados): evolucionar/mantener o despedirse/quedaros
void drawChoiceDialog() {
  if (choiceKind == 3) {
    gfx->fillRoundRect(52, 96, 356, 350, 18, UI_WHITE);
    gfx->drawRoundRect(52, 96, 356, 350, 18, UI_INK);
    gfx->setTextColor(UI_INK);
    setSize(2);
    const char *title = T(S_EVO_Q);
    setCur(CX - textW(title, 2) / 2, 116);
    printT(title);
    uint8_t options = pet.evolutionOptionCount();
    for (uint8_t i = 0; i < options; i++) {
      int16_t target = pet.evolutionOption(i);
      int y = 158 + i * 45;
      gfx->fillRoundRect(82, y, 312, 36, 10, UI_BAR_OK);
      gfx->setTextColor(UI_WHITE);
      const char *name = dexName(target);
      setCur(CX - textW(name, 2) / 2, y + 11);
      printT(name);
    }
    gfx->fillRoundRect(82, 392, 312, 40, 10, UI_TRACK);
    gfx->setTextColor(UI_INK);
    const char *cancel = T(S_EVO_KEEP);
    setCur(CX - textW(cancel, 2) / 2, 404);
    printT(cancel);
    return;
  }
  const char *q, *o1, *o2;
  uint16_t c1, c2, t1, t2;
  if (choiceKind == 1) {  // evolucion
    q = T(S_EVO_Q); o1 = T(S_EVO_TAP); o2 = T(S_EVO_KEEP);
    c1 = UI_BAR_BAD; t1 = UI_WHITE; c2 = UI_TRACK; t2 = UI_INK;
  } else {                // despedida
    q = T(S_FAR_Q); o1 = T(S_FAR_GO); o2 = T(S_FAR_STAY);
    c1 = UI_BAR_WARN; t1 = UI_INK; c2 = UI_BAR_OK; t2 = UI_WHITE;
  }
  gfx->fillRoundRect(73, 156, 320, 188, 16, UI_WHITE);
  gfx->drawRoundRect(73, 156, 320, 188, 16, UI_INK);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(CX - textW(q, 2) / 2, 176);
  printT(q);
  gfx->fillRoundRect(93, 206, 280, 52, 12, c1);     // boton accion
  gfx->setTextColor(t1);
  setCur(CX - textW(o1, 2) / 2, 224);
  printT(o1);
  gfx->fillRoundRect(93, 268, 280, 52, 12, c2);     // boton mantener/quedaros
  gfx->setTextColor(t2);
  setCur(CX - textW(o2, 2) / 2, 286);
  printT(o2);
}

// boton-CTA rojo y grande para evolucionar (pulsa para llamar la atencion)
void drawEvolveButton() {
  uint32_t now = millis();
  int p = (int)(5 * sinf(now * 0.006f));  // late: -5..5
  int x = EVO_BTN_X - p, y = EVO_BTN_Y - p, w = EVO_BTN_W + 2 * p, h = EVO_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 18, UI_BAR_BAD);
  gfx->drawRoundRect(x, y, w, h, 18, UI_WHITE);
  gfx->drawRoundRect(x + 2, y + 2, w - 4, h - 4, 16, UI_WHITE);
  gfx->setTextColor(UI_WHITE);
  setSize(3);
  const char *t = T(S_EVO_TAP);
  setCur(CX - textW(t, 3) / 2, y + h / 2 - 11);
  printT(t);
}

// boton-CTA dorado de despedida: "<nombre> quiere decirte algo..."
void drawFarewellButton() {
  uint32_t now = millis();
  int p = (int)(4 * sinf(now * 0.005f));
  int x = FAR_BTN_X - p, y = FAR_BTN_Y - p, w = FAR_BTN_W + 2 * p, h = FAR_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 16, UI_BAR_WARN);
  gfx->drawRoundRect(x, y, w, h, 16, UI_INK);
  char buf[52];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  snprintf(buf, sizeof(buf), T(S_FAREWELL_BTN), nm);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(CX - textW(buf, 2) / 2, y + h / 2 - 8);
  printT(buf);
}

// boton-CTA sombrio de escapada por abandono: "<nombre> se siente abandonado..."
// (final triste: azul-gris oscuro, latido lento y apagado)
void drawRunawayButton() {
  uint32_t now = millis();
  int p = (int)(3 * sinf(now * 0.003f));
  int x = FAR_BTN_X - p, y = FAR_BTN_Y - p, w = FAR_BTN_W + 2 * p, h = FAR_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 16, C565(0x3a, 0x44, 0x5a));
  gfx->drawRoundRect(x, y, w, h, 16, C565(0x70, 0x80, 0x98));
  char buf[52];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  snprintf(buf, sizeof(buf), T(S_RUNAWAY_BTN), nm);
  gfx->setTextColor(C565(0xc8, 0xd2, 0xe0));
  setSize(2);
  setCur(CX - textW(buf, 2) / 2, y + h / 2 - 8);
  printT(buf);
}

// animacion epica de evolucion: halo radial + rayos giratorios + parpadeo del
// sprite acelerando + chispas que salen disparadas + fogonazo final
void drawEvolveFX(uint32_t now) {
  float t = pet.evolveT();          // 0..1
  int cx = CX, cy = PET_GROUND - 96;

  // halo radial que crece y pulsa
  int halo = 36 + (int)(t * 150) + (int)(8 * sinf(now * 0.02f));
  for (int k = 0; k < 4; k++) {
    int r = halo - k * 7;
    if (r > 0) gfx->drawCircle(cx, cy, r, UI_WHITE);
  }
  // rayos giratorios desde el centro del bicho
  float base = now * 0.004f;
  for (int i = 0; i < 12; i++) {
    float a = base + i * (float)(PI / 6);
    int len = 90 + (int)(70 * (0.5f + 0.5f * sinf(now * 0.012f + i)));
    gfx->drawLine(cx, cy, cx + (int)(cosf(a) * len), cy + (int)(sinf(a) * len), UI_WHITE);
  }
  // parpadeo entre la forma ANTERIOR y la NUEVA (siluetas), acelerando; al
  // final (t>0.9) se queda fija en la nueva para el fogonazo de revelado
  int period = 60 + (int)(220 * (1.0f - t));
  bool showOld = t < 0.9f && evoPmd.loaded && ((now / period) % 2) == 0;
  if (showOld) drawPmdActM(evoPmd, PMD_IDLE, cx, PET_GROUND, 0, true, true, 5);
  else drawPmdAct(PMD_IDLE, cx, PET_GROUND, 0, true, true, 5);
  // chispas que salen disparadas
  for (int i = 0; i < 10; i++) {
    float a = i * (float)(PI / 5) + t * 4.0f;
    int d = (int)((now / 14 + i * 33) % 200);
    int sx = cx + (int)(cosf(a) * d), sy = cy + (int)(sinf(a) * d);
    gfx->fillRect(sx - 2, sy - 2, 5, 5, (i & 1) ? C565(0xff, 0xe0, 0x70) : UI_WHITE);
  }
  // fogonazo final antes de revelar la forma nueva
  if (t > 0.9f) gfx->fillCircle(cx, cy, (int)(300 * (t - 0.9f) / 0.1f), UI_WHITE);
}

void drawPet() {
  if (pmd.loaded) {
    drawPetPMD();
    return;
  }
  if (mon.loaded) {
    drawPetSD();
    return;
  }
  int fi = flashIdxForDex(pet.speciesId);
  if (fi < 0) {
    // sin SD y sin sprite de flash: aviso claro de que faltan sprites
    gfx->setTextColor(inkColor());
    setSize(6);
    setCur(CX - 18, PET_CY - 80);
    printT("?");
    setSize(2);
    const char *l1 = T(S_NO_SPRITES);
    setCur(CX - textW(l1, 2) / 2, PET_CY - 4);
    printT(l1);
    const char *l2 = T(S_LOAD_SPRITES);
    setCur(CX - textW(l2, 2) / 2, PET_CY + 20);
    printT(l2);
    return;
  }
  const Species &sp = SPECIES[fi];
  int s = sp.scale;
  int x = CX - 16 * s;
  int y = PET_CY - 16 * s;

  // animacion de evolucion: alterna la silueta de la forma anterior y la nueva
  if (pet.evolving()) {
    bool flash = (millis() / 300) % 2;
    int16_t showDex = (flash && pet.prevSpeciesId >= 0) ? pet.prevSpeciesId : pet.speciesId;
    int sfi = flashIdxForDex(showDex);
    if (sfi >= 0) {
      const Species &show = SPECIES[sfi];
      drawMap(show.sprite, SPRITE_H, CX - 16 * show.scale, PET_CY - 16 * show.scale, show.scale, flash);
    }
    return;
  }

  PetMood m = pet.mood();
  if (m == MOOD_HAPPY && (millis() / 500) % 2) y -= 6;  // saltito

  drawMap(sp.sprite, SPRITE_H, x, y, s, false);

  // expresiones superpuestas usando las anclas de la especie
  bool blink = (millis() % 3500 < 300);
  if (m == MOOD_SLEEPING || blink) {
    overlayEye(sp, x, y, s, sp.eyeColL);
    overlayEye(sp, x, y, s, sp.eyeColR);
  }
  if (m == MOOD_EATING) overlayMouth(sp, x, y, s, true);
  else if (m == MOOD_SAD) overlayMouth(sp, x, y, s, false);

  if (pet.showHeart()) drawMap(SPR_HEART, 32, x + 20 * s, y - 2 * s, 2, false);
}

// ---------- escena de bano ----------

void startBath() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony || bathUntil) return;
  bathUntil = millis() + 3000;
  bathPending = true;
  sfxPlay(SFX_EVENT_SPARKLE);
  int cx = (int)beh.x;
  for (auto &b : bubbles) {
    b.x = cx - 70 + random(140);
    b.y = PET_GROUND - random(150);
    b.r = 8 + random(16);
    b.ph = random(64);
  }
}

void drawBath() {
  if (!bathUntil) return;
  uint32_t now = millis();
  if (deadlineReached(now, bathUntil)) {
    bathUntil = 0;
    if (bathPending) {
      bathPending = false;
      pet.clean();
      // pose de alegria al quedar limpio
      if (pmd.has(PMD_POSE)) {
        beh.mode = 2;
        beh.act = PMD_POSE;
        beh.t0 = now;
        beh.until = now + pmdActTotalMs(pmd.acts[PMD_POSE]) * 2;
      }
    }
    return;
  }
  uint32_t left = bathUntil - now;
  if (left > 800) {
    // espuma: pompas meciendose y subiendo poco a poco
    float t = now / 220.0f;
    for (auto &b : bubbles) {
      int bx = b.x + (int)(sinf(t + b.ph) * 6);
      int by = b.y - (int)((3000 - left) / 90);
      gfx->fillCircle(bx, by, b.r, UI_WHITE);
      gfx->drawCircle(bx, by, b.r, 0x7E3D);
      gfx->fillCircle(bx - b.r / 3, by - b.r / 3, b.r / 4, UI_BG_DAY);
    }
  } else {
    // las pompas revientan: destellos
    for (int i = 0; i < 8; i++) {
      auto &b = bubbles[i];
      int sx = b.x + (i % 3) * 6 - 6, sy = b.y - 18;
      uint16_t col = (i % 2) ? UI_BAR_WARN : UI_WHITE;
      gfx->fillRect(sx - 6, sy - 1, 13, 3, col);
      gfx->fillRect(sx - 1, sy - 6, 3, 13, col);
    }
  }
}

// ---------- mascota PMD: comportamiento ----------

uint32_t pmdActTotalMs(const PmdAct &a) {
  uint32_t t = 0;
  for (uint8_t i = 0; i < a.frames; i++) t += a.ms[i];
  return t ? t : 100;
}

uint8_t pmdFrameAt(const PmdAct &a, uint32_t t, bool loop) {
  uint32_t total = pmdActTotalMs(a);
  if (!loop && t >= total) return a.frames - 1;
  t %= total;
  uint8_t i = 0;
  while (t >= a.ms[i]) {
    t -= a.ms[i];
    i = (i + 1) % a.frames;
  }
  return i;
}

// dibuja una accion anclada por la base (centro-x, suelo) y devuelve su escala
// dibuja una accion de un PmdMon concreto (m); drawPmdAct usa el global pmd
void drawPmdActM(PmdMon &m, uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS) {
  const PmdAct &a = m.acts[actId];
  if (!a.frames) return;
  uint8_t sBase = m.acts[PMD_IDLE].h ? 170 / m.acts[PMD_IDLE].h : 5;
  if (sBase < 2) sBase = 2;
  if (sBase > maxS) sBase = maxS;
  uint8_t s = sBase;
  while (s > 2 && a.h * s > 250) s--;  // acciones con frame grande (ataque)
  uint8_t fi = pmdFrameAt(a, t, loop);
  const uint8_t *fr = a.data + (uint32_t)fi * a.w * a.h;
  // anclar por los pies (a.base), no por el alto del lienzo: asi las acciones
  // con padding distinto (Hurt, Eat...) quedan todas a la misma altura de suelo
  int x0 = cx - a.w * s / 2, y0 = groundY - (a.base ? a.base : a.h) * s;
  for (int r = 0; r < a.h; r++) {
    const uint8_t *row = fr + r * a.w;
    for (int c = 0; c < a.w; c++) {
      uint8_t idx = row[c];
      if (idx == 0xFF) continue;
      gfx->fillRect(x0 + c * s, y0 + r * s, s, s, sil ? INK_K : m.pal[idx]);
    }
  }
}
void drawPmdAct(uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS) {
  drawPmdActM(pmd, actId, cx, groundY, t, loop, sil, maxS);
}

// elige el siguiente capricho del bicho cuando esta contento
void behNext() {
  uint32_t now = millis();
  beh.t0 = now;
  int r = random(100);
  uint8_t walkChance = (currentDayPhase() == 3) ? 10 : 35;
  if (r < walkChance && (pmd.has(PMD_WALKL) || pmd.has(PMD_WALKR))) {
    beh.mode = 1;  // paseo
    beh.targetX = 150 + random(176);
    beh.until = now + 15000;
  } else if (r < 60) {
    // gesto aleatorio entre los disponibles
    // (Hop fuera: salta demasiado alto; Sit fuera: mira hacia atras)
    static const uint8_t flair[] = { PMD_POSE, PMD_NOD, PMD_BREATH };
    uint8_t pick[3], n = 0;
    for (uint8_t f : flair)
      if (pmd.has(f)) pick[n++] = f;
    if (n) {
      beh.mode = 2;
      beh.act = pick[random(n)];
      beh.until = now + pmdActTotalMs(pmd.acts[beh.act]);
      return;
    }
    beh.mode = 0;
    beh.until = now + 2000 + random(3000);
  } else {
    beh.mode = 0;  // mirar al frente
    beh.until = now + 2000 + random(3000);
  }
}

void drawPetPMD() {
  uint32_t now = millis();

  if (pet.evolving()) {
    drawEvolveFX(now);
    return;
  }
  if (evoPmd.loaded) evoPmd.unload();  // termino la evolucion: libera la forma anterior

  PetMood m = pet.mood();
  uint8_t act;
  bool loop = true;
  if (m == MOOD_SLEEPING && pmd.has(PMD_SLEEP)) {
    act = PMD_SLEEP;
    beh.mode = 0;
  } else if (m == MOOD_EATING && pmd.has(PMD_EAT)) {
    act = PMD_EAT;
    beh.t0 = 0;
  } else if (m == MOOD_SAD && pmd.has(PMD_HURT)) {
    act = PMD_HURT;
  } else {
    // contento: el planificador decide (idle / paseo / gesto)
    if (deadlineReached(now, beh.until)) behNext();
    if (beh.mode == 1) {
      float d = beh.targetX - beh.x;
      if (fabsf(d) < 4) {
        behNext();
        act = PMD_IDLE;
      } else {
        beh.x += (d > 0 ? 3.0f : -3.0f);
        act = (d > 0) ? PMD_WALKR : PMD_WALKL;
      }
    } else {
      act = (beh.mode == 2) ? beh.act : PMD_IDLE;
      loop = false;
    }
    if (!pmd.has(act)) act = PMD_IDLE;
  }

  drawPmdAct(act, (int)beh.x, PET_GROUND, now - beh.t0, loop || act == PMD_IDLE, false, 5);

  if (pet.showHeart()) drawMap(SPR_HEART, 32, (int)beh.x + 50, PET_GROUND - 190, 2, false);
}

// ---------------------------------------------------------- idle main UI ----
static uint8_t idleExpand5(uint16_t v) { return (uint8_t)((v & 0x1F) * 255 / 31); }
static uint8_t idleExpand6(uint16_t v) { return (uint8_t)((v & 0x3F) * 255 / 63); }

// Same pick as TamaClock / MonsPack::pickAccent: area x saturation, skipping
// outline black and glare white, then lift a dark winner until it reads.
uint16_t idleAccentColor() {
  static int16_t cachedDex = -2;
  static bool cachedShiny = false;
  static uint16_t cached = 0xFFFF;
  if (cachedDex == pet.speciesId && cachedShiny == pet.shiny && cached != 0xFFFF)
    return cached;

  cachedDex = pet.speciesId;
  cachedShiny = pet.shiny;
  cached = 0xFFE0;  // warm yellow fallback (egg, missing pack, flat palette)
  if (!pmd.loaded || !pmd.palCount) return cached;
  const PmdAct &a = pmd.acts[PMD_IDLE];
  if (!a.frames || !a.data) return cached;

  uint16_t counts[256] = {0};
  for (uint8_t fr = 0; fr < a.frames; fr++) {
    const uint8_t *f = a.data + (uint32_t)fr * a.w * a.h;
    for (uint32_t i = 0; i < (uint32_t)a.w * a.h; i++) {
      uint8_t idx = f[i];
      if (idx != 0xFF && idx < pmd.palCount) counts[idx]++;
    }
  }

  uint32_t best = 0;
  uint16_t bestPal = 0xFFFF;
  for (uint16_t i = 0; i < pmd.palCount; i++) {
    if (!counts[i]) continue;
    uint8_t r = idleExpand5(pmd.pal[i] >> 11);
    uint8_t g = idleExpand6(pmd.pal[i] >> 5);
    uint8_t b = idleExpand5(pmd.pal[i]);
    uint8_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    uint8_t mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    uint16_t lum = (uint16_t)((r * 30 + g * 59 + b * 11) / 100);
    if (lum < 35 || lum > 240) continue;
    uint32_t score = (uint32_t)counts[i] * (uint32_t)(mx - mn + 40);
    if (lum < 90) score /= 2;
    if (score > best) { best = score; bestPal = pmd.pal[i]; }
  }
  if (bestPal == 0xFFFF) return cached;

  uint16_t r = idleExpand5(bestPal >> 11);
  uint16_t g = idleExpand6(bestPal >> 5);
  uint16_t b = idleExpand5(bestPal);
  uint16_t lum = (uint16_t)((r * 30 + g * 59 + b * 11) / 100);
  if (lum && lum < 145) {
    uint16_t f = (uint16_t)(14500 / lum);
    if (f > 250) f = 250;
    r = (r * f) / 100; g = (g * f) / 100; b = (b * f) / 100;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
  }
  cached = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  return cached;
}

// Visible rows of the idle pose: the packs pad the canvas, so scaling by h
// would leave a small creature inside a big transparent box.
static int idleVisibleHeight(const PmdAct &a) {
  if (!a.frames || !a.data) return a.h;
  uint8_t bottom = a.base ? a.base : a.h;
  int top = bottom;
  for (int r = 0; r < bottom && r < a.h; r++) {
    const uint8_t *row = a.data + (uint32_t)r * a.w;
    bool any = false;
    for (int c = 0; c < a.w && !any; c++)
      if (row[c] != 0xFF) any = true;
    if (any) { top = r; break; }
  }
  int vis = (int)bottom - top;
  return vis > 0 ? vis : a.h;
}

static uint8_t idleSpriteScale(const PmdAct &a) {
  const PmdAct &idle = pmd.acts[PMD_IDLE];
  int span = idleVisibleHeight(idle);  // visible rows, not the padded canvas
  int s = span > 0 ? (175 + span / 2) / span : 3;
  if (s < 2) s = 2;
  while (s > 1 && (a.w * s > 400 || span * s > 178)) s--;
  if (s > 6) s = 6;
  return (uint8_t)s;
}

static void drawIdleActM(PmdMon &m, uint8_t actId, int cx, int groundY,
                         uint32_t t, bool loop, bool sil) {
  const PmdAct &a = m.acts[actId];
  if (!a.frames) return;
  uint8_t s = idleSpriteScale(a);
  uint8_t fi = pmdFrameAt(a, t, loop);
  const uint8_t *fr = a.data + (uint32_t)fi * a.w * a.h;
  int x0 = cx - a.w * s / 2;
  int y0 = groundY - (a.base ? a.base : a.h) * s;
  for (int r = 0; r < a.h; r++) {
    const uint8_t *row = fr + r * a.w;
    for (int c = 0; c < a.w; c++) {
      uint8_t idx = row[c];
      if (idx == 0xFF) continue;
      gfx->fillRect(x0 + c * s, y0 + r * s, s, s, sil ? INK_K : m.pal[idx]);
    }
  }
}

#define IDLE_GROUND 400

void drawIdleEvolveFX(uint32_t now) {
  float t = pet.evolveT();
  int cx = CX, cy = IDLE_GROUND - 90;
  int halo = 36 + (int)(t * 150) + (int)(8 * sinf(now * 0.02f));
  for (int k = 0; k < 4; k++) {
    int r = halo - k * 7;
    if (r > 0) gfx->drawCircle(cx, cy, r, UI_WHITE);
  }
  float base = now * 0.004f;
  for (int i = 0; i < 12; i++) {
    float ang = base + i * (float)(PI / 6);
    int len = 90 + (int)(70 * (0.5f + 0.5f * sinf(now * 0.012f + i)));
    gfx->drawLine(cx, cy, cx + (int)(cosf(ang) * len),
                  cy + (int)(sinf(ang) * len), UI_WHITE);
  }
  int period = 60 + (int)(220 * (1.0f - t));
  bool showOld = t < 0.9f && evoPmd.loaded && ((now / period) % 2) == 0;
  if (showOld) drawIdleActM(evoPmd, PMD_IDLE, cx, IDLE_GROUND, 0, true, true);
  else drawIdleActM(pmd, PMD_IDLE, cx, IDLE_GROUND, 0, true, true);
  for (int i = 0; i < 10; i++) {
    float ang = i * (float)(PI / 5) + t * 4.0f;
    int d = (int)((now / 14 + i * 33) % 200);
    int sx = cx + (int)(cosf(ang) * d), sy = cy + (int)(sinf(ang) * d);
    gfx->fillRect(sx - 2, sy - 2, 5, 5, (i & 1) ? C565(0xff, 0xe0, 0x70) : UI_WHITE);
  }
  if (t > 0.9f) gfx->fillCircle(cx, cy, (int)(300 * (t - 0.9f) / 0.1f), UI_WHITE);
}

void drawIdlePet() {
  uint32_t now = millis();
  if (pet.evolving()) {
    drawIdleEvolveFX(now);
    return;
  }
  if (evoPmd.loaded) evoPmd.unload();

  uint8_t act = PMD_IDLE;
  bool loop = true;
  PetMood m = pet.mood();
  if (m == MOOD_SLEEPING && pmd.has(PMD_SLEEP)) {
    act = PMD_SLEEP;
    beh.mode = 0;
  } else if (m == MOOD_EATING && pmd.has(PMD_EAT)) {
    act = PMD_EAT;
    beh.t0 = 0;
  } else if (m == MOOD_SAD && pmd.has(PMD_HURT)) {
    act = PMD_HURT;
  } else {
    if (deadlineReached(now, beh.until)) behNext();
    if (beh.mode == 1) {
      float d = beh.targetX - beh.x;
      if (fabsf(d) < 4) {
        behNext();
        act = PMD_IDLE;
      } else {
        beh.x += (d > 0 ? 3.0f : -3.0f);
        act = (d > 0) ? PMD_WALKR : PMD_WALKL;
      }
    } else {
      act = (beh.mode == 2) ? beh.act : PMD_IDLE;
      loop = false;
    }
    if (!pmd.has(act)) act = PMD_IDLE;
  }
  if (beh.x < 110) beh.x = 110;
  if (beh.x > 356) beh.x = 356;
  drawIdleActM(pmd, act, (int)beh.x, IDLE_GROUND, now - beh.t0,
               loop || act == PMD_IDLE, false);
  if (pet.showHeart())
    drawMap(SPR_HEART, 32, (int)beh.x + 46, IDLE_GROUND - 150, 2, false);
}

// Default Arduino_GFX 5x7 font, same size as TamaClock: digit height 7*size.
// setSize()/textW() keep this honest in Korean too: the unifont is 16 px at
// scale 1, i.e. twice the classic height, so a raw setTextSize(11) used to draw
// an 80 px-wide-per-glyph clock that no circle this size could hold.
static void idleTextCentered(const char *s, int16_t y, uint8_t size, uint16_t color) {
  gfx->setTextColor(color);
  setSize(size);
  gfx->setCursor(CX - (int16_t)textW(s, size) / 2, y + gFontAscent * gTextSize);
  gfx->print(s);
}

static void idleTextOutlined(const char *s, int16_t y, uint8_t size, uint16_t color) {
  setSize(size);
  const int16_t x = CX - (int16_t)textW(s, size) / 2;
  const int16_t by = y + gFontAscent * gTextSize;
  gfx->setTextColor(UI_BG_NIGHT);
  for (int8_t dy = -2; dy <= 2; dy += 2) {
    for (int8_t dx = -2; dx <= 2; dx += 2) {
      if (!dx && !dy) continue;
      gfx->setCursor(x + dx, by + dy);
      gfx->print(s);
    }
  }
  gfx->setTextColor(color);
  gfx->setCursor(x, by);
  gfx->print(s);
}

void drawIdleClockHud() {
  uint32_t e = mainClockEpoch();
  char hm[8] = "--:--";
  char date[40] = "IDLE POKEMON";
  if (e) {
    time_t tt = (time_t)e;
    struct tm t;
    gmtime_r(&tt, &t);
    snprintf(hm, sizeof(hm), "%02d:%02d", t.tm_hour, t.tm_min);
    // The species name already sits under the creature; the top line is the
    // date only, which keeps the clock's row free of clutter.
    snprintf(date, sizeof(date), "%04d-%02d-%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  }
  idleTextCentered(date, 80, 2, inkColor());
  idleTextOutlined(hm, 108, 10, pet.isEgg() ? UI_BAR_WARN : idleAccentColor());

  // Next evolution countdown + a thin progress bar.  Both stay small: the
  // clock is the thing that should own the upper half of the circle.
  char evo[24];
  uint32_t rem = pet.isEgg() ? 0 : idleRemainingSec();
  if (pet.isEgg()) snprintf(evo, sizeof(evo), "HATCHING");
  else snprintf(evo, sizeof(evo), "EVO %02lu:%02lu",
                (unsigned long)(rem / 60), (unsigned long)(rem % 60));
  idleTextCentered(evo, 196, 1, inkColor());

  uint16_t accent = pet.isEgg() ? UI_BAR_WARN : idleAccentColor();
  const int barX = CX - 125, barY = 208, barW = 250, barH = 5;
  gfx->fillRoundRect(barX, barY, barW, barH, 3, UI_TRACK);
  uint16_t fillW = (uint16_t)((uint32_t)barW * idlePermille() / 1000UL);
  if (fillW > 2) gfx->fillRoundRect(barX, barY, fillW, barH, 3, accent);
}

void drawIdleEgg() {
  int s = 5, x = CX - 16 * s, y = IDLE_GROUND - 32 * s;
  if (deadlineActive(millis(), eggWiggleUntil)) x += ((millis() / 40) % 2) ? 6 : -6;
  drawMap(SPR_EGG, SPRITE_H, x, y, s, false);
  if (pet.eggCracks() >= 1)
    for (auto &c : CRACK1) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
  if (pet.eggCracks() >= 2)
    for (auto &c : CRACK2) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
}

void drawIdleFooter() {
  char label[32];
  if (pet.isEgg()) snprintf(label, sizeof(label), "EGG");
  else snprintf(label, sizeof(label), "%s  #%03d", dexName(pet.speciesId), pet.speciesId);
  idleTextCentered(label, 416, 1, inkColor());
  idleTextCentered("L: DEX  R: SET  HOLD: SET", 430, 1, inkColor());
}

void drawIdleToast() {
  if (!idleToast[0] || !deadlineActive(millis(), idleToastUntil)) return;
  int w = (int)strlen(idleToast) * 6 + 28;
  gfx->fillRoundRect(CX - w / 2, 50, w, 26, 10, UI_BG_NIGHT);
  idleTextCentered(idleToast, 58, 1, idleAccentColor());
}

void drawIdleMain() {
  gNight = isVisualNight(sceneHour(), false);
  drawScene(pet.isEgg() ? 0 : DEX_TBL[pet.speciesId].biome, millis(), gNight);
  drawIdleClockHud();
  if (pet.isEgg()) drawIdleEgg();
  else drawIdlePet();
  drawIdleFooter();
  drawIdleToast();
  flushStampedFrame();
}

// sprite animado desde la SD: zoom entero por pixel, frames a su ritmo
void drawPetSD() {
  int s = mon.scale;
  int w = mon.w * s, h = mon.h * s;
  int x = CX - w / 2;
  int y = PET_CY - h / 2;

  bool sil = false;
  if (pet.evolving()) {
    sil = (millis() / 300) % 2;
  } else if (pet.mood() == MOOD_HAPPY && (millis() / 500) % 2) {
    y -= 6;  // saltito
  }

  uint16_t fm = mon.frameMs ? mon.frameMs : 100;
  uint16_t fi = pet.sleeping ? 0 : (millis() / fm) % mon.frames;
  const uint8_t *fr = mon.data + (uint32_t)fi * mon.w * mon.h;
  for (int r = 0; r < mon.h; r++) {
    const uint8_t *row = fr + r * mon.w;
    for (int c = 0; c < mon.w; c++) {
      uint8_t idx = row[c];
      if (idx == 0xFF) continue;
      gfx->fillRect(x + c * s, y + r * s, s, s, sil ? INK_K : mon.pal[idx]);
    }
  }

  // emotes en vez de expresiones (los sprites importados no tienen anclas)
  if (pet.showHeart()) drawMap(SPR_HEART, 32, x + w - 30, y - 50, 2, false);
}

// ojo cerrado: borra el ojo 3x4 y dibuja el parpado
void overlayEye(const Species &sp, int x, int y, int s, int col) {
  gfx->fillRect(x + col * s, y + sp.eyeRow * s, 3 * s, 4 * s, sp.bodyColor);
  gfx->fillRect(x + col * s, y + (sp.eyeRow + 2) * s, 3 * s, s, INK_K);
}

// borra la sonrisa base y pinta boca abierta (comer) o ceno (triste)
void overlayMouth(const Species &sp, int x, int y, int s, bool open) {
  int mc = sp.mouthCol, mr = sp.mouthRow;
  gfx->fillRect(x + (mc - 3) * s, y + mr * s, 7 * s, 2 * s, sp.bodyColor);
  if (open) {
    gfx->fillRect(x + (mc - 2) * s, y + mr * s, 5 * s, 2 * s, INK_K);
  } else {
    gfx->fillRect(x + (mc - 2) * s, y + mr * s, 5 * s, s, INK_K);
    gfx->fillRect(x + (mc - 3) * s, y + (mr + 1) * s, s, s, INK_K);
    gfx->fillRect(x + (mc + 3) * s, y + (mr + 1) * s, s, s, INK_K);
  }
}

void drawPoops() {
  for (int i = 0; i < pet.poops; i++) {
    drawMap(SPR_POOP, 32, 36 + i * 46, 244, 2, false);
  }
}

void drawBars() {
  drawBar(78, 318, T(S_BAR_FOOD), pet.fullness);
  drawBar(244, 318, T(S_BAR_JOY), pet.joy);
  drawBar(78, 346, T(S_BAR_ENE), pet.energy);
  drawBar(244, 346, T(S_BAR_HYG), pet.hygiene);
}

void drawBar(int x, int y, const char *label, uint8_t val) {
  gfx->setTextColor(inkColor());
  setSize(2);
  setCur(x, y);
  printT(label);
  int bx = x + 48, bw = 100, bh = 15;  // +48: deja sitio a etiquetas de 4 letras (EN)
  uint16_t fill = (val >= 50) ? UI_BAR_OK : (val >= 25) ? UI_BAR_WARN : UI_BAR_BAD;
  gfx->fillRoundRect(bx, y, bw, bh, 4, UI_TRACK);
  int fw = (bw - 4) * val / 100;
  if (fw > 0) gfx->fillRoundRect(bx + 2, y + 2, fw, bh - 4, 3, fill);
}

void drawButtons() {
  for (int i = 0; i < 4; i++) {
    bool off = pet.sleeping && i != 2;  // durmiendo solo funciona LUZ
    int bx = buttons[i].cx - BTN_HALF, by = buttons[i].cy - BTN_HALF;
    if (!pet.sleeping) gfx->fillRoundRect(bx, by, 2 * BTN_HALF, 2 * BTN_HALF, 14, UI_WHITE);
    gfx->drawRoundRect(bx, by, 2 * BTN_HALF, 2 * BTN_HALF, 14, inkColor());
    if (!off) drawMap(buttons[i].icon, 16, buttons[i].cx - 16, buttons[i].cy - 16, 2, false);
  }
}

const char *eggMsg() {
  switch (pet.eggCracks()) {
    case 0: return T(S_EGG_TOUCH);
    case 1: return T(S_EGG_MOVES);
    default: return T(S_EGG_ALMOST);
  }
}

const char *statusMsg() {
  if (pet.evolving()) return T(S_EVOLVING);
  if (bathUntil) return "Splish splash!";  // onomatopeya universal
  if (pet.sleeping) return "Zzz...";
  if (pet.eating()) return T(S_EATING);
  if (pet.showHeart()) return T(S_LIKES);
  if (pet.fullness < 25) return T(S_HUNGRY);
  if (pet.hygiene < 25) return T(S_NEEDS_BATH);
  if (pet.energy < 25) return T(S_EXHAUSTED);
  if (pet.joy < 25) return T(S_SAD);
  if (pet.weight > 60) return T(S_CHUBBY);
  if (pet.shiny && pet.ageMinutes < 15) return T(S_IS_SHINY);
  return T(S_HAPPY);
}

// dibuja un mapa de n x n pixeles escalado; silhouette=true lo pinta en tinta
void drawMap(const char *const *map, int n, int x, int y, int s, bool silhouette) {
  for (int r = 0; r < n; r++) {
    for (int c = 0; c < n; c++) {
      char ch = map[r][c];
      if (ch == '.') continue;
      gfx->fillRect(x + c * s, y + r * s, s, s, silhouette ? INK_K : spriteColor(ch));
    }
  }
}
