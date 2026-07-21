# 🚒 Fahrzeuge antippen — ein Spiel für kleine Kinder (~2 Jahre)

Ein einfaches, buntes Ursache‑und‑Wirkung‑Spiel für das
**Freenove ESP32‑S3‑WROOM Board** mit **2.8″ Touch‑Screen** und Lautsprecher.

**Acht** verschiedene Fahrzeuge fahren langsam über den Bildschirm — in **beide
Richtungen**. Verlässt eines den Bildschirm, kommt ein neues, zufälliges
Fahrzeug nach:

🚒 Feuerwehr · 🚓 Polizei · 🚑 Krankenwagen · 🚜 Traktor · 🚧 Bagger ·
🚛 Müllwagen · 🚗 Auto · 🚂 Zug

Tippt das Kind ein Fahrzeug an, macht es sein typisches Geräusch (Martinshorn,
Sirene, Yelp, Motor‑Tuckern, Rückfahr‑Piepen, Hupe, Zug‑Horn …), hüpft kurz und
die Onboard‑RGB‑LED blinkt in der Fahrzeugfarbe.

---

## 🧩 Benötigte Hardware

Alles Teil des **Freenove Development Kit for ESP32‑S3**:

| Komponente | Detail |
|---|---|
| Freenove ESP32‑S3‑WROOM Board | + ESP32‑S3‑WROOM Shield |
| Freenove 2.8″ Screen | ST7789V (240×320), Touch FT6336U |
| Lautsprecher | am Shield (I2S über PCM5101) |
| USB‑C Kabel | zum Programmieren (**UART‑Port** verwenden) |

Das Display steckt über den Shield auf dem Board — **keine Verkabelung nötig**.
Beim Aufstecken auf die Markierungen achten (siehe Freenove‑Tutorial Kap. 7).

### Verwendete Pins (fest im Kit verdrahtet)

| Funktion | Pins |
|---|---|
| Display (SPI, ST7789) | MOSI 20, SCLK 21, DC 0, CS/RST fest |
| Touch (I2C, FT6336U) | SDA 2, SCL 1 |
| Sound (I2S) | BCLK 42, LRC 14, DOUT 41 |
| RGB‑LED (WS2812) | GPIO 48 |

---

## ▶️ Bauen & Flashen (PlatformIO)

1. [PlatformIO](https://platformio.org/) installieren (VS Code Extension oder CLI).
2. Board per USB‑C anschließen — **den UART‑Port** benutzen (der mit „UART"
   beschriftete Anschluss; siehe Freenove `Arduino_Configuration_USB_UART.png`).
3. Im Projektordner:

   ```bash
   pio run --target upload      # kompilieren + flashen
   pio device monitor           # serielle Ausgabe (115200 Baud)
   ```

   In VS Code alternativ die Buttons **Build** (✓) und **Upload** (→).

---

## 🎨 Anpassen

Alles Wichtige steht oben in `src/main.cpp`:

- **Geschwindigkeiten** der Fahrzeuge: Array `spds[]` in `setup()`.
- **Fahrzeugtypen/Reihenfolge**: Arrays `types[]` / `snds[]` in `setup()`.
- **Geräusche**: Funktion `playSound()` — Frequenzen/Dauer frei änderbar.
- **Farben**: die `C_*` Werte in `setup()`.
- **Fahrzeug‑Aussehen**: die `draw…()`‑Funktionen (mit einfachen Rechtecken,
  Kreisen und Linien gezeichnet — leicht zu erweitern).

---

## 🔊 Echte Geräusche als WAV (SD‑Karte)

Das Spiel spielt **automatisch** echte Sound‑Dateien von einer microSD‑Karte,
wenn welche vorhanden sind — **sonst** fallen die Geräusche auf die eingebauten
synthetischen Töne zurück. Es läuft also **mit und ohne Karte**.

**So geht's:**

1. microSD‑Karte mit **FAT32** formatieren.
2. Acht WAV‑Dateien **ins Hauptverzeichnis** (Root) der Karte legen — exakt so
   benannt:
   `feuerwehr.wav  polizei.wav  krankenwagen.wav  traktor.wav
   bagger.wav  muellwagen.wav  auto.wav  zug.wav`
   (Fehlt eine Datei, nimmt nur dieses eine Fahrzeug den synthetischen Ton.)
3. Format der WAVs: **PCM, 16‑bit, mono oder stereo** (Sample‑Rate egal, wird
   automatisch erkannt). Andere Formate (z. B. 24‑bit, MP3) werden übersprungen
   → dann greift der Fallback.
4. Karte in den SD‑Slot des Shields stecken, einschalten. Der serielle Monitor
   zeigt `SD-Karte erkannt` bzw. `Keine SD-Karte`.

Dateien findest du hier — **frei nutzbar** (CC0 / lizenzfrei, kommerziell ok):

| Quelle | Lizenz | Hinweis |
|---|---|---|
| [Pixabay – Sound Effects](https://pixabay.com/sound-effects/) | Pixabay‑Lizenz, keine Namensnennung | Sirenen top: [firetruck](https://pixabay.com/sound-effects/search/firetruck/), [police‑siren](https://pixabay.com/sound-effects/search/police-siren/), [tractor](https://pixabay.com/sound-effects/search/tractor/), [excavator](https://pixabay.com/sound-effects/search/excavator/) |
| [BigSoundBank](https://bigsoundbank.com/categories.html) | **CC0** (Public Domain) | Direkter WAV‑Download ohne Login, z. B. [Bagger‑Motor](https://bigsoundbank.com/sound-2147-excavator-engine.html) |
| [Freesound](https://freesound.org/) | gemischt – **auf CC0 filtern!** | Riesige Auswahl, Konto nötig |
| [Mixkit](https://mixkit.co/free-sound-effects/) | Mixkit‑Lizenz, kostenlos | Fahrzeug‑/Motor‑Sounds |

**Suchbegriffe:** `fire truck siren`, `police siren`, `ambulance siren`,
`tractor engine`, `excavator` / `digger`, `garbage truck reverse beep`,
`car horn`, `train horn`.

### Dateien fürs ESP32 vorbereiten

Kurze Clips (1–3 s) reichen und sind für Kleinkinder am besten. So werden sie
ins passende Format gebracht (mono, 16‑bit, 22050 Hz) — mit
[ffmpeg](https://ffmpeg.org/):

```bash
ffmpeg -i download.mp3 -ac 1 -ar 22050 -sample_fmt s16 feuerwehr.wav
```

Danach die Datei passend benennen (`feuerwehr.wav`, `polizei.wav`, …, siehe
Liste oben) und ins Root der SD‑Karte kopieren. Fertig — beim nächsten Start
spielt das Spiel automatisch die echten Geräusche.

---

## 🛠️ Fehlerbehebung

| Problem | Lösung |
|---|---|
| **Bildschirm bleibt schwarz** | UART‑Port zum Flashen benutzt? Display richtig aufgesteckt? |
| **Farben sehen „negativ"/vertauscht aus** | In `platformio.ini` die Zeile `-D TFT_INVERSION_ON=1` einkommentieren (entspricht Freenoves Konfiguration „CFG2") und neu flashen. |
| **Rot/Blau vertauscht** | In `platformio.ini` `-D TFT_RGB_ORDER=TFT_BGR` auf `TFT_RGB` ändern. |
| **Antippen reagiert an falscher Stelle (gespiegelt)** | In `src/main.cpp` `TOUCH_INVERT_X` bzw. `TOUCH_INVERT_Y` auf `1` setzen. Zum Prüfen `TOUCH_DEBUG 1` setzen — dann werden die Touch‑Koordinaten im seriellen Monitor ausgegeben. |
| **Kein Ton** | Lautsprecher am Shield angeschlossen? Lautstärke im Code über den `amp`‑Parameter in `playSound()` erhöhen. |

---

## 📁 Projektstruktur

```
platformio.ini        Board-, Display- und Build-Konfiguration
src/main.cpp          das komplette Spiel
lib/FT6336U/          Touch-Bibliothek (identisch zum Freenove-Kit)
```

TFT_eSPI und Adafruit NeoPixel werden von PlatformIO automatisch geladen.

---

## 💡 Ideen für später

- Echte Aufnahme‑Geräusche (WAV/MP3) von SD‑Karte (siehe Abschnitt „Echte
  Geräusche als WAV" oben — Einbau auf Zuruf).
- Tiere statt Fahrzeuge, oder eine Zähl‑Version (jedes Antippen zählt hoch).
- Namen der Fahrzeuge kurz einblenden, wenn man sie antippt.

Viel Spaß! 🎉
