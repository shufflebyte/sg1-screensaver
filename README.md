# Ägyptische Tänzer

Nachbau des Bildschirmschoners von Dr. Daniel Jacksons Monitor aus Stargate SG-1,
Staffel 2, Folge 15 („Die fünfte Spezies“): eigene, prozedural gezeichnete
Vektor-Animation, scharf in jeder Auflösung bis 5K/6K.

## Aufbau

| Ordner | Inhalt |
|---|---|
| `core/` | Gemeinsamer Kern in C: Figur, Posen, Tanzablauf, Wand. Liefert Dreiecke + Hintergrundbild, keine Grafik-API. Wird auch für Linux genutzt. |
| `macos/` | Bildschirmschoner für macOS (Swift, ScreenSaver.framework, Metal mit 4× Kantenglättung) |
| `linux/` | Bildschirmschoner für Linux (C, X11/XWayland, OpenGL 3.3 mit 4× Kantenglättung) |
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

## Linux

Voraussetzungen (Ubuntu/Debian):

```bash
sudo apt install build-essential pkg-config libx11-dev libgl-dev
```

Bauen und ausprobieren:

```bash
cd linux
make
./build/egypt-dancers              # im Fenster: Esc/Q beendet, Leertaste Pause, F Vollbild
./build/egypt-dancers --help       # alle Optionen (Reihen, Tempo, Palette, Röhren-Look …)
```

### Im App-Menü

```bash
make install    # Programm nach ~/.local/bin, Eintrag „Ägyptische Tänzer“ im App-Menü
```

Danach lässt sich der Bildschirmschoner wie eine App starten (Aktivitäten → „Ägyptische
Tänzer“). Er läuft im Vollbild und endet bei Taste, Mausklick oder Mausbewegung.
Bewegungen in den ersten 2 Sekunden nach dem Start werden ignoriert. Per Rechtsklick auf
das Icon gibt es „Im Fenster starten“. Entfernen: `make uninstall`.

Automatisch bei Leerlauf startet er unter GNOME/Wayland nicht: GNOME hat keine
Schnittstelle für Bildschirmschoner, und XScreenSaver läuft unter Wayland nicht.

### XScreenSaver (X11-Sitzungen)

`egypt-dancers` versteht `-root`, `-window-id` und `$XSCREENSAVER_WINDOW` und lässt sich
deshalb als Modul eintragen. In `~/.xscreensaver` unter `programs:` ergänzen:

```
  GL: egypt-dancers --root \n\
```

### Testwerkzeug

```bash
make snapshot     # build/snapshot.ppm in 2560x1440 und Zeit pro Bild
make test-core    # Kern ohne GPU (Software-Rasterung) -> build/core.ppm
```

## Prototyp

```bash
open prototype/index.html
```

Tasten: `H` Panel ein/aus, `F` Vollbild, Leertaste Pause.

## Geplant

- Optionsfenster in den Systemeinstellungen (Reihen, Tempo, Farben …)
- Linux: ein Fenster pro Bildschirm bei mehreren Monitoren (bisher nur der aktuelle)
