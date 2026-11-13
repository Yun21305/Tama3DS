/*
 * Tama3DS - send Tamagotchi P's download files from a 3DS over IrDA OBEX.
 * IrDA/OBEX stack: PspIrDA by Mathieu Albinet, based on BlauLogic's
 * IrDA stack (see COPYRIGHT). This file and irphy_3ds.c are the 3DS port.
 *
 * Files:  sdmc:/tama/  (sub folders = categories, any depth)
 * Logs:   sdmc:/tama/irda_debug.txt, sdmc:/tama/irda_dump.txt
 */
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include "pspirda.h"
#include "irlap.h"
#include "irobex.h"
#include "irphy.h"
#include "lang.h"

Result irphy_init(void);
void irphy_exit(void);
extern u32 irphy_rx_restarts, irphy_tx_frames, irphy_rx_bytes, irphy_send_stuck, irphy_cur_bps, irphy_hw_resets;
extern volatile int irphy_abort;
void (*irphy_idle_cb)(void);

unsigned char *thumb_decode(const unsigned char *buf, int len, int *w, int *h);
void thumb_free(unsigned char *px);

#define ROOT_DIR   "sdmc:/tama"
#define CFG_FILE   ROOT_DIR "/.tama3ds.cfg"
#define APP_VER    "v1.0.0"
#define AUTHOR     "@Yun21305"
#define MAX_ITEMS  512
#define NAME_LEN   128

/* ---------------- colours ---------------- */
#define RGB(r, g, b) C2D_Color32(r, g, b, 0xFF)
#define COL_BG       RGB(0xA3, 0x0F, 0x0F)   /* top screen background   */
#define COL_PANEL    RGB(0xF8, 0xEC, 0xEC)   /* rounded panel           */
#define COL_TEXT     RGB(0x63, 0x00, 0x00)   /* normal text             */
#define COL_HILITE   RGB(0xD1, 0x7B, 0x7B)   /* selected row / border   */
#define COL_WHITE    RGB(0xFF, 0xFF, 0xFF)
#define COL_DARK     RGB(0x63, 0x00, 0x00)   /* bottom gradient top     */
#define COL_SHADOW   RGB(0x84, 0x0A, 0x0A)       /* soft shadow on #a30f0f  */
#define COL_TRACK    RGB(0xE6, 0xCC, 0xCC)       /* progress bar track      */
#define COL_FOLDER2  RGB(0xE4, 0xB0, 0xB0)       /* side folder icons       */
#define COL_DIM      RGB(0xC8, 0x60, 0x60)       /* #d17b7b, same hue, darker */

/* ---------------- logging glue for the IrDA stack ---------------- */
int irda_debug = IRDA_DEBUG;
int irda_dump = IRDA_DUMP;
FILE *debug_fd = NULL;
FILE *dump_fd = NULL;

static int dump_dir = -1;
void dump(unsigned char data, int send_or_receive)
{
    if (!dump_fd) return;
    if (send_or_receive != dump_dir) {
        dump_dir = send_or_receive;
        fprintf(dump_fd, send_or_receive ? "\n--> " : "\n<-- ");
    }
    fprintf(dump_fd, "%02X ", data);
}

/* ---------------- settings ---------------- */
static bool log_on = false;          /* off by default: saves SD space */
static int lang = -1;                /* -1 = follow system language */
static int speed_idx = 0;            /* index into SPEEDS */
#define T(id) STR[lang][id]

static const struct { u8 mask; const char *name; } SPEEDS[] = {
    /* P's does not offer 19200; 115200 stalls on the 3DS side */
    { 0x02, "9600" }, { 0x0A, "38400" }, { 0x1A, "57600" },
};
#define NSPEEDS ((int)(sizeof(SPEEDS) / sizeof(SPEEDS[0])))

static C2D_Font zh_font = NULL;      /* Traditional Chinese system font */
static bool zh_ok = false;

static void load_cfg(void)
{
    FILE *f = fopen(CFG_FILE, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, "log=", 4) == 0) log_on = line[4] == '1';
        else if (strncmp(line, "lang=", 5) == 0) lang = atoi(line + 5);
        else if (strncmp(line, "speed=", 6) == 0) speed_idx = atoi(line + 6);
    if (lang >= LANG_COUNT) lang = -1;
    if (speed_idx < 0 || speed_idx >= NSPEEDS) speed_idx = 0;
    fclose(f);
}

static void save_cfg(void)
{
    FILE *f = fopen(CFG_FILE, "w");
    if (!f) return;
    fprintf(f, "log=%d\nlang=%d\nspeed=%d\n", log_on ? 1 : 0, lang, speed_idx);
    fclose(f);
}

static void apply_log(void)
{
    if (log_on) {
        if (!debug_fd) debug_fd = fopen(ROOT_DIR "/irda_debug.txt", "a");
        if (!dump_fd) dump_fd = fopen(ROOT_DIR "/irda_dump.txt", "a");
    } else {
        if (debug_fd) fclose(debug_fd);
        if (dump_fd) fclose(dump_fd);
        debug_fd = dump_fd = NULL;
    }
}

/* ---------------- directory listing ---------------- */
enum { K_DIR, K_ITEM, K_TXT, K_OTHER };
#define SIZE_UNKNOWN 0xFFFFFFFFu
typedef struct {
    char name[NAME_LEN];
    bool is_dir;
    u8 kind;
    u32 size;
} Item;

static bool ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcasecmp(s + a - b, suf) == 0;
}

/* Kind from the extension only: opening every file during the scan made
   entering big folders slow. The real content check (JPEG + "TAMAGO") is
   done later by the thumbnail loader, only for files shown on screen. */
static u8 kind_from_name(const char *name)
{
    if (ends_with(name, ".txt")) return K_TXT;
    if (ends_with(name, ".jpg") || ends_with(name, ".jpeg")) return K_ITEM;
    return K_OTHER;
}

static Item items[MAX_ITEMS];
static int nitems = 0;
static char cur_path[512] = ROOT_DIR;
static int sel = 0;
static float sel_anim = 0.0f;       /* animated selection, follows sel */

/* remember selection per depth so B returns to the same row */
static int sel_stack[16];
static int depth = 0;

static u32 item_size(int i)
{
    if (items[i].is_dir) return 0;
    if (items[i].size == SIZE_UNKNOWN) {
        char p[700];
        snprintf(p, sizeof(p), "%s/%s", cur_path, items[i].name);
        struct stat st;
        items[i].size = stat(p, &st) == 0 ? (u32)st.st_size : 0;
    }
    return items[i].size;
}

static int item_cmp(const void *a, const void *b)
{
    const Item *x = a, *y = b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static bool is_hidden(const char *n)
{
    if (n[0] == '.') return true;
    if (strncmp(n, "irda_", 5) == 0) return true;
    if (strcasecmp(n, "Thumbs.db") == 0 || strcasecmp(n, "desktop.ini") == 0) return true;
    return false;
}

static void scan_dir(void)
{
    nitems = 0;
    DIR *d = opendir(cur_path);
    if (!d) return;
    struct dirent *e;
    char p[700];
    while ((e = readdir(d)) && nitems < MAX_ITEMS) {
        if (is_hidden(e->d_name)) continue;
        Item *it = &items[nitems];
        strncpy(it->name, e->d_name, NAME_LEN - 1);
        it->name[NAME_LEN - 1] = 0;
        /* d_type avoids one stat() per file (slow on the 3DS SD card);
           file sizes are read later, only for rows shown on screen */
        if (e->d_type == DT_DIR) it->is_dir = true;
        else if (e->d_type == DT_REG) it->is_dir = false;
        else {
            snprintf(p, sizeof(p), "%s/%s", cur_path, e->d_name);
            struct stat st;
            if (stat(p, &st) != 0) continue;
            it->is_dir = S_ISDIR(st.st_mode);
        }
        it->size = SIZE_UNKNOWN;
        it->kind = it->is_dir ? K_DIR : kind_from_name(it->name);
        nitems++;
    }
    closedir(d);
    qsort(items, nitems, sizeof(Item), item_cmp);
}

/* ---------------- thumbnails ---------------- */
#define THUMB_SLOTS 12
typedef struct {
    char path[700];
    bool valid;      /* decode attempted */
    bool ok;         /* has an image */
    bool is_item;    /* JPEG preview + TAMAGO block found */
    int w, h;
    C3D_Tex tex;
    Tex3DS_SubTexture sub;
    u32 used;
} Thumb;

static Thumb thumbs[THUMB_SLOTS];
static u32 thumb_clock = 0;

static int next_pow2(int v)
{
    int p = 8;
    while (p < v) p <<= 1;
    return p;
}

static void thumb_release(Thumb *t)
{
    if (t->ok) C3D_TexDelete(&t->tex);
    t->ok = false;
    t->valid = false;
    t->path[0] = 0;
}

/* upload RGBA8 pixels into a swizzled (tiled) 3DS texture */
static bool upload_rgba(Thumb *t, const u8 *px, int w, int h)
{
    int tw = next_pow2(w), th = next_pow2(h);
    if (tw > 1024 || th > 1024) return false;
    if (!C3D_TexInit(&t->tex, tw, th, GPU_RGBA8)) return false;
    C3D_TexSetFilter(&t->tex, GPU_NEAREST, GPU_NEAREST);   /* crisp pixels */
    u32 *dst = (u32 *)t->tex.data;
    memset(dst, 0, tw * th * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const u8 *s = px + (y * w + x) * 4;
            /* 8x8 tile morton order */
            u32 tile = ((y >> 3) * (tw >> 3) + (x >> 3)) * 64;
            u32 xx = x & 7, yy = y & 7;
            u32 m = (xx & 1) | ((yy & 1) << 1) | ((xx & 2) << 1) |
                    ((yy & 2) << 2) | ((xx & 4) << 2) | ((yy & 4) << 3);
            dst[tile + m] = ((u32)s[0] << 24) | ((u32)s[1] << 16) |
                            ((u32)s[2] << 8) | 0xFF;
        }
    }
    C3D_TexFlush(&t->tex);
    t->w = w; t->h = h;
    t->sub.width = w;
    t->sub.height = h;
    t->sub.left = 0.0f;
    t->sub.top = 1.0f;
    t->sub.right = (float)w / tw;
    t->sub.bottom = 1.0f - (float)h / th;
    return true;
}

static int decodes_this_frame = 0;

static Thumb *thumb_get(const char *path)
{
    thumb_clock++;
    for (int i = 0; i < THUMB_SLOTS; i++)
        if (thumbs[i].valid && strcmp(thumbs[i].path, path) == 0) {
            thumbs[i].used = thumb_clock;
            return &thumbs[i];
        }
    /* only one SD read + JPEG decode per frame, to avoid stutter */
    static Thumb pending = { .valid = false };
    if (decodes_this_frame > 0) return &pending;
    decodes_this_frame++;
    /* pick least recently used slot */
    int v = 0;
    for (int i = 1; i < THUMB_SLOTS; i++)
        if (thumbs[i].used < thumbs[v].used) v = i;
    Thumb *t = &thumbs[v];
    thumb_release(t);
    strncpy(t->path, path, sizeof(t->path) - 1);
    t->valid = true;
    t->used = thumb_clock;

    FILE *f = fopen(path, "rb");
    if (!f) return t;
    u8 *buf = malloc(16384);
    int n = buf ? (int)fread(buf, 1, 16384, f) : 0;
    fclose(f);
    t->is_item = false;
    if (n > 4 && buf[0] == 0xFF && buf[1] == 0xD8) {
        for (int i = 2; i + 6 <= n; i++)
            if (buf[i] == 'T' && memcmp(buf + i, "TAMAGO", 6) == 0) { t->is_item = true; break; }
        if (n == 16384) t->is_item = true;   /* huge preview: TAMAGO further in */
        int w = 0, h = 0;
        u8 *px = thumb_decode(buf, n, &w, &h);
        if (px) {
            /* standard 3DS tiled upload: image row 0 = texture top (v=1),
               sub-texture top=1, bottom=1-h/th. No flip needed. */
            t->ok = upload_rgba(t, px, w, h);
            thumb_free(px);
        }
    }
    free(buf);
    return t;
}

/* ---------------- drawing helpers ---------------- */
static C3D_RenderTarget *top, *bot;
static C2D_TextBuf tbuf;
static C2D_TextBuf fit_buf;   /* scratch buffer for measuring, cleared each try */

static void rounded_rect(float x, float y, float w, float h, float r, u32 c)
{
    C2D_DrawRectSolid(x + r, y, 0.5f, w - 2 * r, h, c);
    C2D_DrawRectSolid(x, y + r, 0.5f, r, h - 2 * r, c);
    C2D_DrawRectSolid(x + w - r, y + r, 0.5f, r, h - 2 * r, c);
    C2D_DrawCircleSolid(x + r, y + r, 0.5f, r, c);
    C2D_DrawCircleSolid(x + w - r, y + r, 0.5f, r, c);
    C2D_DrawCircleSolid(x + r, y + h - r, 0.5f, r, c);
    C2D_DrawCircleSolid(x + w - r, y + h - r, 0.5f, r, c);
}

static bool has_kana(const char *s)
{
    /* UTF-8 of U+3040..U+30FF starts with E3 81/82/83 */
    for (; *s; s++)
        if ((u8)s[0] == 0xE3 && ((u8)s[1] >= 0x81 && (u8)s[1] <= 0x83)) return true;
    return false;
}

/* Chinese UI uses the TWN system font; strings with kana keep the default
   (Japanese) font, because the TWN font has no kana. */
static bool force_zh = false;     /* draw next text with the Chinese font */

static C2D_Font font_for(const char *s)
{
    if ((lang == LANG_ZH || force_zh) && zh_font && !has_kana(s)) return zh_font;
    return NULL;
}

static void text(const char *s, float x, float y, float sc, u32 col, u32 flags)
{
    C2D_Text t;
    C2D_TextFontParse(&t, font_for(s), tbuf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | flags, x, y, 0.6f, sc, sc, col);
}

static float glyph_w_sc(u32 cp, float sc);
static int utf8_next(const char *s, u32 *cp);

static float str_w(const char *s, float sc)
{
    float w = 0; u32 cp;
    while (*s) { int n = utf8_next(s, &cp); w += glyph_w_sc(cp, sc); s += n; }
    return w;
}

/* keep the END of s (the current folder) and put "..." in front */
static void fit_tail(const char *s, char *out, int outlen, float sc, float max_w)
{
    if (str_w(s, sc) <= max_w) { snprintf(out, outlen, "%s", s); return; }
    float dots = str_w("...", sc);
    const char *p = s;
    while (*p && str_w(p, sc) + dots > max_w) {
        u32 cp; p += utf8_next(p, &cp);
    }
    snprintf(out, outlen, "...%s", p);
}

/* shorten s so it fits max_w at scale sc, adds "..." */
static void fit_text(const char *s, char *out, int outlen, float sc, float max_w)
{
    strncpy(out, s, outlen - 1);
    out[outlen - 1] = 0;
    for (;;) {
        C2D_Text t;
        float w, h;
        C2D_TextBufClear(fit_buf);
        C2D_TextFontParse(&t, font_for(out), fit_buf, out);
        C2D_TextGetDimensions(&t, sc, sc, &w, &h);
        int n = strlen(out);
        if (w <= max_w || n <= 4) return;
        /* drop one UTF-8 character before the "..." */
        int cut = n - 1;
        if (n > 3 && strcmp(out + n - 3, "...") == 0) cut = n - 4;
        while (cut > 0 && (out[cut] & 0xC0) == 0x80) cut--;
        out[cut] = 0;
        strcat(out, "...");
    }
}

/* ---------------- screens ---------------- */
#define ROW_H     20.0f
#define LIST_X    14.0f
#define LIST_Y    34.0f
#define LIST_W    372.0f
#define LIST_H    192.0f
#define ROWS_VIS  9

static float scroll_anim = 0.0f;
static char status_line[128] = "";

static void draw_top(void)
{
    C2D_TargetClear(top, COL_BG);
    C2D_SceneBegin(top);
    C2D_TextBufClear(tbuf);

    /* title */
    text("Tama3DS", 14, 8, 0.6f, COL_WHITE, 0);
    char crumb[160];
    const char *rel = cur_path + strlen(ROOT_DIR);
    snprintf(crumb, sizeof(crumb), "%s", *rel ? rel + 1 : "");
    char fitted[160];
    fit_tail(crumb, fitted, sizeof(fitted), 0.45f, 190);   /* max half the screen */
    text(fitted, 386, 11, 0.45f, COL_PANEL, C2D_AlignRight);

    /* panel with soft shadow */
    rounded_rect(LIST_X + 2, LIST_Y + 3, LIST_W, LIST_H, 10, COL_SHADOW);
    rounded_rect(LIST_X, LIST_Y, LIST_W, LIST_H, 10, COL_PANEL);

    if (nitems == 0) {
        text(T(S_EMPTY), 200, LIST_Y + 80, 0.55f, COL_TEXT, C2D_AlignCenter);
        return;
    }

    /* scroll so that selection stays visible */
    float target = 0;
    if (sel > ROWS_VIS / 2) target = sel - ROWS_VIS / 2;
    if (target > nitems - ROWS_VIS) target = nitems - ROWS_VIS;
    if (target < 0) target = 0;
    scroll_anim += (target - scroll_anim) * 0.35f;
    if (fabsf(target - scroll_anim) < 0.01f) scroll_anim = target;

    int first = (int)floorf(scroll_anim);
    float frac = scroll_anim - first;
    float y0 = LIST_Y + 6 - frac * ROW_H;

    /* highlight bar follows sel_anim smoothly */
    float hy = LIST_Y + 6 + (sel_anim - scroll_anim) * ROW_H;
    if (hy > LIST_Y && hy < LIST_Y + LIST_H - ROW_H)
        rounded_rect(LIST_X + 6, hy, LIST_W - 12, ROW_H, 5, COL_HILITE);

    for (int i = first; i < nitems && i < first + ROWS_VIS + 1; i++) {
        float y = y0 + (i - first) * ROW_H;
        if (y < LIST_Y + 2 || y > LIST_Y + LIST_H - ROW_H + 2) continue;
        u32 col = (i == sel) ? COL_WHITE :
                  (items[i].kind == K_OTHER ? COL_DIM : COL_TEXT);
        char label[NAME_LEN + 4], fit[NAME_LEN + 4];
        snprintf(label, sizeof(label), "%s%s", items[i].is_dir ? "> " : "", items[i].name);
        fit_text(label, fit, sizeof(fit), 0.5f, LIST_W - 80);
        text(fit, LIST_X + 14, y + 3, 0.5f, col, 0);
        if (!items[i].is_dir) {
            char sz[24];
            snprintf(sz, sizeof(sz), "%.1f KB", item_size(i) / 1024.0f);
            text(sz, LIST_X + LIST_W - 14, y + 4, 0.42f, col, C2D_AlignRight);
        }
    }

    /* scrollbar */
    if (nitems > ROWS_VIS) {
        float bh = LIST_H * ROWS_VIS / nitems;
        float by = LIST_Y + (LIST_H - bh) * scroll_anim / (nitems - ROWS_VIS);
        C2D_DrawRectSolid(LIST_X + LIST_W - 4, by + 4, 0.55f, 2, bh - 8, COL_HILITE);
    }
}

static void draw_bottom_bg(void)
{
    /* #630000 solid for top quarter, then gradient down to #f8ecec */
    C2D_TargetClear(bot, COL_PANEL);
    C2D_SceneBegin(bot);
    C2D_DrawRectSolid(0, 0, 0.1f, 320, 60, COL_DARK);
    C2D_DrawRectangle(0, 60, 0.1f, 320, 180, COL_DARK, COL_DARK, COL_PANEL, COL_PANEL);
}

static void build_path(int idx, char *out, int len)
{
    snprintf(out, len, "%s/%s", cur_path, items[idx].name);
}

/* draw one carousel card centred at cx,cy with max box size box */
static void draw_card(int idx, float cx, float cy, float box, float selected)
{
    if (idx < 0 || idx >= nitems) return;
    if (items[idx].is_dir) {
        /* folders: simple tab shape */
        float w = box * 0.8f, h = box * 0.6f;
        u32 c = selected > 0.5f ? COL_HILITE : COL_FOLDER2;
        rounded_rect(cx - w / 2, cy - h / 2 + 6, w * 0.45f, 12, 4, c);
        rounded_rect(cx - w / 2, cy - h / 2 + 12, w, h, 6, c);
        return;
    }
    if (items[idx].kind == K_TXT || items[idx].kind == K_OTHER) {
        float w = box * 0.55f, h = box * 0.72f;
        if (selected > 0.0f) {
            float b = 4 * selected;
            rounded_rect(cx - w / 2 - b, cy - h / 2 - b, w + 2 * b, h + 2 * b, 6, COL_HILITE);
        }
        rounded_rect(cx - w / 2, cy - h / 2, w, h, 5, COL_WHITE);
        for (int l = 0; l < 4; l++)
            C2D_DrawRectSolid(cx - w / 2 + w * 0.18f, cy - h / 2 + h * (0.24f + l * 0.16f), 0.55f,
                              w * (l == 3 ? 0.4f : 0.64f), 2, COL_FOLDER2);
        return;
    }
    char p[700];
    build_path(idx, p, sizeof(p));
    Thumb *t = thumb_get(p);
    if (!t->valid) return;                    /* not decoded yet, next frame */
    if (!t->is_item) items[idx].kind = K_OTHER;   /* content check result */
    if (!t->ok) {
        float s = box * 0.6f;
        C2D_DrawRectSolid(cx - s / 2, cy - s / 2, 0.4f, s, s, COL_FOLDER2);
        return;
    }
    /* Size at rest uses an integer scale of the 96px centre box (crisp pixel
       art). While sliding, the size changes continuously with box/96, so
       there are no sudden jumps between integer steps. */
    float m = (float)(t->w > t->h ? t->w : t->h);
    float rest = floorf(96.0f / m);
    if (rest < 1) rest = 96.0f / m;           /* big images: fractional fit */
    float f = rest * (box / 96.0f);
    float w = t->w * f, h = t->h * f;
    if (selected > 0.0f) {                    /* border fades in smoothly */
        float b = 4 * selected;
        C2D_DrawRectSolid(cx - w / 2 - b, cy - h / 2 - b, 0.45f, w + 2 * b, h + 2 * b, COL_HILITE);
    }
    C2D_Image img = { &t->tex, &t->sub };
    C2D_DrawParams dp = { { cx - w / 2, cy - h / 2, w, h }, { 0, 0 }, 0.5f, 0 };
    C2D_DrawImage(img, &dp, NULL);
}

static void draw_bottom(void)
{
    draw_bottom_bg();
    C2D_TextBufClear(tbuf);

    if (nitems == 0) {
        text(T(S_PUT_FILES), 160, 120, 0.5f, COL_TEXT, C2D_AlignCenter);
        return;
    }

    /* carousel: offset follows sel_anim for a sliding effect */
    float off = sel_anim - sel;            /* -1..1 while animating */
    float spacing = 112;
    float cy = 120;
    for (int d = -2; d <= 2; d++) {
        int idx = sel + d;
        float pos = d - off;               /* continuous position */
        float cx = 160 + pos * spacing;
        if (cx < -80 || cx > 400) continue;
        float a = fabsf(pos);
        float box = 96 - (a > 1 ? 1 : a) * 40;   /* 96 centre, 56 sides */
        float hl = 1.0f - a;                     /* 1 at centre, 0 at sides */
        draw_card(idx, cx, cy, box, hl > 0 ? hl : 0);
    }

    /* name + info under the centre card */
    char name[NAME_LEN], fit[NAME_LEN + 4];
    strncpy(name, items[sel].name, NAME_LEN - 1);
    name[NAME_LEN - 1] = 0;
    fit_text(name, fit, sizeof(fit), 0.5f, 300);
    text(fit, 160, 186, 0.5f, COL_TEXT, C2D_AlignCenter);

    char info[160];
    const Item *it = &items[sel];
    if (it->is_dir)
        snprintf(info, sizeof(info), "%s", T(S_OPEN_BACK));
    else if (it->kind == K_TXT)
        snprintf(info, sizeof(info), "%.1f KB   %s   %s", item_size(sel) / 1024.0f, T(S_VIEW), T(S_BACK));
    else {
        u32 bps = atoi(SPEEDS[speed_idx].name);
        /* measured: ~2.0 s/KB at 9600; scale with speed, plus fixed handshake */
        u32 est = (u32)(item_size(sel) * 2.0f / 1000.0f * 9600.0f / bps) + 3;
        snprintf(info, sizeof(info), "%.1f KB  ~%lus   %s   %s", item_size(sel) / 1024.0f,
                 (unsigned long)est, T(S_SEND), T(S_BACK));
    }
    text(info, 160, 208, 0.42f, COL_TEXT, C2D_AlignCenter);
    if (it->kind == K_OTHER)
        text(T(S_NOT_ITEM), 160, 168, 0.4f, COL_DIM, C2D_AlignCenter);

    text(T(S_SELECT_HINT), 160, 226, 0.36f, COL_DIM, C2D_AlignCenter);
    text(AUTHOR, 314, 226, 0.36f, COL_TEXT, C2D_AlignRight);

    /* header text on the dark area */
    if (status_line[0])
        text(status_line, 160, 18, 0.5f, COL_WHITE, C2D_AlignCenter);
    else
        text(T(S_HINT), 160, 18, 0.45f, COL_PANEL, C2D_AlignCenter);
}

/* decode neighbours ahead of time so they are ready before they slide in */
static void preload(void)
{
    char p[700];
    for (int d = 0; d <= 3; d++) {
        int ids[2] = { sel + d, sel - d };
        for (int k = 0; k < 2; k++) {
            int i = ids[k];
            if (i < 0 || i >= nitems || items[i].kind != K_ITEM) continue;
            build_path(i, p, sizeof(p));
            thumb_get(p);
            if (decodes_this_frame) return;
        }
    }
}

static void render(void)
{
    decodes_this_frame = 0;
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    draw_top();
    draw_bottom();
    C3D_FrameEnd(0);
    preload();
}

/* ---------------- sending ---------------- */
static FILE *send_fp = NULL;
static char send_name[NAME_LEN];
static u32 send_size = 0, send_done = 0;
static u64 send_t0 = 0;
static u64 last_draw = 0;

static int16_t get_name_cb(char *name, uint16_t size, void *ud)
{
    strncpy(name, send_name, size);
    name[size - 1] = 0;
    return strlen(name);
}

static int16_t get_data_cb(uint8_t *data, uint16_t size, void *ud)
{
    int16_t n = (int16_t)fread(data, 1, size, send_fp);
    if (n > 0) send_done += n;
    return n;
}

static int progress_state = 0;   /* 0 running, 1 done, 2 failed */

static bool draw_nonblock;

static void draw_progress(const char *msg, u32 col)
{
    decodes_this_frame = 1;                /* never read the SD card mid-transfer */
    if (!C3D_FrameBegin(draw_nonblock ? C3D_FRAME_NONBLOCK : C3D_FRAME_SYNCDRAW)) return;
    draw_top();
    draw_bottom_bg();
    C2D_TextBufClear(tbuf);

    char fit[NAME_LEN + 4];
    fit_text(send_name, fit, sizeof(fit), 0.5f, 290);
    text(fit, 160, 16, 0.5f, COL_WHITE, C2D_AlignCenter);

    /* centre card stays visible while sending */
    draw_card(sel, 160, 104, 96, 1.0f);

    float pct = send_size ? (float)send_done / send_size : 0;
    if (pct > 1) pct = 1;
    rounded_rect(30, 172, 260, 14, 7, COL_TRACK);
    if (pct > 0.02f) rounded_rect(30, 172, 260 * pct, 14, 7, COL_HILITE);

    char line[96];
    u64 el = (osGetTime() - send_t0) / 1000;
    if (send_done || progress_state)
        snprintf(line, sizeof(line), "%s  %d%%  %llus  (%lu bps)", msg, (int)(pct * 100),
                 (unsigned long long)el, (unsigned long)irphy_cur_bps);
    else   /* still negotiating at 9600: do not show a misleading speed */
        snprintf(line, sizeof(line), "%s  %llus", msg, (unsigned long long)el);
    text(line, 160, 194, 0.46f, col, C2D_AlignCenter);
    if (progress_state == 0)
        text(T(S_HOLD_START), 160, 216, 0.4f, COL_TEXT, C2D_AlignCenter);
    else if (progress_state == 2 && speed_idx > 0)
        text(T(S_FAIL_HINT), 160, 216, 0.4f, COL_TEXT, C2D_AlignCenter);
    else
        text(T(S_BACK), 160, 216, 0.4f, COL_TEXT, C2D_AlignCenter);
    C3D_FrameEnd(0);
}

/* called by irphy while waiting for IR data: keep the screen alive */
static void idle_draw(void)
{
    u64 now = osGetTime();
    if (now - last_draw < 250) return;     /* 4 fps is enough for a progress bar */
    last_draw = now;
    draw_nonblock = true;                  /* skip the frame if the GPU is busy */
    draw_progress(send_done ? T(S_SENDING) : T(S_CONNECTING), COL_TEXT);
    draw_nonblock = false;
}

static void wait_a_or_b(void)
{
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & (KEY_A | KEY_B)) break;
        gspWaitForVBlank();
    }
}

static void send_file(int idx)
{
    char path[700];
    build_path(idx, path, sizeof(path));
    send_fp = fopen(path, "rb");
    if (!send_fp) { snprintf(status_line, sizeof(status_line), "open failed"); return; }
    strncpy(send_name, items[idx].name, NAME_LEN - 1);
    send_name[NAME_LEN - 1] = 0;
    send_size = item_size(idx);
    send_done = 0;
    send_t0 = osGetTime();
    last_draw = 0;

    if (debug_fd) fprintf(debug_fd, "\n===== send %s %lu =====\n", send_name, (unsigned long)send_size);
    if (dump_fd) fprintf(dump_fd, "\n===== send %s =====\n", send_name);
    dump_dir = -1;
    irphy_abort = 0;
    irphy_rx_restarts = irphy_tx_frames = irphy_rx_bytes = irphy_send_stuck = 0;
    irphy_hw_resets = 0;

    irphy_baud_mask = SPEEDS[speed_idx].mask;
    irphy_idle_cb = idle_draw;
    IrLAP_Context ctx;
    irlap_init_context(&ctx);
    irphy_reset();
    u32 size = send_size;
    uint8_t ok = irobex_send(&ctx, 0, get_name_cb, get_data_cb, &size);
    irphy_idle_cb = NULL;

    fclose(send_fp);
    send_fp = NULL;
    if (debug_fd) fflush(debug_fd);
    if (dump_fd) fflush(dump_fd);
    if (debug_fd) fprintf(debug_fd, "recover calls %lu\n", (unsigned long)irphy_hw_resets);
    if (debug_fd) fprintf(debug_fd, "result %s, %llu ms, tx %lu, rx %lu, restarts %lu\n",
                          irphy_abort ? "ABORT" : ok ? "OK" : "FAIL",
                          (unsigned long long)(osGetTime() - send_t0),
                          (unsigned long)irphy_tx_frames, (unsigned long)irphy_rx_bytes,
                          (unsigned long)irphy_rx_restarts);

    if (irphy_abort) ok = 0;
    if (ok) send_done = send_size;
    const char *msg = ok ? T(S_DONE) : irphy_abort ? T(S_ABORTED) : T(S_FAILED);
    u32 col = ok ? RGB(0x2E, 0x7D, 0x32) : COL_TEXT;
    progress_state = ok ? 1 : 2;
    draw_progress(msg, col);
    progress_state = 0;
    irphy_abort = 0;
    wait_a_or_b();
    snprintf(status_line, sizeof(status_line), "%s: %s", msg, send_name);
}

/* ---------------- text viewer ---------------- */
#define TXT_MAX   (64 * 1024)
#define TXT_LINES 4096
#define TXT_SC    0.46f
#define TXT_LH    15.0f
#define TXT_W     (LIST_W - 34)
#define TXT_VIS   12

static char *txt_buf = NULL;
static int txt_off[TXT_LINES];      /* start offset of each wrapped line */
static u16 txt_len[TXT_LINES];
static int txt_n = 0;

static float glyph_w_sc(u32 cp, float sc);
static float glyph_w(u32 cp) { return glyph_w_sc(cp, TXT_SC); }
static float glyph_w_sc(u32 cp, float sc)
{
    C2D_Font f = (lang == LANG_ZH && zh_font && !(cp >= 0x3040 && cp <= 0x30FF)) ? zh_font : NULL;
    int gi = C2D_FontGlyphIndexFromCodePoint(f, cp);
    charWidthInfo_s *w = C2D_FontGetCharWidthInfo(f, gi);
    /* citro2d draws glyphs scaled by 30/cellHeight; the width table is in
       raw font pixels, so apply the same factor (missing before -> lines
       were measured too narrow and ran off the panel) */
    FINF_s *fi = C2D_FontGetInfo(f);
    float k = (fi && fi->tglp && fi->tglp->cellHeight) ? 30.0f / fi->tglp->cellHeight : 1.0f;
    return (w ? w->charWidth : 8) * sc * k;
}

static int utf8_next(const char *s, u32 *cp)
{
    const u8 *u = (const u8 *)s;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    if ((u[0] & 0xE0) == 0xC0 && u[1]) { *cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); return 2; }
    if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) {
        *cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F); return 3; }
    if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) {
        *cp = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F); return 4; }
    *cp = '?'; return 1;
}

/* split into lines that fit the panel; long words break anywhere */
static void txt_wrap(void)
{
    txt_n = 0;
    int i = 0, start = 0, last_space = -1;
    float w = 0;
    while (txt_buf[i] && txt_n < TXT_LINES - 1) {
        if (txt_buf[i] == '\n') {
            txt_off[txt_n] = start; txt_len[txt_n++] = i - start;
            start = ++i; w = 0; last_space = -1; continue;
        }
        u32 cp; int n = utf8_next(txt_buf + i, &cp);
        float cw = cp == '\t' ? glyph_w(' ') * 4 : glyph_w(cp);
        if (w + cw > TXT_W && i > start) {
            int cut = (last_space > start) ? last_space + 1 : i;
            txt_off[txt_n] = start; txt_len[txt_n++] = cut - start;
            start = cut; i = cut; w = 0; last_space = -1; continue;
        }
        if (cp == ' ') last_space = i;
        w += cw; i += n;
    }
    if (i > start || txt_n == 0) { txt_off[txt_n] = start; txt_len[txt_n++] = i - start; }
}

static bool txt_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    free(txt_buf);
    txt_buf = malloc(TXT_MAX + 1);
    if (!txt_buf) { fclose(f); return false; }
    size_t n = fread(txt_buf, 1, TXT_MAX, f);
    fclose(f);
    txt_buf[n] = 0;
    /* strip UTF-8 BOM and CR */
    char *src = txt_buf, *dst = txt_buf;
    if ((u8)src[0] == 0xEF && (u8)src[1] == 0xBB && (u8)src[2] == 0xBF) src += 3;
    for (; *src; src++) if (*src != '\r') *dst++ = *src;
    *dst = 0;
    txt_wrap();
    return true;
}

static void text_viewer(int idx)
{
    char path[700];
    build_path(idx, path, sizeof(path));
    if (!txt_load(path)) return;
    float scroll = 0, target = 0;
    int max_top = txt_n > TXT_VIS ? txt_n - TXT_VIS : 0;
    char line[512];

    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown(), kr = hidKeysDownRepeat();
        if (k & KEY_B) break;
        if (kr & KEY_DOWN) target += 1;
        if (kr & KEY_UP) target -= 1;
        if (kr & (KEY_R | KEY_RIGHT)) target += TXT_VIS - 1;
        if (kr & (KEY_L | KEY_LEFT)) target -= TXT_VIS - 1;
        circlePosition cp; hidCircleRead(&cp);
        if (cp.dy < -40 || cp.dy > 40) target -= cp.dy / 400.0f;   /* circle pad: smooth */
        if (target < 0) target = 0;
        if (target > max_top) target = max_top;
        scroll += (target - scroll) * 0.3f;
        if (fabsf(target - scroll) < 0.01f) scroll = target;

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top, COL_BG);
        C2D_SceneBegin(top);
        C2D_TextBufClear(tbuf);
        char fit[NAME_LEN + 4];
        fit_text(items[idx].name, fit, sizeof(fit), 0.5f, 300);
        text(fit, 14, 10, 0.5f, COL_WHITE, 0);
        char pos[32];
        snprintf(pos, sizeof(pos), "%d/%d", (int)scroll + 1, txt_n);
        text(pos, 386, 11, 0.42f, COL_PANEL, C2D_AlignRight);

        rounded_rect(LIST_X + 2, LIST_Y + 3, LIST_W, LIST_H, 10, COL_SHADOW);
        rounded_rect(LIST_X, LIST_Y, LIST_W, LIST_H, 10, COL_PANEL);

        int first = (int)floorf(scroll);
        float fr = scroll - first;
        for (int i = first; i < txt_n && i <= first + TXT_VIS; i++) {
            float y = LIST_Y + 7 + (i - first - fr) * TXT_LH;
            if (y < LIST_Y + 2 || y > LIST_Y + LIST_H - TXT_LH) continue;
            int n = txt_len[i] < sizeof(line) - 1 ? txt_len[i] : sizeof(line) - 1;
            memcpy(line, txt_buf + txt_off[i], n);
            line[n] = 0;
            if (n) text(line, LIST_X + 14, y, TXT_SC, COL_TEXT, 0);
        }
        if (txt_n == 1 && txt_len[0] == 0)
            text(T(S_TXT_EMPTY), 200, LIST_Y + 80, 0.5f, COL_TEXT, C2D_AlignCenter);
        if (max_top > 0) {
            float bh = LIST_H * TXT_VIS / txt_n;
            if (bh < 12) bh = 12;
            float by = LIST_Y + (LIST_H - bh) * scroll / max_top;
            C2D_DrawRectSolid(LIST_X + LIST_W - 5, by + 4, 0.55f, 3, bh - 8, COL_HILITE);
        }

        draw_bottom_bg();
        C2D_TextBufClear(tbuf);
        draw_card(idx, 160, 110, 96, 1.0f);
        text(T(S_TXT_HELP), 160, 208, 0.42f, COL_TEXT, C2D_AlignCenter);
        C3D_FrameEnd(0);
    }
    free(txt_buf);
    txt_buf = NULL;
}

/* ---------------- settings + about (SELECT) ---------------- */
static const char *thanks_lines[] = {
    "PspIrDA - Mathieu Albinet (BSD)",
    "IrDA stack - Gerd Rausch, BlauLogic",
    "stb_image - Sean Barrett",
    "3dsir - Doridian (IR send method)",
    "libctru / citro2d / citro3d - devkitPro",
    "3dbrew.org - IR documentation",
    NULL
};

static void outline_rect(float x, float y, float w, float h, float r, float t, u32 c, u32 inner)
{
    rounded_rect(x, y, w, h, r, c);
    rounded_rect(x + t, y + t, w - 2 * t, h - 2 * t, r - t > 1 ? r - t : 1, inner);
}

static int next_lang(int l, int dir)
{
    do { l = (l + dir + LANG_COUNT) % LANG_COUNT; } while (l == LANG_ZH && !zh_ok);
    return l;
}

/*
 * Browse: up/down moves an outlined cursor.
 * A: edit mode (solid highlight, white text); left/right change the value,
 *    nothing is applied yet. A again: apply + save. B: cancel.
 */
static void settings_page(void)
{
    int row = 0;
    bool editing = false;
    int e_lang = lang, e_speed = speed_idx, e_log = log_on;
    float row_anim = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown(), kr = hidKeysDownRepeat();
        if (!editing) {
            if (k & (KEY_B | KEY_SELECT)) break;
            if (kr & KEY_DOWN) row = (row + 1) % 3;
            if (kr & KEY_UP) row = (row + 2) % 3;
            if (k & KEY_A) {
                editing = true;
                e_lang = lang; e_speed = speed_idx; e_log = log_on;
            }
        } else {
            int dir = (kr & KEY_RIGHT) ? 1 : (kr & KEY_LEFT) ? -1 : 0;
            if (dir) {
                if (row == 0) e_lang = next_lang(e_lang, dir);
                else if (row == 1) e_speed = (e_speed + dir + NSPEEDS) % NSPEEDS;
                else e_log = !e_log;
            }
            if (k & KEY_A) {                      /* confirm */
                lang = e_lang; speed_idx = e_speed;
                if (log_on != (bool)e_log) { log_on = e_log; apply_log(); }
                save_cfg();
                editing = false;
            } else if (k & KEY_B) {               /* cancel */
                editing = false;
            }
        }
        row_anim += (row - row_anim) * 0.3f;

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        /* top: about */
        C2D_TargetClear(top, COL_BG);
        C2D_SceneBegin(top);
        C2D_TextBufClear(tbuf);
        rounded_rect(LIST_X + 2, 17, LIST_W, 212, 10, COL_SHADOW);
        rounded_rect(LIST_X, 14, LIST_W, 212, 10, COL_PANEL);
        text("Tama3DS " APP_VER, 200, 24, 0.66f, COL_TEXT, C2D_AlignCenter);
        text(T(S_SUBTITLE), 200, 46, 0.44f, COL_TEXT, C2D_AlignCenter);
        C2D_DrawRectSolid(120, 68, 0.5f, 160, 1, COL_HILITE);
        char by[64];
        snprintf(by, sizeof(by), "%s  " AUTHOR "  2026", T(S_MADE_BY));
        text(by, 200, 76, 0.48f, COL_TEXT, C2D_AlignCenter);
        text(T(S_THANKS), 200, 104, 0.46f, COL_DIM, C2D_AlignCenter);
        for (int i = 0; thanks_lines[i]; i++)
            text(thanks_lines[i], 200, 122 + i * 17, 0.42f, COL_TEXT, C2D_AlignCenter);

        /* bottom: settings rows */
        draw_bottom_bg();
        C2D_TextBufClear(tbuf);
        text(T(S_SETTINGS), 160, 18, 0.52f, COL_WHITE, C2D_AlignCenter);
        rounded_rect(18, 70, 284, 114, 9, COL_PANEL);
        float cy = 76 + row_anim * 34;
        if (editing) rounded_rect(24, cy, 272, 30, 6, COL_HILITE);
        else outline_rect(24, cy, 272, 30, 6, 2, COL_HILITE, COL_PANEL);

        int show_lang = (editing && row == 0) ? e_lang : lang;
        int show_speed = (editing && row == 1) ? e_speed : speed_idx;
        int show_log = (editing && row == 2) ? e_log : log_on;
        const char *lab[3] = { T(S_LANG), T(S_SPEED), T(S_LOG) };
        char val[3][48];
        snprintf(val[0], 48, "%s", LANG_NAME[show_lang]);
        snprintf(val[1], 48, "%s bps", SPEEDS[show_speed].name);
        snprintf(val[2], 48, "%s", show_log ? T(S_ON) : T(S_OFF));
        for (int i = 0; i < 3; i++) {
            bool act = editing && i == row;
            u32 c = act ? COL_WHITE : COL_TEXT;
            text(lab[i], 36, 83 + i * 34, 0.48f, c, 0);
            char v[64];
            if (act) snprintf(v, sizeof(v), "\u2190 %s \u2192", val[i]);
            else snprintf(v, sizeof(v), "%s", val[i]);
            force_zh = (i == 0 && show_lang == LANG_ZH);
            text(v, 284, 83 + i * 34, 0.48f, c, C2D_AlignRight);
            force_zh = false;
        }
        const char *note = row == 1 ? (show_speed == 0 ? T(S_STABLE) : T(S_SPEED_NOTE))
                         : row == 2 ? T(S_LOG_NOTE) : (!zh_ok ? T(S_FONT_MISSING) : "");
        text(note, 160, 190, 0.4f, COL_TEXT, C2D_AlignCenter);
        text(editing ? T(S_SET_EDIT) : T(S_SET_HELP), 160, 212, 0.4f, COL_TEXT, C2D_AlignCenter);
        C3D_FrameEnd(0);
    }
}

/* ---------------- navigation ---------------- */
static void enter_dir(int idx)
{
    if (depth < 15) sel_stack[depth++] = sel;
    size_t l = strlen(cur_path);
    snprintf(cur_path + l, sizeof(cur_path) - l, "/%s", items[idx].name);
    scan_dir();
    sel = 0; sel_anim = 0; scroll_anim = 0;
}

static void leave_dir(void)
{
    if (strcmp(cur_path, ROOT_DIR) == 0) return;
    char *s = strrchr(cur_path, '/');
    if (s) *s = 0;
    scan_dir();
    sel = depth > 0 ? sel_stack[--depth] : 0;
    if (sel >= nitems) sel = nitems ? nitems - 1 : 0;
    sel_anim = sel;
    scroll_anim = sel > ROWS_VIS / 2 ? sel - ROWS_VIS / 2 : 0;
}

int main(int argc, char **argv)
{
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 2);
    C2D_Prepare();
    top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    tbuf = C2D_TextBufNew(4096);
    fit_buf = C2D_TextBufNew(256);
    hidSetRepeatParameters(18, 5);

    mkdir(ROOT_DIR, 0777);
    Result r = irphy_init();
    if (R_FAILED(r))
        snprintf(status_line, sizeof(status_line), "IR init failed %08lX", (unsigned long)r);

    load_cfg();
    apply_log();
    cfguInit();
    zh_font = C2D_FontLoadSystem(CFG_REGION_TWN);   /* NULL if TWN = system font */
    u8 region = 0, syslang = 0;
    CFGU_SecureInfoGetRegion(&region);
    CFGU_GetSystemLanguage(&syslang);
    zh_ok = zh_font != NULL || region == CFG_REGION_TWN;
    if (lang < 0) {
        if (syslang == CFG_LANGUAGE_JP) lang = LANG_JA;
        else if ((syslang == CFG_LANGUAGE_TW || syslang == CFG_LANGUAGE_ZH) && zh_ok) lang = LANG_ZH;
        else lang = LANG_EN;
    }
    if (lang == LANG_ZH && !zh_ok) lang = LANG_EN;
    scan_dir();

    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        u32 kr = hidKeysDownRepeat();
        if (k & KEY_START) break;

        if (nitems) {
            if (kr & KEY_DOWN) sel = (sel + 1) % nitems;
            if (kr & KEY_UP) sel = (sel + nitems - 1) % nitems;
            if (kr & KEY_RIGHT) { sel += ROWS_VIS; if (sel >= nitems) sel = nitems - 1; }
            if (kr & KEY_LEFT) { sel -= ROWS_VIS; if (sel < 0) sel = 0; }
            if (k & KEY_A) {
                if (items[sel].is_dir) enter_dir(sel);
                else if (items[sel].kind == K_TXT) text_viewer(sel);
                else if (R_SUCCEEDED(r)) send_file(sel);
            }
        }
        if (k & KEY_B) leave_dir();
        if (k & KEY_SELECT) settings_page();

        /* smooth follow; wrap-around jumps instantly */
        if (fabsf(sel - sel_anim) > 2.5f) sel_anim = sel;
        sel_anim += (sel - sel_anim) * 0.22f;
        if (fabsf(sel - sel_anim) < 0.01f) sel_anim = sel;

        render();
    }

    for (int i = 0; i < THUMB_SLOTS; i++) thumb_release(&thumbs[i]);
    if (debug_fd) fclose(debug_fd);
    if (dump_fd) fclose(dump_fd);
    if (R_SUCCEEDED(r)) irphy_exit();
    C2D_TextBufDelete(tbuf);
    C2D_TextBufDelete(fit_buf);
    if (zh_font) C2D_FontFree(zh_font);
    cfguExit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
