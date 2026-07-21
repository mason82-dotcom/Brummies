# 🚒 Fahrzeuge antippen — ein Spiel für kleine Kinder (~2 Jahre)

Ein einfaches, buntes Ursache‑und‑Wirkung‑Spiel für das
**Freenove ESP32‑S3‑WROOM Board** mit **2.8″ Touch‑Screen** und Lautsprecher.

Feuerwehr 🚒, Polizei 🚓, Traktor 🚜 und Bagger 🚧 fahren langsam über den
Bildschirm. Tippt das Kind ein Fahrzeug an, macht es sein typisches Geräusch
(Martinshorn, Sirene, Motor‑Tuckern, Rückfahr‑Piepen), hüpft kurz und die
Onboard‑RGB‑LED blinkt in der Fahrzeugfarbe.

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

- Echte Aufnahme‑Geräusche (WAV/MP3) von einer SD‑Karte statt synthetischer Töne
  (das Kit hat SD‑Slot + `ESP32-audioI2S`‑Bibliothek).
- Tiere statt Fahrzeuge, oder eine Zähl‑Version (jedes Antippen zählt hoch).
- Fahrzeuge in beide Richtungen fahren lassen.

Viel Spaß! 🎉
