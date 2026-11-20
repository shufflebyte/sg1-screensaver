/*
 * Ägyptische Tänzer – Linux (X11/XWayland + OpenGL 3.3)
 *
 * Zeichnet dieselben Daten wie der macOS-Renderer: Wand als Textur, Figuren als
 * Dreiecke aus core/egypt_core.c, darüber der Röhren-Look. Kantenglättung über ein
 * 4×-Multisample-Framebuffer.
 *
 * Betriebsarten:
 *   egypt-dancers                 Fenster (zum Ausprobieren; Esc/Q beendet, Leertaste Pause, F Vollbild)
 *   egypt-dancers --fullscreen    Bildschirmschoner: Vollbild, ohne Mauszeiger, endet bei Eingabe
 *   egypt-dancers -root / -window-id ID / $XSCREENSAVER_WINDOW
 *                                 als Modul für XScreenSaver (zeichnet in dessen Fenster)
 *   egypt-dancers --snapshot bild.ppm --size 2560x1440 --time 3
 *                                 ein Bild rendern und speichern (ohne sichtbares Fenster)
 */
#define _POSIX_C_SOURCE 200809L
#define GL_GLEXT_PROTOTYPES 1

#include "egypt_core.h"

#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef GLX_CONTEXT_MAJOR_VERSION_ARB
#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define GLX_CONTEXT_PROFILE_MASK_ARB 0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#endif

typedef GLXContext (*CreateContextAttribsFn)(Display *, GLXFBConfig, GLXContext, Bool, const int *);
typedef void (*SwapIntervalEXTFn)(Display *, GLXDrawable, int);
typedef int (*SwapIntervalMESAFn)(unsigned int);

enum { MODE_WINDOW, MODE_FULLSCREEN, MODE_XSCREENSAVER, MODE_SNAPSHOT };

typedef struct {
    int mode;
    Window target;        /* MODE_XSCREENSAVER: fremdes Fenster */
    int use_root;
    EgyptSettings s;
    float dpr;            /* Pixeldichte für die Scanlines; 0 = automatisch */
    double fps;           /* 0 = nur Vsync */
    double duration;      /* 0 = unbegrenzt */
    const char *snapshot;
    int snap_w, snap_h;
    double snap_t;
} Options;

static volatile sig_atomic_t quit_flag = 0;
static void on_signal(int sig) { (void)sig; quit_flag = 1; }

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void sleep_sec(double s)
{
    if (s <= 0) return;
    struct timespec ts = { (time_t)s, (long)((s - (time_t)s) * 1e9) };
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ Shader */

static const char *POLY_VS =
    "#version 330 core\n"
    "layout(location = 0) in vec2 pos;\n"
    "layout(location = 1) in vec4 col;\n"
    "uniform vec2 size;\n"
    "out vec4 vcol;\n"
    "void main() {\n"
    "    gl_Position = vec4(pos.x / size.x * 2.0 - 1.0, 1.0 - pos.y / size.y * 2.0, 0.0, 1.0);\n"
    "    vcol = col;\n"
    "}\n";

static const char *POLY_FS =
    "#version 330 core\n"
    "in vec4 vcol;\n"
    "out vec4 frag;\n"
    "void main() { frag = vec4(vcol.rgb * vcol.a, vcol.a); }\n";

static const char *FULL_VS =
    "#version 330 core\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "    uv = vec2(p.x, 1.0 - p.y);\n"   /* Zeile 0 des Wandbilds = oben */
    "}\n";

static const char *BG_FS =
    "#version 330 core\n"
    "in vec2 uv;\n"
    "uniform sampler2D tex;\n"
    "out vec4 frag;\n"
    "void main() { frag = vec4(texture(tex, uv).rgb, 1.0); }\n";

/* Scanlines und abgedunkelte Ränder wie im Prototyp und unter macOS */
static const char *CRT_FS =
    "#version 330 core\n"
    "uniform vec2 size;\n"
    "uniform float dpr;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "    vec2 px = gl_FragCoord.xy;\n"
    "    px.y = size.y - px.y;\n"
    "    float step = max(3.0, floor(dpr * 3.0 + 0.5));\n"
    "    float scan = mod(px.y, step) < max(1.0, step / 3.0) ? 0.16 : 0.0;\n"
    "    float r0 = min(size.x, size.y) * 0.35;\n"
    "    float r1 = length(size) * 0.6;\n"
    "    float vig = clamp((length(px - size * 0.5) - r0) / (r1 - r0), 0.0, 1.0) * 0.45;\n"
    "    float a = 1.0 - (1.0 - scan) * (1.0 - vig);\n"
    "    frag = vec4(0.0, 0.0, 0.0, a);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "egypt-dancers: Shader-Fehler:\n%s\n", log);
        exit(1);
    }
    return sh;
}

static GLuint program(const char *vs, const char *fs)
{
    GLuint p = glCreateProgram();
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "egypt-dancers: Shader-Programm-Fehler:\n%s\n", log);
        exit(1);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

/* ---------------------------------------------------------------- Renderer */

typedef struct {
    GLuint poly, bg, crt;
    GLint poly_size, crt_size, crt_dpr, bg_tex;
    GLuint vao_poly, vao_empty, vbo;
    GLuint bg_texture;
    GLuint ms_fbo, ms_rb;        /* Multisample-Ziel */
    GLuint rs_fbo, rs_tex;       /* aufgelöstes Bild */
    int w, h, samples;
    EgyptVertex *verts;
    size_t cap;
} Renderer;

static void renderer_init(Renderer *r)
{
    memset(r, 0, sizeof *r);
    r->poly = program(POLY_VS, POLY_FS);
    r->bg = program(FULL_VS, BG_FS);
    r->crt = program(FULL_VS, CRT_FS);
    r->poly_size = glGetUniformLocation(r->poly, "size");
    r->bg_tex = glGetUniformLocation(r->bg, "tex");
    r->crt_size = glGetUniformLocation(r->crt, "size");
    r->crt_dpr = glGetUniformLocation(r->crt, "dpr");

    glGenVertexArrays(1, &r->vao_empty);
    glGenVertexArrays(1, &r->vao_poly);
    glGenBuffers(1, &r->vbo);
    glBindVertexArray(r->vao_poly);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(EgyptVertex), (void *)offsetof(EgyptVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(EgyptVertex), (void *)offsetof(EgyptVertex, r));
    glBindVertexArray(0);

    GLint max_samples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    r->samples = max_samples < 4 ? max_samples : 4;
}

/* Wandtextur, Framebuffer und Vertexpuffer an die Größe anpassen. */
static void renderer_resize(Renderer *r, const EgyptSettings *s, int w, int h)
{
    if (w == r->w && h == r->h) return;
    r->w = w;
    r->h = h;

    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) { fprintf(stderr, "egypt-dancers: kein Speicher\n"); exit(1); }
    egypt_build_background(s, w, h, px);
    if (!r->bg_texture) glGenTextures(1, &r->bg_texture);
    glBindTexture(GL_TEXTURE_2D, r->bg_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(px);

    if (!r->ms_fbo) {
        glGenFramebuffers(1, &r->ms_fbo);
        glGenRenderbuffers(1, &r->ms_rb);
        glGenFramebuffers(1, &r->rs_fbo);
        glGenTextures(1, &r->rs_tex);
    }
    glBindRenderbuffer(GL_RENDERBUFFER, r->ms_rb);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, r->samples, GL_RGBA8, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, r->ms_fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, r->ms_rb);

    glBindTexture(GL_TEXTURE_2D, r->rs_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, r->rs_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->rs_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "egypt-dancers: Framebuffer unvollständig\n");
        exit(1);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    size_t cap = egypt_max_vertices(s, (float)w, (float)h);
    if (cap < 3) cap = 3;
    if (cap > r->cap) {
        free(r->verts);
        r->verts = malloc(cap * sizeof *r->verts);
        if (!r->verts) { fprintf(stderr, "egypt-dancers: kein Speicher\n"); exit(1); }
        r->cap = cap;
    }
}

/* Ein Bild nach r->rs_fbo rendern. */
static void renderer_draw(Renderer *r, const EgyptSettings *s, double t, float dpr)
{
    float W = (float)r->w, H = (float)r->h;
    size_t n = egypt_build_scene(s, W, H, t, r->verts, r->cap);
    if (n > r->cap) n = r->cap;

    glBindFramebuffer(GL_FRAMEBUFFER, r->ms_fbo);
    glViewport(0, 0, r->w, r->h);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glDisable(GL_BLEND);           /* Wand deckt alles ab, ersetzt das Löschen */
    glUseProgram(r->bg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->bg_texture);
    glUniform1i(r->bg_tex, 0);
    glBindVertexArray(r->vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glEnable(GL_BLEND);

    if (n > 0) {
        glUseProgram(r->poly);
        glUniform2f(r->poly_size, W, H);
        glBindVertexArray(r->vao_poly);
        glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * sizeof(EgyptVertex)), r->verts, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)n);
    }

    if (s->crt) {
        glUseProgram(r->crt);
        glUniform2f(r->crt_size, W, H);
        glUniform1f(r->crt_dpr, dpr < 1 ? 1 : dpr);
        glBindVertexArray(r->vao_empty);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glBindVertexArray(0);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, r->ms_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, r->rs_fbo);
    glBlitFramebuffer(0, 0, r->w, r->h, 0, 0, r->w, r->h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

/* Aufgelöstes Bild ins Fenster kopieren. */
static void renderer_present(Renderer *r)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, r->rs_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDrawBuffer(GL_BACK);
    glBlitFramebuffer(0, 0, r->w, r->h, 0, 0, r->w, r->h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

static int renderer_save_ppm(Renderer *r, const char *path)
{
    size_t row = (size_t)r->w * 3;
    uint8_t *px = malloc(row * r->h);
    if (!px) return 0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, r->rs_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, r->w, r->h, GL_RGB, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); free(px); return 0; }
    fprintf(f, "P6\n%d %d\n255\n", r->w, r->h);
    for (int y = r->h - 1; y >= 0; y--) fwrite(px + (size_t)y * row, 1, row, f);  /* GL: unten zuerst */
    fclose(f);
    free(px);
    return 1;
}

/* ------------------------------------------------------------------- X11 */

static int x_error_seen = 0;
static int ignore_x_error(Display *d, XErrorEvent *e) { (void)d; (void)e; x_error_seen = 1; return 0; }

static int fb_int(Display *dpy, GLXFBConfig c, int attr)
{
    int v = 0;
    glXGetFBConfigAttrib(dpy, c, attr, &v);
    return v;
}

/* FBConfig für ein eigenes Fenster: RGB8, doppelt gepuffert, ohne Multisample/Tiefe.
 * Bevorzugt 24-Bit-Visuals, damit der Compositor den Alphakanal nicht als Transparenz liest. */
static GLXFBConfig choose_config(Display *dpy, int screen, XVisualInfo **vi_out)
{
    static const int attrs[] = {
        GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
        GLX_DOUBLEBUFFER, True, None
    };
    int n = 0;
    GLXFBConfig *list = glXChooseFBConfig(dpy, screen, attrs, &n);
    GLXFBConfig best = NULL;
    int best_score = -1;
    for (int i = 0; i < n; i++) {
        XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, list[i]);
        if (!vi) continue;
        int score = 0;
        if (vi->depth == 24) score += 4;
        if (fb_int(dpy, list[i], GLX_SAMPLES) == 0) score += 2;
        if (fb_int(dpy, list[i], GLX_DEPTH_SIZE) == 0) score += 1;
        if (score > best_score) { best_score = score; best = list[i]; }
        XFree(vi);
    }
    if (list) XFree(list);
    *vi_out = best ? glXGetVisualFromFBConfig(dpy, best) : NULL;
    return best;
}

/* FBConfig passend zum Visual eines fremden Fensters (XScreenSaver). */
static GLXFBConfig config_for_window(Display *dpy, int screen, Window w)
{
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa)) return NULL;
    VisualID want = XVisualIDFromVisual(wa.visual);
    int n = 0;
    GLXFBConfig *list = glXGetFBConfigs(dpy, screen, &n);
    GLXFBConfig best = NULL;
    int best_score = -1;
    for (int i = 0; i < n; i++) {
        if ((VisualID)fb_int(dpy, list[i], GLX_VISUAL_ID) != want) continue;
        if (!(fb_int(dpy, list[i], GLX_RENDER_TYPE) & GLX_RGBA_BIT)) continue;
        if (!(fb_int(dpy, list[i], GLX_DRAWABLE_TYPE) & GLX_WINDOW_BIT)) continue;
        int score = fb_int(dpy, list[i], GLX_DOUBLEBUFFER) ? 2 : 0;
        if (fb_int(dpy, list[i], GLX_SAMPLES) == 0) score += 1;
        if (score > best_score) { best_score = score; best = list[i]; }
    }
    if (list) XFree(list);
    return best;
}

static GLXContext make_context(Display *dpy, GLXFBConfig cfg)
{
    CreateContextAttribsFn create =
        (CreateContextAttribsFn)glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
    if (!create) return NULL;
    static const int attrs[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3,
        GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None
    };
    int (*old)(Display *, XErrorEvent *) = XSetErrorHandler(ignore_x_error);
    x_error_seen = 0;
    GLXContext ctx = create(dpy, cfg, NULL, True, attrs);
    XSync(dpy, False);
    XSetErrorHandler(old);
    if (x_error_seen) ctx = NULL;
    return ctx;
}

static void set_vsync(Display *dpy, GLXDrawable d)
{
    SwapIntervalEXTFn ext = (SwapIntervalEXTFn)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
    if (ext) { ext(dpy, d, 1); return; }
    SwapIntervalMESAFn mesa = (SwapIntervalMESAFn)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalMESA");
    if (mesa) mesa(1);
}

static void set_fullscreen(Display *dpy, Window w, int on, int mapped)
{
    Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom fs = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    if (!mapped) {
        if (on) XChangeProperty(dpy, w, state, XA_ATOM, 32, PropModeReplace, (unsigned char *)&fs, 1);
        return;
    }
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = w;
    e.xclient.message_type = state;
    e.xclient.format = 32;
    e.xclient.data.l[0] = on ? 1 : 0;   /* _NET_WM_STATE_ADD / _REMOVE */
    e.xclient.data.l[1] = (long)fs;
    e.xclient.data.l[3] = 1;
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

static void hide_cursor(Display *dpy, Window w)
{
    static char empty[1] = { 0 };
    Pixmap pm = XCreateBitmapFromData(dpy, w, empty, 1, 1);
    XColor black = { 0 };
    Cursor c = XCreatePixmapCursor(dpy, pm, pm, &black, &black, 0, 0);
    XDefineCursor(dpy, w, c);
    XFreeCursor(dpy, c);
    XFreePixmap(dpy, pm);
}

static Window create_window(Display *dpy, int screen, XVisualInfo *vi, int w, int h, int fullscreen, int map)
{
    Window root = RootWindow(dpy, screen);
    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof swa);
    swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
    swa.background_pixel = BlackPixel(dpy, screen);
    swa.border_pixel = 0;
    swa.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask | ButtonPressMask | PointerMotionMask
                   | FocusChangeMask;
    Window win = XCreateWindow(dpy, root, 0, 0, (unsigned)w, (unsigned)h, 0, vi->depth, InputOutput, vi->visual,
                               CWColormap | CWBackPixel | CWBorderPixel | CWEventMask, &swa);

    const char *title = "Ägyptische Tänzer";
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_NAME", False), utf8, 8, PropModeReplace,
                    (const unsigned char *)title, (int)strlen(title));
    XStoreName(dpy, win, "Aegyptische Taenzer");
    XClassHint ch = { (char *)"egypt-dancers", (char *)"EgyptDancers" };
    XSetClassHint(dpy, win, &ch);
    Atom del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &del, 1);

    if (fullscreen) {
        set_fullscreen(dpy, win, 1, 0);
        long one = 1;   /* Compositor umgehen, spart eine Kopie pro Bild */
        XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_BYPASS_COMPOSITOR", False), XA_CARDINAL, 32,
                        PropModeReplace, (unsigned char *)&one, 1);
        hide_cursor(dpy, win);
    }
    if (map) XMapRaised(dpy, win);
    return win;
}

/* Pixeldichte aus Xft.dpi (GNOME setzt sie für XWayland passend zur Skalierung). */
static float detect_dpr(Display *dpy)
{
    const char *dpi = XGetDefault(dpy, "Xft", "dpi");
    float d = dpi ? (float)atof(dpi) / 96.0f : 1.0f;
    return d < 1 ? 1 : d;
}

/* ---------------------------------------------------------------- Optionen */

static void usage(FILE *f)
{
    fputs(
        "Ägyptische Tänzer – Bildschirmschoner aus Stargate SG-1\n"
        "\n"
        "Aufruf: egypt-dancers [Optionen]\n"
        "\n"
        "Betriebsart:\n"
        "  (ohne)                 im Fenster (Esc/Q beendet, Leertaste Pause, F Vollbild)\n"
        "  -f, --fullscreen       als Bildschirmschoner: Vollbild, endet bei Taste/Maus\n"
        "  -root, -window-id ID   in ein Fenster von XScreenSaver zeichnen\n"
        "                         (auch über die Umgebungsvariable XSCREENSAVER_WINDOW)\n"
        "  --snapshot DATEI.ppm   ein Bild rendern und speichern\n"
        "      --size BxH         Bildgröße für --snapshot (Standard 2560x1440)\n"
        "      --time SEK         Zeitpunkt für --snapshot (Standard 3)\n"
        "\n"
        "Aussehen:\n"
        "  --rows N               Reihen (Standard 5)\n"
        "  --spacing F            Figurenabstand (Standard 1.05)\n"
        "  --tempo F              Posen pro Sekunde (Standard 1.05)\n"
        "  --pattern NAME         wave, canon, sync, random (Standard wave)\n"
        "  --scroll NAME          off, same, alt (Standard same)\n"
        "  --palette NAME         rose, olive (Standard rose)\n"
        "  --crt / --no-crt       Röhren-Look an/aus (Standard an)\n"
        "  --scale F              Pixeldichte für die Scanlines (Standard: aus Xft.dpi)\n"
        "\n"
        "Sonstiges:\n"
        "  --fps N                Bildrate begrenzen (Standard: Bildschirmfrequenz)\n"
        "  --duration SEK         nach SEK Sekunden beenden\n"
        "  -h, --help             diese Hilfe\n",
        f);
}

static int pick(const char *v, const char *const *names, int n, const char *opt)
{
    for (int i = 0; i < n; i++)
        if (strcmp(v, names[i]) == 0) return i;
    fprintf(stderr, "egypt-dancers: unbekannter Wert für %s: %s\n", opt, v);
    exit(2);
}

static void parse(Options *o, int argc, char **argv)
{
    static const char *const patterns[] = { "wave", "canon", "sync", "random" };
    static const char *const scrolls[] = { "off", "same", "alt" };
    static const char *const palettes[] = { "rose", "olive" };

    memset(o, 0, sizeof *o);
    o->mode = MODE_WINDOW;
    egypt_default_settings(&o->s);
    o->snap_w = 2560;
    o->snap_h = 1440;
    o->snap_t = 3.0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == '-') a++;     /* -x und --x gleich behandeln (XScreenSaver nutzt -x) */
#define ARG() (i + 1 < argc ? argv[++i] : (fprintf(stderr, "egypt-dancers: %s braucht einen Wert\n", argv[i]), exit(2), ""))
        if (!strcmp(a, "-h") || !strcmp(a, "-help")) { usage(stdout); exit(0); }
        else if (!strcmp(a, "-f") || !strcmp(a, "-fullscreen")) o->mode = MODE_FULLSCREEN;
        else if (!strcmp(a, "-root")) { o->mode = MODE_XSCREENSAVER; o->use_root = 1; }
        else if (!strcmp(a, "-window-id")) {
            const char *v = ARG();
            o->mode = MODE_XSCREENSAVER;
            o->target = (Window)strtoul(v, NULL, 0);
        }
        else if (!strcmp(a, "-window")) o->mode = MODE_WINDOW;
        else if (!strcmp(a, "-snapshot")) { o->mode = MODE_SNAPSHOT; o->snapshot = ARG(); }
        else if (!strcmp(a, "-size")) {
            const char *v = ARG();
            if (sscanf(v, "%dx%d", &o->snap_w, &o->snap_h) != 2 || o->snap_w < 16 || o->snap_h < 16) {
                fprintf(stderr, "egypt-dancers: --size erwartet BxH, z.B. 2560x1440\n");
                exit(2);
            }
        }
        else if (!strcmp(a, "-time")) o->snap_t = atof(ARG());
        else if (!strcmp(a, "-rows")) o->s.rows = atoi(ARG());
        else if (!strcmp(a, "-spacing")) o->s.spacing = (float)atof(ARG());
        else if (!strcmp(a, "-tempo")) o->s.tempo = (float)atof(ARG());
        else if (!strcmp(a, "-pattern")) o->s.pattern = pick(ARG(), patterns, 4, "--pattern");
        else if (!strcmp(a, "-scroll")) o->s.scroll = pick(ARG(), scrolls, 3, "--scroll");
        else if (!strcmp(a, "-palette")) o->s.palette = pick(ARG(), palettes, 2, "--palette");
        else if (!strcmp(a, "-crt")) o->s.crt = 1;
        else if (!strcmp(a, "-no-crt")) o->s.crt = 0;
        else if (!strcmp(a, "-scale")) o->dpr = (float)atof(ARG());
        else if (!strcmp(a, "-fps")) o->fps = atof(ARG());
        else if (!strcmp(a, "-duration")) o->duration = atof(ARG());
        else if (!strcmp(a, "-display")) setenv("DISPLAY", ARG(), 1);
        else {
            fprintf(stderr, "egypt-dancers: unbekannte Option: %s\n\n", argv[i]);
            usage(stderr);
            exit(2);
        }
#undef ARG
    }
    if (o->mode == MODE_WINDOW || o->mode == MODE_FULLSCREEN) {
        const char *env = getenv("XSCREENSAVER_WINDOW");
        if (env && *env) { o->mode = MODE_XSCREENSAVER; o->target = (Window)strtoul(env, NULL, 0); }
    }
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    Options o;
    parse(&o, argc, argv);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "egypt-dancers: keine Verbindung zum X-Server (DISPLAY=%s)\n", getenv("DISPLAY")); return 1; }
    int screen = DefaultScreen(dpy);

    GLXFBConfig cfg = NULL;
    XVisualInfo *vi = NULL;
    Window win = 0;
    int own_window = 1;

    if (o.mode == MODE_XSCREENSAVER) {
        win = o.use_root ? RootWindow(dpy, screen) : o.target;
        cfg = config_for_window(dpy, screen, win);
        if (!cfg) { fprintf(stderr, "egypt-dancers: kein passendes OpenGL-Format für Fenster 0x%lx\n", win); return 1; }
        XWindowAttributes wa;
        XGetWindowAttributes(dpy, win, &wa);
        XSelectInput(dpy, win, wa.your_event_mask | StructureNotifyMask | ExposureMask);
        own_window = 0;
    } else {
        cfg = choose_config(dpy, screen, &vi);
        if (!cfg || !vi) { fprintf(stderr, "egypt-dancers: kein passendes OpenGL-Format gefunden\n"); return 1; }
        int w = 1280, h = 800;
        if (o.mode == MODE_FULLSCREEN) { w = DisplayWidth(dpy, screen); h = DisplayHeight(dpy, screen); }
        if (o.mode == MODE_SNAPSHOT) { w = o.snap_w; h = o.snap_h; }
        win = create_window(dpy, screen, vi, w, h, o.mode == MODE_FULLSCREEN, o.mode != MODE_SNAPSHOT);
    }

    GLXContext ctx = make_context(dpy, cfg);
    if (!ctx) { fprintf(stderr, "egypt-dancers: OpenGL 3.3 wird nicht unterstützt\n"); return 1; }
    glXMakeCurrent(dpy, win, ctx);
    if (o.mode != MODE_SNAPSHOT) set_vsync(dpy, win);

    Renderer r;
    renderer_init(&r);
    float dpr = o.dpr > 0 ? o.dpr : detect_dpr(dpy);

    if (o.mode == MODE_SNAPSHOT) {
        double t0 = now_sec();
        renderer_resize(&r, &o.s, o.snap_w, o.snap_h);
        double t1 = now_sec();
        renderer_draw(&r, &o.s, o.snap_t, o.dpr > 0 ? o.dpr : 1.0f);
        glFinish();
        double t2 = now_sec();
        for (int i = 0; i < 30; i++) renderer_draw(&r, &o.s, o.snap_t + i / 60.0, o.dpr > 0 ? o.dpr : 1.0f);
        glFinish();
        double t3 = now_sec();
        int ok = renderer_save_ppm(&r, o.snapshot);
        fprintf(stderr, "%dx%d: Wand %.1f ms, erstes Bild %.1f ms, danach %.2f ms pro Bild (%s)\n",
                o.snap_w, o.snap_h, (t1 - t0) * 1e3, (t2 - t1) * 1e3, (t3 - t2) * 1e3 / 30,
                (const char *)glGetString(GL_RENDERER));
        glXMakeCurrent(dpy, None, NULL);
        glXDestroyContext(dpy, ctx);
        XCloseDisplay(dpy);
        return ok ? 0 : 1;
    }

    XWindowAttributes wa;
    XGetWindowAttributes(dpy, win, &wa);
    int W = wa.width, H = wa.height;

    int fullscreen = o.mode == MODE_FULLSCREEN;
    int quit_on_input = o.mode == MODE_FULLSCREEN;
    double start = now_sec(), paused_at = 0, paused_total = 0;
    int paused = 0, have_pointer = 0, px0 = 0, py0 = 0;
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    int grabbed = 0;

    while (!quit_flag) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            switch (e.type) {
            case ConfigureNotify:
                if (e.xconfigure.window == win) { W = e.xconfigure.width; H = e.xconfigure.height; }
                break;
            case ClientMessage:
                if ((Atom)e.xclient.data.l[0] == wm_delete) quit_flag = 1;
                break;
            case DestroyNotify:
                if (e.xdestroywindow.window == win) quit_flag = 1;
                break;
            case KeyPress:
                if (quit_on_input) { quit_flag = 1; break; }
                if (own_window) {
                    KeySym k = XLookupKeysym(&e.xkey, 0);
                    if (k == XK_Escape || k == XK_q) quit_flag = 1;
                    else if (k == XK_space) {
                        paused = !paused;
                        if (paused) paused_at = now_sec(); else paused_total += now_sec() - paused_at;
                    } else if (k == XK_f) {
                        fullscreen = !fullscreen;
                        set_fullscreen(dpy, win, fullscreen, 1);
                    }
                }
                break;
            case ButtonPress:
                if (quit_on_input && now_sec() - start > 1.0) quit_flag = 1;
                break;
            case MotionNotify:
                if (!quit_on_input) break;
                /* Zittern der Maus und Bewegungen direkt nach dem Start (Klick im Menü) ignorieren */
                if (!have_pointer || now_sec() - start < 2.0) {
                    have_pointer = 1; px0 = e.xmotion.x_root; py0 = e.xmotion.y_root;
                } else if (abs(e.xmotion.x_root - px0) + abs(e.xmotion.y_root - py0) > 12) {
                    quit_flag = 1;
                }
                break;
            case FocusIn:
                /* Tastatur festhalten, damit Tippen nicht im Fenster darunter landet */
                if (quit_on_input && !grabbed)
                    grabbed = XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
                break;
            }
        }
        if (quit_flag) break;

        double frame_start = now_sec();
        if (o.duration > 0 && frame_start - start >= o.duration) break;
        if (W <= 0 || H <= 0) { sleep_sec(0.05); continue; }

        double t = (paused ? paused_at : frame_start) - start - paused_total;
        renderer_resize(&r, &o.s, W, H);
        renderer_draw(&r, &o.s, t, dpr);
        renderer_present(&r);
        glXSwapBuffers(dpy, win);

        if (o.fps > 0) sleep_sec(1.0 / o.fps - (now_sec() - frame_start));
        if (paused) sleep_sec(0.03);
    }

    if (grabbed) XUngrabKeyboard(dpy, CurrentTime);
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, ctx);
    if (own_window) XDestroyWindow(dpy, win);
    if (vi) XFree(vi);
    XCloseDisplay(dpy);
    return 0;
}
