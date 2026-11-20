/*
 * Testprogramm für den Kern ohne GPU: rastert die Dreiecke in Software und
 * schreibt ein PPM-Bild. Prüft Geometrie und Wand unabhängig von Metal/OpenGL.
 *
 *   cc -O2 -std=c11 core/egypt_core.c core/test_render.c -lm -o test_render
 *   ./test_render out.ppm [breite] [höhe] [sekunde] [palette]
 */
#include "egypt_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static float edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static void blend_tri(uint8_t *img, int W, int H, const EgyptVertex *v)
{
    float minx = fminf(v[0].x, fminf(v[1].x, v[2].x)), maxx = fmaxf(v[0].x, fmaxf(v[1].x, v[2].x));
    float miny = fminf(v[0].y, fminf(v[1].y, v[2].y)), maxy = fmaxf(v[0].y, fmaxf(v[1].y, v[2].y));
    int x0 = (int)fmaxf(0, floorf(minx)), x1 = (int)fminf(W - 1, ceilf(maxx));
    int y0 = (int)fmaxf(0, floorf(miny)), y1 = (int)fminf(H - 1, ceilf(maxy));
    float area = edge(v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y);
    if (fabsf(area) < 1e-9f) return;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = edge(v[1].x, v[1].y, v[2].x, v[2].y, px, py) / area;
            float w1 = edge(v[2].x, v[2].y, v[0].x, v[0].y, px, py) / area;
            float w2 = edge(v[0].x, v[0].y, v[1].x, v[1].y, px, py) / area;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            uint8_t *p = img + ((size_t)y * W + x) * 4;
            float a = v[0].a;
            p[0] = (uint8_t)lroundf(p[0] * (1 - a) + v[0].r * 255 * a);
            p[1] = (uint8_t)lroundf(p[1] * (1 - a) + v[0].g * 255 * a);
            p[2] = (uint8_t)lroundf(p[2] * (1 - a) + v[0].b * 255 * a);
        }
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "Aufruf: %s out.ppm [w] [h] [t] [palette]\n", argv[0]); return 1; }
    int W = argc > 2 ? atoi(argv[2]) : 1600, H = argc > 3 ? atoi(argv[3]) : 1000;
    double t = argc > 4 ? atof(argv[4]) : 2.0;
    EgyptSettings s;
    egypt_default_settings(&s);
    if (argc > 5) s.palette = atoi(argv[5]);

    uint8_t *img = malloc((size_t)W * H * 4);
    egypt_build_background(&s, W, H, img);
    size_t cap = egypt_max_vertices(&s, (float)W, (float)H);
    EgyptVertex *v = malloc(cap * sizeof *v);
    size_t n = egypt_build_scene(&s, (float)W, (float)H, t, v, cap);
    fprintf(stderr, "%zu Vertices (%zu Dreiecke), Kapazität %zu\n", n, n / 3, cap);
    for (size_t i = 0; i + 2 < n; i += 3) blend_tri(img, W, H, &v[i]);

    FILE *f = fopen(argv[1], "wb");
    if (!f) { perror(argv[1]); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (size_t i = 0; i < (size_t)W * H; i++) fwrite(img + i * 4, 1, 3, f);
    fclose(f);
    free(v);
    free(img);
    return 0;
}
