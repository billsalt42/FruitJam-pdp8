/*
 * PDP-8/E emulator for the Adafruit Fruit Jam (RP2350B) - runs OS/8, DEC TSS/8.24
 * and UWM TSS/8.25.
 *
 *   core 0: PDP-8/E CPU + devices, USB host (keyboard) and device (serial
 *           ports), microSD disk images, system menu
 *   core 1: 640x480 DVI text display (80x30)
 *
 * OS/8 boots from RK05 cartridge images; TSS/8 loads its INIT tape and runs
 * from an RF08 fixed-head disk image.  Under TSS/8 the console (K00) and four
 * KL8E multiplexer lines (K01-K04) each get a virtual screen (Alt-F1..F5) and
 * a USB serial port.  All media are SIMH-format files on the microSD card.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <stdlib.h>
#include <stdarg.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "tusb.h"
#include "pio_usb.h"
#include "fatfs/ff.h"
#include "video.h"
#include "term.h"
#include "keyboard.h"
#include "panel.h"
#include "pidp.h"
#include "pico/rand.h"
#include "../net/sshsrv.h"
#include "../net/ssh.h"
#include "../net/wifi.h"
#include "../core/pdp8.h"

#define VERSION "1.8"

#define PIN_USB_HOST_DP   1
#define PIN_USB_HOST_5V   11
#define PIN_LED           29
#define PIN_BUTTON1       0

#define RK_IMAGE_BYTES (RK_SECTORS * 512u)     /* 3,325,952 */
#define NTERM 5                                /* console + 4 multiplexer lines */
#define NDT   4                                /* DECtape drives offered (TC08 has 8) */
#define NRX   2                                /* floppy drives */
#define DT_IMAGE_BYTES (PDP8_DT_BLOCKS * PDP8_DT_BLKWORDS * 2u)   /* 380,292 */
#define RX01_BYTES     256256u
#define RX02_BYTES     512512u
enum { BOOT_RK = 0, BOOT_DT = 1, BOOT_RX = 2 };
static const char *boot_dev_name[] = { "RK05 RK0", "DECtape DTA0", "floppy RXA0" };

enum { SYS_OS8 = 0, SYS_TSS8 = 1, SYS_UWM = 2, SYS_MULTOS = 3, SYS_ETOS = 4, NSYS = 5 };
static const char *sys_name[] = { "OS/8", "TSS/8", "UWM TSS/8", "MULTOS/8", "ETOS" };
#define IS_TSS(s) ((s) == SYS_TSS8 || (s) == SYS_UWM)
#define RKTS(s)   ((s) == SYS_MULTOS || (s) == SYS_ETOS) /* time-sharing from its own RK05 pack */
#define MULTI(s)  (IS_TSS(s) || RKTS(s))                /* uses the K01-K04 terminal lines */

/* ---------------- settings ---------------- */
typedef struct {
    char rk[RK_NUMDR][64];
    char tss8_rf[64], tss8_bin[64];
    char uwm_rf[64], uwm_bin[64];
    char multos_rk[64];
    int  multos_lines;
    char etos_rk[64];
    int  etos_lines;
    char dt[NDT][64];                   /* TC08 DECtape images, DTA0-DTA3 */
    char rx[NRX][64];                   /* RX01/RX02 floppy images, RXA0-RXA1 */
    int  os8_boot;                      /* OS/8 boots from: BOOT_RK, BOOT_DT, BOOT_RX */
    int  system;
    int  color;
    bool upper;
    bool real_speed;
    int  autoboot;
    char wifi_ssid[33], wifi_pass[65], ssh_pass[65], hostname[33];
    int  ssh_port;
} settings_t;

static settings_t cfg = {
    .rk = { "os8.rk05", "", "", "" },
    .tss8_rf = "tss8_rf.dsk", .tss8_bin = "tss8_init.bin",
    .uwm_rf = "uwm_rf.dsk", .uwm_bin = "uwm_init.bin",
    .multos_rk = "multos8.rk05", .multos_lines = 1,
    .etos_rk = "etosv5b-demo.rk05", .etos_lines = 1,
    .dt = { "os8.tu56", "blank.tu56", "", "" }, .rx = { "blank.rx01", "" }, .os8_boot = BOOT_RK,
    .system = SYS_OS8, .color = 0, .upper = true, .real_speed = false, .autoboot = 3,
    .hostname = "pdp8", .ssh_port = 22,
};

static const struct { const char *name; uint8_t fg, bold, bg; } colors[] = {
    { "green", 0x1c, 0x5f, 0x00 },
    { "amber", 0xf0, 0xfd, 0x00 },
    { "white", 0xb6, 0xff, 0x00 },
    { "blue",  0x5b, 0xbf, 0x01 },
};
#define NCOLORS (int)(sizeof colors / sizeof colors[0])

/* ---------------- terminals ---------------- */
static term_t terms[NTERM];         /* 0 = console, 1..4 = TSS/8 lines K01..K04 */
static term_t menu_term;
static term_t *ui = &terms[0];      /* where firmware messages go */
static int active;                  /* terminal shown on the monitor */
static int running = -1;            /* system currently running */

static void cdc_out(int itf, const char *s, size_t n)
{
    if (!tud_cdc_n_connected(itf)) return;
    if (tud_cdc_n_write_available(itf) < n) { tud_task(); tud_cdc_n_write_flush(itf); }
    if (tud_cdc_n_write_available(itf) >= n) tud_cdc_n_write(itf, s, (uint32_t)n);
}

static void line_out(int n, uint8_t c)          /* PDP-8 output to terminal n */
{
    sshsrv_line_out(n, c);
    term_putc(&terms[n], c);
    char ch = (char)c;
    cdc_out(n, &ch, 1);
}

static void ui_char(uint8_t c)
{
    term_putc(ui, c);
    char ch = (char)c;
    cdc_out(0, &ch, 1);
}

static void out_str(const char *s)
{
    while (*s) {
        if (*s == '\n') ui_char('\r');
        ui_char((uint8_t)*s++);
    }
    tud_cdc_n_write_flush(0);
}

static void out_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    out_str(buf);
}

/* keystroke from the keyboard or a serial port, for terminal n */
static void send_key(int n, int c)
{
    if (n == 0) pdp8_key((uint8_t)(c | 0200));                 /* KL8E console: KSR mark bit */
    else if (MULTI(running)) pdp8_ttx_key(n - 1, (uint8_t)(c & 0177));
}

static void answerback(term_t *t, const char *s)
{
    while (*s) send_key(t->id, (uint8_t)*s++);
}

static bool panel_on;
extern bool panel_halted;

static void show_terminal(int n)
{
    active = n;
    if (!panel_on) video_show(&terms[n]);
}

static void set_panel(bool on)
{
    panel_on = on;
    panel_show(on);
    video_show(on ? panel_screen() : &terms[active]);
}

/* ---------------- SD card / disk images ---------------- */
static FATFS fs;
static bool  sd_mounted;
static FIL   rk_fil[RK_NUMDR];
static bool  rk_open[RK_NUMDR], rk_ro[RK_NUMDR], rk_dirty[RK_NUMDR];
static DWORD rk_clmt[RK_NUMDR][64];
static FIL   rf_fil;
static bool  rf_open, rf_dirty;
static uint32_t rf_capacity;
static DWORD rf_clmt[64];
static uint32_t last_write_ms, last_rf_flush_ms, last_dt_flush_ms;
static FIL   lpt_fil;
static bool  lpt_open, lpt_dirty;
static FIL   ptr_fil, ptp_fil;          /* paper tape reader / punch */
static bool  ptr_open, ptp_open, ptp_dirty;

static void service(void);
static bool net_paused;                 /* deep in a disk operation: leave SSH for later (stack) */

static bool sd_mount(void)
{
    if (sd_mounted) return true;
    sd_mounted = (f_mount(&fs, "", 1) == FR_OK);
    return sd_mounted;
}

/* Pad an open file with zeros up to 'size' bytes (whole 512-byte sectors). */
static void pad_file(FIL *f, const char *name, FSIZE_t size)
{
    FSIZE_t sz = f_size(f);
    if (sz >= size) return;
    static uint8_t buf[512];
    out_printf("  Extending %s to %lu bytes", name, (unsigned long)size);
    f_lseek(f, sz & ~(FSIZE_t)511);
    memset(buf, 0, sizeof buf);
    if (sz & 511) {
        UINT br, bw;
        f_read(f, buf, sz & 511, &br);
        f_lseek(f, sz & ~(FSIZE_t)511);
        f_write(f, buf, 512, &bw);
        memset(buf, 0, sizeof buf);
    }
    uint32_t n = 0;
    while (f_tell(f) < size) {
        UINT bw;
        if (f_write(f, buf, 512, &bw) != FR_OK || bw != 512) break;
        if ((++n & 255) == 0) { ui_char('.'); net_paused = true; service(); net_paused = false; }
    }
    f_sync(f);
    out_str(" done\n");
}

static void fast_seek(FIL *f, DWORD *tbl, UINT n)
{
    tbl[0] = n;
    f->cltbl = tbl;
    if (f_lseek(f, CREATE_LINKMAP) != FR_OK) f->cltbl = NULL;
}

static void disk_close(int u)
{
    if (rk_open[u]) { f_close(&rk_fil[u]); rk_open[u] = false; }
}

static bool disk_open(int u, const char *name)
{
    disk_close(u);
    if (!name[0]) return false;
    FIL *f = &rk_fil[u];
    rk_ro[u] = false;
    if (f_open(f, name, FA_READ | FA_WRITE) != FR_OK) {
        if (f_open(f, name, FA_READ) != FR_OK) return false;
        rk_ro[u] = true;
    }
    if (!rk_ro[u]) pad_file(f, name, RK_IMAGE_BYTES);
    fast_seek(f, rk_clmt[u], sizeof rk_clmt[u] / sizeof(DWORD));
    rk_open[u] = true;
    rk_dirty[u] = false;
    return true;
}

bool host_disk_present(int u) { return u >= 0 && u < RK_NUMDR && rk_open[u]; }
bool host_disk_readonly(int u) { return rk_ro[u]; }

static bool file_read512(FIL *f, uint32_t sector, void *buf)
{
    UINT br = 0;
    if (f_lseek(f, (FSIZE_t)sector * 512u) != FR_OK) return false;
    if (f_read(f, buf, 512, &br) != FR_OK) return false;
    if (br < 512) memset((uint8_t *)buf + br, 0, 512 - br);
    return true;
}

static bool file_write512(FIL *f, uint32_t sector, const void *buf)
{
    UINT bw = 0;
    if (f_lseek(f, (FSIZE_t)sector * 512u) != FR_OK) return false;
    if (f_write(f, buf, 512, &bw) != FR_OK || bw != 512) return false;
    last_write_ms = to_ms_since_boot(get_absolute_time());
    gpio_put(PIN_LED, 1);
    return true;
}

bool host_disk_read(int u, uint32_t sector, uint16_t *w) { return file_read512(&rk_fil[u], sector, w); }

bool host_disk_write(int u, uint32_t sector, const uint16_t *w)
{
    if (!file_write512(&rk_fil[u], sector, w)) return false;
    rk_dirty[u] = true;
    return true;
}

/* ---- TC08 DECtapes and RX floppies: SIMH-format image files ---- */
static FIL   dt_fil[NDT], rx_fil[NRX];
static bool  dt_open[NDT], dt_ro[NDT], dt_dirty[NDT];
static bool  rx_open[NRX], rx_ro[NRX], rx_dirty[NRX];

static bool media_open(FIL *f, const char *name, bool *ro)
{
    *ro = false;
    if (!name[0]) return false;
    if (f_open(f, name, FA_READ | FA_WRITE) != FR_OK) {
        if (f_open(f, name, FA_READ) != FR_OK) return false;
        *ro = true;
    }
    return true;
}

static void rx_update_type(void)       /* RX28 controller if an RX02 floppy is mounted */
{
    bool rx02 = false;
    for (int u = 0; u < NRX; u++) if (rx_open[u] && f_size(&rx_fil[u]) > RX01_BYTES) rx02 = true;
    pdp8_rx_set_rx28(rx02);
}

static void dt_close(int u)
{
    if (dt_open[u]) { pdp8_dt_changed(u); f_close(&dt_fil[u]); dt_open[u] = false; }
}
static bool dt_attach(int u, const char *name)
{
    dt_close(u);
    if (!media_open(&dt_fil[u], name, &dt_ro[u])) return false;
    dt_open[u] = true;
    dt_dirty[u] = false;
    pdp8_dt_changed(u);
    return true;
}
static void rx_close(int u)
{
    if (rx_open[u]) { f_close(&rx_fil[u]); rx_open[u] = false; rx_update_type(); }
}
static bool rx_attach(int u, const char *name)
{
    rx_close(u);
    if (!media_open(&rx_fil[u], name, &rx_ro[u])) return false;
    rx_open[u] = true;
    rx_dirty[u] = false;
    rx_update_type();
    return true;
}

static bool file_rw(FIL *f, FSIZE_t off, void *buf, UINT n, bool wr)
{
    UINT done = 0;
    if (f_lseek(f, off) != FR_OK) return false;
    if (wr) {
        if (f_write(f, buf, n, &done) != FR_OK || done != n) return false;
        last_write_ms = to_ms_since_boot(get_absolute_time());
        gpio_put(PIN_LED, 1);
    } else {
        if (f_read(f, buf, n, &done) != FR_OK) return false;
        if (done < n) memset((uint8_t *)buf + done, 0, n - done);
    }
    return true;
}

bool host_dt_present(int u) { return u >= 0 && u < NDT && dt_open[u]; }
bool host_dt_readonly(int u) { return u >= 0 && u < NDT && dt_ro[u]; }
bool host_dt_read(int u, uint32_t blk, uint16_t *w)
{
    if (!host_dt_present(u)) return false;
    if (!file_rw(&dt_fil[u], (FSIZE_t)blk * PDP8_DT_BLKWORDS * 2, w, PDP8_DT_BLKWORDS * 2, false)) return false;
    for (int i = 0; i < PDP8_DT_BLKWORDS; i++) w[i] &= 07777;
    return true;
}
bool host_dt_write(int u, uint32_t blk, const uint16_t *w)
{
    if (!host_dt_present(u) || dt_ro[u]) return false;
    if (!file_rw(&dt_fil[u], (FSIZE_t)blk * PDP8_DT_BLKWORDS * 2, (void *)w, PDP8_DT_BLKWORDS * 2, true)) return false;
    dt_dirty[u] = true;
    return true;
}
bool host_rx_present(int u) { return u >= 0 && u < NRX && rx_open[u]; }
bool host_rx_readonly(int u) { return u >= 0 && u < NRX && rx_ro[u]; }
uint32_t host_rx_bytes(int u) { return host_rx_present(u) ? (uint32_t)f_size(&rx_fil[u]) : 0; }
bool host_rx_read(int u, uint32_t off, uint8_t *buf, uint32_t n)
{
    return host_rx_present(u) && file_rw(&rx_fil[u], off, buf, n, false);
}
bool host_rx_write(int u, uint32_t off, const uint8_t *buf, uint32_t n)
{
    if (!host_rx_present(u) || rx_ro[u]) return false;
    if (!file_rw(&rx_fil[u], off, (void *)buf, n, true)) return false;
    rx_dirty[u] = true;
    return true;
}
bool host_rx_format(int u, uint32_t bytes)      /* RX28 SET DENSITY: erase to the new size */
{
    if (!host_rx_present(u) || rx_ro[u]) return false;
    static uint8_t z[512];
    memset(z, 0, sizeof z);
    FIL *f = &rx_fil[u];
    if (f_lseek(f, 0) != FR_OK || f_truncate(f) != FR_OK) return false;
    for (uint32_t off = 0; off < bytes; off += sizeof z) {
        UINT bw, n = bytes - off < sizeof z ? (UINT)(bytes - off) : (UINT)sizeof z;
        if (f_write(f, z, n, &bw) != FR_OK || bw != n) return false;
    }
    f_sync(f);
    rx_update_type();
    return true;
}

/* Create a blank image of exactly 'bytes' (the sizes SIMH expects). */
static bool create_blank(const char *name, uint32_t bytes)
{
    FIL f;
    static uint8_t z[512];
    memset(z, 0, sizeof z);
    if (f_open(&f, name, FA_WRITE | FA_CREATE_NEW) != FR_OK) return false;
    bool ok = true;
    for (uint32_t off = 0; ok && off < bytes; off += sizeof z) {
        UINT bw, n = bytes - off < sizeof z ? (UINT)(bytes - off) : (UINT)sizeof z;
        ok = f_write(&f, z, n, &bw) == FR_OK && bw == n;
        if ((off & 0xffff) == 0) { net_paused = true; service(); net_paused = false; }
    }
    f_close(&f);
    return ok;
}

/* RF08 backing store for TSS/8 */
uint32_t host_rf_words(void) { return rf_open ? rf_capacity : 0; }
bool host_rf_read(uint32_t blk, uint16_t *w) { return rf_open && file_read512(&rf_fil, blk, w); }
bool host_rf_write(uint32_t blk, const uint16_t *w)
{
    if (!rf_open || !file_write512(&rf_fil, blk, w)) return false;
    rf_dirty = true;
    return true;
}

static void rf_close(void)
{
    if (rf_open) {
        pdp8_rf_flush();
        f_close(&rf_fil);
        rf_open = false;
    }
}

static bool rf_attach(const char *name)
{
    rf_close();
    if (f_open(&rf_fil, name, FA_READ | FA_WRITE) != FR_OK) return false;
    uint32_t words = (uint32_t)(f_size(&rf_fil) / 2);
    uint32_t plat = (words + 262143) / 262144;       /* 256K words per RS08 platter */
    if (plat < 1) plat = 1;
    if (plat > 4) plat = 4;
    rf_capacity = plat * 262144u;
    pad_file(&rf_fil, name, (FSIZE_t)rf_capacity * 2);
    fast_seek(&rf_fil, rf_clmt, sizeof rf_clmt / sizeof(DWORD));
    rf_open = true;
    rf_dirty = false;
    return true;
}

static void disks_sync(void)
{
    for (int u = 0; u < RK_NUMDR; u++)
        if (rk_open[u] && rk_dirty[u]) { f_sync(&rk_fil[u]); rk_dirty[u] = false; }
    if (rf_open && rf_dirty) { f_sync(&rf_fil); rf_dirty = false; }
    for (int u = 0; u < NDT; u++) if (dt_open[u] && dt_dirty[u]) { f_sync(&dt_fil[u]); dt_dirty[u] = false; }
    for (int u = 0; u < NRX; u++) if (rx_open[u] && rx_dirty[u]) { f_sync(&rx_fil[u]); rx_dirty[u] = false; }
    if (lpt_open && lpt_dirty) { f_sync(&lpt_fil); lpt_dirty = false; }
    if (ptp_open && ptp_dirty) { f_sync(&ptp_fil); ptp_dirty = false; }
    gpio_put(PIN_LED, 0);
}

static void flush_everything(void)
{
    pdp8_rf_flush();
    pdp8_dt_flush();
    disks_sync();
}

void host_tty_out(uint8_t c) { line_out(0, c & 0177); }
void host_ttx_out(int line, uint8_t c) { if (line >= 0 && line < NTERM - 1) line_out(line + 1, c & 0177); }

void host_lpt_out(uint8_t c)
{
    if (!sd_mounted) return;
    if (!lpt_open) {
        if (f_open(&lpt_fil, "printer.txt", FA_OPEN_APPEND | FA_WRITE) != FR_OK) return;
        lpt_open = true;
    }
    c &= 0177;
    if (c == 0 || c == 0177) return;
    UINT bw;
    f_write(&lpt_fil, &c, 1, &bw);
    lpt_dirty = true;
    last_write_ms = to_ms_since_boot(get_absolute_time());
}

/* PC8E paper tape: the reader reads READER.BIN from the card (from the start
 * again after each boot); the punch appends to PUNCH.BIN. */

static void ptr_rewind(void)
{
    if (ptr_open) { f_close(&ptr_fil); ptr_open = false; }
}

int host_ptr_getc(void)
{
    if (!sd_mounted) return -1;
    if (!ptr_open) {
        if (f_open(&ptr_fil, "reader.bin", FA_READ) != FR_OK) return -1;
        ptr_open = true;
    }
    uint8_t c;
    UINT br = 0;
    if (f_read(&ptr_fil, &c, 1, &br) != FR_OK || br != 1) return -1;
    return c;
}

void host_ptp_putc(uint8_t c)
{
    if (!sd_mounted) return;
    if (!ptp_open) {
        if (f_open(&ptp_fil, "punch.bin", FA_OPEN_APPEND | FA_WRITE) != FR_OK) return;
        ptp_open = true;
    }
    UINT bw;
    f_write(&ptp_fil, &c, 1, &bw);
    ptp_dirty = true;
    last_write_ms = to_ms_since_boot(get_absolute_time());
}

uint32_t host_millis(void) { return to_ms_since_boot(get_absolute_time()); }

/* ---------------- settings file ---------------- */
static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s;
    while (*b && isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

static void copy_str(char *dst, const char *src, size_t n)
{
    strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
}

static void load_settings(void)
{
    FIL f;
    if (f_open(&f, "pdp8.cfg", FA_READ) != FR_OK) return;
    char line[128];
    while (f_gets(line, sizeof line, &f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k); trim(v);
        for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (!strncmp(k, "rk", 2) && k[2] >= '0' && k[2] <= '3' && !k[3]) {
            copy_str(cfg.rk[k[2] - '0'], v, sizeof cfg.rk[0]);
        } else if (!strcmp(k, "system")) {
            cfg.system = (!strcasecmp(v, "tss8") || !strcasecmp(v, "tss/8")) ? SYS_TSS8 :
                         (!strcasecmp(v, "uwm") || !strcasecmp(v, "uwm-tss8")) ? SYS_UWM :
                         (!strcasecmp(v, "multos") || !strcasecmp(v, "multos8")) ? SYS_MULTOS :
                         !strcasecmp(v, "etos") ? SYS_ETOS : SYS_OS8;
        } else if (!strcmp(k, "tss8_rf")) {
            copy_str(cfg.tss8_rf, v, sizeof cfg.tss8_rf);
        } else if (!strcmp(k, "tss8_bin")) {
            copy_str(cfg.tss8_bin, v, sizeof cfg.tss8_bin);
        } else if (!strcmp(k, "uwm_rf")) {
            copy_str(cfg.uwm_rf, v, sizeof cfg.uwm_rf);
        } else if (!strcmp(k, "uwm_bin")) {
            copy_str(cfg.uwm_bin, v, sizeof cfg.uwm_bin);
        } else if (!strcmp(k, "multos_rk")) {
            copy_str(cfg.multos_rk, v, sizeof cfg.multos_rk);
        } else if (!strcmp(k, "multos_lines")) {
            cfg.multos_lines = atoi(v);
            if (cfg.multos_lines < 1) cfg.multos_lines = 1;
            if (cfg.multos_lines > NTERM - 1) cfg.multos_lines = NTERM - 1;
        } else if (!strncmp(k, "dt", 2) && k[2] >= '0' && k[2] < '0' + NDT && !k[3]) {
            copy_str(cfg.dt[k[2] - '0'], v, sizeof cfg.dt[0]);
        } else if (!strncmp(k, "rx", 2) && k[2] >= '0' && k[2] < '0' + NRX && !k[3]) {
            copy_str(cfg.rx[k[2] - '0'], v, sizeof cfg.rx[0]);
        } else if (!strcmp(k, "os8_boot")) {
            cfg.os8_boot = (!strcasecmp(v, "dectape") || !strcasecmp(v, "dt") || !strcasecmp(v, "dta0")) ? BOOT_DT :
                           (!strcasecmp(v, "floppy") || !strcasecmp(v, "rx") || !strcasecmp(v, "rxa0")) ? BOOT_RX : BOOT_RK;
        } else if (!strcmp(k, "etos_rk")) {
            copy_str(cfg.etos_rk, v, sizeof cfg.etos_rk);
        } else if (!strcmp(k, "etos_lines")) {
            cfg.etos_lines = atoi(v);
            if (cfg.etos_lines < 1) cfg.etos_lines = 1;
            if (cfg.etos_lines > NTERM - 1) cfg.etos_lines = NTERM - 1;
        } else if (!strcmp(k, "color") || !strcmp(k, "colour")) {
            for (int i = 0; i < NCOLORS; i++) if (!strcasecmp(v, colors[i].name)) cfg.color = i;
        } else if (!strcmp(k, "uppercase")) {
            cfg.upper = !(strcasecmp(v, "off") == 0 || strcasecmp(v, "no") == 0 || !strcmp(v, "0"));
        } else if (!strcmp(k, "speed")) {
            cfg.real_speed = (strcasecmp(v, "real") == 0 || strcasecmp(v, "slow") == 0);
        } else if (!strcmp(k, "autoboot")) {
            cfg.autoboot = atoi(v);
        } else if (!strcmp(k, "wifi_ssid")) {
            copy_str(cfg.wifi_ssid, v, sizeof cfg.wifi_ssid);
        } else if (!strcmp(k, "wifi_password")) {
            copy_str(cfg.wifi_pass, v, sizeof cfg.wifi_pass);
        } else if (!strcmp(k, "ssh_password")) {
            copy_str(cfg.ssh_pass, v, sizeof cfg.ssh_pass);
        } else if (!strcmp(k, "ssh_port")) {
            cfg.ssh_port = atoi(v);
        } else if (!strcmp(k, "hostname")) {
            copy_str(cfg.hostname, v, sizeof cfg.hostname);
        }
    }
    f_close(&f);
}

static bool save_settings(void)
{
    FIL f;
    if (f_open(&f, "pdp8.cfg", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    f_printf(&f, "# PDP-8/E emulator settings (Adafruit Fruit Jam)\n");
    f_printf(&f, "system=%s\n", cfg.system == SYS_TSS8 ? "tss8" : cfg.system == SYS_UWM ? "uwm" :
                                 cfg.system == SYS_MULTOS ? "multos" : cfg.system == SYS_ETOS ? "etos" : "os8");
    for (int u = 0; u < RK_NUMDR; u++) f_printf(&f, "rk%d=%s\n", u, cfg.rk[u]);
    for (int u = 0; u < NDT; u++) f_printf(&f, "dt%d=%s\n", u, cfg.dt[u]);
    for (int u = 0; u < NRX; u++) f_printf(&f, "rx%d=%s\n", u, cfg.rx[u]);
    f_printf(&f, "os8_boot=%s\n", cfg.os8_boot == BOOT_DT ? "dectape" : cfg.os8_boot == BOOT_RX ? "floppy" : "rk");
    f_printf(&f, "tss8_rf=%s\n", cfg.tss8_rf);
    f_printf(&f, "tss8_bin=%s\n", cfg.tss8_bin);
    f_printf(&f, "uwm_rf=%s\n", cfg.uwm_rf);
    f_printf(&f, "uwm_bin=%s\n", cfg.uwm_bin);
    f_printf(&f, "multos_rk=%s\n", cfg.multos_rk);
    f_printf(&f, "multos_lines=%d\n", cfg.multos_lines);
    f_printf(&f, "etos_rk=%s\n", cfg.etos_rk);
    f_printf(&f, "etos_lines=%d\n", cfg.etos_lines);
    f_printf(&f, "color=%s\n", colors[cfg.color].name);
    f_printf(&f, "uppercase=%s\n", cfg.upper ? "on" : "off");
    f_printf(&f, "speed=%s\n", cfg.real_speed ? "real" : "full");
    f_printf(&f, "autoboot=%d\n", cfg.autoboot);
    f_printf(&f, "# WiFi + SSH: ssh to the address shown in the menu; user k01-k04 or k00 picks a line\n");
    f_printf(&f, "wifi_ssid=%s\n", cfg.wifi_ssid);
    f_printf(&f, "wifi_password=%s\n", cfg.wifi_pass);
    f_printf(&f, "ssh_password=%s\n", cfg.ssh_pass);
    f_printf(&f, "ssh_port=%d\n", cfg.ssh_port);
    f_printf(&f, "hostname=%s\n", cfg.hostname);
    f_close(&f);
    return true;
}

/* ---------------- USB ---------------- */
static void usb_init(void)
{
    gpio_init(PIN_USB_HOST_5V);
    gpio_set_dir(PIN_USB_HOST_5V, GPIO_OUT);
    gpio_put(PIN_USB_HOST_5V, 1);

    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = PIN_USB_HOST_DP;
    pio_cfg.tx_ch = 2;                  /* DMA 0/1 belong to the video */
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(BOARD_TUH_RHPORT);
    tud_init(BOARD_TUD_RHPORT);
}

/* ---------------- PiDP-8/I panel interface (USB CDC device on the host port) ---------------- */
static int pidp_idx = -1;
static uint32_t pidp_rx_bytes;

/* every USB device on the host port, for the diagnostics line in the menu */
static struct { uint16_t vid, pid; bool used; } usb_dev[CFG_TUH_DEVICE_MAX + CFG_TUH_HUB + 1];

void tuh_mount_cb(uint8_t daddr)
{
    if (daddr < sizeof usb_dev / sizeof usb_dev[0]) {
        uint16_t vid = 0, pid = 0;
        tuh_vid_pid_get(daddr, &vid, &pid);
        usb_dev[daddr].vid = vid;
        usb_dev[daddr].pid = pid;
        usb_dev[daddr].used = true;
    }
    kbd_usb_event(daddr, true);
}

void tuh_umount_cb(uint8_t daddr)
{
    if (daddr < sizeof usb_dev / sizeof usb_dev[0]) usb_dev[daddr].used = false;
}

void tuh_cdc_mount_cb(uint8_t idx)
{
    kbd_usb_event(0, false);
    if (pidp_idx < 0) {
        pidp_idx = idx;
        pidp_rx_bytes = 0;
        pidp_link(true);
    }
}

void tuh_cdc_umount_cb(uint8_t idx)
{
    if ((int)idx == pidp_idx) { pidp_idx = -1; pidp_link(false); }
}

static void pidp_poll(void)
{
    if (pidp_idx < 0) return;
    uint8_t buf[64];
    uint32_t n;
    while ((n = tuh_cdc_read((uint8_t)pidp_idx, buf, sizeof buf)) > 0) {
        pidp_rx_bytes += n;
        pidp_rx(buf, n);
    }
}

static void show_usb(void)
{
    out_str("USB host:");
    int n = 0;
    for (unsigned a = 0; a < sizeof usb_dev / sizeof usb_dev[0]; a++) {
        if (!usb_dev[a].used) continue;
        out_printf(" %04x:%04x", usb_dev[a].vid, usb_dev[a].pid);
        n++;
    }
    if (!n) out_str(" no devices");
    out_printf("   keyboard: %s\n", kbd_connected() ? "yes" : "no");
    if (pidp_idx < 0)
        out_str("PiDP-8/I panel: not detected (no USB serial device on the host port)\n");
    else if (!pidp_present())
        out_printf("PiDP-8/I panel: USB serial device found, no reply yet (%lu bytes received)\n",
                   (unsigned long)pidp_rx_bytes);
    else
        out_printf("PiDP-8/I panel: connected   switches %04o %04o %04o\n",
                   (unsigned)pidp_switches(0), (unsigned)pidp_switches(1), (unsigned)pidp_switches(2));
}

static void pidp_send(const uint8_t *msg, int n)
{
    if (pidp_idx < 0 || n <= 0) return;
    if (tuh_cdc_write_available((uint8_t)pidp_idx) < (uint32_t)n) return;   /* drop a frame */
    tuh_cdc_write((uint8_t)pidp_idx, msg, (uint32_t)n);
    tuh_cdc_write_flush((uint8_t)pidp_idx);
}

/* Lamp samples feed both the on-screen panel and the PiDP-8/I, ~50 times a second. */
static void lamps_service(bool panel_visible)
{
    static uint32_t last_ms;
    static bool sampling;
    bool want = panel_visible || pidp_present();
    if (want != sampling) {
        sampling = want;
        pdp8_lamps_enable(want);
        pdp8_lamps_t discard;
        pdp8_lamps_take(&discard);
    }
    uint32_t now = to_ms_since_boot(get_absolute_time());
    pdp8_lamps_t l, *lp = NULL;
    if (sampling && now - last_ms >= 20) {
        last_ms = now;
        pdp8_lamps_take(&l);
        lp = &l;
    }
    panel_update(now, lp);
    uint8_t msg[PIDP_MSG_MAX];
    pidp_send(msg, pidp_update(now, lp, msg));
}

/* ---------------- SSH over WiFi (ESP32-C6 coprocessor) ---------------- */
static bool ssh_on;
static char ssh_note[80];
static char ssh_fp[64];

void ssh_random(uint8_t *buf, size_t n)
{
    while (n) {
        uint64_t r = get_rand_64();
        for (int i = 0; i < 8 && n; i++, n--) { *buf++ = (uint8_t)r; r >>= 8; }
    }
}
uint32_t sshp_millis(void) { return to_ms_since_boot(get_absolute_time()); }
int sshp_system(void)
{
    if (running < 0) return SSHP_SYS_NONE;
    return IS_TSS(running) ? SSHP_SYS_TSS : running == SYS_MULTOS ? SSHP_SYS_MULTOS :
           running == SYS_ETOS ? SSHP_SYS_ETOS : SSHP_SYS_OS8;
}
int sshp_rk_lines(void) { return running == SYS_ETOS ? cfg.etos_lines : cfg.multos_lines; }
bool sshp_line_free(int line) { return line == 0 || !tud_cdc_n_connected((uint8_t)line); }
int sshp_line_room(int line)
{
    int b = line == 0 ? pdp8_key_backlog() : MULTI(running) ? pdp8_ttx_backlog(line - 1) : 99;
    return b < 32 ? 32 - b : 0;
}
void sshp_line_key(int line, uint8_t c)
{
    c &= 0177;
    if (c == 010) c = 0177;                           /* backspace -> RUBOUT */
    if (cfg.upper && c >= 'a' && c <= 'z') c -= 32;
    if (line == 0 && pdp8.halted && !panel_halted) { pdp8.halted = false; return; }
    send_key(line, c);
}

/* host key: 32 random bytes kept in SSHKEY.BIN on the card */
static bool ssh_hostkey(uint8_t seed[32])
{
    FIL f;
    UINT n = 0;
    if (f_open(&f, "sshkey.bin", FA_READ) == FR_OK) {
        f_read(&f, seed, 32, &n);
        f_close(&f);
        if (n == 32) return true;
    }
    ssh_random(seed, 32);
    if (f_open(&f, "sshkey.bin", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    f_write(&f, seed, 32, &n);
    f_close(&f);
    return n == 32;
}

static void net_start(void)
{
    ssh_on = false;
    if (!cfg.wifi_ssid[0]) { snprintf(ssh_note, sizeof ssh_note, "off (set wifi_ssid in PDP8.CFG)"); return; }
    if (!cfg.ssh_pass[0]) { snprintf(ssh_note, sizeof ssh_note, "off (set ssh_password in PDP8.CFG)"); return; }
    uint8_t seed[32];
    if (!ssh_hostkey(seed)) { snprintf(ssh_note, sizeof ssh_note, "off (could not save SSHKEY.BIN)"); return; }
    sshsrv_init(seed, cfg.ssh_pass);
    memset(seed, 0, sizeof seed);
    ssh_hostkey_fingerprint(ssh_fp);
    wifi_start(cfg.wifi_ssid, cfg.wifi_pass, cfg.hostname, (uint16_t)cfg.ssh_port);
    ssh_on = true;
}

static void net_service(void)
{
    static uint32_t last;
    if (!ssh_on || net_paused) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    wifi_poll(now);
    if (wifi_up() && now - last >= 10) {
        last = now;
        sshsrv_poll();
    }
}

static void show_network(void)
{
    if (!ssh_on) { out_printf("SSH: %s\n", ssh_note); return; }
    out_printf("SSH: %s", wifi_status());
    int n = sshsrv_sessions();
    if (n) out_printf("   (%d session%s)", n, n == 1 ? "" : "s");
    out_printf("\n     host key %s\n", ssh_fp);
}

/* Poll USB, keyboard repeat, network and lazy disk flush. */
static void service(void)
{
    tuh_task();
    pidp_poll();
    tud_task();
    net_service();
    kbd_task();
    for (int i = 0; i < CFG_TUD_CDC; i++) tud_cdc_n_write_flush(i);
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (rf_open && (uint32_t)(now - last_rf_flush_ms) > 1000) {
        last_rf_flush_ms = now;
        pdp8_rf_flush();                /* write back the RF08 block cache */
    }
    if ((uint32_t)(now - last_dt_flush_ms) > 1000) {
        last_dt_flush_ms = now;
        pdp8_dt_flush();                /* and the DECtape block being worked on */
    }
    if ((uint32_t)(now - last_write_ms) > 500) disks_sync();
}

/* Read one character from serial port n (-1 if none), with the usual fix-ups. */
static int cdc_get(int n)
{
    if (!tud_cdc_n_available(n)) return -1;
    int c = tud_cdc_n_read_char(n);
    if (n == 0 && c == 034) return KEY_MENU;        /* Ctrl-\ opens the menu from serial */
    if (c == '\n') return -2;                       /* terminals sending CR LF */
    if (c == 010) c = 0177;                         /* backspace -> RUBOUT */
    if (cfg.upper && c >= 'a' && c <= 'z') c -= 32;
    return c;
}

/* Input for the firmware's own screens: keyboard or serial port 0. */
static int input_get(void)
{
    int k = kbd_get();
    if (k >= 0) return k;
    k = cdc_get(0);
    return k == -2 ? -1 : k;
}

static int wait_key(void)
{
    int k;
    while ((k = input_get()) < 0) service();
    return k;
}

static bool file_exists(const char *name)
{
    FILINFO fi;
    return name[0] && f_stat(name, &fi) == FR_OK;
}

/* ---------------- image picker: RK05 disks, DECtapes, floppies ---------------- */
#define MAX_FILES 40
static char files[MAX_FILES][64];
static int nfiles;

enum { MED_RK, MED_DT, MED_RX };
static const char *med_drive[] = { "RK", "DTA", "RXA" };
static const int   med_units[] = { RK_NUMDR, NDT, NRX };

static bool ext_is(const char *dot, int kind)
{
    static const char *const ext[3][5] = {
        { ".rk05", ".rk", NULL },
        { ".tu56", ".dt8", ".dta", ".dt", NULL },
        { ".rx01", ".rx02", ".rx1", ".rx2", ".rx" },
    };
    for (int i = 0; i < 5 && ext[kind][i]; i++) if (!strcasecmp(dot, ext[kind][i])) return true;
    return false;
}

static void scan_images_kind(int kind)
{
    DIR d;
    FILINFO fi;
    nfiles = 0;
    if (f_opendir(&d, "/") != FR_OK) return;
    while (nfiles < MAX_FILES && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & (AM_DIR | AM_HID | AM_SYS)) continue;
        const char *dot = strrchr(fi.fname, '.');
        if (dot && ext_is(dot, kind)) {
            if (strlen(fi.fname) >= sizeof files[0]) continue;
            memcpy(files[nfiles], fi.fname, strlen(fi.fname) + 1);
            nfiles++;
        }
    }
    f_closedir(&d);
    for (int i = 0; i < nfiles; i++)
        for (int j = i + 1; j < nfiles; j++)
            if (strcasecmp(files[i], files[j]) > 0) {
                char t[64];
                strcpy(t, files[i]); strcpy(files[i], files[j]); strcpy(files[j], t);
            }
}
static void scan_images(void) { scan_images_kind(MED_RK); }

static char *med_name(int kind, int u) { return kind == MED_RK ? cfg.rk[u] : kind == MED_DT ? cfg.dt[u] : cfg.rx[u]; }
static bool med_open(int kind, int u) { return kind == MED_RK ? rk_open[u] : kind == MED_DT ? dt_open[u] : rx_open[u]; }
static bool med_ro(int kind, int u) { return kind == MED_RK ? rk_ro[u] : kind == MED_DT ? dt_ro[u] : rx_ro[u]; }
static void med_close(int kind, int u)
{
    if (kind == MED_RK) disk_close(u); else if (kind == MED_DT) dt_close(u); else rx_close(u);
}
static bool med_attach(int kind, int u, const char *name)
{
    return kind == MED_RK ? disk_open(u, name) : kind == MED_DT ? dt_attach(u, name) : rx_attach(u, name);
}

static int read_number(int maxdigits)
{
    char buf[4]; int n = 0;
    for (;;) {
        int k = wait_key();
        if (k == '\r') break;
        if (k == 27) return -1;
        if (k >= 'a' && k <= 'z') k -= 32;
        if (k == 'N' && n == 0) { ui_char('N'); return -2; }
        if (k == 0177 && n > 0) { n--; out_str("\b \b"); continue; }
        if (k >= '0' && k <= '9' && n < maxdigits) { buf[n++] = (char)k; ui_char((uint8_t)k); }
    }
    if (n == 0) return -1;
    buf[n] = 0;
    return atoi(buf);
}

static bool pick_media(int kind, int unit)
{
    scan_images_kind(kind);
    out_printf("\nSelect image for %s%d:\n", med_drive[kind], unit);
    out_str("   0  (none - unload drive)\n");
    for (int i = 0; i < nfiles; i++)
        out_printf("  %2d  %s%s\n", i + 1, files[i],
                   !strcasecmp(files[i], med_name(kind, unit)) ? "   <- current" : "");
    if (kind != MED_RK) out_str("   N  new blank image\n");
    out_str("Number (Enter = cancel): ");
    int sel = read_number(2);
    out_str("\n");
    if (sel == -1) return false;
    char newname[16];
    const char *name;
    if (sel == -2) {
        if (kind == MED_RK) return false;
        for (int i = 1; i < 100; i++) {          /* blank01.tu56, blank02.tu56 ... */
            snprintf(newname, sizeof newname, "blank%02d.%s", i, kind == MED_DT ? "tu56" : "rx01");
            if (!file_exists(newname)) break;
        }
        out_printf("Creating %s ...", newname);
        if (!create_blank(newname, kind == MED_DT ? DT_IMAGE_BYTES : RX01_BYTES)) {
            out_str(" failed\n");
            sleep_ms(1200);
            return false;
        }
        out_str(" done\n");
        name = newname;
    } else if (sel == 0) {
        med_name(kind, unit)[0] = 0;
        med_close(kind, unit);
        return true;
    } else {
        if (sel < 1 || sel > nfiles) return false;
        name = files[sel - 1];
    }
    for (int u = 0; u < med_units[kind]; u++)
        if (u != unit && !strcasecmp(med_name(kind, u), name)) {
            out_printf("That image is already mounted on %s%d.\n", med_drive[kind], u);
            sleep_ms(1200);
            return false;
        }
    copy_str(med_name(kind, unit), name, 64);
    if (!med_attach(kind, unit, name)) {
        out_printf("Cannot open %s\n", name);
        sleep_ms(1200);
    }
    return true;
}

static bool pick_image(int unit) { return pick_media(MED_RK, unit); }

static void mount_all(void)
{
    for (int u = 0; u < RK_NUMDR; u++) {
        if (!cfg.rk[u][0]) { disk_close(u); continue; }
        if (!disk_open(u, cfg.rk[u])) {
            if (u == 0) {
                scan_images();
                if (nfiles > 0 && disk_open(0, files[0])) {
                    strcpy(cfg.rk[0], files[0]);
                    continue;
                }
            }
            out_printf("  RK%d: cannot open %s\n", u, cfg.rk[u]);
        }
    }
    for (int u = 0; u < NDT; u++)
        if (cfg.dt[u][0] && !dt_attach(u, cfg.dt[u])) out_printf("  DTA%d: cannot open %s\n", u, cfg.dt[u]);
    for (int u = 0; u < NRX; u++)
        if (cfg.rx[u][0] && !rx_attach(u, cfg.rx[u])) out_printf("  RXA%d: cannot open %s\n", u, cfg.rx[u]);
}


static void show_kind(int kind, bool full)
{
    for (int u = 0; u < med_units[kind]; u++) {
        bool o = med_open(kind, u);
        if (full)
            out_printf("  %d  %s%d: %s%s\n", kind == MED_RK ? u : kind == MED_DT ? 4 + u : 8 + u,
                       med_drive[kind], u, o ? med_name(kind, u) : "(empty)", o && med_ro(kind, u) ? "  [read-only]" : "");
        else
            out_printf("%s%s", u ? ", " : "", o ? med_name(kind, u) : "-");
    }
}

static void show_mounts(void)
{
    out_str("  RK0-3: ");   show_kind(MED_RK, false);
    out_str("\n  DTA0-3: "); show_kind(MED_DT, false);
    out_str("   RXA0-1: "); show_kind(MED_RX, false);
    out_printf("\n  Packs found: TSS/8 %s, UWM %s, MULTOS/8 %s, ETOS %s\n",
               (file_exists(cfg.tss8_bin) && file_exists(cfg.tss8_rf)) ? "yes" : "no",
               (file_exists(cfg.uwm_bin) && file_exists(cfg.uwm_rf)) ? "yes" : "no",
               file_exists(cfg.multos_rk) ? "yes" : "no", file_exists(cfg.etos_rk) ? "yes" : "no");
}

/* Drives screen: change any image, or boot OS/8 from DECtape or floppy.
 * Returns BOOT_DT / BOOT_RX to boot, or -1. */
static int drives_menu(void)
{
    for (;;) {
        term_clear(ui);
        if (tud_cdc_n_connected(0)) cdc_out(0, "\033[2J\033[H", 7);
        term_set_attr(ui, ATTR_REVERSE);
        out_str(" Drives                                                                         \n");
        term_set_attr(ui, 0);
        out_str("\nRK05 cartridge disks (RK8E):\n");          show_kind(MED_RK, true);
        out_str("\nDECtapes (TC08 / TU56, SIMH .tu56 images):\n"); show_kind(MED_DT, true);
        out_str("\nFloppies (RX8E, RX01 .rx01 / RX02 .rx02 images):\n"); show_kind(MED_RX, true);
        out_printf("\n  B  Boot OS/8 from DECtape DTA0\n");
        out_printf("  F  Boot OS/8 from floppy RXA0\n");
        out_printf("  O  Boot OS/8 from RK0\n");
        out_printf("\n  OS/8 boots from: %s (saved with W in the menu)\n", boot_dev_name[cfg.os8_boot]);
        out_str("\nDrive number to change, or B/F/O, Esc = back: ");
        int k = wait_key();
        if (k >= 'a' && k <= 'z') k -= 32;
        if (k == 27 || k == KEY_MENU || k == '\r') return -1;
        if (k >= '0' && k <= '3') { if (sd_mounted) pick_media(MED_RK, k - '0'); continue; }
        if (k >= '4' && k <= '7') { if (sd_mounted) pick_media(MED_DT, k - '4'); continue; }
        if (k >= '8' && k <= '9') { if (sd_mounted) pick_media(MED_RX, k - '8'); continue; }
        if (k == 'B' || k == 'F' || k == 'O') {
            int dev = k == 'B' ? BOOT_DT : k == 'F' ? BOOT_RX : BOOT_RK;
            bool there = dev == BOOT_DT ? dt_open[0] : dev == BOOT_RX ? rx_open[0] : rk_open[0];
            if (!there) {
                out_printf("\n%s is empty - choose an image first.\n", dev == BOOT_DT ? "DTA0" : dev == BOOT_RX ? "RXA0" : "RK0");
                sleep_ms(1200);
                continue;
            }
            return dev;
        }
    }
}

/* ---------------- booting ---------------- */
static void apply_colors(void)
{
    video_set_colors(colors[cfg.color].fg, colors[cfg.color].bold, colors[cfg.color].bg);
}

static void reset_terminals(void)
{
    for (int i = 0; i < NTERM; i++) {
        term_init(&terms[i], i);
        terms[i].answerback = answerback;
    }
}

static bool rk0_alt;                    /* RK0 holds the MULTOS/8 or ETOS pack, not cfg.rk[0] */

static bool boot_os8(void)
{
    if (rk0_alt) {                      /* put the configured OS/8 pack back */
        disk_close(0);
        rk0_alt = false;
        if (cfg.rk[0][0]) disk_open(0, cfg.rk[0]);
    }
    int dev = cfg.os8_boot;
    if (dev == BOOT_DT && !dt_open[0]) dev = BOOT_RK;      /* fall back to the disk */
    if (dev == BOOT_RX && !rx_open[0]) dev = BOOT_RK;
    if (dev == BOOT_RK && !rk_open[0]) {
        if (dt_open[0]) dev = BOOT_DT; else if (rx_open[0]) dev = BOOT_RX; else return false;
    }
    ptr_rewind();
    rf_close();
    reset_terminals();
    show_terminal(0);
    running = SYS_OS8;
    panel_set_system(sys_name[running]);
    if (dev == BOOT_DT) pdp8_boot_dt();
    else if (dev == BOOT_RX) pdp8_boot_rx(0);
    else pdp8_boot_rk(0);
    return true;
}

static bool boot_tss8(int sys)
{
    const char *bin = sys == SYS_UWM ? cfg.uwm_bin : cfg.tss8_bin;
    const char *rfn = sys == SYS_UWM ? cfg.uwm_rf : cfg.tss8_rf;
    ui = &terms[0];
    FIL f;
    if (f_open(&f, bin, FA_READ) != FR_OK) {
        out_printf("\nCannot open %s\n", bin);
        return false;
    }
    UINT n = 0;
    FSIZE_t sz = f_size(&f);
    uint8_t *tape = malloc(sz ? (size_t)sz : 1);
    if (!tape) { f_close(&f); return false; }
    f_read(&f, tape, (UINT)sz, &n);
    f_close(&f);
    if (!rf_attach(rfn)) {
        free(tape);
        out_printf("\nCannot open %s\n", rfn);
        return false;
    }
    pdp8_reset();                       /* also drops any stale RF08 cache */
    int w = pdp8_load_bin(tape, n);
    free(tape);
    if (w < 0) {
        out_printf("\n%s is not a valid BIN tape (%d)\n", bin, w);
        rf_close();
        return false;
    }
    reset_terminals();
    for (int i = 1; i < NTERM; i++) {
        term_set_attr(&terms[i], ATTR_REVERSE);
        char hdr[96];
        if (sys == SYS_UWM)
            snprintf(hdr, sizeof hdr,
                     " UWM TSS/8 terminal K%02d  (Alt-F%d)  - press Enter, then Ctrl-B LOGIN 2 LXHE ", i, i + 1);
        else
            snprintf(hdr, sizeof hdr,
                     " TSS/8 terminal K%02d   (Alt-F%d)   - press Enter, then LOGIN 2 LXHE      ", i, i + 1);
        term_puts(&terms[i], hdr);
        term_set_attr(&terms[i], 0);
        term_puts(&terms[i], "\r\n\n");
    }
    show_terminal(0);
    running = sys;
    panel_set_system(sys_name[running]);
    ptr_rewind();
    pdp8_start(024200);
    return true;
}

/* MULTOS/8 and ETOS: OS/8 from the system's own RK05 pack.  MULTOS/8: the
 * console starts the time-sharing with DATE and R MULTOS, then every terminal
 * logs on with Ctrl-H.  ETOS: R ETOS, option T, then LOGIN on each terminal. */
static const char *rkts_pack(int sys) { return sys == SYS_ETOS ? cfg.etos_rk : cfg.multos_rk; }

static bool boot_rkts(int sys)
{
    const char *pack = rkts_pack(sys);
    int lines = sys == SYS_ETOS ? cfg.etos_lines : cfg.multos_lines;
    ui = &terms[0];
    if (!file_exists(pack)) {
        out_printf("\n%s needs %s on the card.\n", sys_name[sys], pack);
        return false;
    }
    disk_close(0);
    if (!disk_open(0, pack)) {
        out_printf("\nCannot open %s\n", pack);
        rk0_alt = false;
        if (cfg.rk[0][0]) disk_open(0, cfg.rk[0]);
        return false;
    }
    rk0_alt = true;
    ptr_rewind();
    rf_close();
    reset_terminals();
    for (int i = 1; i < NTERM; i++) {
        term_set_attr(&terms[i], ATTR_REVERSE);
        char hdr[96];
        if (i > lines)
            snprintf(hdr, sizeof hdr, " K%02d: not a %s terminal (%s_lines=%d in PDP8.CFG) ", i, sys_name[sys],
                     sys == SYS_ETOS ? "etos" : "multos", lines);
        else if (sys == SYS_ETOS)
            snprintf(hdr, sizeof hdr, " ETOS terminal K%02d  (Alt-F%d)  - Enter, LOGIN; Ctrl-V S Enter, LOGOUT ", i, i + 1);
        else
            snprintf(hdr, sizeof hdr, " MULTOS/8 terminal K%02d  (Alt-F%d)  - Ctrl-H logs on, R LOGOFF signs off", i, i + 1);
        term_puts(&terms[i], hdr);
        term_set_attr(&terms[i], 0);
        term_puts(&terms[i], "\r\n\n");
    }
    term_set_attr(&terms[0], ATTR_REVERSE);
    term_puts(&terms[0], sys == SYS_ETOS ?
              " ETOS: type R ETOS, option T, then press Enter and LOGIN (demo: 0,4 USER1). " :
              " MULTOS/8: type DATE dd-MMM-yy, then R MULTOS and the time (hhmm). Ctrl-H logs on. ");
    term_set_attr(&terms[0], 0);
    term_puts(&terms[0], "\r\n");
    show_terminal(0);
    running = sys;
    panel_set_system(sys_name[running]);
    pdp8_boot_rk(0);
    return true;
}

static bool boot_system(int sys)
{
    pdp8_set_tsc(sys == SYS_ETOS);      /* only ETOS has the TSC8-75 board */
    return IS_TSS(sys) ? boot_tss8(sys) : RKTS(sys) ? boot_rkts(sys) : boot_os8();
}

/* ---------------- system menu ---------------- */
#define MENU_CONTINUE (-1)          /* otherwise the menu returns the system to boot */

static int system_menu(void)
{
    flush_everything();
    term_init(&menu_term, -1);
    term_t *saved_ui = ui;
    ui = &menu_term;
    video_show(&menu_term);
    int result = MENU_CONTINUE;
    for (;;) {
        term_clear(&menu_term);
        if (tud_cdc_n_connected(0)) cdc_out(0, "\033[2J\033[H", 7);
        term_set_attr(ui, ATTR_REVERSE);
        out_str(" PDP-8/E for Adafruit Fruit Jam  v" VERSION "  -  System Menu                            \n");
        term_set_attr(ui, 0);
        out_printf("\n%s: %s at %05o   AC=%04o L=%o MQ=%04o   %llu instructions\n\n",
                   running >= 0 ? sys_name[running] : "CPU",
                   pdp8.halted ? "HALTED" : "running",
                   (unsigned)(pdp8.halted ? pdp8.halt_pc : (pdp8.ifld | pdp8.pc)),
                   (unsigned)(pdp8.lac & 07777), (unsigned)((pdp8.lac >> 12) & 1), (unsigned)pdp8.mq,
                   (unsigned long long)pdp8.icount);
        show_mounts();
        show_usb();
        show_network();
        out_str("\n");
        out_str("  C  Continue\n");
        out_printf("  O  Boot OS/8 (from %s)\n", boot_dev_name[cfg.os8_boot]);
        out_str("  T  Boot TSS/8 (DEC TSS/8.24, reset)\n");
        out_str("  U  Boot UWM TSS/8 (TSS/8.25 from the University of Wisconsin-Milwaukee)\n");
        out_printf("  M  Boot MULTOS/8 (time-sharing OS/8, %s)\n", cfg.multos_rk);
        out_printf("  E  Boot ETOS (virtual-machine time-sharing, %s)\n", cfg.etos_rk);
        if (running == SYS_OS8) out_str("  R  Restart OS/8 monitor (jump to 07600)\n");
        out_str("  I  Drives: change disks, DECtapes, floppies; boot OS/8 from tape or floppy\n");
        out_printf("  D  Default system at power-up: %s\n", sys_name[cfg.system]);
        out_printf("  K  Keyboard upper-case: %s\n", cfg.upper ? "ON" : "off");
        out_printf("  S  Speed: %s\n", cfg.real_speed ? "real PDP-8/E (~400K instr/s)" : "full speed");
        out_printf("  P  Phosphor colour: %s\n", colors[cfg.color].name);
        out_str("  W  Write these settings to PDP8.CFG\n");
        out_str("\nChoice: ");
        int k = wait_key();
        if (k >= 'a' && k <= 'z') k -= 32;
        if ((k == 'C' || k == 27 || k == KEY_MENU || k == '\r') && running >= 0) break;
        if (k == 'O' || k == 'B') {
            if (!rk_open[0] && !dt_open[0] && !rx_open[0]) {
                out_str("\nNo OS/8 drive has an image - choose one first (I).\n"); sleep_ms(1200); continue;
            }
            result = SYS_OS8; break;
        }
        if (k == 'I') {
            int dev = drives_menu();
            if (dev >= 0) { cfg.os8_boot = dev; result = SYS_OS8; break; }
            continue;
        }
        if (k == 'M' || k == 'E') {
            int sys = k == 'M' ? SYS_MULTOS : SYS_ETOS;
            if (!file_exists(rkts_pack(sys))) {
                out_printf("\n%s needs %s on the card.\n", sys_name[sys], rkts_pack(sys));
                sleep_ms(1500);
                continue;
            }
            result = sys; break;
        }
        if (k == 'T' || k == 'U') {
            int sys = k == 'T' ? SYS_TSS8 : SYS_UWM;
            const char *bin = sys == SYS_UWM ? cfg.uwm_bin : cfg.tss8_bin;
            const char *rfn = sys == SYS_UWM ? cfg.uwm_rf : cfg.tss8_rf;
            if (!file_exists(bin) || !file_exists(rfn)) {
                out_printf("\n%s needs %s and %s on the card.\n", sys_name[sys], bin, rfn);
                sleep_ms(1500);
                continue;
            }
            result = sys; break;
        }
        if (k == 'R' && running == SYS_OS8) {
            pdp8.ifld = pdp8.ib = pdp8.df = 0;
            pdp8.pc = 07600;
            pdp8.ion = false;
            pdp8.cif_pending = false;
            pdp8.uf = pdp8.ub = 0;
            pdp8.halted = false;
            break;
        }
        if (k >= '0' && k <= '3') { if (sd_mounted) pick_image(k - '0'); continue; }
        if (k == 'D') { cfg.system = (cfg.system + 1) % NSYS; continue; }
        if (k == 'K') { cfg.upper = !cfg.upper; kbd_set_caps(cfg.upper); continue; }
        if (k == 'S') { cfg.real_speed = !cfg.real_speed; continue; }
        if (k == 'P') { cfg.color = (cfg.color + 1) % NCOLORS; apply_colors(); continue; }
        if (k == 'W') {
            out_str(save_settings() ? "\nSaved.\n" : "\nCould not write PDP8.CFG\n");
            sleep_ms(800);
            continue;
        }
    }
    ui = saved_ui;
    show_terminal(active);
    if (tud_cdc_n_connected(0)) cdc_out(0, "\r\n", 2);
    return result;
}

/* ---------------- boot screen ---------------- */
static void banner(void)
{
    term_clear(ui);
    term_set_attr(ui, ATTR_BOLD);
    out_str("PDP-8/E emulator for the Adafruit Fruit Jam  v" VERSION "\n");
    term_set_attr(ui, 0);
    out_str("32K, EAE, KL8E + 4-line mux, RK8E, RF08, TC08, RX8E, DK8-E, LE8, PC8E, TSC8-75\n\n");
}

/* returns the system to boot, or -1 for the menu */
static int boot_countdown(void)
{
    int sys = cfg.system;
    if (cfg.autoboot < 0) {
        out_printf("\nEnter = boot %s,  O = OS/8,  T = TSS/8,  U = UWM TSS/8,  M = MULTOS/8,  E = ETOS,  F12 (Ctrl-\\ on serial) = menu\n",
                   sys_name[sys]);
    } else {
        out_printf("\nBooting %s in %d s.  O = OS/8,  T = TSS/8,  U = UWM,  M = MULTOS/8,  E = ETOS,  F12 (Ctrl-\\ on serial) = menu",
                   sys_name[sys], cfg.autoboot);
    }
    absolute_time_t t = make_timeout_time_ms(cfg.autoboot < 0 ? 0x7fffffff : cfg.autoboot * 1000);
    while (!time_reached(t)) {
        service();
        int k = input_get();
        if (k == KEY_MENU) return -1;
        if (k == '\r' || k == ' ') break;
        if (k == 'O' || k == 'o') { sys = SYS_OS8; break; }
        if (k == 'T' || k == 't') { sys = SYS_TSS8; break; }
        if (k == 'U' || k == 'u') { sys = SYS_UWM; break; }
        if (k == 'M' || k == 'm') { sys = SYS_MULTOS; break; }
        if (k == 'E' || k == 'e') { sys = SYS_ETOS; break; }
    }
    out_str("\n\n");
    return sys;
}

/* ---------------- main ---------------- */
int main(void)
{
    video_clock_init();

    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_init(PIN_BUTTON1);
    gpio_set_dir(PIN_BUTTON1, GPIO_IN);
    gpio_pull_up(PIN_BUTTON1);

    reset_terminals();
    panel_init();
    show_terminal(0);
    video_start_core1();
    apply_colors();
    usb_init();
    banner();

    out_str("Mounting microSD card... ");
    while (!sd_mount()) {
        out_str("\nNo readable microSD card. Insert a FAT32/exFAT card holding OS8.RK05; retrying...");
        for (int i = 0; i < 200; i++) { service(); sleep_ms(10); }
        out_str("\n");
    }
    out_str("ok\n");
    load_settings();
    net_start();
    kbd_set_caps(cfg.upper);
    apply_colors();
    mount_all();
    show_mounts();

    int want = boot_countdown();
    for (;;) {
        if (want >= 0 && boot_system(want)) break;
        if (want >= 0) sleep_ms(1500);
        int m = system_menu();
        want = m;
    }

    absolute_time_t t0 = get_absolute_time();
    uint64_t base_icount = pdp8.icount;
    bool button_was = true;
    bool halt_announced = false;

    for (;;) {
        int menu = -1;

        /* keyboard -> front panel, or the terminal on screen */
        int k;
        while ((k = kbd_get()) >= 0) {
            if (k == KEY_MENU) { menu = 1; break; }
            if (k == KEY_BREAK) continue;
            if (k == KEY_PANEL) { set_panel(!panel_on); continue; }
            if (panel_on && k >= KEY_VT0 && k < KEY_VT0 + NTERM) {   /* Alt-Fn leaves the panel */
                set_panel(false);
                if (MULTI(running)) show_terminal(k - KEY_VT0);
                continue;
            }
            if (panel_on) {
                if (k == 27) {                       /* VT52 cursor keys arrive as ESC + letter */
                    int n = kbd_get();
                    if (n == 'A') k = PANEL_KEY_UP;
                    else if (n == 'B') k = PANEL_KEY_DOWN;
                    else if (n >= 0) k = n;
                }
                if (!panel_key(k)) set_panel(false);
                continue;
            }
            if (k >= KEY_VT0 && k < KEY_VT0 + NTERM) {
                if (MULTI(running)) show_terminal(k - KEY_VT0);
                continue;
            }
            if (pdp8.halted && !panel_halted) {      /* any key continues after a HLT */
                pdp8.halted = false;
                continue;
            }
            send_key(active, k);
        }
        /* serial port n -> terminal n */
        for (int n = 0; n < NTERM && menu < 0; n++) {
            if (n > 0 && !MULTI(running)) {              /* drain unused ports */
                while (tud_cdc_n_available(n)) tud_cdc_n_read_char(n);
                continue;
            }
            int backlog = n == 0 ? pdp8_key_backlog() : pdp8_ttx_backlog(n - 1);
            while (backlog++ < 32) {
                int c = cdc_get(n);
                if (c == -1) break;
                if (c == -2) continue;
                if (c == KEY_MENU) { menu = 1; break; }
                if (n == 0 && pdp8.halted && !panel_halted) { pdp8.halted = false; continue; }
                send_key(n, c);
            }
        }
        /* BUTTON1 also opens the menu */
        bool b = gpio_get(PIN_BUTTON1);
        if (!b && button_was) menu = 1;
        button_was = b;

        if (menu >= 0) {
            bool was_panel = panel_on;
            set_panel(false);
            int m = system_menu();
            while (m != MENU_CONTINUE && !boot_system(m)) {
                sleep_ms(1500);
                m = system_menu();
            }
            if (m == MENU_CONTINUE && was_panel) set_panel(true);
            halt_announced = false;
            t0 = get_absolute_time();
            base_icount = pdp8.icount;
            continue;
        }

        /* run the CPU for ~1 ms worth of work */
        if (pdp8.halted) {
            if (!halt_announced && !panel_halted) {
                flush_everything();
                if (!panel_on) show_terminal(0);
                ui = &terms[0];
                out_printf("\r\n[HALT at %05o - F11 front panel, F12 menu, any other key continues]\r\n",
                           (unsigned)pdp8.halt_pc);
            }
            halt_announced = true;
            t0 = get_absolute_time();
            base_icount = pdp8.icount;
        } else {
            halt_announced = false;
            if (cfg.real_speed) {
                uint64_t us = absolute_time_diff_us(t0, get_absolute_time());
                uint64_t target = base_icount + us * 2 / 5;   /* 400K instructions/s */
                if (pdp8.icount < target) {
                    uint64_t n = target - pdp8.icount;
                    if (n > 4000) n = 4000;
                    pdp8_run((uint32_t)n);
                }
                if (pdp8.icount + 100000 < target) {
                    t0 = get_absolute_time();
                    base_icount = pdp8.icount;
                }
            } else {
                pdp8_run(20000);
            }
        }
        lamps_service(panel_on);
        service();
    }
}
