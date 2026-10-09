/* display.c -- Carta 2 screen test: the real ramp_display.c, ramp_input.c and
 * common code, compiled for Linux, drawing into a 240x240 framebuffer.
 *
 * The device's RAM (0x840000-0x84ffff), its registers page (0x800000) and its
 * flash (from 0x10000, filled from your own stock PROD-111224 image -- not in
 * this repo) are mapped at their real addresses, so the patch's own pointers,
 * glyph tables and struct offsets are used unchanged. Stock functions are C
 * stand-ins (sim_fn), with fill_rect and blit reproducing the decompiled
 * primitives exactly (FUN_000074d0, FUN_00007e2c): an (w+1) x (h+1) window;
 * blit rows of ceil((w+1)/8) bytes, bits LSB first, set bit = black.
 *
 * Checks every primitive stays inside the 240x240 screen, the picker box
 * and the ramp screen draw, and writes PPM frames for a visual check.
 *
 *   sh tools/hosttest/run_display.sh path/to/carta2-PROD-111224.bin
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "ramp.h"

void ramp_trampoline(void);
void ramp_d3c0_full(void);
void ramp_d3c0_view(void);
void ramp_event_entry(void);

#define W 240
#define H 240
static unsigned short fb[H][W];
static int oob, prims, fails;
static unsigned char flash_store[0x1000];

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put(int x, int y, unsigned short c)
{
    if (x < 0 || y < 0 || x >= W || y >= H) { oob++; return; }
    fb[y][x] = c;
}

/* ---- stock stand-ins -------------------------------------------------------- */
static void s_fill(u8 x, u8 y, u8 w, u8 h, u8 hi, u8 lo)
{
    int i, j;
    prims++;
    if (x + w >= W || y + h >= H) oob++;
    for (j = 0; j <= h; j++)
        for (i = 0; i <= w; i++)
            put(x + i, y + j, (unsigned short)(hi << 8 | lo));
}

static void s_blit(u8 x, u8 y, u8 w, u8 h, u8 hi, u8 lo, const void *glyph)
{
    const u8 *g = glyph;
    int cols = w + 1, rows = h + 1, bpr = (cols + 7) / 8, r, c;
    prims++;
    if (x + w >= W || y + h >= H) oob++;
    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++) {
            int bit = (g[r * bpr + c / 8] >> (c % 8)) & 1;
            put(x + c, y + r, bit ? 0x0000 : (unsigned short)(hi << 8 | lo));
        }
}

static void s_batt_anim(int x, int y, int w, int h, int a, int b, int c, int d, int e, int f)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    s_fill((u8)x, (u8)y, (u8)w, (u8)h, 0x07, 0xe0);
}

static void s_noop(void) {}
static short s_div(int a, int b) { return (short)(a / b); }
static void s_flash_read(int addr, int len, void *buf) { memcpy(buf, flash_store + (addr - DEV_RAMP_FLASH), len); }
static void s_flash_erase(int addr) { memset(flash_store + (addr - DEV_RAMP_FLASH), 0xff, sizeof flash_store); }
static void s_flash_write(int addr, int len, void *buf)
{
    int i;
    for (i = 0; i < len; i++) flash_store[addr - DEV_RAMP_FLASH + i] &= ((u8 *)buf)[i];
}
static int s_notify(int h, const u8 *d, int l) { (void)h; (void)d; (void)l; return 0; }
static void s_draw_screen(unsigned int s) { (void)s; }

#define S(off)    (STRUCT_BASE[off])
#define S16(off)  (*(volatile u16 *)(STRUCT_BASE + (off)))
static int tick_no, target_f;

/* Carta 2 stock tick model: target from the active slot while not reached,
 * 2 F per tick toward it, countdown once a second from the start of a stage. */
static void s_pid_tick(void)
{
    int meas, slot;
    if (!S(OFF_SESSION)) return;
    slot = DEV_PRESET_OFF(DEV_MODE_IS_CONC() ? TBL_CO_F : TBL_FL_F, DEV_RANK(DEV_MODE_IS_CONC() ? 1 : 0));
    if (!S(OFF_REACHED)) target_f = S16(slot);
    meas = S16(OFF_MEAS_F);
    meas += meas < target_f ? 2 : (meas > target_f ? -2 : 0);
    if ((meas - target_f) * (meas - target_f) <= 1) meas = target_f;
    S16(OFF_MEAS_F) = (u16)meas;
    S16(OFF_MEAS_C) = (u16)((meas - 32) * 5 / 9);
    if (!S(OFF_REACHED) && meas == target_f) S(OFF_REACHED) = 1;
    if (tick_no % 10 == 0 && S16(OFF_COUNTDOWN) > 0) S16(OFF_COUNTDOWN)--;
}
static void s_stop(void) { S(OFF_SESSION) = 0; }

void *sim_fn(unsigned int a)
{
    switch (a) {
    case 0x74d0: return (void *)s_fill;
    case 0x7e2c: return (void *)s_blit;
    case 0x7a24: return (void *)s_batt_anim;
    case DEV_PID_TICK: return (void *)s_pid_tick;
    case DEV_STOP: return (void *)s_stop;
    case DEV_ROM_DIV: return (void *)s_div;
    case DEV_FLASH_READ: return (void *)s_flash_read;
    case DEV_FLASH_ERASE: return (void *)s_flash_erase;
    case DEV_FLASH_WRITE: return (void *)s_flash_write;
    case DEV_NOTIFY: return (void *)s_notify;
    case 0xf338: return (void *)s_draw_screen;
    default: return (void *)s_noop;   /* stock screen elements and the event consumer */
    }
}

/* ---- setup ------------------------------------------------------------------- */
static void map_fixed(unsigned long addr, unsigned long len)
{
    void *p = mmap((void *)addr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)addr) { perror("mmap"); exit(2); }
}

static void dump(const char *path)
{
    FILE *f = fopen(path, "wb");
    int x, y;
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            unsigned short c = fb[y][x];
            fputc((c >> 11) << 3, f); fputc(((c >> 5) & 63) << 2, f); fputc((c & 31) << 3, f);
        }
    fclose(f);
}

static int lit_in(int x0, int y0, int x1, int y1)
{
    int x, y, n = 0;
    for (y = y0; y <= y1; y++) for (x = x0; x <= x1; x++) n += fb[y][x] != 0;
    return n;
}

static void tick(void) { tick_no++; *(volatile u32 *)0x800740 += 1600000u; ramp_trampoline(); }

int main(int argc, char **argv)
{
    FILE *img;
    long n;
    int i, drawn = 0;
    volatile ramp_state_t *st;
    const char *out = argc > 2 ? argv[2] : ".";
    char path[512];

    if (argc < 2) { fprintf(stderr, "usage: display <carta2-PROD-111224.bin> [outdir]\n"); return 2; }
    map_fixed(0x10000, 0x30000);
    map_fixed(0x800000, 0x1000);
    map_fixed(0x840000, 0x10000);
    img = fopen(argv[1], "rb");
    if (!img) { perror(argv[1]); return 2; }
    fseek(img, 0x10000, SEEK_SET);
    n = (long)fread((void *)0x10000, 1, 0x30000, img);
    fclose(img);
    printf("Carta 2 screen test (%ld image bytes mapped from 0x10000)\n", n);
    memset(flash_store, 0xff, sizeof flash_store);

    /* concentrate session at the sentinel, F scale, 68 % battery */
    S(0x07) = 1; S(0x09) = 1; S(0x0b) = 0;
    S16(DEV_PRESET_OFF(TBL_CO_F, 0)) = 150; S16(DEV_PRESET_OFF(TBL_CO_C, 0)) = 65; S16(DEV_PRESET_OFF(TBL_CO_HOLD, 0)) = 30;
    S16(OFF_COUNTDOWN) = 30; S16(OFF_MEAS_F) = 77; S(OFF_REACHED) = 0; S(OFF_SCREEN) = 1;
    *(volatile u8 *)(0x84309c + 0x14) = 68;
    S(OFF_SESSION) = 1;

    st = RAMP_STATE;
    for (i = 0; i < 20000 && S(OFF_SESSION); i++) {
        tick();
        if (ramp_active(st)) {
            if (!drawn) { memset(fb, 0, sizeof fb); ramp_d3c0_full(); drawn = 1; }
            else ramp_d3c0_view();
            if (st->stage == 3 && drawn == 1) {
                snprintf(path, sizeof path, "%s/carta2-ramp-stage3.ppm", out);
                dump(path);
                drawn = 2;
                CHECK(lit_in(0, 0, 239, 60) > 200, "header (battery, time, goal, live temp) barely drawn");
                CHECK(lit_in(6, 70, 219, 196) > 300, "chart area barely drawn");
                CHECK(lit_in(226, 72, 233, 196) > 30, "heat meter not drawn");
            }
        }
    }
    CHECK(drawn == 2, "ramp never reached stage 3 on screen");
    CHECK(prims > 100, "only %d draw calls", prims);
    CHECK(oob == 0, "%d draws outside the 240x240 screen", oob);
    printf("  ramp screen: %d draw calls, %d out of bounds, ended after %d ticks\n", prims, oob, i);

    /* the picker box, on and off */
    memset(fb, 0, sizeof fb);
    oob = 0;
    ramp_picker_draw(2, 1);
    snprintf(path, sizeof path, "%s/carta2-picker.ppm", out);
    dump(path);
    CHECK(oob == 0, "picker draws outside the screen");
    CHECK(fb[98 + 14 + 3][40 + 40 + 2 * 18 + 5] == 0xffff, "selected marker (preset 3) not lit");
    CHECK(fb[98 + 14 + 3][40 + 40 + 0 * 18 + 5] == 0x18e3, "unselected marker not dim");
    CHECK(lit_in(40 + 14, 98 + 10, 40 + 24, 98 + 26) > 10, "preset number not drawn");
    ramp_picker_draw(2, 0);
    snprintf(path, sizeof path, "%s/carta2-picker-off.ppm", out);
    dump(path);
    CHECK(fb[98 + 14 + 3][40 + 40 + 2 * 18 + 5] == 0xfe40, "off state not shown in orange");

    printf(fails ? "\n%d FAILED\n" : "\nALL DISPLAY TESTS PASSED\n", fails);
    return fails != 0;
}
