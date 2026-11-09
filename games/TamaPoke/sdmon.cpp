#include "sdmon.h"
#include "pin_config.h"
#include "dex.h"
#include "thumb_format.h"
#include <FS.h>
#include <SD_MMC.h>

// Multi-game: each game owns a top-level directory; upstream both use /mons.
#define MONS_ROOT "/tamapoke/mons"
#define MONS_PARENT "/tamapoke"

bool sdReady = false;
bool sdDirty = false;
SdThumbs thumbs;

// mkdir() creates one level only.  A freshly formatted card has no
// /tamapoke parent, so creating /tamapoke/mons alone fails and every write
// reports ERR.  Create both levels explicitly.
static bool ensureMonsRoot() {
  if (!SD_MMC.exists(MONS_PARENT)) SD_MMC.mkdir(MONS_PARENT);
  if (!SD_MMC.exists(MONS_ROOT)) SD_MMC.mkdir(MONS_ROOT);
  return SD_MMC.exists(MONS_ROOT);
}

bool PmdMon::load(uint16_t dexNum, bool shiny) {
  unload();
  if (!sdReady) return false;

  char path[32];
  snprintf(path, sizeof(path), MONS_ROOT "/p%s%03u.bin", shiny ? "s" : "", dexNum);
  File f = SD_MMC.open(path, FILE_READ);
  if (!f && shiny) {  // sin shiny PMD: usa el normal
    snprintf(path, sizeof(path), MONS_ROOT "/p%03u.bin", dexNum);
    f = SD_MMC.open(path, FILE_READ);
  }
  if (!f) return false;

  uint32_t size = f.size();
  if (size < 7 || size > 3UL * 1024 * 1024) { f.close(); return false; }
  blob = (uint8_t *)ps_malloc(size);
  if (!blob || f.read(blob, size) != size || memcmp(blob, "TPK2", 4) != 0) {
    if (blob) { free(blob); blob = nullptr; }
    f.close();
    return false;
  }
  f.close();

  uint8_t nActs = blob[4];
  memcpy(&palCount, blob + 5, 2);
  if (palCount > 256 || (uint32_t)7 + palCount * 2 > size) { unload(); return false; }
  memcpy(pal, blob + 7, palCount * 2);

  const uint8_t *p = blob + 7 + palCount * 2;
  const uint8_t *end = blob + size;
  for (uint8_t i = 0; i < nActs && p + 4 <= end; i++) {
    uint8_t id = p[0], w = p[1], h = p[2], nf = p[3];
    p += 4;
    if (id >= PMD_NACTS || nf > 24) { unload(); return false; }
    // valida que ms[] y los datos del frame caben en el blob (archivo truncado)
    uint32_t bytes = (uint32_t)nf * 2 + (uint32_t)w * h * nf;
    if (w == 0 || h == 0 || nf == 0 || p + bytes > end) { unload(); return false; }
    PmdAct &a = acts[id];
    a.w = w;
    a.h = h;
    a.frames = nf;
    for (uint8_t k = 0; k < nf; k++) {
      a.ms[k] = p[0] | (p[1] << 8);
      p += 2;
    }
    a.data = p;
    p += (uint32_t)w * h * nf;
    // fila mas baja con contenido en cualquier frame: anclar por los pies
    uint8_t base = 1;
    for (uint8_t f = 0; f < nf; f++) {
      const uint8_t *fr = a.data + (uint32_t)f * w * h;
      for (int r = h - 1; r >= 0; r--) {
        bool any = false;
        for (int c = 0; c < w && !any; c++)
          if (fr[r * w + c] != 0xFF) any = true;
        if (any) { if (r + 1 > base) base = r + 1; break; }
      }
    }
    a.base = base;
  }
  loaded = true;
  Serial.printf("cargado %s (%u KB)\n", path, size / 1024);
  return true;
}

void PmdMon::unload() {
  if (blob) {
    free(blob);
    blob = nullptr;
  }
  for (auto &a : acts) {
    a.w = a.h = a.frames = a.base = 0;
    a.data = nullptr;
  }
  loaded = false;
}

bool SdThumbs::load() {
  unload();
  if (!sdReady) return false;
  File f = SD_MMC.open(MONS_ROOT "/thumbs.bin", FILE_READ);
  if (!f) {
    Serial.println("sin thumbs.bin (galeria sin miniaturas)");
    return false;
  }
  uint32_t size = f.size();
  data = (uint8_t *)ps_malloc(size);
  uint16_t parsedCount = 0;
  if (!data || f.read(data, size) != size ||
      !validateThumbBlob(data, size, DEX_COUNT, &parsedCount)) {
    Serial.println("thumbs.bin invalido");
    if (data) { free(data); data = nullptr; }
    f.close();
    return false;
  }
  f.close();
  dataSize = size;
  count = parsedCount;
  loaded = true;
  Serial.printf("miniaturas cargadas: %u (%u KB)\n", count, size / 1024);
  return true;
}

void SdThumbs::unload() {
  if (data) free(data);
  data = nullptr;
  dataSize = 0;
  count = 0;
  loaded = false;
}

const uint8_t *SdThumbs::get(int16_t dex) const {
  if (!loaded || dex < 1 || dex > count) return nullptr;
  uint32_t off;
  memcpy(&off, data + 6 + 4 * (dex - 1), 4);
  if (off > dataSize - 3) return nullptr;
  return data + off;
}

bool sdBegin() {
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  // formatIfMountFailed = false: one game must never wipe the other's sprites.
  sdReady = SD_MMC.begin("/sdcard", true /* modo 1-bit */, false /* nunca formatear */);
  if (sdReady) {
    Serial.printf("SD montada: %llu MB\n", SD_MMC.cardSize() / (1024ULL * 1024ULL));
    ensureMonsRoot();
  } else {
    Serial.println("SD no detectada (el juego usa los sprites de flash)");
  }
  return sdReady;
}

bool SdMon::load(uint16_t dexNum, bool shiny) {
  unload();
  if (!sdReady) return false;

  char path[32];
  snprintf(path, sizeof(path), MONS_ROOT "/%s%03u.bin", shiny ? "s" : "", dexNum);
  File f = SD_MMC.open(path, FILE_READ);
  if (!f && shiny) {  // sin variante shiny: usa la normal
    snprintf(path, sizeof(path), MONS_ROOT "/%03u.bin", dexNum);
    f = SD_MMC.open(path, FILE_READ);
  }
  if (!f) {
    Serial.printf("no existe %s\n", path);
    return false;
  }

  char magic[4];
  uint16_t header[4];
  if (f.read((uint8_t *)magic, 4) != 4 || memcmp(magic, "TPK1", 4) != 0 ||
      f.read((uint8_t *)header, 8) != 8) {
    f.close();
    return false;
  }
  w = header[0];
  h = header[1];
  frames = header[2];
  frameMs = header[3];
  // acota dimensiones: evita size desbordado o absurdo con archivo corrupto
  if (f.read((uint8_t *)&palCount, 2) != 2 || palCount > 256 ||
      w == 0 || w > 256 || h == 0 || h > 256 || frames == 0 || frames > 64) {
    f.close();
    return false;
  }
  if (f.read((uint8_t *)pal, palCount * 2) != palCount * 2) {
    f.close();
    return false;
  }

  uint32_t size = (uint32_t)w * h * frames;
  data = (uint8_t *)ps_malloc(size);
  if (!data) {
    Serial.println("sin PSRAM para el sprite");
    f.close();
    return false;
  }
  uint32_t got = f.read(data, size);
  f.close();
  if (got != size) {
    Serial.printf("%s truncado (%u de %u)\n", path, got, size);
    unload();
    return false;
  }

  // zoom entero para que el bicho mida ~200 px de alto en pantalla
  scale = 200 / h;
  if (scale < 2) scale = 2;
  if (scale > 5) scale = 5;

  Serial.printf("cargado %s: %ux%u x%u frames @%ums, escala %u\n",
                path, w, h, frames, frameMs, scale);
  loaded = true;
  return true;
}

void SdMon::unload() {
  if (data) {
    free(data);
    data = nullptr;
  }
  loaded = false;
}

// ---------------------------------------------------------------------------
// Protocolo de carga por USB (para llenar la SD sin sacarla de la placa):
//   PUT <ruta> <bytes>\n  + datos crudos   -> "OK" ... "DONE"
//   LS\n                                   -> listado de /mons
// Usar con tools/send_sd.py
// ---------------------------------------------------------------------------

bool sdSerialCommand(const String &line) {
  if (line == "WHOAMI") {
    Serial.println("TAMAPOKE");
    return true;
  }
  if (line == "SDSTAT") {
    Serial.printf("SD %s total=%llu used=%llu\n", sdReady ? "READY" : "ERR",
                  (unsigned long long)SD_MMC.totalBytes(),
                  (unsigned long long)SD_MMC.usedBytes());
    Serial.println("DONE");
    return true;
  }
  if (line == "SDTEST") {
    bool ok = false;
    if (sdReady && ensureMonsRoot()) {
      File f = SD_MMC.open(MONS_ROOT "/.sdtest", FILE_WRITE);
      if (f) {
        uint8_t test[512];
        memset(test, 0xA5, sizeof(test));
        ok = f.write(test, sizeof(test)) == sizeof(test);
        f.close();
        SD_MMC.remove(MONS_ROOT "/.sdtest");
      }
    }
    Serial.println(ok ? "SDTEST OK" : "SDTEST ERR");
    Serial.println("DONE");
    return true;
  }
  if (line == "SDCLEAR") {
    bool ok = sdReady;
    if (ok && SD_MMC.exists(MONS_ROOT)) {
      File dir = SD_MMC.open(MONS_ROOT);
      if (dir) {
        File e;
        while ((e = dir.openNextFile())) {
          if (!e.isDirectory() && !SD_MMC.remove(e.path())) ok = false;
          e.close();
        }
        dir.close();
      } else {
        ok = false;
      }
    } else if (ok) {
      ok = true;  // nothing to clear on a fresh card
    }
    if (ok) {
      thumbs.unload();
      sdDirty = true;
    }
    Serial.println(ok ? "SD CLEAR OK" : "SD CLEAR ERR");
    Serial.println("DONE");
    return true;
  }
  if (line == "SDFORMAT") {
    SD_MMC.end();
    sdReady = SD_MMC.begin("/sdcard", true /* modo 1-bit */, true /* formatea si no monta */);
    if (sdReady) {
      ensureMonsRoot();
      thumbs.unload();
      sdDirty = true;
    }
    Serial.println(sdReady ? "SD FORMAT OK" : "SD FORMAT ERR");
    Serial.println("DONE");
    return true;
  }
  if (line.startsWith("PUT ")) {
    int sp = line.lastIndexOf(' ');
    String path = line.substring(4, sp);
    uint32_t size = line.substring(sp + 1).toInt();
    if (!sdReady || size == 0 || size > 4 * 1024 * 1024) {
      Serial.println("ERR");
      return true;
    }
    if (!path.startsWith("/")) path = "/" + path;
    if (path.startsWith("/mons/")) path = String(MONS_ROOT) + path.substring(5);
    if (!path.startsWith(MONS_ROOT "/") || path.indexOf("..") >= 0) {
      Serial.println("ERR");
      return true;
    }
    if (!ensureMonsRoot()) {
      Serial.println("ERR");
      return true;
    }
    // FILE_WRITE appends if the file exists; remove a partial/old copy first.
    if (SD_MMC.exists(path)) SD_MMC.remove(path);
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) {
      Serial.println("ERR");
      return true;
    }
    Serial.println("OK");
    static uint8_t buf[2048];
    uint32_t remaining = size;
    Serial.setTimeout(5000);
    while (remaining > 0) {
      size_t want = remaining > sizeof(buf) ? sizeof(buf) : remaining;
      size_t n = Serial.readBytes(buf, want);
      if (n == 0) break;  // timeout
      if (f.write(buf, n) != n) break;
      remaining -= n;
      Serial.println("#");
    }
    f.close();
    Serial.setTimeout(1000);
    sdDirty = (remaining == 0);
    Serial.println(remaining == 0 ? "DONE" : "ERR");
    return true;
  } else if (line == "LS") {
    File dir = SD_MMC.open(MONS_ROOT);
    if (dir) {
      File e;
      while ((e = dir.openNextFile())) {
        Serial.printf("%s %u\n", e.name(), (uint32_t)e.size());
        e.close();
      }
      dir.close();
    }
    Serial.println("DONE");
    return true;
  }
  return false;
}
