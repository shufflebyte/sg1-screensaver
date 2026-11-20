/*
 * Ägyptische Tänzer – gemeinsamer Kern (macOS und Linux)
 *
 * Der Kern kennt keine Grafik-API. Er liefert
 *   - die Figuren eines Zeitpunkts als Dreiecksliste mit Farben (Pixelkoordinaten,
 *     Ursprung oben links, y nach unten, Farben premultiplied-fähig als RGBA 0..1)
 *   - die Sandsteinwand als RGBA8-Bild.
 * Die Plattform-Renderer (Metal, OpenGL) zeichnen nur noch diese Daten.
 */
#ifndef EGYPT_CORE_H
#define EGYPT_CORE_H

#include <stddef.h>
#include <stdint.h>

enum { EGYPT_PATTERN_WAVE = 0, EGYPT_PATTERN_CANON = 1, EGYPT_PATTERN_SYNC = 2, EGYPT_PATTERN_RANDOM = 3 };
enum { EGYPT_SCROLL_OFF = 0, EGYPT_SCROLL_SAME = 1, EGYPT_SCROLL_ALT = 2 };
enum { EGYPT_PALETTE_ROSE = 0, EGYPT_PALETTE_OLIVE = 1 };

typedef struct {
    int   rows;     /* Anzahl Bildreihen (2..14) */
    float spacing;  /* Figurenabstand relativ zur Reihenhöhe */
    float tempo;    /* Posen pro Sekunde */
    int   pattern;  /* EGYPT_PATTERN_* */
    int   scroll;   /* EGYPT_SCROLL_* */
    int   palette;  /* EGYPT_PALETTE_* */
    int   crt;      /* Röhren-Look an/aus (wirkt hier auf das Leuchten der Schurze) */
} EgyptSettings;

typedef struct {
    float x, y;        /* Pixel */
    float r, g, b, a;  /* 0..1, nicht premultipliziert */
} EgyptVertex;

void   egypt_default_settings(EgyptSettings *s);

/* Obergrenze an Vertices für egypt_build_scene bei dieser Bildgröße. */
size_t egypt_max_vertices(const EgyptSettings *s, float width, float height);

/* Erzeugt die Figuren zum Zeitpunkt t (Sekunden). Gibt die Anzahl geschriebener
 * Vertices zurück (immer Vielfaches von 3, höchstens cap). */
size_t egypt_build_scene(const EgyptSettings *s, float width, float height, double t,
                         EgyptVertex *out, size_t cap);

/* Füllt rgba (width*height*4 Bytes, Zeile 0 = oben) mit Wand, Bodenstreifen und Sims. */
void   egypt_build_background(const EgyptSettings *s, int width, int height, uint8_t *rgba);

#endif
