# Ägyptische Tänzer

Nachbau des Bildschirmschoners von Dr. Daniel Jacksons Monitor aus Stargate SG-1,
Staffel 2, Folge 15 („Die fünfte Spezies“): eigene, prozedural gezeichnete
Vektor-Animation, scharf in jeder Auflösung bis 5K/6K.

## Aufbau

| Ordner | Inhalt |
|---|---|
| `core/` | Gemeinsamer Kern in C: Figur, Posen, Tanzablauf, Wand. Liefert Dreiecke + Hintergrundbild, keine Grafik-API. Wird auch für Linux genutzt. |
| `macos/` | Bildschirmschoner für macOS (Swift, ScreenSaver.framework, Metal mit 4× Kantenglättung) |
| `prototype/` | Browser-Prototyp zum Ausprobieren von Look und Einstellungen |
| `reference/` | Einzelbilder aus den Referenzclips |

## macOS

Voraussetzung: Xcode (nicht nur die Command Line Tools). Läuft ab macOS 13 auf
Apple Silicon und Intel.

Bauen und installieren:

```bash
cd macos
./build.sh install
```

Danach in **Systemeinstellungen → Bildschirmschoner** „Ägyptische Tänzer“ auswählen.
Nur bauen ohne Installation: `./build.sh` → `macos/build/Aegyptische Taenzer.saver`
(lässt sich auch per Doppelklick installieren). Der Dateiname hat absichtlich keine
Umlaute: macOS 14+ speichert die Auswahl als URL in zerlegter Unicode-Form und findet
ein Bundle namens `Ägyptische Tänzer.saver` danach nicht wieder (es läuft dann das
Luftbild statt des Bildschirmschoners).

`./build.sh install` startet danach `WallpaperAgent` und `legacyScreenSaver` neu. Ohne
diesen Neustart lädt macOS weiter das alte Bundle und der Bildschirmschoner bleibt schwarz.

Feste Einstellungen (in `core/egypt_core.c`, `egypt_default_settings`):
5 Reihen, Abstand 1,05, Tempo 1,05, Welle, alle Reihen scrollen nach rechts,
Sandstein rosé, Röhren-Look an.

### Testwerkzeug

Rendert mit demselben Metal-Renderer einzelne Bilder als PNG und misst die Zeit pro Bild:

```bash
cd macos
./build.sh snapshot
./build/snapshot test.png 5120 2880 3.0
```

Der Kern lässt sich auch ohne GPU prüfen (Software-Rasterung, PPM-Ausgabe):
`core/test_render.c`.

## Prototyp

```bash
open prototype/index.html
```

Tasten: `H` Panel ein/aus, `F` Vollbild, Leertaste Pause.

## Geplant

- Optionsfenster in den Systemeinstellungen (Reihen, Tempo, Farben …)
- Linux: XScreenSaver-Modul (C + OpenGL) auf Basis von `core/`
