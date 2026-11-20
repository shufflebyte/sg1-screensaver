/*
 * Ägyptische Tänzer – gemeinsamer Kern. Portiert aus prototype/index.html.
 *
 * Figureneinheiten: Füße bei y = 0, y wächst nach unten, Scheitel bei etwa y = -97.
 * Gliedmaßen-Winkel in Grad: [oberes Segment, Knick des unteren Segments],
 *   0 = nach unten, 90 = waagrecht nach außen, 180 = nach oben.
 */
#include "egypt_core.h"

#include <math.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ Grundtypen */

typedef struct { float x, y; } P;
typedef struct { float r, g, b, a; } C4;
typedef struct { P p[8]; int n; C4 c; } Shape; /* immer konvex */

static P  pt(float x, float y) { P p = { x, y }; return p; }
static C4 rgba(int r, int g, int b, float a) { C4 c = { r / 255.f, g / 255.f, b / 255.f, a }; return c; }

#define DEG ((float)(M_PI / 180.0))

/* ------------------------------------------------------------------ Figur */

static const float SHOULDER_X = 11.5f, SHOULDER_Y = -77.f, UPPER_ARM = 14.f, FORE_ARM = 12.5f;
static const float HIP_X = 6.f, HIP_Y = -46.f, THIGH = 21.f, SHIN = 21.f, FOOT_H = 1.6f;

typedef struct { float la[2], ra[2], ll[2], rl[2], lift; } Pose;

enum { RUHE, KRAFT, FLUEGEL, KA, HOCKE, SPRUNG, KRONE, GREIFEN_L, GREIFEN_R };

static const Pose POSES[] = {
    [RUHE]      = { { 12,  -8 }, { 12,  -8 }, {  8,  -2 }, {  8,  -2 },  0 },
    [KRAFT]     = { { 78,  98 }, { 78,  98 }, { 20, -24 }, { 20, -24 },  0 },
    [FLUEGEL]   = { {100, -10 }, {100, -10 }, { 14,  -8 }, { 14,  -8 },  0 },
    [KA]        = { { 92,  88 }, { 92,  88 }, { 24, -34 }, { 24, -34 },  0 },
    [HOCKE]     = { { 70, -60 }, { 70, -60 }, { 48, -82 }, { 48, -82 },  0 },
    [SPRUNG]    = { {128,  30 }, {128,  30 }, { 38, -75 }, { 38, -75 }, 12 },
    [KRONE]     = { {165,  60 }, {165,  60 }, {  8,  -2 }, {  8,  -2 },  0 },
    [GREIFEN_L] = { {165,   5 }, { 45,-105 }, { 26, -14 }, {  2,   0 },  0 },
    [GREIFEN_R] = { { 45,-105 }, {165,   5 }, {  2,   0 }, { 26, -14 },  0 },
};

static const int SEQ[] = { RUHE, KRAFT, FLUEGEL, KA, HOCKE, SPRUNG, KRAFT, KRONE,
                           GREIFEN_L, KRAFT, GREIFEN_R, FLUEGEL };
#define SEQ_N ((long)(sizeof SEQ / sizeof SEQ[0]))

/* Aufteilung einer Reihe (Anteile der Reihenhöhe, von unten gemessen) */
static const float ROW_GROUND_TOP = 0.13f, ROW_LEDGE_TOP = 0.04f, ROW_FEET = 0.075f;
static const float FIGURE_SCALE = 0.0077f; /* Figureneinheit -> Anteil der Reihenhöhe */
static const float SCROLL_SPEED = 0.3f;    /* Figurenabstände pro Sekunde */

#define COL_SKIN rgba(0x94, 0x50, 0x2f, 1.f)
#define COL_LINE rgba(45, 20, 8, .85f)
#define COL_WIG  rgba(0x1b, 0x12, 0x0d, 1.f)
#define COL_KILT rgba(0xea, 0xf3, 0xfd, 1.f)
#define COL_GLOW rgba(215, 232, 255, .30f)

static float clamp01(float t) { return t < 0 ? 0 : t > 1 ? 1 : t; }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static Pose lerp_pose(const Pose *a, const Pose *b, float t)
{
    Pose o;
    for (int i = 0; i < 2; i++) {
        o.la[i] = lerpf(a->la[i], b->la[i], t);
        o.ra[i] = lerpf(a->ra[i], b->ra[i], t);
        o.ll[i] = lerpf(a->ll[i], b->ll[i], t);
        o.rl[i] = lerpf(a->rl[i], b->rl[i], t);
    }
    o.lift = lerpf(a->lift, b->lift, t);
    return o;
}

/* Pro Takt eine Pose: 35 % halten, 65 % weich überblenden. */
static Pose pose_at(double beat)
{
    double kf = floor(beat);
    long k = (long)kf;
    float f = (float)(beat - kf);
    const Pose *a = &POSES[SEQ[((k % SEQ_N) + SEQ_N) % SEQ_N]];
    const Pose *b = &POSES[SEQ[(((k + 1) % SEQ_N) + SEQ_N) % SEQ_N]];
    float u = clamp01((f - 0.35f) / 0.65f);
    return lerp_pose(a, b, u * u * (3 - 2 * u));
}

static P dirv(float s, float deg) { return pt(s * sinf(deg * DEG), cosf(deg * DEG)); }

static void limb(float s, P base, float l1, float l2, const float ang[2], P out[3])
{
    P d1 = dirv(s, ang[0]);
    P j = pt(base.x + d1.x * l1, base.y + d1.y * l1);
    P d2 = dirv(s, ang[0] + ang[1]);
    out[0] = base;
    out[1] = j;
    out[2] = pt(j.x + d2.x * l2, j.y + d2.y * l2);
}

/* Viereck entlang a->b, Breiten wa/wb, Enden um e0/e1 verlängert (eckige Gelenke). */
static Shape quad(P a, P b, float wa, float wb, float e0, float e1, C4 c)
{
    float dx = b.x - a.x, dy = b.y - a.y, L = hypotf(dx, dy);
    if (L < 1e-6f) L = 1;
    float ux = dx / L, uy = dy / L, nx = -uy, ny = ux;
    P a2 = pt(a.x - ux * e0, a.y - uy * e0), b2 = pt(b.x + ux * e1, b.y + uy * e1);
    Shape s = { .n = 4, .c = c };
    s.p[0] = pt(a2.x + nx * wa / 2, a2.y + ny * wa / 2);
    s.p[1] = pt(b2.x + nx * wb / 2, b2.y + ny * wb / 2);
    s.p[2] = pt(b2.x - nx * wb / 2, b2.y - ny * wb / 2);
    s.p[3] = pt(a2.x - nx * wa / 2, a2.y - ny * wa / 2);
    return s;
}

static Shape poly(const P *p, int n, C4 c)
{
    Shape s = { .n = n, .c = c };
    for (int i = 0; i < n; i++) s.p[i] = p[i];
    return s;
}

/* Konvexes Polygon um d nach außen versetzen (Gehrung, begrenzt). Ersetzt Umrisslinien. */
static Shape expand(const Shape *s, float d)
{
    Shape o = *s;
    int n = s->n;
    float cx = 0, cy = 0;
    for (int i = 0; i < n; i++) { cx += s->p[i].x; cy += s->p[i].y; }
    cx /= n; cy /= n;

    P nrm[8];
    for (int i = 0; i < n; i++) {
        P a = s->p[i], b = s->p[(i + 1) % n];
        float dx = b.x - a.x, dy = b.y - a.y, L = hypotf(dx, dy);
        if (L < 1e-6f) { nrm[i] = pt(0, 0); continue; }
        P nn = pt(dy / L, -dx / L);
        float mx = (a.x + b.x) / 2 - cx, my = (a.y + b.y) / 2 - cy;
        if (nn.x * mx + nn.y * my < 0) { nn.x = -nn.x; nn.y = -nn.y; }
        nrm[i] = nn;
    }
    for (int i = 0; i < n; i++) {
        P n1 = nrm[(i + n - 1) % n], n2 = nrm[i];
        if (n1.x == 0 && n1.y == 0) n1 = n2;
        if (n2.x == 0 && n2.y == 0) n2 = n1;
        float k = 1 + n1.x * n2.x + n1.y * n2.y;
        if (k < 0.2f) k = 0.2f;
        o.p[i] = pt(s->p[i].x + (n1.x + n2.x) * d / k, s->p[i].y + (n1.y + n2.y) * d / k);
    }
    return o;
}

/* ------------------------------------------------------------------ Ausgabe */

typedef struct {
    EgyptVertex *buf;
    size_t cap, n;
    float ox, oy, sc; /* Figureneinheit -> Pixel */
} Em;

static void vtx(Em *e, P p, C4 c)
{
    if (e->n >= e->cap) return;
    EgyptVertex *v = &e->buf[e->n++];
    v->x = e->ox + p.x * e->sc;
    v->y = e->oy + p.y * e->sc;
    v->r = c.r; v->g = c.g; v->b = c.b; v->a = c.a;
}

static void fill(Em *e, const Shape *s)
{
    if (e->n + (size_t)(s->n - 2) * 3 > e->cap) return; /* nur ganze Polygone */
    for (int i = 1; i < s->n - 1; i++) {
        vtx(e, s->p[0], s->c);
        vtx(e, s->p[i], s->c);
        vtx(e, s->p[i + 1], s->c);
    }
}

/* Eine Ebene: erst alle Umrisse, dann alle Flächen (wie im Prototyp). */
static void layer(Em *e, const Shape *sh, int n, int outline)
{
    if (outline)
        for (int i = 0; i < n; i++) {
            Shape o = expand(&sh[i], 0.65f);
            o.c = COL_LINE;
            fill(e, &o);
        }
    for (int i = 0; i < n; i++) fill(e, &sh[i]);
}

static void figure(Em *e, const Pose *po, float x, float groundY, float scale, int glow)
{
    P arms[2][3], legs[2][3];
    limb(-1, pt(-SHOULDER_X, SHOULDER_Y), UPPER_ARM, FORE_ARM, po->la, arms[0]);
    limb( 1, pt( SHOULDER_X, SHOULDER_Y), UPPER_ARM, FORE_ARM, po->ra, arms[1]);
    limb(-1, pt(-HIP_X, HIP_Y), THIGH, SHIN, po->ll, legs[0]);
    limb( 1, pt( HIP_X, HIP_Y), THIGH, SHIN, po->rl, legs[1]);

    /* Tieferen Fuß auf den Boden setzen, minus Sprunghöhe. */
    float lowest = fmaxf(legs[0][2].y, legs[1][2].y) + FOOT_H;
    float dy = -lowest - po->lift;
    e->ox = x;
    e->oy = groundY + dy * scale;
    e->sc = scale;

    Shape sh[10];
    int n = 0;

    /* Beine mit keilförmigen Füßen */
    for (int i = 0; i < 2; i++) {
        float s = i ? 1.f : -1.f;
        const P *l = legs[i];
        sh[n++] = quad(l[0], l[1], 7.0f, 5.4f, 0, 2.4f, COL_SKIN);
        sh[n++] = quad(l[1], l[2], 5.4f, 3.8f, 2.4f, 0, COL_SKIN);
        P an = l[2];
        P f[4] = { pt(an.x - s * 1.8f, an.y - 2.4f), pt(an.x + s * 1.8f, an.y - 2.4f),
                   pt(an.x + s * 6.8f, an.y + FOOT_H), pt(an.x - s * 2.2f, an.y + FOOT_H) };
        sh[n++] = poly(f, 4, COL_SKIN);
    }
    layer(e, sh, n, 1);

    /* Schurz: gerades Trapez bis zum Knie */
    P k[4] = { pt(-7.5f, -59), pt(7.5f, -59), pt(14.5f, -30), pt(-14.5f, -30) };
    Shape kilt = poly(k, 4, COL_KILT);
    if (glow) { Shape g = expand(&kilt, 1.6f); g.c = COL_GLOW; fill(e, &g); }
    { Shape o = expand(&kilt, 0.45f); o.c = COL_LINE; fill(e, &o); }
    fill(e, &kilt);

    /* Hals und Oberkörper (V-Form) */
    P neck[4]  = { pt(-2.4f, -84), pt(2.4f, -84), pt(2.4f, -78), pt(-2.4f, -78) };
    P torso[6] = { pt(-7, -58), pt(-14, -74.5f), pt(-12.5f, -80), pt(12.5f, -80), pt(14, -74.5f), pt(7, -58) };
    n = 0;
    sh[n++] = poly(neck, 4, COL_SKIN);
    sh[n++] = poly(torso, 6, COL_SKIN);
    layer(e, sh, n, 1);

    /* Gesicht */
    P face[6] = { pt(-3.8f, -91), pt(3.8f, -91), pt(3.6f, -85.5f), pt(1.8f, -82.6f),
                  pt(-1.8f, -82.6f), pt(-3.6f, -85.5f) };
    sh[0] = poly(face, 6, COL_SKIN);
    layer(e, sh, 1, 1);

    /* Perücke aus drei konvexen Teilen: Kappe und zwei Strähnen bis zur Schulter */
    P cap[6]  = { pt(-4.4f, -97.6f), pt(4.4f, -97.6f), pt(6.6f, -94.2f), pt(6.73f, -89.6f),
                  pt(-6.73f, -89.6f), pt(-6.6f, -94.2f) };
    P lstr[4] = { pt(-6.73f, -89.6f), pt(-3.8f, -89.6f), pt(-3.8f, -80.2f), pt(-7, -80.2f) };
    P rstr[4] = { pt(3.8f, -89.6f), pt(6.73f, -89.6f), pt(7, -80.2f), pt(3.8f, -80.2f) };
    sh[0] = poly(cap, 6, COL_WIG);
    sh[1] = poly(lstr, 4, COL_WIG);
    sh[2] = poly(rstr, 4, COL_WIG);
    layer(e, sh, 3, 0);

    /* Augen */
    P le[4] = { pt(-2.6f, -88.4f), pt(-1.1f, -88.4f), pt(-1.1f, -87.6f), pt(-2.6f, -87.6f) };
    P re[4] = { pt(1.1f, -88.4f), pt(2.6f, -88.4f), pt(2.6f, -87.6f), pt(1.1f, -87.6f) };
    sh[0] = poly(le, 4, COL_WIG);
    sh[1] = poly(re, 4, COL_WIG);
    layer(e, sh, 2, 0);

    /* Arme mit Fäusten zuletzt */
    n = 0;
    for (int i = 0; i < 2; i++) {
        const P *a = arms[i];
        float dx = a[2].x - a[1].x, dyy = a[2].y - a[1].y, L = hypotf(dx, dyy);
        if (L < 1e-6f) L = 1;
        P fist = pt(a[2].x + dx / L * 3, a[2].y + dyy / L * 3);
        sh[n++] = quad(a[0], a[1], 5.4f, 4.6f, 0, 2.2f, COL_SKIN);
        sh[n++] = quad(a[1], a[2], 4.6f, 3.4f, 2.2f, 0, COL_SKIN);
        sh[n++] = quad(a[2], fist, 4.2f, 4.2f, 0, 0, COL_SKIN);
    }
    layer(e, sh, n, 1);
}

/* ------------------------------------------------------------------ Szene */

static int clamp_rows(int r) { return r < 1 ? 1 : r > 40 ? 40 : r; }

void egypt_default_settings(EgyptSettings *s)
{
    s->rows = 5;
    s->spacing = 1.05f;
    s->tempo = 1.05f;
    s->pattern = EGYPT_PATTERN_WAVE;
    s->scroll = EGYPT_SCROLL_SAME;
    s->palette = EGYPT_PALETTE_ROSE;
    s->crt = 1;
}

static float spacing_px(const EgyptSettings *s, float rh)
{
    float sp = s->spacing < 0.3f ? 0.3f : s->spacing;
    return rh * sp;
}

size_t egypt_max_vertices(const EgyptSettings *s, float width, float height)
{
    int rows = clamp_rows(s->rows);
    float rh = height / rows;
    if (rh <= 0 || width <= 0) return 0;
    size_t cols = (size_t)ceilf(width / spacing_px(s, rh)) + 4;
    return (size_t)rows * cols * 1200; /* großzügig: eine Figur braucht < 1000 */
}

static double hash2(long r, long c)
{
    double s = sin(r * 127.1 + c * 311.7) * 43758.5453;
    return s - floor(s);
}

static double beat_for(const EgyptSettings *s, double t, long r, long c)
{
    double b = t * s->tempo;
    switch (s->pattern) {
    case EGYPT_PATTERN_SYNC:   return b;
    case EGYPT_PATTERN_CANON:  return b - r;
    case EGYPT_PATTERN_RANDOM: return b + floor(hash2(r, c) * SEQ_N);
    default:                   return b - c * 0.18 - r * 0.5;
    }
}

size_t egypt_build_scene(const EgyptSettings *s, float width, float height, double t,
                         EgyptVertex *out, size_t cap)
{
    Em e = { out, cap, 0, 0, 0, 1 };
    int rows = clamp_rows(s->rows);
    float rh = height / rows;
    if (rh <= 0 || width <= 0) return 0;
    float scale = rh * FIGURE_SCALE;
    double spacing = spacing_px(s, rh);

    for (long r = 0; r < rows; r++) {
        float ground = (r + 1) * rh - rh * ROW_FEET;
        double off = 0;
        if (s->scroll != EGYPT_SCROLL_OFF) {
            double dirn = (s->scroll == EGYPT_SCROLL_ALT && (r % 2)) ? -1 : 1;
            off = dirn * t * spacing * SCROLL_SPEED;
        }
        long first = (long)floor(-off / spacing) - 1;
        long last  = (long)ceil((width - off) / spacing) + 1;
        for (long c = first; c <= last; c++) {
            float x = (float)(c * spacing + off + spacing / 2);
            Pose p = pose_at(beat_for(s, t, r, c));
            figure(&e, &p, x, ground, scale, s->crt);
        }
    }
    return e.n;
}

/* ------------------------------------------------------------------ Hintergrund */

typedef struct {
    uint8_t stone[3];
    float grain, blotch;
    uint8_t ground0[3], ground1[3], ledge[3], ledgeHi[3], ledgeLine[3];
} Palette;

static const Palette PALETTES[] = {
    [EGYPT_PALETTE_ROSE]  = { { 201, 146, 118 }, 20, 18, { 0xbd, 0xb7, 0x9c }, { 0xa3, 0x9e, 0x84 },
                              { 0xe6, 0xcf, 0xb2 }, { 0xf3, 0xe3, 0xcc }, { 0x6b, 0x51, 0x3d } },
    [EGYPT_PALETTE_OLIVE] = { { 176, 138,  98 }, 18, 18, { 0x82, 0xb0, 0x5e }, { 0x5f, 0x8e, 0x44 },
                              { 0xd8, 0xbf, 0x94 }, { 0xea, 0xd6, 0xb0 }, { 0x4e, 0x44, 0x30 } },
};

static void fill_rows(uint8_t *px, int W, int H, int y0, int y1, float r, float g, float b)
{
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    uint8_t R = (uint8_t)lroundf(r), G = (uint8_t)lroundf(g), B = (uint8_t)lroundf(b);
    for (int y = y0; y < y1; y++) {
        uint8_t *row = px + (size_t)y * W * 4;
        for (int x = 0; x < W; x++) {
            row[x * 4] = R; row[x * 4 + 1] = G; row[x * 4 + 2] = B; row[x * 4 + 3] = 255;
        }
    }
}

/* Gleicher Zufallsgenerator wie im Prototyp (mulberry32), damit die Textur stabil ist. */
static uint32_t rng_state;
static float rnd(void)
{
    rng_state += 0x6D2B79F5u;
    uint32_t s = rng_state;
    uint32_t t = (s ^ (s >> 15)) * (1u | s);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return (float)((t ^ (t >> 14)) / 4294967296.0);
}

static float smooth(float t) { return t * t * (3 - 2 * t); }

void egypt_build_background(const EgyptSettings *s, int W, int H, uint8_t *px)
{
    if (W <= 0 || H <= 0) return;
    const Palette *P = &PALETTES[s->palette == EGYPT_PALETTE_OLIVE ? 1 : 0];
    fill_rows(px, W, H, 0, H, P->stone[0], P->stone[1], P->stone[2]);

    int rows = clamp_rows(s->rows);
    float rh = (float)H / rows;
    for (int r = 0; r < rows; r++) {
        float bottom = (r + 1) * rh;
        float gTop = bottom - rh * ROW_GROUND_TOP, lTop = bottom - rh * ROW_LEDGE_TOP;
        int yg = (int)lroundf(gTop), yl = (int)lroundf(lTop), yb = (int)lroundf(bottom);
        /* Bodenstreifen mit senkrechtem Verlauf */
        for (int y = yg; y < yl; y++) {
            float t = clamp01((y + 0.5f - gTop) / (lTop - gTop));
            fill_rows(px, W, H, y, y + 1,
                      lerpf(P->ground0[0], P->ground1[0], t),
                      lerpf(P->ground0[1], P->ground1[1], t),
                      lerpf(P->ground0[2], P->ground1[2], t));
        }
        /* Sims mit heller Oberkante und dunkler Fuge unten */
        fill_rows(px, W, H, yl, yb, P->ledge[0], P->ledge[1], P->ledge[2]);
        int hi = (int)fmaxf(1, lroundf(rh * 0.007f));
        fill_rows(px, W, H, yl, yl + hi, P->ledgeHi[0], P->ledgeHi[1], P->ledgeHi[2]);
        int lw = (int)fmaxf(1, lroundf(rh * 0.01f));
        fill_rows(px, W, H, yb - lw, yb, P->ledgeLine[0], P->ledgeLine[1], P->ledgeLine[2]);
    }

    /* Körnung + großflächige Flecken (Wertrauschen) */
    rng_state = 1998;
    int mn = W < H ? W : H;
    int cell = (int)lroundf(mn / 14.f);
    if (cell < 24) cell = 24;
    int gw = (int)ceilf((float)W / cell) + 2, gh = (int)ceilf((float)H / cell) + 2;
    float *grid = malloc(sizeof(float) * (size_t)gw * gh);
    if (!grid) return;
    for (int i = 0; i < gw * gh; i++) grid[i] = rnd() * 2 - 1;

    for (int y = 0; y < H; y++) {
        float gy = (float)y / cell;
        int iy = (int)gy;
        float fy = smooth(gy - iy);
        uint8_t *row = px + (size_t)y * W * 4;
        for (int x = 0; x < W; x++) {
            float gx = (float)x / cell;
            int ix = (int)gx;
            float fx = smooth(gx - ix);
            float a = grid[iy * gw + ix], b = grid[iy * gw + ix + 1];
            float c = grid[(iy + 1) * gw + ix], d = grid[(iy + 1) * gw + ix + 1];
            float n = lerpf(lerpf(a, b, fx), lerpf(c, d, fx), fy) * P->blotch + (rnd() - 0.5f) * P->grain;
            const float k[3] = { 1.f, 0.92f, 0.85f };
            for (int ch = 0; ch < 3; ch++) {
                float v = row[x * 4 + ch] + n * k[ch];
                row[x * 4 + ch] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : lroundf(v));
            }
        }
    }
    free(grid);
}
