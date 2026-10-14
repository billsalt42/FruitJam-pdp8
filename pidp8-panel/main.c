/*
 * PiDP-8/I panel interface - Raspberry Pi Pico 2 (or Pico) firmware.
 *
 * Takes the place of the Raspberry Pi behind a PiDP-8/I front panel: it
 * multiplexes the 8x12 lamp matrix with 32 brightness levels, scans the 3x12
 * switch matrix, and talks to the PDP-8 emulator over USB (one CDC serial
 * port, protocol in pidp_proto.h).
 *
 *   core 0: USB, protocol, self-test pattern
 *   core 1: lamp multiplexing and switch scanning (about 105 frames/s)
 *
 * Wiring (Pico GPIO -> PiDP-8/I 40-pin header, Raspberry Pi BCM numbering):
 *   GP0-GP11   matrix columns 0-11   BCM 13,12,11,10,9,8,7,6,5,4,15,14
 *              (boards with the serial mod use BCM 3,2 for columns 10,11)
 *   GP12-GP19  lamp rows 0-7         BCM 20-27
 *   GP20-GP22  switch rows 0-2       BCM 16,17,18
 * On the adapter board (hardware/pico-adapter) GP28 is grounded and the
 * lines use the GPIOs listed in board_map.h instead, chosen for its layout.
 *
 * Electrically this does what the pidp8i software does on the Pi: a lamp
 * row is enabled by driving its row line high (UDN2981 source driver); a
 * lamp is lit by pulling its column low.  Switches are read with the
 * columns as pulled-up inputs and one switch row driven low at a time.
 *
 * With no program talking to it (USB port not opened, or on a plain USB
 * power supply) it runs a self-test: the PC, MA and MB rows mirror the three
 * switch rows, the AC row shows a brightness ramp and the rest chase.
 */
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "tusb.h"
#include "pidp_proto.h"

#define FW_MAJOR 1
#define FW_MINOR 2

/* Pin maps.  Hand wiring (README table): GP0-11 columns, GP12-19 lamp rows,
 * GP20-22 switch rows.  The adapter board (hardware/pico-adapter) uses the
 * GPIOs that suited its layout and ties BOARD_STRAP_GP to ground. */
#include "board_map.h"
static const uint8_t wired_cols[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t wired_lrows[8] = { 12, 13, 14, 15, 16, 17, 18, 19 };
static const uint8_t wired_srows[3] = { 20, 21, 22 };
static const uint8_t *col_gp = wired_cols, *lrow_gp = wired_lrows, *srow_gp = wired_srows;
static uint32_t col_mask;                         /* all column GPIOs */
static uint32_t col_bit[PIDP_COLS];               /* GPIO bit of each column */
static bool on_board;

static inline uint32_t cols_to_gpio(uint32_t c)  /* 12-bit column set -> GPIO bits */
{
    uint32_t g = 0;
    for (int i = 0; i < PIDP_COLS; i++) if (c & (1u << i)) g |= col_bit[i];
    return g;
}

static void choose_map(void)
{
    gpio_init(BOARD_STRAP_GP);
    gpio_set_dir(BOARD_STRAP_GP, GPIO_IN);
    gpio_pull_up(BOARD_STRAP_GP);
    busy_wait_us_32(100);
    on_board = !gpio_get(BOARD_STRAP_GP);
    if (on_board) { col_gp = board_cols; lrow_gp = board_lrows; srow_gp = board_srows; }
    else gpio_disable_pulls(BOARD_STRAP_GP);
    col_mask = 0;
    for (int i = 0; i < PIDP_COLS; i++) { col_bit[i] = 1u << col_gp[i]; col_mask |= col_bit[i]; }
}

#define SLICE_US   36            /* 32 slices -> 1.15 ms per lamp row */
#define GAP_US     10            /* all off between rows (driver ghosting) */
#define SETTLE_US  15            /* switch row settle time */
#define DEBOUNCE   3             /* scans a change must persist (~30 ms) */

/* ------------------------------------------------------------ shared state */
static uint8_t fb[2][PIDP_ROWS][PIDP_COLS];      /* lamp brightness, double buffered */
static volatile int fb_front;
static volatile uint16_t sw_stable[PIDP_SWROWS];  /* debounced, 1 = closed */
static volatile uint32_t sw_seq;                  /* bumps on every change */

/* ------------------------------------------------------------ core 1 */
static void matrix_init(void)
{
    for (int i = 0; i < PIDP_COLS; i++) {
        uint p = col_gp[i];
        gpio_init(p);
        gpio_put(p, 1);                          /* high = lamp off */
        gpio_set_dir(p, GPIO_OUT);
        gpio_pull_up(p);
        gpio_set_drive_strength(p, GPIO_DRIVE_STRENGTH_12MA);
    }
    for (int r = 0; r < PIDP_ROWS; r++) {
        uint p = lrow_gp[r];
        gpio_init(p);
        gpio_put(p, 0);
        gpio_set_dir(p, GPIO_OUT);
        gpio_set_drive_strength(p, GPIO_DRIVE_STRENGTH_12MA);
    }
    for (int r = 0; r < PIDP_SWROWS; r++) {
        uint p = srow_gp[r];
        gpio_init(p);
        gpio_put(p, 0);
        gpio_set_dir(p, GPIO_IN);                /* high-Z until scanned */
        gpio_pull_up(p);
    }
}

static void scan_switches(void)
{
    static uint16_t cand[PIDP_SWROWS];
    static uint8_t count[PIDP_SWROWS];

    gpio_set_dir_in_masked(col_mask);            /* columns: pulled-up inputs */
    for (int r = 0; r < PIDP_SWROWS; r++) {
        uint p = srow_gp[r];
        gpio_set_dir(p, GPIO_OUT);               /* drive this row low */
        busy_wait_us_32(SETTLE_US);
        uint32_t all = gpio_get_all();
        uint16_t closed = 0;
        for (int c = 0; c < PIDP_COLS; c++) if (!(all & col_bit[c])) closed |= (uint16_t)(1u << c);
        gpio_set_dir(p, GPIO_IN);

        if (closed != cand[r]) { cand[r] = closed; count[r] = 0; }
        else if (count[r] < DEBOUNCE && ++count[r] == DEBOUNCE && sw_stable[r] != closed) {
            sw_stable[r] = closed;
            sw_seq++;
        }
    }
    gpio_put_masked(col_mask, col_mask);
    gpio_set_dir_out_masked(col_mask);
}

static void core1_main(void)
{
    matrix_init();
    for (;;) {
        for (int r = 0; r < PIDP_ROWS; r++) {
            uint8_t lv[PIDP_COLS];
            memcpy(lv, fb[fb_front][r], sizeof lv);
            uint32_t t = time_us_32();
            uint32_t low = 0;                        /* columns pulled low = lit */
            for (int c = 0; c < PIDP_COLS; c++) if (lv[c]) low |= 1u << c;
            gpio_put_masked(col_mask, cols_to_gpio(~low & 0xfffu));
            gpio_put(lrow_gp[r], 1);
            for (int s = 1; s < PIDP_LEVELS; s++) {  /* lamps with level <= s go dark */
                t += SLICE_US;
                uint32_t off = 0;
                for (int c = 0; c < PIDP_COLS; c++) if (lv[c] == s) off |= 1u << c;
                while ((int32_t)(time_us_32() - t) < 0) tight_loop_contents();
                if (off) {
                    low &= ~off;
                    gpio_put_masked(col_mask, cols_to_gpio(~low & 0xfffu));
                }
            }
            t += SLICE_US;
            while ((int32_t)(time_us_32() - t) < 0) tight_loop_contents();
            gpio_put(lrow_gp[r], 0);
            gpio_put_masked(col_mask, col_mask);
            busy_wait_us_32(GAP_US);
        }
        scan_switches();
    }
}

/* ------------------------------------------------------------ core 0 */
static void show(const uint8_t *levels)          /* 96 levels -> back buffer -> front */
{
    int back = !fb_front;
    memcpy(fb[back], levels, PIDP_LAMPS_LEN);
    __dmb();
    fb_front = back;
}

static void self_test(uint32_t now)
{
    static const uint8_t tail[] = { 31, 16, 8, 4, 2, 1 };
    uint8_t l[PIDP_ROWS][PIDP_COLS];
    memset(l, 0, sizeof l);
    for (int r = 0; r < PIDP_SWROWS; r++)
        for (int c = 0; c < PIDP_COLS; c++) l[r][c] = (sw_stable[r] >> c) & 1 ? 31 : 0;
    for (int c = 0; c < PIDP_COLS; c++) l[3][c] = (uint8_t)(c * 31 / 11);   /* brightness ramp */
    int pos = (int)((now / 60) % (4 * PIDP_COLS));                        /* chase rows 4-7 */
    for (int k = 0; k < (int)sizeof tail; k++) {
        int p = pos - k;
        if (p < 0) p += 4 * PIDP_COLS;
        l[4 + p / PIDP_COLS][PIDP_COLS - 1 - p % PIDP_COLS] = tail[k];
    }
    show(&l[0][0]);
}

static void send(const uint8_t *m, uint32_t n)
{
    if (tud_cdc_write_available() < n) return;
    tud_cdc_write(m, n);
    tud_cdc_write_flush();
}

static void send_hello(void)
{
    const uint8_t m[] = { PIDP_HELLO, 'P', '8', PIDP_PROTO_VERSION, FW_MAJOR, FW_MINOR };
    send(m, sizeof m);
}

static void send_switches(void)
{
    uint8_t m[1 + PIDP_SWITCH_LEN] = { PIDP_SWITCH };
    for (int r = 0; r < PIDP_SWROWS; r++) {
        uint16_t v = sw_stable[r];
        m[1 + 2 * r] = v & 077;
        m[2 + 2 * r] = (v >> 6) & 077;
    }
    send(m, sizeof m);
}

int main(void)
{
    memset(fb, 0, sizeof fb);
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif
    choose_map();
    multicore_launch_core1(core1_main);
    tud_init(BOARD_TUD_RHPORT);

    uint8_t rx_type = 0, rx_buf[PIDP_LAMPS_LEN];
    uint32_t rx_len = 0;
    uint32_t last_frame = 0, last_hello = 0, last_sw = 0, last_test = 0, seq_sent = 0;
    bool was_connected = false, was_mounted = false, frames_seen = false;

    for (;;) {
        tud_task();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        bool connected = tud_cdc_connected();

        /* messages from the emulator */
        uint8_t buf[64];
        uint32_t n;
        while (tud_cdc_available() && (n = tud_cdc_read(buf, sizeof buf)) > 0) {
            for (uint32_t i = 0; i < n; i++) {
                uint8_t c = buf[i];
                if (c & 0x80) { rx_type = (c == PIDP_LAMPS) ? c : 0; rx_len = 0; continue; }
                if (!rx_type) continue;
                rx_buf[rx_len++] = c;
                if (rx_len == PIDP_LAMPS_LEN) {
                    for (int k = 0; k < PIDP_LAMPS_LEN; k++)
                        if (rx_buf[k] >= PIDP_LEVELS) rx_buf[k] = PIDP_LEVELS - 1;
                    show(rx_buf);
                    last_frame = now;
                    frames_seen = true;
                    rx_type = 0;
                }
            }
        }

        /* Talk whenever the host has configured us.  DTR is not required: some
         * hosts never raise it, so lamp frames arriving also count as a link. */
        bool mounted = tud_mounted();
        bool frames = frames_seen && now - last_frame < 1500;
        bool active = connected || frames;
        if (mounted && ((connected && !was_connected) || !was_mounted)) {
            send_hello();
            send_switches();
            last_hello = last_sw = now;
            seq_sent = sw_seq;
        }
        was_connected = connected;
        was_mounted = mounted;

        if (mounted) {
            if (!frames && now - last_hello >= 1000) { send_hello(); last_hello = now; }
            if (sw_seq != seq_sent || now - last_sw >= 100) {
                seq_sent = sw_seq;
                last_sw = now;
                send_switches();
            }
        }
        if (!active && now - last_test >= 20) {
            last_test = now;
            self_test(now);
        }
#ifdef PICO_DEFAULT_LED_PIN
        /* on-board LED: steady = lamp frames arriving; slow blink = USB configured
         * by the host but no frames; short flash once a second = no USB host */
        bool led = frames ? true : mounted ? ((now / 500) & 1) : (now % 1000) < 60;
        gpio_put(PICO_DEFAULT_LED_PIN, led);
#endif
    }
}
