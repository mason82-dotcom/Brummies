// =====================================================================
//  FAHRZEUGE ANTIPPEN  -  ein einfaches Spiel fuer kleine Kinder (~2 J.)
//
//  Hardware: Freenove ESP32-S3-WROOM + Shield + 2.8" Touch-Screen
//            Display : ST7789V 240x320 (SPI)   -> via TFT_eSPI
//            Touch   : FT6336U (I2C, SDA=2 SCL=1)
//            Sound   : I2S-Lautsprecher (BCLK=42, LRC=14, DOUT=41)
//            RGB-LED : WS2812 (GPIO48)
//
//  Acht verschiedene Fahrzeuge (Feuerwehr, Polizei, Krankenwagen,
//  Traktor, Bagger, Muellwagen, Auto, Zug) fahren langsam ueber den
//  Bildschirm - in BEIDE Richtungen. Verlaesst eines den Bildschirm,
//  kommt ein neues, zufaelliges Fahrzeug nach. Tippt das Kind ein
//  Fahrzeug an, macht es sein Geraeusch, huepft kurz und die
//  Onboard-RGB-LED blinkt in der Fahrzeugfarbe.
// =====================================================================

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Adafruit_NeoPixel.h>
#include "FT6336U.h"
#include "driver/i2s.h"
#include <math.h>

// ---------------------------------------------------------------------
//  Pins / Hardware-Konfiguration
// ---------------------------------------------------------------------
#define TOUCH_SDA   2
#define TOUCH_SCL   1
#define TOUCH_RST  -1
#define TOUCH_INT  -1

#define WS2812_PIN 48

#define I2S_BCLK   42
#define I2S_LRC    14
#define I2S_DOUT   41
#define SAMPLE_RATE 22050

// Falls Antippen an der falschen Stelle reagiert (gespiegelt),
// hier auf 1 setzen und neu flashen:
#define TOUCH_INVERT_X 0
#define TOUCH_INVERT_Y 0
// Touch-Rohwerte im seriellen Monitor ausgeben (zum Kalibrieren):
#define TOUCH_DEBUG 0

// ---------------------------------------------------------------------
//  Objekte
// ---------------------------------------------------------------------
TFT_eSPI      tft = TFT_eSPI();
TFT_eSprite   spr = TFT_eSprite(&tft);           // Sprite fuer ein Fahrzeug
FT6336U       ctp(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);
Adafruit_NeoPixel led(1, WS2812_PIN, NEO_GRB + NEO_KHZ800);

// ---------------------------------------------------------------------
//  Bildschirm-Geometrie (Hochformat 240 x 320)
// ---------------------------------------------------------------------
static const int SCR_W = 240;
static const int SCR_H = 320;
static const int SKY_H = 52;
static const int N_LANES = 4;
static const int LANE_H = (SCR_H - SKY_H) / N_LANES;   // 67

// Fahrzeug-Box
static const int VW = 94;   // Breite
static const int VH = 44;   // Hoehe
static const int MARG = 6;  // horizontaler Rand im Sprite (>= max. Speed)
static const int BNC  = 9;  // vertikaler Rand (Huepf-Reserve)
static const int SPRW = VW + 2 * MARG;
static const int SPRH = VH + 2 * BNC;

// ---------------------------------------------------------------------
//  Farben (werden in setup() gefuellt)
// ---------------------------------------------------------------------
uint16_t C_SKY, C_ROAD, C_LINE, C_SUN, C_WHITE, C_BLACK, C_WINDOW, C_TIRE, C_HUB;
uint16_t C_RED, C_BLUE, C_GREEN, C_YELLOW, C_DGREY, C_ORANGE, C_CYAN, C_MAROON;

// ---------------------------------------------------------------------
//  Fahrzeuge
// ---------------------------------------------------------------------
enum VType {
  V_FEUERWEHR = 0, V_POLIZEI, V_KRANKENWAGEN, V_TRAKTOR,
  V_BAGGER, V_MUELLWAGEN, V_AUTO, V_ZUG, V_COUNT
};

QueueHandle_t soundQueue;

struct Vehicle {
  VType    type;
  float    x;            // linke Kante (Weltkoordinate)
  int      cy;           // Mittelpunkt y (Fahrbahn)
  float    speed;        // px pro Tick
  int      dir;          // +1 = nach rechts, -1 = nach links
  uint32_t bounceUntil;  // huepft bis zu diesem Zeitpunkt
};

Vehicle vehicles[N_LANES];

static uint16_t colorForType(VType t) {
  switch (t) {
    case V_FEUERWEHR:    return C_RED;
    case V_POLIZEI:      return C_BLUE;
    case V_KRANKENWAGEN: return C_WHITE;
    case V_TRAKTOR:      return C_GREEN;
    case V_BAGGER:       return C_YELLOW;
    case V_MUELLWAGEN:   return C_ORANGE;
    case V_AUTO:         return C_CYAN;
    case V_ZUG:          return C_MAROON;
    default:             return C_WHITE;
  }
}

// =====================================================================
//  AUDIO  ---  laeuft als eigener Task auf Core 0
// =====================================================================
static void i2sSetup() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;      // Stereo
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;   // PCM5101 braucht keinen MCLK
  pins.bck_io_num = I2S_BCLK;
  pins.ws_io_num  = I2S_LRC;
  pins.data_out_num = I2S_DOUT;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;

  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

static float s_phase = 0.0f;

// Erzeugt einen Ton: Frequenz laeuft linear von f0 -> f1 ueber "ms".
// wave: 0=Sinus, 1=Rechteck.  amp: 0..1.  Tremolo moduliert die Lautstaerke.
static void synth(float f0, float f1, int ms, uint8_t wave, float amp,
                  float tremHz = 0, float tremDepth = 0) {
  const int total = (SAMPLE_RATE * ms) / 1000;
  const int ramp  = SAMPLE_RATE * 4 / 1000;    // 4 ms weiche Flanke gegen Knacken
  int16_t buf[256 * 2];
  int done = 0;
  while (done < total) {
    int n = total - done; if (n > 256) n = 256;
    for (int k = 0; k < n; k++) {
      int idx = done + k;
      float t = (float)idx / (float)total;
      float freq = f0 + (f1 - f0) * t;
      s_phase += 2.0f * (float)M_PI * freq / SAMPLE_RATE;
      if (s_phase > 2.0f * (float)M_PI) s_phase -= 2.0f * (float)M_PI;

      float s = (wave == 0) ? sinf(s_phase)
                            : (sinf(s_phase) >= 0 ? 0.7f : -0.7f);

      float env = 1.0f;
      if (idx < ramp)             env = (float)idx / ramp;
      else if (idx > total - ramp) env = (float)(total - idx) / ramp;

      float trem = 1.0f;
      if (tremHz > 0)
        trem = 1.0f - tremDepth * 0.5f * (1.0f - cosf(2.0f * (float)M_PI * tremHz * idx / SAMPLE_RATE));

      int16_t v = (int16_t)(s * amp * env * trem * 26000.0f);
      buf[2 * k]     = v;   // links
      buf[2 * k + 1] = v;   // rechts
    }
    size_t written;
    i2s_write(I2S_NUM_0, buf, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
    done += n;
  }
}

static void silence(int ms) {
  const int total = (SAMPLE_RATE * ms) / 1000;
  int16_t buf[256 * 2] = {0};
  int done = 0;
  while (done < total) {
    int n = total - done; if (n > 256) n = 256;
    size_t written;
    i2s_write(I2S_NUM_0, buf, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
    done += n;
  }
}

static void playSound(int id) {
  switch ((VType)id) {
    case V_FEUERWEHR:                          // Martinshorn, langsam "Tatü-Taata"
      for (int i = 0; i < 3; i++) {
        synth(659, 659, 300, 0, 0.8f);
        synth(494, 494, 300, 0, 0.8f);
      }
      break;
    case V_POLIZEI:                            // heulende Sirene
      for (int i = 0; i < 3; i++) {
        synth(600, 1200, 350, 0, 0.7f);
        synth(1200, 600, 350, 0, 0.7f);
      }
      break;
    case V_KRANKENWAGEN:                       // schnelles "Yelp" hi-lo
      for (int i = 0; i < 6; i++) {
        synth(950, 950, 140, 0, 0.75f);
        synth(760, 760, 140, 0, 0.75f);
      }
      break;
    case V_TRAKTOR:                            // tuckernder Motor + Hupe
      synth(85, 90, 1400, 1, 0.9f, 9.0f, 0.9f);
      synth(300, 300, 250, 1, 0.7f);
      break;
    case V_BAGGER:                             // Rueckfahr-Piepen + Grummeln
      for (int i = 0; i < 3; i++) {
        synth(1000, 1000, 220, 1, 0.6f);
        silence(140);
      }
      synth(70, 70, 500, 1, 0.9f, 12.0f, 0.8f);
      break;
    case V_MUELLWAGEN:                         // schwerer Motor + langsame Piepser
      synth(110, 110, 900, 1, 0.8f, 6.0f, 0.7f);
      for (int i = 0; i < 2; i++) {
        synth(760, 760, 240, 1, 0.6f);
        silence(160);
      }
      break;
    case V_AUTO:                               // "Hup Hup"
      synth(520, 520, 180, 1, 0.7f);
      silence(90);
      synth(520, 520, 180, 1, 0.7f);
      break;
    case V_ZUG:                                // tiefes Horn + Tuckern
      synth(330, 330, 450, 0, 0.7f);
      synth(262, 262, 550, 0, 0.7f);
      for (int i = 0; i < 4; i++) {
        synth(90, 90, 120, 1, 0.7f);
        silence(70);
      }
      break;
    default: break;
  }
  i2s_zero_dma_buffer(I2S_NUM_0);
}

static void audioTask(void *param) {
  int id;
  for (;;) {
    if (xQueueReceive(soundQueue, &id, portMAX_DELAY) == pdTRUE) {
      playSound(id);
    }
  }
}

// =====================================================================
//  ZEICHNEN  (alle Fahrzeuge zeigen nach RECHTS; nach links wird der
//             Sprite gespiegelt)
// =====================================================================
static void wheel(int x, int y, int r) {
  spr.fillCircle(x, y, r, C_TIRE);
  spr.fillCircle(x, y, r / 2, C_HUB);
}

// ---- Feuerwehr (rot, Leiter, Blaulicht) ----
static void drawFeuerwehr(int ox, int oy) {
  int bodyTop = oy + 6, bodyH = VH - 16;
  spr.fillRoundRect(ox + 2, bodyTop, VW - 6, bodyH, 4, C_RED);
  spr.fillRoundRect(ox + VW - 34, bodyTop - 8, 32, bodyH + 8, 4, C_RED);
  spr.fillRect(ox + VW - 30, bodyTop - 4, 22, 14, C_WINDOW);
  for (int i = 0; i < 6; i++)
    spr.drawLine(ox + 8 + i * 6, bodyTop + 4, ox + 8 + i * 6, bodyTop + 14, C_DGREY);
  spr.drawLine(ox + 8, bodyTop + 4, ox + 40, bodyTop + 4, C_DGREY);
  spr.drawLine(ox + 8, bodyTop + 14, ox + 40, bodyTop + 14, C_DGREY);
  spr.fillRect(ox + VW - 26, bodyTop - 14, 14, 6, C_BLUE);
  wheel(ox + 20, oy + VH - 7, 9);
  wheel(ox + VW - 20, oy + VH - 7, 9);
}

// ---- Polizei (blau/weiss, Lichtbalken) ----
static void drawPolizei(int ox, int oy) {
  int top = oy + 14;
  spr.fillRoundRect(ox + 4, top, VW - 8, VH - 22, 5, C_BLUE);
  spr.fillRoundRect(ox + 24, oy + 2, VW - 48, 18, 5, C_WHITE);
  spr.fillRoundRect(ox + 28, oy + 5, VW - 56, 12, 3, C_WINDOW);
  spr.fillRect(ox + 10, top + 2, VW - 20, 8, C_WHITE);
  spr.fillRect(ox + VW / 2 - 12, oy, 12, 5, C_RED);
  spr.fillRect(ox + VW / 2,      oy, 12, 5, C_BLUE);
  wheel(ox + 22, oy + VH - 7, 9);
  wheel(ox + VW - 22, oy + VH - 7, 9);
}

// ---- Krankenwagen (weiss, rotes Kreuz, blaues Licht) ----
static void drawKrankenwagen(int ox, int oy) {
  int bodyTop = oy + 4, bodyH = VH - 14;
  spr.fillRoundRect(ox + 2, bodyTop, VW - 4, bodyH, 4, C_WHITE);        // Kastenaufbau
  spr.fillRect(ox + VW - 30, bodyTop + 4, 22, 14, C_WINDOW);           // Fahrerscheibe
  spr.fillRect(ox + 2, oy + VH - 16, VW - 4, 6, C_RED);               // roter Streifen
  // rotes Kreuz
  spr.fillRect(ox + 22, bodyTop + 8, 16, 5, C_RED);
  spr.fillRect(ox + 27, bodyTop + 3, 5, 15, C_RED);
  spr.fillRect(ox + VW - 24, bodyTop - 4, 12, 5, C_BLUE);            // Blaulicht
  wheel(ox + 22, oy + VH - 7, 9);
  wheel(ox + VW - 22, oy + VH - 7, 9);
}

// ---- Traktor (gruen, grosses Hinterrad, Auspuff) ----
static void drawTraktor(int ox, int oy) {
  spr.fillRoundRect(ox + VW - 40, oy + 16, 36, VH - 26, 3, C_GREEN);
  spr.fillRoundRect(ox + 20, oy + 2, 30, VH - 14, 3, C_GREEN);
  spr.fillRect(ox + 24, oy + 5, 22, 14, C_WINDOW);
  spr.fillRect(ox + VW - 34, oy + 6, 5, 12, C_DGREY);
  spr.fillCircle(ox + VW - 6, oy + 22, 3, C_YELLOW);
  wheel(ox + VW - 18, oy + VH - 10, 8);
  wheel(ox + 24, oy + VH - 6, 15);
}

// ---- Bagger (gelb, Baggerarm mit Schaufel) ----
static void drawBagger(int ox, int oy) {
  spr.fillRoundRect(ox + 6, oy + VH - 16, VW - 24, 14, 6, C_BLACK);
  for (int i = 0; i < 5; i++) spr.fillCircle(ox + 16 + i * 14, oy + VH - 9, 5, C_HUB);
  spr.fillRoundRect(ox + 8, oy + 8, 34, VH - 22, 4, C_YELLOW);
  spr.fillRect(ox + 12, oy + 11, 22, 14, C_WINDOW);
  spr.drawLine(ox + 40, oy + 16, ox + VW - 20, oy + 4,  C_YELLOW);
  spr.drawLine(ox + 41, oy + 17, ox + VW - 19, oy + 5,  C_YELLOW);
  spr.drawLine(ox + VW - 20, oy + 4, ox + VW - 6, oy + 22, C_YELLOW);
  spr.drawLine(ox + VW - 19, oy + 4, ox + VW - 5, oy + 22, C_YELLOW);
  spr.fillTriangle(ox + VW - 12, oy + 20, ox + VW - 2, oy + 22,
                   ox + VW - 8,  oy + 30, C_DGREY);
}

// ---- Muellwagen (orange, grosser Ladebehaelter) ----
static void drawMuellwagen(int ox, int oy) {
  int bodyTop = oy + 4, bodyH = VH - 14;
  spr.fillRoundRect(ox + 2, bodyTop, VW - 30, bodyH, 3, C_ORANGE);      // Ladebehaelter
  spr.fillRoundRect(ox + VW - 30, bodyTop + 6, 28, bodyH - 6, 3, C_ORANGE); // Fahrerhaus
  spr.fillRect(ox + VW - 26, bodyTop + 9, 20, 13, C_WINDOW);
  spr.drawRect(ox + 6, bodyTop + 4, VW - 40, bodyH - 10, C_DGREY);      // Behaelter-Kante
  spr.fillRect(ox + 4, oy + VH - 14, VW - 34, 4, C_DGREY);             // Ladekante hinten
  wheel(ox + 20, oy + VH - 7, 9);
  wheel(ox + VW - 18, oy + VH - 7, 9);
}

// ---- Auto (buntes kleines Auto) ----
static void drawAuto(int ox, int oy) {
  int top = oy + 16;
  spr.fillRoundRect(ox + 6, top, VW - 12, VH - 24, 6, C_CYAN);          // Karosserie
  spr.fillRoundRect(ox + 26, oy + 4, VW - 50, 16, 6, C_CYAN);           // Dach
  spr.fillRoundRect(ox + 30, oy + 7, VW - 58, 10, 3, C_WINDOW);         // Scheiben
  spr.fillCircle(ox + VW - 8, top + 4, 3, C_YELLOW);                    // Scheinwerfer
  wheel(ox + 24, oy + VH - 7, 9);
  wheel(ox + VW - 24, oy + VH - 7, 9);
}

// ---- Zug / Lokomotive (dunkelrot, Schornstein, Dampf) ----
static void drawZug(int ox, int oy) {
  spr.fillRoundRect(ox + 2, oy + 10, VW - 4, VH - 18, 4, C_MAROON);     // Kessel/Koerper
  spr.fillRoundRect(ox + 6, oy + 2, 30, 20, 3, C_MAROON);              // Fuehrerhaus (links)
  spr.fillRect(ox + 10, oy + 5, 20, 12, C_WINDOW);
  spr.fillRect(ox + VW - 20, oy + 2, 10, 12, C_BLACK);                 // Schornstein (rechts)
  spr.fillCircle(ox + VW - 15, oy + 2, 5, C_HUB);                      // Dampf
  spr.fillCircle(ox + VW - 8,  oy - 2, 4, C_HUB);
  spr.fillRect(ox + 2, oy + VH - 10, VW - 4, 4, C_DGREY);             // Rahmen
  wheel(ox + 22, oy + VH - 6, 11);
  wheel(ox + VW - 24, oy + VH - 6, 11);
  spr.fillCircle(ox + (VW / 2), oy + VH - 6, 7, C_TIRE);              // mittleres Rad
  spr.fillCircle(ox + (VW / 2), oy + VH - 6, 3, C_HUB);
}

static void drawVehicleArt(VType t, int ox, int oy) {
  switch (t) {
    case V_FEUERWEHR:    drawFeuerwehr(ox, oy);    break;
    case V_POLIZEI:      drawPolizei(ox, oy);      break;
    case V_KRANKENWAGEN: drawKrankenwagen(ox, oy); break;
    case V_TRAKTOR:      drawTraktor(ox, oy);      break;
    case V_BAGGER:       drawBagger(ox, oy);       break;
    case V_MUELLWAGEN:   drawMuellwagen(ox, oy);   break;
    case V_AUTO:         drawAuto(ox, oy);         break;
    case V_ZUG:          drawZug(ox, oy);          break;
    default: break;
  }
}

// Sprite waagerecht spiegeln (fuer nach links fahrende Fahrzeuge)
static void mirrorSprite() {
  uint16_t *p = (uint16_t *)spr.getPointer();
  if (!p) return;
  for (int y = 0; y < SPRH; y++) {
    uint16_t *row = p + y * SPRW;
    for (int a = 0, b = SPRW - 1; a < b; a++, b--) {
      uint16_t tmp = row[a]; row[a] = row[b]; row[b] = tmp;
    }
  }
}

// Statische Szene einmalig zeichnen: Himmel, Sonne, Titel, Strasse, Fahrspuren
static void drawScene() {
  tft.fillRect(0, 0, SCR_W, SKY_H, C_SKY);
  tft.fillCircle(SCR_W - 26, 22, 16, C_SUN);
  tft.setTextColor(C_WHITE, C_SKY);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Tipp ein Auto!", 10, 16, 4);

  tft.fillRect(0, SKY_H, SCR_W, SCR_H - SKY_H, C_ROAD);
  for (int i = 1; i < N_LANES; i++) {
    int y = SKY_H + i * LANE_H;
    for (int x = 0; x < SCR_W; x += 22)
      tft.fillRect(x, y - 1, 12, 3, C_LINE);
  }
}

// Ein Fahrzeug (flicker-frei) an seine aktuelle Position zeichnen
static void drawVehicle(Vehicle &v) {
  spr.fillSprite(C_ROAD);
  int bounce = (millis() < v.bounceUntil)
                 ? (int)(-7 * fabsf(sinf(millis() * 0.03f)))
                 : 0;
  drawVehicleArt(v.type, MARG, BNC + bounce);
  if (v.dir < 0) mirrorSprite();
  spr.pushSprite((int)v.x - MARG, v.cy - VH / 2 - BNC);
}

// Fahrzeug neu bestuecken: zufaelliger Typ, Richtung, Tempo
static void respawn(Vehicle &v) {
  v.type  = (VType)random(0, V_COUNT);
  v.dir   = (random(0, 2) == 0) ? 1 : -1;
  v.speed = 0.9f + random(0, 16) * 0.1f;        // 0.9 .. 2.4 px/Tick
  v.x     = (v.dir > 0) ? -(VW + MARG) : (SCR_W + MARG);
  v.bounceUntil = 0;
}

// =====================================================================
//  TOUCH
// =====================================================================
static bool wasTouched = false;
uint32_t ledOffAt = 0;

static void handleTouch() {
  FT6336U_TouchPointType tp = ctp.scan();
  bool now = (tp.touch_count > 0);

  if (now && !wasTouched) {
    int sx = tp.tp[0].x;
    int sy = tp.tp[0].y;
#if TOUCH_INVERT_X
    sx = (SCR_W - 1) - sx;
#endif
#if TOUCH_INVERT_Y
    sy = (SCR_H - 1) - sy;
#endif
#if TOUCH_DEBUG
    Serial.printf("touch raw(%d,%d) -> screen(%d,%d)\n", tp.tp[0].x, tp.tp[0].y, sx, sy);
#endif
    for (int i = 0; i < N_LANES; i++) {
      Vehicle &v = vehicles[i];
      int left = (int)v.x, right = (int)v.x + VW;
      int topY = v.cy - VH / 2 - BNC, botY = v.cy + VH / 2 + BNC;
      if (sx >= left && sx <= right && sy >= topY && sy <= botY) {
        int id = (int)v.type;
        xQueueSend(soundQueue, &id, 0);
        v.bounceUntil = millis() + 700;
        uint16_t c = colorForType(v.type);
        led.setPixelColor(0, led.Color(
          ((c >> 11) & 0x1F) << 3, ((c >> 5) & 0x3F) << 2, (c & 0x1F) << 3));
        led.show();
        ledOffAt = millis() + 600;
        break;
      }
    }
  }
  wasTouched = now;
}

// =====================================================================
//  SETUP / LOOP
// =====================================================================
void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(0);

  C_SKY    = tft.color565(120, 190, 245);
  C_ROAD   = tft.color565(95, 95, 105);
  C_LINE   = tft.color565(250, 230, 90);
  C_SUN    = tft.color565(255, 220, 60);
  C_WHITE  = tft.color565(245, 245, 245);
  C_BLACK  = tft.color565(20, 20, 20);
  C_WINDOW = tft.color565(150, 210, 235);
  C_TIRE   = tft.color565(30, 30, 30);
  C_HUB    = tft.color565(190, 190, 190);
  C_RED    = tft.color565(225, 45, 40);
  C_BLUE   = tft.color565(35, 80, 210);
  C_GREEN  = tft.color565(50, 165, 70);
  C_YELLOW = tft.color565(250, 205, 40);
  C_DGREY  = tft.color565(70, 70, 75);
  C_ORANGE = tft.color565(245, 140, 30);
  C_CYAN   = tft.color565(40, 190, 190);
  C_MAROON = tft.color565(150, 40, 45);

  spr.setColorDepth(16);
  spr.createSprite(SPRW, SPRH);

  ctp.begin();

  led.begin();
  led.setBrightness(60);
  led.clear();
  led.show();

  i2sSetup();
  soundQueue = xQueueCreate(4, sizeof(int));
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, NULL, 1, NULL, 0);

  randomSeed(esp_random());

  drawScene();

  // Startaufstellung: vier verschiedene Fahrzeuge, abwechselnde Richtung
  VType startTypes[N_LANES] = {V_FEUERWEHR, V_POLIZEI, V_TRAKTOR, V_BAGGER};
  for (int i = 0; i < N_LANES; i++) {
    Vehicle &v = vehicles[i];
    v.type  = startTypes[i];
    v.cy    = SKY_H + i * LANE_H + LANE_H / 2;
    v.dir   = (i % 2 == 0) ? 1 : -1;
    v.speed = 1.0f + i * 0.4f;
    v.x     = (v.dir > 0) ? -(VW + i * 50) : (SCR_W + i * 50);
    v.bounceUntil = 0;
  }
}

void loop() {
  static uint32_t nextTick = 0;
  handleTouch();

  if (millis() >= nextTick) {
    nextTick = millis() + 33;                       // ~30 FPS

    for (int i = 0; i < N_LANES; i++) {
      Vehicle &v = vehicles[i];
      v.x += v.speed * v.dir;
      if (v.dir > 0 && v.x > SCR_W + MARG)      respawn(v);   // rechts raus
      else if (v.dir < 0 && v.x < -(VW + MARG)) respawn(v);   // links raus
      drawVehicle(v);
    }
  }

  if (ledOffAt && millis() > ledOffAt) {
    led.clear(); led.show();
    ledOffAt = 0;
  }
}
