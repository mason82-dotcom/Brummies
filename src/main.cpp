// =====================================================================
//  FAHRZEUGE ANTIPPEN  -  ein einfaches Spiel fuer kleine Kinder (~2 J.)
//
//  Hardware: Freenove ESP32-S3-WROOM + Shield + 2.8" Touch-Screen
//            Display : ST7789V 240x320 (SPI)   -> via TFT_eSPI
//            Touch   : FT6336U (I2C, SDA=2 SCL=1)
//            Sound   : I2S-Lautsprecher (BCLK=42, LRC=14, DOUT=41)
//            RGB-LED : WS2812 (GPIO48)
//
//  Feuerwehr, Polizei, Traktor und Bagger fahren langsam ueber den
//  Bildschirm. Tippt das Kind ein Fahrzeug an, macht es sein Geraeusch,
//  huepft kurz und die Onboard-LED blinkt in der Fahrzeugfarbe.
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
static const int MARG = 5;  // horizontaler Rand im Sprite (>= max. Speed)
static const int BNC  = 9;  // vertikaler Rand (Huepf-Reserve)
static const int SPRW = VW + 2 * MARG;   // 104
static const int SPRH = VH + 2 * BNC;    // 62

// ---------------------------------------------------------------------
//  Farben (werden in setup() gefuellt)
// ---------------------------------------------------------------------
uint16_t C_SKY, C_ROAD, C_LINE, C_SUN, C_WHITE, C_BLACK, C_WINDOW, C_TIRE, C_HUB;
uint16_t C_RED, C_BLUE, C_GREEN, C_YELLOW, C_DGREY, C_ORANGE, C_LBLUE;

// ---------------------------------------------------------------------
//  Sound-IDs
// ---------------------------------------------------------------------
enum { SND_FEUERWEHR = 0, SND_POLIZEI, SND_TRAKTOR, SND_BAGGER };
QueueHandle_t soundQueue;

// ---------------------------------------------------------------------
//  Fahrzeuge
// ---------------------------------------------------------------------
enum VType { V_FEUERWEHR = 0, V_POLIZEI, V_TRAKTOR, V_BAGGER };

struct Vehicle {
  VType   type;
  float   x;          // linke Kante (Weltkoordinate)
  int     cy;         // Mittelpunkt y (Fahrbahn)
  float   speed;      // px pro Tick
  int     sound;
  uint32_t bounceUntil;   // huepft bis zu diesem Zeitpunkt
};

Vehicle vehicles[N_LANES];

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
  switch (id) {
    case SND_FEUERWEHR:                       // Martinshorn "Tatü-Taata"
      for (int i = 0; i < 3; i++) {
        synth(659, 659, 300, 0, 0.8f);        // hoch  (E5)
        synth(494, 494, 300, 0, 0.8f);        // tief  (H4)
      }
      break;
    case SND_POLIZEI:                          // heulende Sirene
      for (int i = 0; i < 3; i++) {
        synth(600, 1200, 350, 0, 0.7f);
        synth(1200, 600, 350, 0, 0.7f);
      }
      break;
    case SND_TRAKTOR:                          // tuckernder Motor + Hupe
      synth(85, 90, 1400, 1, 0.9f, 9.0f, 0.9f);
      synth(300, 300, 250, 1, 0.7f);           // "pöp"
      break;
    case SND_BAGGER:                           // Rueckfahr-Piepen + Grummeln
      for (int i = 0; i < 3; i++) {
        synth(1000, 1000, 220, 1, 0.6f);
        silence(140);
      }
      synth(70, 70, 500, 1, 0.9f, 12.0f, 0.8f);
      break;
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
//  ZEICHNEN
// =====================================================================
// kleines Rad in den Sprite zeichnen
static void wheel(int x, int y, int r) {
  spr.fillCircle(x, y, r, C_TIRE);
  spr.fillCircle(x, y, r / 2, C_HUB);
}

// ---- Feuerwehr (rot, mit Leiter und Blaulicht) ----
static void drawFeuerwehr(int ox, int oy) {
  int bodyTop = oy + 6, bodyH = VH - 16;
  spr.fillRoundRect(ox + 2, bodyTop, VW - 6, bodyH, 4, C_RED);     // Aufbau
  spr.fillRoundRect(ox + VW - 34, bodyTop - 8, 32, bodyH + 8, 4, C_RED); // Fahrerhaus
  spr.fillRect(ox + VW - 30, bodyTop - 4, 22, 14, C_WINDOW);       // Scheibe
  // Leiter
  for (int i = 0; i < 6; i++)
    spr.drawLine(ox + 8 + i * 6, bodyTop + 4, ox + 8 + i * 6, bodyTop + 14, C_DGREY);
  spr.drawLine(ox + 8, bodyTop + 4, ox + 40, bodyTop + 4, C_DGREY);
  spr.drawLine(ox + 8, bodyTop + 14, ox + 40, bodyTop + 14, C_DGREY);
  // Blaulicht
  spr.fillRect(ox + VW - 26, bodyTop - 14, 14, 6, C_BLUE);
  wheel(ox + 20, oy + VH - 7, 9);
  wheel(ox + VW - 20, oy + VH - 7, 9);
}

// ---- Polizei (blau/weiss mit Lichtbalken) ----
static void drawPolizei(int ox, int oy) {
  int top = oy + 14;
  spr.fillRoundRect(ox + 4, top, VW - 8, VH - 22, 5, C_BLUE);       // unterer Koerper
  // Dach/Kabine
  spr.fillRoundRect(ox + 24, oy + 2, VW - 48, 18, 5, C_WHITE);
  spr.fillRoundRect(ox + 28, oy + 5, VW - 56, 12, 3, C_WINDOW);     // Scheiben
  spr.fillRect(ox + 10, top + 2, VW - 20, 8, C_WHITE);             // weisser Streifen
  // Lichtbalken auf dem Dach
  spr.fillRect(ox + VW / 2 - 12, oy, 12, 5, C_RED);
  spr.fillRect(ox + VW / 2,      oy, 12, 5, C_BLUE);
  wheel(ox + 22, oy + VH - 7, 9);
  wheel(ox + VW - 22, oy + VH - 7, 9);
}

// ---- Traktor (gruen, grosses Hinterrad, Auspuff) ----
static void drawTraktor(int ox, int oy) {
  // Motorhaube vorne
  spr.fillRoundRect(ox + VW - 40, oy + 16, 36, VH - 26, 3, C_GREEN);
  // Fahrerkabine
  spr.fillRoundRect(ox + 20, oy + 2, 30, VH - 14, 3, C_GREEN);
  spr.fillRect(ox + 24, oy + 5, 22, 14, C_WINDOW);
  // Auspuff
  spr.fillRect(ox + VW - 34, oy + 6, 5, 12, C_DGREY);
  // Scheinwerfer
  spr.fillCircle(ox + VW - 6, oy + 22, 3, C_YELLOW);
  wheel(ox + VW - 18, oy + VH - 10, 8);    // kleines Vorderrad
  wheel(ox + 24, oy + VH - 6, 15);         // grosses Hinterrad
}

// ---- Bagger (gelb, mit Baggerarm und Schaufel) ----
static void drawBagger(int ox, int oy) {
  // Raupen/Fahrwerk
  spr.fillRoundRect(ox + 6, oy + VH - 16, VW - 24, 14, 6, C_BLACK);
  for (int i = 0; i < 5; i++) spr.fillCircle(ox + 16 + i * 14, oy + VH - 9, 5, C_HUB);
  // Kabine
  spr.fillRoundRect(ox + 8, oy + 8, 34, VH - 22, 4, C_YELLOW);
  spr.fillRect(ox + 12, oy + 11, 22, 14, C_WINDOW);
  // Baggerarm (Ausleger + Stiel + Schaufel)
  spr.drawLine(ox + 40, oy + 16, ox + VW - 20, oy + 4,  C_YELLOW);
  spr.drawLine(ox + 41, oy + 17, ox + VW - 19, oy + 5,  C_YELLOW);
  spr.drawLine(ox + VW - 20, oy + 4, ox + VW - 6, oy + 22, C_YELLOW);
  spr.drawLine(ox + VW - 19, oy + 4, ox + VW - 5, oy + 22, C_YELLOW);
  // Schaufel
  spr.fillTriangle(ox + VW - 12, oy + 20, ox + VW - 2, oy + 22,
                   ox + VW - 8,  oy + 30, C_DGREY);
}

static void drawVehicleArt(VType t, int ox, int oy) {
  switch (t) {
    case V_FEUERWEHR: drawFeuerwehr(ox, oy); break;
    case V_POLIZEI:   drawPolizei(ox, oy);   break;
    case V_TRAKTOR:   drawTraktor(ox, oy);   break;
    case V_BAGGER:    drawBagger(ox, oy);    break;
  }
}

// Statische Szene einmalig zeichnen: Himmel, Sonne, Titel, Strasse, Fahrspuren
static void drawScene() {
  tft.fillRect(0, 0, SCR_W, SKY_H, C_SKY);
  tft.fillCircle(SCR_W - 26, 22, 16, C_SUN);          // Sonne
  tft.setTextColor(C_WHITE, C_SKY);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Tipp ein Auto!", 10, 16, 4);

  tft.fillRect(0, SKY_H, SCR_W, SCR_H - SKY_H, C_ROAD);   // Strasse
  // gestrichelte Fahrspur-Linien zwischen den Spuren
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
                 ? (int)(-7 * fabsf(sinf(millis() * 0.03f)))   // huepft nach oben
                 : 0;
  drawVehicleArt(v.type, MARG, BNC + bounce);
  spr.pushSprite((int)v.x - MARG, v.cy - VH / 2 - BNC);
}

// =====================================================================
//  TOUCH
// =====================================================================
static bool wasTouched = false;
uint32_t ledOffAt = 0;                 // Zeitpunkt, zu dem die LED wieder ausgeht

static void handleTouch() {
  FT6336U_TouchPointType tp = ctp.scan();
  bool now = (tp.touch_count > 0);

  if (now && !wasTouched) {                 // nur bei neuem Antippen
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
    // welches Fahrzeug wurde getroffen?
    for (int i = 0; i < N_LANES; i++) {
      Vehicle &v = vehicles[i];
      int left = (int)v.x, right = (int)v.x + VW;
      int topY = v.cy - VH / 2 - BNC, botY = v.cy + VH / 2 + BNC;
      if (sx >= left && sx <= right && sy >= topY && sy <= botY) {
        xQueueSend(soundQueue, &v.sound, 0);         // Geraeusch abspielen
        v.bounceUntil = millis() + 700;              // huepfen
        // LED in Fahrzeugfarbe blinken
        uint16_t c = (v.type == V_FEUERWEHR) ? C_RED  :
                     (v.type == V_POLIZEI)   ? C_BLUE :
                     (v.type == V_TRAKTOR)   ? C_GREEN : C_YELLOW;
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

  // Display
  tft.init();
  tft.setRotation(0);              // Hochformat 240x320

  // Farben
  C_SKY    = tft.color565(120, 190, 245);
  C_ROAD   = tft.color565(95, 95, 105);
  C_LINE   = tft.color565(250, 230, 90);
  C_SUN    = tft.color565(255, 220, 60);
  C_WHITE  = tft.color565(245, 245, 245);
  C_BLACK  = tft.color565(20, 20, 20);
  C_WINDOW = tft.color565(150, 210, 235);
  C_TIRE   = tft.color565(30, 30, 30);
  C_HUB    = tft.color565(180, 180, 180);
  C_RED    = tft.color565(225, 45, 40);
  C_BLUE   = tft.color565(35, 80, 210);
  C_GREEN  = tft.color565(50, 165, 70);
  C_YELLOW = tft.color565(250, 205, 40);
  C_DGREY  = tft.color565(70, 70, 75);
  C_ORANGE = tft.color565(245, 140, 30);
  C_LBLUE  = tft.color565(120, 200, 235);

  // Sprite
  spr.setColorDepth(16);
  spr.createSprite(SPRW, SPRH);

  // Touch
  ctp.begin();

  // LED
  led.begin();
  led.setBrightness(60);
  led.clear();
  led.show();

  // Audio
  i2sSetup();
  soundQueue = xQueueCreate(4, sizeof(int));
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, NULL, 1, NULL, 0);

  // Szene
  drawScene();

  // Fahrzeuge in die 4 Spuren setzen (jeweils andere Farbe & Geschwindigkeit)
  VType types[N_LANES] = {V_FEUERWEHR, V_POLIZEI, V_TRAKTOR, V_BAGGER};
  int   snds [N_LANES] = {SND_FEUERWEHR, SND_POLIZEI, SND_TRAKTOR, SND_BAGGER};
  float spds [N_LANES] = {1.6f, 2.2f, 1.1f, 1.4f};
  for (int i = 0; i < N_LANES; i++) {
    vehicles[i].type  = types[i];
    vehicles[i].sound = snds[i];
    vehicles[i].speed = spds[i];
    vehicles[i].cy    = SKY_H + i * LANE_H + LANE_H / 2;
    vehicles[i].x     = -VW - i * 60;          // versetzt starten
    vehicles[i].bounceUntil = 0;
  }
}

void loop() {
  static uint32_t nextTick = 0;
  handleTouch();                                    // so oft wie moeglich abfragen

  if (millis() >= nextTick) {
    nextTick = millis() + 33;                       // ~30 FPS

    for (int i = 0; i < N_LANES; i++) {
      Vehicle &v = vehicles[i];
      v.x += v.speed;
      if (v.x > SCR_W + MARG) v.x = -VW;             // von rechts raus -> links rein
      drawVehicle(v);
    }
  }

  if (ledOffAt && millis() > ledOffAt) {            // LED wieder aus
    led.clear(); led.show();
    ledOffAt = 0;
  }
}
