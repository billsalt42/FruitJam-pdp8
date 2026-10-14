/*
 * pidp.c - PiDP-8/I hardware front panel support.
 *
 * Lamps follow the PiDP-8/I layout (a PDP-8/I panel) and use the same
 * averaged sampling as the on-screen panel, sent as 32 brightness levels.
 * Switches behave as in the pidp8i software:
 *   SR          the switch register (applied whenever a switch is flipped)
 *   LOAD ADD    PC = SR, IF = IF switches, DF = DF switches      (halted)
 *   START       clear CPU and devices, AC = L = 0, run from PC    (halted)
 *   EXAM / DEP  read / write memory at IF:PC, then PC + 1          (halted)
 *   CONT        continue; with SING STEP or SING INST up, do one instruction
 *   STOP        halt; closing SING STEP or SING INST also halts
 */
#include <string.h>
#include "pidp.h"

extern bool panel_halted;             /* panel.c: halted from a front panel */

static bool link_up, hello_seen, sw_known;
static uint32_t sw[PIDP_SWROWS];      /* debounced switches from the panel, 1 = closed */
static uint32_t sw_prev[PIDP_SWROWS];
static uint16_t bright[PIDP_ROWS][PIDP_COLS];   /* smoothed brightness, 8.8 fixed point */

/* receive state */
static uint8_t rx_type, rx_len, rx_buf[16];

void pidp_link(bool up)
{
    link_up = up;
    hello_seen = sw_known = false;
    rx_type = 0;
}

bool pidp_present(void) { return link_up && hello_seen; }
uint32_t pidp_switches(int row) { return row >= 0 && row < PIDP_SWROWS ? sw[row] : 0; }

static void rx_message(void)
{
    if (rx_type == PIDP_HELLO) {
        if (rx_buf[0] == 'P' && rx_buf[1] == '8') hello_seen = true;
    } else if (rx_type == PIDP_SWITCH) {
        for (int r = 0; r < PIDP_SWROWS; r++)
            sw[r] = (rx_buf[2 * r] & 077u) | ((rx_buf[2 * r + 1] & 077u) << 6);
        if (!sw_known) memcpy(sw_prev, sw, sizeof sw);   /* first report: no edges */
        sw_known = true;
        hello_seen = true;
    }
}

void pidp_rx(const uint8_t *buf, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = buf[i];
        if (c & 0x80) {
            rx_type = c;
            rx_len = 0;
            if (c != PIDP_HELLO && c != PIDP_SWITCH) rx_type = 0;
            continue;
        }
        if (!rx_type) continue;
        rx_buf[rx_len++] = c;
        int want = rx_type == PIDP_HELLO ? PIDP_HELLO_LEN : PIDP_SWITCH_LEN;
        if (rx_len == want) { rx_message(); rx_type = 0; }
    }
}

/* ------------------------------------------------------------ switches */
static void halt_here(void)
{
    pdp8.halted = true;
    pdp8.halt_pc = pdp8.ifld | pdp8.pc;
    panel_halted = true;
}

static void switch_actions(void)
{
    uint32_t now2 = sw[2], was2 = sw_prev[2];
    uint32_t pressed = now2 & ~was2;
    bool halted = pdp8.halted;
    bool single = (now2 & (PIDP_SW_SSTEP | PIDP_SW_SINST)) != 0;

    if (sw[0] != sw_prev[0]) pdp8.sr = sw[0] & 07777;

    if (!halted) {
        if ((now2 & PIDP_SW_STOP) || (pressed & (PIDP_SW_SSTEP | PIDP_SW_SINST))) halt_here();
    } else if (pressed & PIDP_SW_LOAD) {
        pdp8.pc = sw[0] & 07777;
        pdp8.ifld = pdp8.ib = ((sw[1] >> 6) & 7) << 12;
        pdp8.df = ((sw[1] >> 9) & 7) << 12;
        pdp8.ma = pdp8.ifld | pdp8.pc;
    } else if (pressed & PIDP_SW_START) {
        pdp8_caf();
        pdp8.lac = 0;
        pdp8.md = 0;
        pdp8.ib = pdp8.ifld;
        pdp8.uf = pdp8.ub = 0;
        if (!single) { pdp8.halted = false; panel_halted = false; }
    } else if (pressed & (PIDP_SW_EXAM | PIDP_SW_DEP)) {
        uint32_t a = pdp8.ifld | (pdp8.pc & 07777);
        if (pressed & PIDP_SW_DEP) pdp8_mem[a] = (uint16_t)(sw[0] & 07777);
        pdp8.ma = a;
        pdp8.md = pdp8_mem[a];
        pdp8.pc = (pdp8.pc + 1) & 07777;
    } else if (pressed & PIDP_SW_CONT) {
        if (single) {
            pdp8.halted = false;
            pdp8_run(1);
            halt_here();
        } else {
            pdp8.halted = false;
            panel_halted = false;
        }
    }
    memcpy(sw_prev, sw, sizeof sw);
}

/* ------------------------------------------------------------ lamps */
/* target brightness 0..31 for each lamp, from sampled counts or live registers */
static void targets(const pdp8_lamps_t *l, uint8_t t[PIDP_ROWS][PIDP_COLS])
{
    memset(t, 0, PIDP_ROWS * PIDP_COLS);
    bool running = !pdp8.halted;
    if (running && l && l->samples) {
        uint32_t n = l->samples;
#define LV(c) (uint8_t)(((c) * 31u + n / 2) / n)
        for (int j = 0; j < 12; j++) {
            t[0][j] = LV(l->pc[11 - j]);
            t[1][j] = LV(l->ma[14 - j]);
            t[2][j] = LV(l->md[11 - j]);
            t[3][j] = LV(l->ac[12 - j]);
            t[4][j] = LV(l->mq[11 - j]);
        }
        for (int op = 0; op < 8; op++) t[5][11 - op] = LV(l->op[op]);
        t[5][3] = LV(l->fetch);
        t[5][2] = LV(l->exec);
        t[5][1] = LV(l->defer);
        t[6][7] = 31;                                   /* RUN */
        t[6][9] = LV(l->ion);
        for (int k = 0; k < 5; k++) t[6][2 + k] = LV(l->sc[4 - k]);
        t[7][5] = LV(l->ac[0]);                         /* LINK */
        for (int k = 0; k < 3; k++) {
            t[7][6 + k] = LV(l->ifl[2 - k]);
            t[7][9 + k] = LV(l->dfl[2 - k]);
        }
#undef LV
        return;
    }
    /* halted (or no samples yet): show the registers as they stand */
    uint32_t rows[5] = { pdp8.pc & 07777, pdp8.ma & 07777, pdp8.md & 07777,
                         pdp8.lac & 07777, pdp8.mq & 07777 };
    for (int r = 0; r < 5; r++)
        for (int j = 0; j < 12; j++) t[r][j] = ((rows[r] >> j) & 1) ? 31 : 0;
    t[5][11 - ((pdp8.ir >> 9) & 7)] = 31;
    t[5][3] = 31;                                       /* next cycle: FETCH */
    t[6][7] = running ? 31 : 0;
    t[6][9] = pdp8.ion ? 31 : 0;
    for (int k = 0; k < 5; k++) t[6][2 + k] = ((pdp8.sc >> k) & 1) ? 31 : 0;
    t[7][5] = (pdp8.lac & 010000) ? 31 : 0;
    for (int k = 0; k < 3; k++) {
        t[7][6 + k] = ((pdp8.ifld >> (12 + k)) & 1) ? 31 : 0;
        t[7][9 + k] = ((pdp8.df >> (12 + k)) & 1) ? 31 : 0;
    }
}

int pidp_update(uint32_t now_ms, const pdp8_lamps_t *l, uint8_t *out)
{
    (void)now_ms;
    if (!pidp_present()) return 0;
    if (sw_known) switch_actions();
    if (!l) return 0;

    uint8_t t[PIDP_ROWS][PIDP_COLS];
    targets(l, t);
    out[0] = PIDP_LAMPS;
    for (int r = 0; r < PIDP_ROWS; r++)
        for (int c = 0; c < PIDP_COLS; c++) {
            /* a little persistence, like incandescent bulbs */
            int b = bright[r][c], goal = t[r][c] << 8;
            b += (goal - b) / 2;
            if (goal - b < 256 && goal - b > -256) b = goal;
            bright[r][c] = (uint16_t)b;
            out[1 + r * PIDP_COLS + c] = (uint8_t)((b + 128) >> 8);
        }
    return 1 + PIDP_LAMPS_LEN;
}
