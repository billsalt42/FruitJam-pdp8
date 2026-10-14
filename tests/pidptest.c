/* pidptest.c - exercise the PiDP-8/I panel logic (firmware/pidp.c) against the
 * emulator core: toggle in and run a program from the switches, and check the
 * lamp frames.  Build: make pidptest */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../core/pdp8.h"
#include "../firmware/pidp.h"

bool panel_halted;
bool     host_disk_present(int u) { (void)u; return false; }
bool     host_disk_readonly(int u) { (void)u; return false; }
bool     host_disk_read(int u, uint32_t s, uint16_t *w) { (void)u; (void)s; (void)w; return false; }
bool     host_disk_write(int u, uint32_t s, const uint16_t *w) { (void)u; (void)s; (void)w; return false; }
void     host_tty_out(uint8_t c) { (void)c; }
void     host_lpt_out(uint8_t c) { (void)c; }
uint32_t host_millis(void) { return 0; }
void     host_ttx_out(int l, uint8_t c) { (void)l; (void)c; }
uint32_t host_rf_words(void) { return 0; }
bool     host_rf_read(uint32_t b, uint16_t *w) { (void)b; (void)w; return false; }
bool     host_rf_write(uint32_t b, const uint16_t *w) { (void)b; (void)w; return false; }
int      host_ptr_getc(void) { return -1; }
bool     host_dt_present(int u) { (void)u; return false; }
bool     host_dt_readonly(int u) { (void)u; return false; }
bool     host_dt_read(int u, uint32_t b, uint16_t *w) { (void)u; (void)b; (void)w; return false; }
bool     host_dt_write(int u, uint32_t b, const uint16_t *w) { (void)u; (void)b; (void)w; return false; }
bool     host_rx_present(int u) { (void)u; return false; }
bool     host_rx_readonly(int u) { (void)u; return false; }
uint32_t host_rx_bytes(int u) { (void)u; return 0; }
bool     host_rx_read(int u, uint32_t o, uint8_t *b, uint32_t n) { (void)u; (void)o; (void)b; (void)n; return false; }
bool     host_rx_write(int u, uint32_t o, const uint8_t *b, uint32_t n) { (void)u; (void)o; (void)b; (void)n; return false; }
bool     host_rx_format(int u, uint32_t n) { (void)u; (void)n; return false; }
void     host_ptp_putc(uint8_t c) { (void)c; }

static int fails;
#define CHECK(c, msg) do { if (c) printf("ok   %s\n", msg); else { printf("FAIL %s\n", msg); fails++; } } while (0)

static uint32_t sw[3];
static uint8_t frame[PIDP_MSG_MAX];
static int frame_len;

static void send_switches(void)
{
    uint8_t m[7] = { PIDP_SWITCH };
    for (int r = 0; r < 3; r++) { m[1 + 2 * r] = sw[r] & 077; m[2 + 2 * r] = (sw[r] >> 6) & 077; }
    pidp_rx(m, 7);
    frame_len = pidp_update(0, NULL, frame);
}

static void press(uint32_t key)
{
    sw[2] |= key; send_switches();
    sw[2] &= ~key; send_switches();
}

static void set_sr(uint32_t v) { sw[0] = v; send_switches(); }

static void deposit(uint32_t addr, const uint16_t *w, int n)
{
    set_sr(addr); press(PIDP_SW_LOAD);
    for (int i = 0; i < n; i++) { set_sr(w[i]); press(PIDP_SW_DEP); }
}

static int lamp(int row, int col) { return frame[1 + row * PIDP_COLS + col]; }

static uint32_t lamp_word(int row)          /* lamps fully lit -> bits */
{
    uint32_t v = 0;
    for (int j = 0; j < 12; j++) if (lamp(row, j) == 31) v |= 1u << j;
    return v;
}

static void frames(int n)                   /* sample for a while, send n lamp frames */
{
    for (int i = 0; i < n; i++) {
        pdp8_lamps_t l;
        if (!pdp8.halted) pdp8_run(20000);
        pdp8_lamps_take(&l);
        frame_len = pidp_update(0, &l, frame);
    }
}

int main(void)
{
    pdp8_reset();
    pdp8.halted = true;
    pdp8_lamps_enable(true);

    /* link: nothing happens until the panel speaks */
    pidp_link(true);
    CHECK(!pidp_present(), "silent panel is not present");
    uint8_t hello[] = { 0x13, PIDP_HELLO, 'P', '8', 1, 1, 0 };
    pidp_rx(hello, sizeof hello);
    CHECK(pidp_present(), "HELLO makes the panel present");

    /* START is held as the panel connects: the first report must not act on it */
    sw[2] = PIDP_SW_START; send_switches();
    CHECK(pdp8.halted, "switch already closed at connect is not a press");
    sw[2] = 0; send_switches();

    /* toggle in: CLA CLL / TAD 205 / DCA 206 / HLT / 0 / 1234 */
    static const uint16_t prog[] = { 07300, 01205, 03206, 07402, 0, 01234 };
    deposit(0200, prog, 6);
    CHECK(pdp8_mem[0200] == 07300 && pdp8_mem[0205] == 01234, "DEP stores the switch register and advances");
    CHECK(pdp8.pc == 0206, "PC advanced past the deposits");

    set_sr(0205); press(PIDP_SW_LOAD); press(PIDP_SW_EXAM);
    frames(8);
    CHECK(lamp_word(2) == 01234 && lamp_word(1) == 0205, "EXAM shows MA/MB on the lamps");

    set_sr(0200); press(PIDP_SW_LOAD);
    press(PIDP_SW_START);
    CHECK(!pdp8.halted, "START runs");
    for (int i = 0; i < 10 && !pdp8.halted; i++) pdp8_run(1000);
    CHECK(pdp8.halted && pdp8_mem[0206] == 01234, "program ran to its HLT and stored 1234");
    frames(8);
    CHECK(frame_len == 1 + 96 && frame[0] == PIDP_LAMPS, "lamp frame is 97 bytes");
    CHECK(lamp_word(0) == 0204, "halted: PC lamps show 0204");
    CHECK(lamp_word(3) == 0 && lamp(5, 11) == 0, "halted: AC lamps show 0 after DCA");
    CHECK(lamp(6, 7) == 0, "halted: RUN lamp off");
    CHECK(lamp(5, 4) == 31 && lamp(5, 3) == 31, "halted: OPR + FETCH lamps lit");

    /* IF/DF switches with LOAD ADD */
    sw[1] = (5u << 9) | (3u << 6); set_sr(01000); press(PIDP_SW_LOAD);
    CHECK(pdp8.ifld == (3u << 12) && pdp8.df == (5u << 12) && pdp8.pc == 01000, "LOAD ADD sets IF, DF, PC");
    frames(8);
    CHECK(lamp_word(7) == ((5u << 9) | (3u << 6)), "IF/DF lamps");
    sw[1] = 0; set_sr(0);

    /* a loop in field 0: IAC / JMP .-1  -> running lamps are averaged */
    static const uint16_t loop[] = { 07001, 05300 };
    press(PIDP_SW_LOAD);
    deposit(0300, loop, 2);
    set_sr(0300); press(PIDP_SW_LOAD); press(PIDP_SW_START);
    frames(10);
    CHECK(lamp(6, 7) == 31, "running: RUN lamp on");
    int pc0 = lamp(0, 0), pc6 = lamp(0, 6), pc7 = lamp(0, 7);
    printf("     PC bit0 %d, bit6 %d, bit7 %d; OPR %d JMP %d FETCH %d EXEC %d\n",
           pc0, pc6, pc7, lamp(5, 4), lamp(5, 6), lamp(5, 3), lamp(5, 2));
    CHECK(pc6 == 31 && pc7 == 31 && pc0 > 8 && pc0 < 24, "running: PC lamps averaged (bit 0 half lit)");
    CHECK(lamp(5, 4) > 8 && lamp(5, 4) < 24 && lamp(5, 6) > 8 && lamp(5, 6) < 24, "running: OPR and JMP half lit");
    CHECK(lamp(5, 2) == 0, "running: no EXECUTE cycles in this loop");

    /* STOP, then single instruction with SING INST + CONT */
    press(PIDP_SW_STOP);
    CHECK(pdp8.halted && panel_halted, "STOP halts (as a panel halt)");
    uint64_t ic = pdp8.icount;
    sw[2] = PIDP_SW_SINST; send_switches();
    press(PIDP_SW_CONT | PIDP_SW_SINST); sw[2] = PIDP_SW_SINST; send_switches();
    CHECK(pdp8.halted && pdp8.icount == ic + 1, "SING INST + CONT executes one instruction");
    sw[2] = 0; send_switches();
    press(PIDP_SW_CONT);
    CHECK(!pdp8.halted && !panel_halted, "CONT resumes");
    sw[2] = PIDP_SW_SSTEP; send_switches();
    CHECK(pdp8.halted, "closing SING STEP while running halts");
    sw[2] = 0; send_switches();

    /* resync after garbage and a torn message */
    uint8_t junk[] = { 0x7f, PIDP_SWITCH, 1, 2, PIDP_SWITCH, 5, 0, 0, 0, 0, 0 };
    pidp_rx(junk, sizeof junk);
    CHECK(pidp_switches(0) == 5, "receiver resynchronises on a new message byte");

    pidp_link(false);
    CHECK(!pidp_present() && pidp_update(0, NULL, frame) == 0, "unplugging drops the panel");

    printf(fails ? "PIDP TEST FAILED (%d)\n" : "PIDP TEST PASS\n", fails);
    return fails != 0;
}
