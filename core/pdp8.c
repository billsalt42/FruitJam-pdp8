/*
 * pdp8.c - portable PDP-8/E emulator core (see pdp8.h)
 *
 * Instruction semantics follow SIMH's pdp8_cpu.c, pdp8_tt.c, pdp8_rk.c,
 * pdp8_clk.c and pdp8_lp.c (Copyright (c) 1993-2013 Robert M Supnik,
 * MIT-style licence).  This is an independent, compact re-implementation
 * intended for microcontrollers.
 */
#include "pdp8.h"
#include <string.h>

pdp8_cpu_t pdp8;
uint16_t   pdp8_mem[PDP8_MEMSIZE];
#define M pdp8_mem

/* ---------------- interrupt bookkeeping ---------------- */
/* devices with an enable mask (dev_done & int_enable) */
#define INT_LPT   (1u << 0)
#define INT_PTP   (1u << 1)
#define INT_PTR   (1u << 2)
#define INT_TTO   (1u << 3)
#define INT_TTI   (1u << 4)
#define INT_CLK   (1u << 5)
#define INT_TTO1  (1u << 6)          /* KL8E multiplexer, output summary */
#define INT_TTI1  (1u << 10)         /* KL8E multiplexer, input summary */
#define INT_DEV_ENABLE ((1u << 14) - 1)
/* direct requests */
#define INT_RK    (1u << 15)
#define INT_RF    (1u << 16)
#define INT_DTA   (1u << 17)         /* TC08 */
#define INT_RX    (1u << 11)         /* RX8E (done flag, has an enable) */
#define INT_UF    (1u << 23)
#define INT_TSC   (1u << 24)         /* TSC8-75 (ETOS) */
#define INT_INIT_ENABLE (INT_TTI | INT_TTO | INT_LPT | INT_PTR | INT_PTP)

static uint32_t int_req, dev_done, int_enable;

static inline void int_update(void)
{
    int_req = (int_req & ~INT_DEV_ENABLE) | (dev_done & int_enable);
}

/* ---------------- simple event scheduler ---------------- */
enum { EV_TTO, EV_LPT, EV_RK0, EV_RK1, EV_RK2, EV_RK3, EV_POLL,
       EV_TTOX0, EV_TTOX1, EV_TTOX2, EV_TTOX3, EV_RF, EV_PCELL, EV_PTR, EV_PTP,
       EV_DT0, EV_DT1, EV_DT2, EV_DT3, EV_DT4, EV_DT5, EV_DT6, EV_DT7, EV_RX0, EV_RX1, EV_COUNT };
static uint64_t ev_time[EV_COUNT];      /* 0 = inactive, else icount deadline */
static uint64_t ev_next = 1;

static void ev_recalc(void)
{
    uint64_t n = UINT64_MAX;
    for (int i = 0; i < EV_COUNT; i++)
        if (ev_time[i] && ev_time[i] < n) n = ev_time[i];
    ev_next = n;
}
static void ev_schedule(int ev, uint32_t delay)
{
    if (delay == 0) delay = 1;
    ev_time[ev] = pdp8.icount + delay;
    if (ev_time[ev] < ev_next) ev_next = ev_time[ev];
}
static bool ev_active(int ev) { return ev_time[ev] != 0; }

/* ---------------- KL8E console ---------------- */
#define KEYQ_SIZE 256
static uint8_t  keyq[KEYQ_SIZE];
static volatile uint32_t keyq_head, keyq_tail;
static uint32_t tti_buf, tto_buf;
static uint32_t tti_time;
static uint32_t tto_wait = 100;
#define POLL_INTERVAL 1000

void pdp8_key(uint8_t ch)
{
    uint32_t n = (keyq_head + 1) % KEYQ_SIZE;
    if (n == keyq_tail) return;              /* full: drop */
    keyq[keyq_head] = ch;
    keyq_head = n;
}
int pdp8_key_backlog(void) { return (int)((keyq_head + KEYQ_SIZE - keyq_tail) % KEYQ_SIZE); }
void pdp8_set_tto_delay(uint32_t n) { tto_wait = n ? n : 1; }

static void tti_poll(void)
{
    if (keyq_head == keyq_tail) return;
    /* like SIMH: don't overwrite an unread character unless it's stale (500 ms) */
    if ((dev_done & INT_TTI) && (uint32_t)(host_millis() - tti_time) < 500) return;
    tti_buf = keyq[keyq_tail];
    keyq_tail = (keyq_tail + 1) % KEYQ_SIZE;
    tti_time = host_millis();
    dev_done |= INT_TTI;
    int_update();
}

static uint32_t iot_tti(uint32_t ir, uint32_t ac, bool *skip)
{

    switch (ir & 7) {
    case 0: dev_done &= ~INT_TTI; int_req &= ~INT_TTI; return ac;           /* KCF */
    case 1: if (dev_done & INT_TTI) *skip = true; return ac;                 /* KSF */
    case 2: dev_done &= ~INT_TTI; int_req &= ~INT_TTI; return 0;             /* KCC */
    case 4: return ac | tti_buf;                                             /* KRS */
    case 5:                                                                  /* KIE */
        if (ac & 1) int_enable |= (INT_TTI | INT_TTO);
        else        int_enable &= ~(INT_TTI | INT_TTO);
        int_update();
        return ac;
    case 6:                                                                  /* KRB */
        dev_done &= ~INT_TTI; int_req &= ~INT_TTI;
        return tti_buf;
    default: return ac;
    }
}

static uint32_t iot_tto(uint32_t ir, uint32_t ac, bool *skip)
{
    switch (ir & 7) {
    case 0: dev_done |= INT_TTO; int_update(); return ac;                    /* TFL */
    case 1: if (dev_done & INT_TTO) *skip = true; return ac;                 /* TSF */
    case 2: dev_done &= ~INT_TTO; int_req &= ~INT_TTO; return ac;            /* TCF */
    case 5: if (int_req & (INT_TTI | INT_TTO)) *skip = true; return ac;      /* SPI */
    case 6: dev_done &= ~INT_TTO; int_req &= ~INT_TTO;                       /* TLS */
        /* fall through */
    case 4:                                                                  /* TPC */
        tto_buf = ac & 0377;
        ev_schedule(EV_TTO, tto_wait);
        return ac;
    default: return ac;
    }
}

static void tto_svc(void)
{
    host_tty_out((uint8_t)(tto_buf & 0177));
    dev_done |= INT_TTO;
    int_update();
}

/* ---------------- DK8-E clock ---------------- */
static uint32_t clk_last_ms;
static uint32_t iot_clk(uint32_t ir, uint32_t ac, bool *skip)
{
    switch (ir & 7) {
    case 1: int_enable |= INT_CLK; int_update(); return ac;                  /* CLEI */
    case 2: int_enable &= ~INT_CLK; int_req &= ~INT_CLK; return ac;          /* CLDI */
    case 3:                                                                  /* CLSC */
        if (dev_done & INT_CLK) {
            dev_done &= ~INT_CLK; int_req &= ~INT_CLK; *skip = true;
        }
        return ac;
    case 5:                                                                  /* CLLE */
        if (ac & 1) int_enable |= INT_CLK; else int_enable &= ~INT_CLK;
        int_update();
        return ac;
    case 6: dev_done &= ~INT_CLK; int_req &= ~INT_CLK; return ac;            /* CLCL */
    case 7: if (dev_done & INT_CLK) *skip = true; return ac;                 /* CLSK */
    default: return ac;
    }
}
void pdp8_clock_tick(void) { dev_done |= INT_CLK; int_update(); }

/* ---------------- LE8 line printer ---------------- */
static uint32_t lpt_buf;
static uint32_t iot_lpt(uint32_t ir, uint32_t ac, bool *skip)
{
    switch (ir & 7) {
    case 0: dev_done |= INT_LPT; int_update(); return ac;                    /* PSKF-ish: set flag */
    case 1: if (dev_done & INT_LPT) *skip = true; return ac;                 /* PSKF */
    case 2: dev_done &= ~INT_LPT; int_req &= ~INT_LPT; return ac;            /* PCLF */
    case 3: return ac;                                                       /* PSKE: never in error */
    case 6: dev_done &= ~INT_LPT; int_req &= ~INT_LPT;                       /* PCLF!PSTB */
        /* fall through */
    case 4:                                                                  /* PSTB */
        lpt_buf = ac & 0177;
        ev_schedule(EV_LPT, (lpt_buf == 015 || lpt_buf == 014 || lpt_buf == 012) ? 500 : 10);
        return ac;
    case 5: int_enable |= INT_LPT; int_update(); return ac;                  /* PSIE */
    case 7: int_enable &= ~INT_LPT; int_req &= ~INT_LPT; return ac;          /* PCIE */
    }
    return ac;
}
static void lpt_svc(void)
{
    host_lpt_out((uint8_t)lpt_buf);
    dev_done |= INT_LPT;
    int_update();
}

/* ---------------- RK8E disk ---------------- */
#define RKS_DONE 04000
#define RKS_HMOV 02000
#define RKS_NRDY 00200
#define RKS_BUSY 00100
#define RKS_WLK  00020
#define RKS_STAT 00002
#define RKS_ERR  (RKS_BUSY | 00040 | RKS_WLK | 00010 | 00004 | RKS_STAT | 00001)
#define RKC_READ  0
#define RKC_RALL  1
#define RKC_WLK   2
#define RKC_SEEK  3
#define RKC_WRITE 4
#define RKC_WALL  5
#define RKC_IE   00400
#define RKC_SKDN 00200
#define RKC_HALF 00100
#define RK_NUMCY 203
#define RK_SWAIT 10
#define RK_RWAIT 10
#define RK_MIN   50

static uint32_t rk_sta, rk_cmd, rk_da, rk_ma, rk_busy;
static uint32_t rk_cyl[RK_NUMDR], rk_func[RK_NUMDR];
static bool     rk_swlk[RK_NUMDR];
static uint16_t rk_secbuf[RK_WORDS_PER_SECTOR];

#define RK_DRIVE(c) (((c) >> 1) & 3)
#define RK_FUNC(c)  (((c) >> 9) & 7)
#define RK_MEX(c)   (((c) & 070) << 9)

static inline void rk_int_update(void)
{
    if ((rk_sta & (RKS_DONE | RKS_ERR)) && (rk_cmd & RKC_IE)) int_req |= INT_RK;
    else int_req &= ~INT_RK;
}

static void rk_go(uint32_t func, uint32_t cyl)
{
    if (func == RKC_RALL) func = RKC_READ;
    if (func == RKC_WALL) func = RKC_WRITE;
    int u = RK_DRIVE(rk_cmd);
    if (!host_disk_present(u)) { rk_sta |= RKS_DONE | RKS_NRDY | RKS_STAT; return; }
    if (ev_active(EV_RK0 + u) || cyl >= RK_NUMCY) { rk_sta |= RKS_DONE | RKS_STAT; return; }
    if (func == RKC_WRITE && (rk_swlk[u] || host_disk_readonly(u))) {
        rk_sta |= RKS_DONE | RKS_WLK; return;
    }
    if (func == RKC_WLK) { rk_swlk[u] = true; rk_sta |= RKS_DONE; return; }
    uint32_t d = (cyl > rk_cyl[u] ? cyl - rk_cyl[u] : rk_cyl[u] - cyl) * RK_SWAIT;
    if (func == RKC_SEEK) {
        ev_schedule(EV_RK0 + u, d > RK_MIN ? d : RK_MIN);
        rk_sta |= RKS_DONE;
    } else {
        ev_schedule(EV_RK0 + u, d + RK_RWAIT);
        rk_busy = 1;
    }
    rk_func[u] = func;
    rk_cyl[u] = cyl;
}

static void rk_svc(int u)
{
    if (rk_func[u] == RKC_SEEK) {
        if (u == (int)RK_DRIVE(rk_cmd) && (rk_cmd & RKC_SKDN)) {
            rk_sta |= RKS_DONE;
            rk_int_update();
        }
        return;
    }
    if (!host_disk_present(u)) {
        rk_sta |= RKS_DONE | RKS_NRDY | RKS_STAT; rk_busy = 0; rk_int_update(); return;
    }
    if (rk_func[u] == RKC_WRITE && (rk_swlk[u] || host_disk_readonly(u))) {
        rk_sta |= RKS_DONE | RKS_WLK; rk_busy = 0; rk_int_update(); return;
    }
    uint32_t field = RK_MEX(rk_cmd);
    uint32_t sector = ((rk_cmd & 1) << 12) | rk_da;
    uint32_t wc = (rk_cmd & RKC_HALF) ? 128 : 256;
    bool ok = true;

    if (rk_func[u] == RKC_READ) {
        ok = host_disk_read(u, sector, rk_secbuf);
        if (!ok) memset(rk_secbuf, 0, sizeof rk_secbuf);
        for (uint32_t i = 0; i < wc; i++)
            M[field | ((rk_ma + i) & 07777)] = rk_secbuf[i] & 07777;
    } else if (rk_func[u] == RKC_WRITE) {
        for (uint32_t i = 0; i < 256; i++)
            rk_secbuf[i] = (i < wc) ? M[field | ((rk_ma + i) & 07777)] : 0;
        ok = host_disk_write(u, sector, rk_secbuf);
    }
    rk_ma = (rk_ma + wc) & 07777;
    rk_sta |= RKS_DONE;
    if (!ok) rk_sta |= RKS_STAT;
    rk_busy = 0;
    rk_int_update();
}

static uint32_t iot_rk(uint32_t ir, uint32_t ac, bool *skip)
{
    switch (ir & 7) {
    case 1:                                                                  /* DSKP */
        if (rk_sta & (RKS_DONE | RKS_ERR)) *skip = true;
        return ac;
    case 2:                                                                  /* DCLR */
        rk_sta = 0;
        switch (ac & 3) {
        case 0: if (rk_busy) rk_sta |= RKS_BUSY; break;
        case 3: break;
        case 1:
            rk_cmd = rk_busy = 0; rk_ma = rk_da = 0;
            for (int i = 0; i < RK_NUMDR; i++) ev_time[EV_RK0 + i] = 0;
            ev_recalc();
            break;
        case 2:
            if (rk_busy) rk_sta |= RKS_BUSY; else rk_go(RKC_SEEK, 0);
            break;
        }
        break;
    case 3:                                                                  /* DLAG */
        if (rk_busy) rk_sta |= RKS_BUSY;
        else { rk_da = ac; rk_go(RK_FUNC(rk_cmd), ((rk_cmd & 1) << 7) | ((rk_da >> 5) & 0177)); }
        break;
    case 4:                                                                  /* DLCA */
        if (rk_busy) rk_sta |= RKS_BUSY; else rk_ma = ac;
        break;
    case 5: {                                                                /* DRST */
        int u = RK_DRIVE(rk_cmd);
        rk_sta &= ~(RKS_HMOV | RKS_NRDY);
        if (!host_disk_present(u)) rk_sta |= RKS_NRDY;
        if (ev_active(EV_RK0 + u)) rk_sta |= RKS_HMOV;
        return rk_sta;
    }
    case 6:                                                                  /* DLDC */
        if (rk_busy) rk_sta |= RKS_BUSY; else { rk_cmd = ac; rk_sta = 0; }
        break;
    case 0: return ac;
    case 7: break;                                                           /* DMAN */
    }
    rk_int_update();
    return 0;
}

static void rk_reset(void)
{
    rk_cmd = rk_ma = rk_da = rk_sta = rk_busy = 0;
    int_req &= ~INT_RK;
    for (int i = 0; i < RK_NUMDR; i++) {
        ev_time[EV_RK0 + i] = 0; rk_swlk[i] = false; rk_cyl[i] = rk_func[i] = 0;
    }
    ev_recalc();
}

/* ---------------- KL8E terminal multiplexer: 4 lines, devices 40-47 ---------------- */
#define TTX_LINES 4
static uint32_t ttix_done, ttox_done, ttx_enbl;
static uint8_t  ttix_buf[TTX_LINES], ttox_buf[TTX_LINES];
#define TTXQ_SIZE 64
static uint8_t  ttxq[TTX_LINES][TTXQ_SIZE];
static volatile uint32_t ttxq_head[TTX_LINES], ttxq_tail[TTX_LINES];

static void ttx_new_flags(uint32_t i, uint32_t o, uint32_t e)
{
    ttix_done = i; ttox_done = o; ttx_enbl = e;
    if (ttix_done & ttx_enbl) dev_done |= INT_TTI1; else dev_done &= ~INT_TTI1;
    if (ttox_done & ttx_enbl) dev_done |= INT_TTO1; else dev_done &= ~INT_TTO1;
    int_enable |= (INT_TTI1 | INT_TTO1);
    int_update();
}

void pdp8_ttx_key(int line, uint8_t ch)
{
    if (line < 0 || line >= TTX_LINES) return;
    uint32_t n = (ttxq_head[line] + 1) % TTXQ_SIZE;
    if (n == ttxq_tail[line]) return;
    ttxq[line][ttxq_head[line]] = ch;
    ttxq_head[line] = n;
}
int pdp8_ttx_backlog(int line)
{
    return (int)((ttxq_head[line] + TTXQ_SIZE - ttxq_tail[line]) % TTXQ_SIZE);
}

static void ttx_poll(void)
{
    for (int ln = 0; ln < TTX_LINES; ln++) {
        if ((ttix_done >> ln) & 1) continue;
        if (ttxq_head[ln] == ttxq_tail[ln]) continue;
        ttix_buf[ln] = ttxq[ln][ttxq_tail[ln]];
        ttxq_tail[ln] = (ttxq_tail[ln] + 1) % TTXQ_SIZE;
        ttx_new_flags(ttix_done | (1u << ln), ttox_done, ttx_enbl);
    }
}

static uint32_t iot_ttx(uint32_t ir, uint32_t ac, bool *skip)
{
    uint32_t dev = (ir >> 3) & 077;
    int ln = (int)((dev - 040) >> 1);
    uint32_t m = 1u << ln;
    if (!(dev & 1)) {                                   /* input side */
        switch (ir & 7) {
        case 0: ttx_new_flags(ttix_done & ~m, ttox_done, ttx_enbl); return ac;       /* KCF */
        case 1: if (ttix_done & m) *skip = true; return ac;                          /* KSF */
        case 2: ttx_new_flags(ttix_done & ~m, ttox_done, ttx_enbl); return 0;        /* KCC */
        case 4: return ac | ttix_buf[ln];                                            /* KRS */
        case 5:                                                                      /* KIE */
            ttx_new_flags(ttix_done, ttox_done, (ac & 1) ? (ttx_enbl | m) : (ttx_enbl & ~m));
            return ac;
        case 6: ttx_new_flags(ttix_done & ~m, ttox_done, ttx_enbl); return ttix_buf[ln]; /* KRB */
        default: return ac;
        }
    } else {                                            /* output side */
        switch (ir & 7) {
        case 0: ttx_new_flags(ttix_done, ttox_done | m, ttx_enbl); return ac;        /* TLF */
        case 1: if (ttox_done & m) *skip = true; return ac;                          /* TSF */
        case 2: ttx_new_flags(ttix_done, ttox_done & ~m, ttx_enbl); return ac;       /* TCF */
        case 5: if (((ttix_done | ttox_done) & m) && (ttx_enbl & m)) *skip = true;   /* SPI */
                return ac;
        case 6: ttx_new_flags(ttix_done, ttox_done & ~m, ttx_enbl);                  /* TLS */
            /* fall through */
        case 4:                                                                      /* TPC */
            ttox_buf[ln] = ac & 0377;
            ev_schedule(EV_TTOX0 + ln, tto_wait);
            return ac;
        default: return ac;
        }
    }
}

static void ttox_svc(int ln)
{
    host_ttx_out(ln, (uint8_t)(ttox_buf[ln] & 0177));
    ttx_new_flags(ttix_done, ttox_done | (1u << ln), ttx_enbl);
}

static void ttx_reset(void)
{
    for (int ln = 0; ln < TTX_LINES; ln++) {
        ttix_buf[ln] = ttox_buf[ln] = 0;
        ev_time[EV_TTOX0 + ln] = 0;
    }
    ttx_new_flags(0, 0, (1u << TTX_LINES) - 1);
}

/* ---------------- RF08 fixed-head disk (devices 60-62, 64) ----------------
 * Words are fetched through a small write-back cache of 256-word blocks;
 * the host supplies the backing store (a file on the SD card). */
#define RF_NUMWD   2048
#define RF_WC      07750
#define RF_MA      07751
#define RFS_PCA    04000
#define RFS_WLS    01000
#define RFS_EIE    00400
#define RFS_PIE    00200
#define RFS_CIE    00100
#define RFS_MEX    00070
#define RFS_DRL    00004
#define RFS_NXD    00002
#define RFS_PER    00001
#define RFS_ERR    (RFS_WLS | RFS_DRL | RFS_NXD | RFS_PER)
#define RF_TIME    10
#define RF_READ    2
#define RF_CACHE   32

static uint32_t rf_sta, rf_da, rf_done = 1, rf_func;
static struct { int32_t blk; bool dirty; uint32_t lru; uint16_t w[256]; } rf_cache[RF_CACHE];
static uint32_t rf_lru_clock;

static inline bool rf_present(void) { return host_rf_words() != 0; }
static inline uint32_t rf_pos(void) { return (uint32_t)((pdp8.icount / RF_TIME) % RF_NUMWD); }

static void rf_update_pcell(void)
{
    if (rf_pos() < 6) rf_sta |= RFS_PCA; else rf_sta &= ~RFS_PCA;
}
static void rf_int_update(void)
{
    if ((rf_done && (rf_sta & RFS_CIE)) || ((rf_sta & RFS_ERR) && (rf_sta & RFS_EIE)) ||
        ((rf_sta & RFS_PCA) && (rf_sta & RFS_PIE)))
        int_req |= INT_RF;
    else int_req &= ~INT_RF;
}

static uint16_t *rf_block(uint32_t blk)
{
    int victim = 0;
    for (int i = 0; i < RF_CACHE; i++) {
        if (rf_cache[i].blk == (int32_t)blk) { rf_cache[i].lru = ++rf_lru_clock; return rf_cache[i].w; }
        if (rf_cache[i].lru < rf_cache[victim].lru) victim = i;
    }
    if (rf_cache[victim].dirty) {
        host_rf_write(rf_cache[victim].blk, rf_cache[victim].w);
        rf_cache[victim].dirty = false;
    }
    if (!host_rf_read(blk, rf_cache[victim].w)) memset(rf_cache[victim].w, 0, 512);
    rf_cache[victim].blk = (int32_t)blk;
    rf_cache[victim].lru = ++rf_lru_clock;
    return rf_cache[victim].w;
}

void pdp8_rf_flush(void)
{
    for (int i = 0; i < RF_CACHE; i++)
        if (rf_cache[i].blk >= 0 && rf_cache[i].dirty) {
            host_rf_write(rf_cache[i].blk, rf_cache[i].w);
            rf_cache[i].dirty = false;
        }
}

void pdp8_rf_invalidate(void)
{
    pdp8_rf_flush();
    for (int i = 0; i < RF_CACHE; i++) { rf_cache[i].blk = -1; rf_cache[i].lru = 0; rf_cache[i].dirty = false; }
}

static uint32_t iot_rf(uint32_t ir, uint32_t ac, bool *skip)
{
    uint32_t dev = (ir >> 3) & 077, pulse = ir & 7;
    if (!rf_present()) return ac;                       /* controller absent */
    rf_update_pcell();
    switch (dev) {
    case 060:
        if (pulse & 1) {                                /* DCMA */
            rf_da &= ~07777u;
            rf_done = 0;
            rf_sta &= ~RFS_ERR;
            rf_int_update();
        }
        if (pulse & 6) {                                /* DMAR, DMAW */
            rf_da |= ac;
            rf_func = pulse & ~1u;
            int32_t t = (int32_t)(rf_da & (RF_NUMWD - 1)) - (int32_t)rf_pos();
            if (t < 0) t += RF_NUMWD;
            ev_schedule(EV_RF, (uint32_t)t * RF_TIME);
            ac = 0;
        }
        return ac;
    case 061:
        switch (pulse) {
        case 1: rf_sta &= 07007; int_req &= ~INT_RF; ev_time[EV_PCELL] = 0; return ac;  /* DCIM */
        case 2: if ((rf_da & (RF_NUMWD - 1)) == rf_pos()) *skip = true; return 0;         /* DSAC */
        case 5:                                                                          /* DIML */
            rf_sta = (rf_sta & 07007) | (ac & 0770);
            if (rf_sta & RFS_PIE) ev_schedule(EV_PCELL, (RF_NUMWD - rf_pos()) * RF_TIME);
            else ev_time[EV_PCELL] = 0;
            rf_int_update();
            return 0;
        case 6: return rf_sta;                                                           /* DIMA */
        }
        return ac;
    case 062:
        if (pulse & 1) { if (rf_sta & RFS_ERR) *skip = true; }                           /* DFSE */
        if (pulse & 2) {                                                                 /* DFSC */
            if (pulse & 4) ac &= ~07777u;
            else if (rf_done) *skip = true;
        }
        if (pulse & 4) ac |= rf_da & 07777;                                              /* DMAC */
        return ac;
    case 064:
        switch (pulse) {
        case 1: rf_da &= 07777; break;                                                   /* DCXA */
        case 3: rf_da &= 07777;                                                          /* DXAL */
            /* fall through */
        case 2: rf_da |= (ac & 0377) << 12; ac = 0; break;
        case 5: ac = 0;                                                                  /* DXAC */
            /* fall through */
        case 4: ac |= (rf_da >> 12) & 0377; break;
        default: break;
        }
        if (rf_da >= host_rf_words()) rf_sta |= RFS_NXD; else rf_sta &= ~RFS_NXD;
        rf_int_update();
        return ac;
    }
    return ac;
}

static void rf_svc(void)
{
    uint32_t mex = (rf_sta & RFS_MEX) << 9;
    uint32_t cap = host_rf_words();
    uint32_t wc;
    rf_update_pcell();
    do {
        if (rf_da >= cap) { rf_sta |= RFS_NXD; break; }
        wc = M[RF_WC] = (M[RF_WC] + 1) & 07777;
        M[RF_MA] = (M[RF_MA] + 1) & 07777;
        uint32_t pa = mex | M[RF_MA];
        uint16_t *b = rf_block(rf_da >> 8);
        if (rf_func == RF_READ) {
            M[pa] = b[rf_da & 0377] & 07777;
        } else {
            b[rf_da & 0377] = M[pa];
            for (int i = 0; i < RF_CACHE; i++)
                if (rf_cache[i].w == b) { rf_cache[i].dirty = true; break; }
        }
        rf_da = (rf_da + 1) & 03777777;
    } while (wc != 0);
    rf_done = 1;
    rf_int_update();
}

static void pcell_svc(void)
{
    rf_sta |= RFS_PCA;
    if (rf_sta & RFS_PIE) {
        ev_schedule(EV_PCELL, RF_NUMWD * RF_TIME);
        int_req |= INT_RF;
    }
}

static void rf_reset(void)
{
    /* Initialize (power-up / CAF) leaves the completion flag clear.  SIMH sets
     * it here, but the UWM TSS/8 monitor attributes any interrupt to the disc
     * while DFSC skips, so a stale flag after CAF crashes it at start-up. */
    rf_sta = rf_da = 0;
    rf_done = 0;
    int_req &= ~INT_RF;
    ev_time[EV_RF] = ev_time[EV_PCELL] = 0;
}

/* ---------------- BIN-format paper tape loader ---------------- */
int pdp8_load_bin(const uint8_t *tape, uint32_t len)
{
    uint32_t i = 0, field = 0, newf = 0, origin = 0, words = 0;
    int hi, lo;
    #define BIN_GETC(v) do { v = -1; \
        while (i < len) { int c_ = tape[i++]; \
            if (rubout) { if (c_ == 0377) rubout = 0; continue; } \
            if (c_ == 0377) { rubout = 1; continue; } \
            if (c_ > 0200) { newf = (uint32_t)(c_ & 070) << 9; continue; } \
            v = c_; break; } } while (0)
    int rubout = 0;
    do { BIN_GETC(hi); if (hi < 0) return -1; } while (hi == 0 || hi >= 0200);
    uint32_t csum = 0;
    for (;;) {
        BIN_GETC(lo); if (lo < 0) return -1;
        uint32_t wd = ((uint32_t)hi << 6) | (uint32_t)lo;
        int t = hi;
        BIN_GETC(hi); if (hi < 0) return -1;
        if (hi == 0200) {                                  /* trailer: wd is checksum */
            if ((csum - wd) & 07777) return -2;
            return (int)words;
        }
        csum += (uint32_t)t + (uint32_t)lo;
        if (wd > 07777) origin = wd & 07777;
        else {
            M[(field | origin) & (PDP8_MEMSIZE - 1)] = (uint16_t)wd;
            origin = (origin + 1) & 07777;
        }
        words++;
        field = newf;
    }
    #undef BIN_GETC
}

void pdp8_start(uint32_t addr)
{
    pdp8.ifld = pdp8.ib = addr & 070000;
    pdp8.df = 0;
    pdp8.pc = addr & 07777;
    pdp8.halted = false;
}

/* ---------------- PC8E paper tape reader (01) and punch (02) ---------------- */
static uint32_t ptr_buf, ptp_buf;
static uint32_t iot_ptr(uint32_t ir, uint32_t ac, bool *skip)
{
    switch (ir & 7) {
    case 0: int_enable |= INT_PTR | INT_PTP; int_update(); return ac;        /* RPE */
    case 1: if (dev_done & INT_PTR) *skip = true; return ac;                 /* RSF */
    case 6: ev_schedule(EV_PTR, 100);                                        /* RFC!RRB */
        /* fall through */
    case 2: dev_done &= ~INT_PTR; int_req &= ~INT_PTR; return ac | ptr_buf;  /* RRB */
    case 4: ev_schedule(EV_PTR, 100);                                        /* RFC */
        dev_done &= ~INT_PTR; int_req &= ~INT_PTR; return ac;
    default: return ac;
    }
}
static void ptr_svc(void)
{
    int c = host_ptr_getc();
    if (c < 0) return;                  /* no tape / end of tape: flag stays clear */
    ptr_buf = (uint32_t)c & 0377;
    dev_done |= INT_PTR;
    int_update();
}
static uint32_t iot_ptp(uint32_t ir, uint32_t ac, bool *skip)
{
    switch (ir & 7) {
    case 0: int_enable &= ~(INT_PTR | INT_PTP); int_update(); return ac;     /* PCE */
    case 1: if (dev_done & INT_PTP) *skip = true; return ac;                 /* PSF */
    case 2: dev_done &= ~INT_PTP; int_req &= ~INT_PTP; return ac;            /* PCF */
    case 6: dev_done &= ~INT_PTP; int_req &= ~INT_PTP;                       /* PLS */
        /* fall through */
    case 4: ptp_buf = ac & 0377; ev_schedule(EV_PTP, 100); return ac;        /* PPC */
    default: return ac;
    }
}
static void ptp_svc(void)
{
    host_ptp_putc((uint8_t)ptp_buf);
    dev_done |= INT_PTP;
    int_update();
}

/* ---------------- TC08 DECtape (devices 76, 77) ----------------
 * After SIMH's pdp8_dt.c (Robert M Supnik).  The tape moves with simulated
 * acceleration, speed and deceleration; the program sees block numbers and
 * data words go past as on the real drive.  Only the PDP-8 (12-bit) image
 * format: 1474 blocks of 129 words.  SIMH keeps the whole tape in memory;
 * here one block at a time is cached and written back when the tape moves
 * on (and by pdp8_dt_flush).
 */
#define DT_NUMDR        8
#define DT_WC           07754
#define DT_CA           07755
#define DT_LPERMC       6
#define DT_BLKWD        1
#define DT_CSMWD        4
#define DT_HTWRD        5
#define DT_EZLIN        (8192 * DT_LPERMC)
#define DT_BLKLN        (DT_BLKWD * DT_LPERMC)
#define DT_HTLIN        (DT_HTWRD * DT_LPERMC)
#define DT_WSIZE        4
#define DT_BSIZE        PDP8_DT_BLKWORDS
#define DT_TSIZE        PDP8_DT_BLOCKS
#define DT_LPERB        (DT_HTLIN + (DT_BSIZE * DT_WSIZE) + DT_HTLIN)
#define DT_FWDEZ        (DT_EZLIN + (DT_LPERB * DT_TSIZE))
#define DT_LIN2BL(p)    (((p) - DT_EZLIN) / DT_LPERB)
#define DT_LIN2OF(p)    (((p) - DT_EZLIN) % DT_LPERB)
#define DT_LIN2WD(p)    ((DT_LIN2OF (p) - DT_HTLIN) / DT_WSIZE)
#define DT_BLK2LN(b)    (((b) * DT_LPERB) + DT_EZLIN)
#define DT_QREZ(u)      (dt_pos[u] < DT_EZLIN)
#define DT_QFEZ(u)      (dt_pos[u] >= DT_FWDEZ)
#define DT_QEZ(u)       (DT_QREZ (u) || DT_QFEZ (u))

#define DTA_V_UNIT      9
#define DTA_V_MOT       7
#define DTA_V_MODE      6
#define DTA_V_FNC       3
#define  FNC_MOVE        00
#define  FNC_SRCH        01
#define  FNC_READ        02
#define  FNC_RALL        03
#define  FNC_WRIT        04
#define  FNC_WALL        05
#define  FNC_WMRK        06
#define DTA_FWDRV       (1u << (DTA_V_MOT + 1))
#define DTA_STSTP       (1u << DTA_V_MOT)
#define DTA_MODE        (1u << DTA_V_MODE)
#define DTA_ENB         (1u << 2)
#define DTA_CERF        (1u << 1)
#define DTA_CDTF        (1u << 0)
#define DTA_RW          (07777 & ~(DTA_CERF | DTA_CDTF))
#define DTA_GETUNIT(x)  (((x) >> DTA_V_UNIT) & 07)
#define DTA_GETFNC(x)   (((x) >> DTA_V_FNC) & 07)

#define DTB_ERF         (1u << 11)
#define DTB_MRK         (1u << 10)
#define DTB_END         (1u << 9)
#define DTB_SEL         (1u << 8)
#define DTB_PAR         (1u << 7)
#define DTB_TIM         (1u << 6)
#define DTB_MEX         (07u << 3)
#define DTB_DTF         (1u << 0)
#define DTB_ALLERR      (DTB_ERF | DTB_MRK | DTB_END | DTB_SEL | DTB_PAR | DTB_TIM)
#define DTB_GETMEX(x)   (((x) & DTB_MEX) << (12 - 3))

#define  DTS_STOP        0
#define  DTS_DECF        2
#define  DTS_ACCF        4
#define  DTS_ATSF        6
#define DTS_DIR         01
#define  DTS_OFR        7
#define DTS_GETMOT(x)   (((x) >> 3) & 07)
#define DTS_GETFNC(x)   ((x) & 07)
#define DTS_STA(y,z)    (((y) << 3) | (z))
#define DTS_SETSTA(u,y,z) dt_state[u] = DTS_STA (y, z)
#define DTS_SET2ND(u,y,z) dt_state[u] = (dt_state[u] & 077) | ((DTS_STA (y, z)) << 6)
#define DTS_SET3RD(u,y,z) dt_state[u] = (dt_state[u] & 07777) | ((DTS_STA (y, z)) << 12)
#define DTS_NXTSTA(x)   ((x) >> 6)

#define DTO_WCO         1
#define DTO_SOB         2

static uint32_t dtsa, dtsb;
static int32_t  dt_state[DT_NUMDR], dt_pos[DT_NUMDR];
static uint64_t dt_lastt[DT_NUMDR];
static bool     dt_offreel[DT_NUMDR];      /* ran off the reel: unloaded until remounted */
static int      dt_substate;
static const int32_t dt_ltime = 12, dt_dctime = 40000;

static int      dtc_unit = -1;               /* one-block cache */
static int32_t  dtc_blk = -1;
static bool     dtc_dirty;
static uint16_t dtc[DT_BSIZE];

static void dt_updint(void)
{
    if ((dtsa & DTA_ENB) && (dtsb & (DTB_ERF | DTB_DTF))) int_req |= INT_DTA;
    else int_req &= ~INT_DTA;
}

static bool dt_attached(int u) { return host_dt_present(u) && !dt_offreel[u]; }

static void dtc_flush(void)
{
    if (dtc_dirty && dtc_unit >= 0) host_dt_write(dtc_unit, (uint32_t)dtc_blk, dtc);
    dtc_dirty = false;
}

static uint16_t *dt_block(int u, int32_t blk)
{
    if (u != dtc_unit || blk != dtc_blk) {
        dtc_flush();
        if (!host_dt_read(u, (uint32_t)blk, dtc)) memset(dtc, 0, sizeof dtc);
        dtc_unit = u; dtc_blk = blk;
    }
    return dtc;
}

void pdp8_dt_flush(void) { dtc_flush(); }
void pdp8_dt_changed(int u)                  /* host mounted or removed a tape */
{
    if (u == dtc_unit) { dtc_flush(); dtc_unit = -1; dtc_blk = -1; }
    if (u >= 0 && u < DT_NUMDR) {
        ev_time[EV_DT0 + u] = 0;
        dt_state[u] = 0;
        dt_pos[u] = DT_EZLIN;
        dt_lastt[u] = pdp8.icount;
        dt_offreel[u] = false;
    }
}

static void dt_seterr(int u, uint32_t e);
static void dt_newfnc(int u, int32_t newsta);

static uint32_t dt_comobv(uint32_t dat)
{
    dat ^= 07777;
    return ((dat >> 9) & 07) | ((dat >> 3) & 070) | ((dat & 070) << 3) | ((dat & 07) << 9);
}

static uint32_t dt_csum(int u, int32_t blk)
{
    uint16_t *b = dt_block(u, blk);
    uint32_t csum = 077;
    for (int i = 0; i < DT_BSIZE; i++) {
        uint32_t w = b[i] ^ 07777;
        csum = csum ^ (w >> 6) ^ w;
    }
    return csum & 077;
}

static uint32_t dt_gethdr(int u, int32_t blk, int32_t relpos, int dir)
{
    if (relpos >= DT_HTLIN) relpos -= DT_WSIZE * DT_BSIZE;
    if (dir) {
        switch (relpos / DT_WSIZE) {
        case 6:  return 077;
        case 2:  return dt_comobv((blk & 077) << 6);
        case 1:  return dt_comobv(blk >> 6);
        case 12: return (blk >> 6) & 07777;
        case 11: return (blk & 077) << 6;
        case 7:  return dt_comobv(dt_csum(u, blk)) << 6;
        default: return 07777;
        }
    }
    switch (relpos / DT_WSIZE) {
    case 8:  return dt_csum(u, blk) << 6;
    case 12: return dt_comobv((blk & 077) << 6);
    case 13: return dt_comobv(blk >> 6);
    case 2:  return (blk >> 6) & 07777;
    case 3:  return (blk & 077) << 6;
    case 7:  return 077;
    default: return 0;
    }
}

static void dt_unload(int u)                 /* off the reel: as SIMH's detach */
{
    if (u == dtc_unit) { dtc_flush(); dtc_unit = -1; dtc_blk = -1; }
    ev_time[EV_DT0 + u] = 0;
    dt_offreel[u] = true;
    dt_state[u] = dt_pos[u] = 0;
}

/* position from the elapsed time; true if the tape ran off the reel */
static bool dt_setpos(int u)
{
    uint64_t now = pdp8.icount;
    uint32_t ut = (uint32_t)(now - dt_lastt[u]), ulin, udelt;
    int mot = DTS_GETMOT(dt_state[u]);
    int32_t delta = 0;
    if (ut == 0) return false;
    dt_lastt[u] = now;
    switch (mot & ~DTS_DIR) {
    case DTS_STOP: delta = 0; break;
    case DTS_DECF:
        ulin = ut / (uint32_t)dt_ltime;
        udelt = dt_dctime / dt_ltime;
        delta = (int32_t)(((uint64_t)ulin * udelt * 2 - (uint64_t)ulin * ulin) / (2 * udelt));
        break;
    case DTS_ACCF:
        ulin = ut / (uint32_t)dt_ltime;
        udelt = (dt_dctime - (dt_dctime >> 2)) / dt_ltime;
        delta = (int32_t)(((uint64_t)ulin * ulin) / (2 * udelt));
        break;
    case DTS_ATSF: delta = (int32_t)(ut / (uint32_t)dt_ltime); break;
    }
    dt_pos[u] += (mot & DTS_DIR) ? -delta : delta;
    if (dt_pos[u] < 0 || dt_pos[u] > DT_FWDEZ + DT_EZLIN) {
        dt_unload(u);
        if (u == (int)DTA_GETUNIT(dtsa)) dt_seterr(u, DTB_SEL);
        return true;
    }
    return false;
}

static void dt_schedez(int u, int dir)
{
    int32_t newpos = dir ? DT_EZLIN - DT_WSIZE : DT_FWDEZ + DT_WSIZE;
    int32_t d = newpos - dt_pos[u];
    ev_schedule(EV_DT0 + u, (uint32_t)(d < 0 ? -d : d) * dt_ltime);
}

static void dt_seterr(int u, uint32_t e)
{
    int mot = DTS_GETMOT(dt_state[u]);
    dtsa &= ~DTA_STSTP;
    dtsb |= DTB_ERF | e;
    if (mot >= DTS_ACCF) {
        ev_time[EV_DT0 + u] = 0;
        if (dt_setpos(u)) return;
        ev_schedule(EV_DT0 + u, dt_dctime);
        DTS_SETSTA(u, DTS_DECF | (mot & DTS_DIR), 0);
    } else DTS_SETSTA(u, mot, 0);
    dt_updint();
}

static void dt_deselect(uint32_t oldf)
{
    int u = DTA_GETUNIT(oldf);
    int old_mot = DTS_GETMOT(dt_state[u]);
    if (old_mot >= DTS_ATSF) dt_newfnc(u, DTS_STA(old_mot, DTS_OFR));
    else if (old_mot >= DTS_ACCF) DTS_SET2ND(u, DTS_ATSF | (old_mot & DTS_DIR), DTS_OFR);
}

static void dt_newsa(uint32_t newf)
{
    int u = DTA_GETUNIT(newf);
    if (!dt_attached(u)) { dt_seterr(u, DTB_SEL); return; }
    int prev_mot = DTS_GETMOT(dt_state[u]);
    int prev_mving = prev_mot != DTS_STOP;
    int prev_dir = prev_mot & DTS_DIR;
    int new_mving = (newf & DTA_STSTP) != 0;
    int new_dir = (newf & DTA_FWDRV) != 0;
    int new_fnc = DTA_GETFNC(newf);

    if ((prev_mving | new_mving) == 0) return;
    if (new_mving & ~prev_mving) {                      /* start */
        if (dt_setpos(u)) return;
        ev_schedule(EV_DT0 + u, dt_dctime - (dt_dctime >> 2));
        DTS_SETSTA(u, DTS_ACCF | new_dir, 0);
        DTS_SET2ND(u, DTS_ATSF | new_dir, new_fnc);
        return;
    }
    if (prev_mving & ~new_mving) {                      /* stop */
        if ((prev_mot & ~DTS_DIR) != DTS_DECF) {
            if (dt_setpos(u)) return;
            ev_schedule(EV_DT0 + u, dt_dctime);
        }
        DTS_SETSTA(u, DTS_DECF | prev_dir, 0);
        return;
    }
    if (prev_dir ^ new_dir) {                           /* reverse */
        if ((prev_mot & ~DTS_DIR) != DTS_DECF) {
            if (dt_setpos(u)) return;
            ev_schedule(EV_DT0 + u, dt_dctime);
        }
        DTS_SETSTA(u, DTS_DECF | prev_dir, 0);
        DTS_SET2ND(u, DTS_ACCF | new_dir, 0);
        DTS_SET3RD(u, DTS_ATSF | new_dir, new_fnc);
        return;
    }
    if (prev_mot < DTS_ACCF) {
        if (dt_setpos(u)) return;
        ev_schedule(EV_DT0 + u, dt_dctime - (dt_dctime >> 2));
        DTS_SETSTA(u, DTS_ACCF | new_dir, 0);
        DTS_SET2ND(u, DTS_ATSF | new_dir, new_fnc);
        return;
    }
    if (prev_mot < DTS_ATSF) {
        DTS_SET2ND(u, DTS_ATSF | new_dir, new_fnc);
        return;
    }
    dt_newfnc(u, DTS_STA(DTS_ATSF | new_dir, new_fnc));
}

static void dt_newfnc(int u, int32_t newsta)
{
    int32_t oldpos = dt_pos[u], newpos, relpos, blk;
    if (dt_setpos(u)) return;
    dt_state[u] = newsta;
    int fnc = DTS_GETFNC(dt_state[u]);
    int dir = DTS_GETMOT(dt_state[u]) & DTS_DIR;
    if (oldpos == dt_pos[u]) dt_pos[u] += dir ? -1 : 1;
    blk = DT_LIN2BL(dt_pos[u]);
    if (dir ? DT_QREZ(u) : DT_QFEZ(u)) { dt_seterr(u, DTB_END); return; }
    ev_time[EV_DT0 + u] = 0;
    dt_substate = DTO_SOB;
    switch (fnc) {
    case DTS_OFR:
        newpos = dir ? -1000 : DT_FWDEZ + DT_EZLIN + 1000;
        break;
    case FNC_MOVE:
        dt_schedez(u, dir);
        return;
    case FNC_SRCH:
        if (dir) newpos = DT_BLK2LN(DT_QFEZ(u) ? DT_TSIZE : blk) - DT_BLKLN - DT_WSIZE;
        else newpos = DT_BLK2LN(DT_QREZ(u) ? 0 : blk + 1) + DT_BLKLN + (DT_WSIZE - 1);
        break;
    case FNC_WRIT: case FNC_READ: case FNC_RALL: case FNC_WALL:
        if (DT_QEZ(u)) {
            newpos = dir ? DT_FWDEZ - DT_HTLIN - DT_WSIZE : DT_EZLIN + DT_HTLIN + (DT_WSIZE - 1);
            break;
        }
        relpos = DT_LIN2OF(dt_pos[u]);
        if (relpos >= DT_HTLIN && relpos < DT_LPERB - DT_HTLIN) { dt_seterr(u, DTB_SEL); return; }
        if (dir) newpos = DT_BLK2LN(relpos >= DT_LPERB - DT_HTLIN ? blk + 1 : blk) - DT_HTLIN - DT_WSIZE;
        else newpos = DT_BLK2LN(relpos < DT_HTLIN ? blk : blk + 1) + DT_HTLIN + (DT_WSIZE - 1);
        break;
    default:
        dt_seterr(u, DTB_SEL);
        return;
    }
    int32_t d = newpos - dt_pos[u];
    ev_schedule(EV_DT0 + u, (uint32_t)(d < 0 ? -d : d) * dt_ltime);
}

static inline void dt_store(uint32_t ma, uint32_t v) { if (ma < PDP8_MEMSIZE) M[ma] = (uint16_t)(v & 07777); }

static void dt_svc(int u)
{
    int mot = DTS_GETMOT(dt_state[u]);
    int dir = mot & DTS_DIR;
    int fnc = DTS_GETFNC(dt_state[u]);
    int32_t blk, wrd, relpos;
    uint32_t ma, dat;

    switch (mot) {
    case DTS_DECF: case DTS_DECF | 1:
        if (dt_setpos(u)) return;
        dt_state[u] = DTS_NXTSTA(dt_state[u]);
        if (dt_state[u]) ev_schedule(EV_DT0 + u, dt_dctime - (dt_dctime >> 2));
        else if (u == dtc_unit) dtc_flush();      /* tape stopped: write back */
        return;
    case DTS_ACCF: case DTS_ACCF | 1:
        dt_newfnc(u, DTS_NXTSTA(dt_state[u]));
        return;
    case DTS_ATSF: case DTS_ATSF | 1:
        break;
    default:
        dt_seterr(u, DTB_SEL);
        return;
    }
    if (dt_setpos(u)) return;
    if (DT_QEZ(u)) { dt_seterr(u, DTB_END); return; }
    blk = DT_LIN2BL(dt_pos[u]);
    switch (fnc) {
    case FNC_MOVE:
        dt_seterr(u, DTB_END);
        return;

    case FNC_SRCH:
        if (dtsb & DTB_DTF) { dt_seterr(u, DTB_TIM); return; }
        ev_schedule(EV_DT0 + u, DT_LPERB * dt_ltime);
        M[DT_WC] = (M[DT_WC] + 1) & 07777;
        ma = DTB_GETMEX(dtsb) | M[DT_CA];
        if ((dtsa & DTA_MODE) == 0 || M[DT_WC] == 0) dtsb |= DTB_DTF;
        dt_store(ma, (uint32_t)blk);
        break;

    case DTS_OFR:
        dt_unload(u);
        break;

    case FNC_READ:
        wrd = DT_LIN2WD(dt_pos[u]);
        switch (dt_substate) {
        case DTO_SOB:
            if (dtsb & DTB_DTF) { dt_seterr(u, DTB_TIM); return; }
            dt_substate = 0;
            /* fall through */
        case 0:
            M[DT_WC] = (M[DT_WC] + 1) & 07777;
            M[DT_CA] = (M[DT_CA] + 1) & 07777;
            if (M[DT_WC] == 0) dt_substate = DTO_WCO;
            ma = DTB_GETMEX(dtsb) | M[DT_CA];
            dat = dt_block(u, blk)[wrd];
            if (dir) dat = dt_comobv(dat);
            dt_store(ma, dat);
            /* fall through */
        case DTO_WCO:
            if (wrd != (dir ? 0 : DT_BSIZE - 1)) ev_schedule(EV_DT0 + u, DT_WSIZE * dt_ltime);
            else {
                dt_substate |= DTO_SOB;
                ev_schedule(EV_DT0 + u, ((2 * DT_HTLIN) + DT_WSIZE) * dt_ltime);
                if ((dtsa & DTA_MODE) == 0 || dt_substate == DTO_WCO) dtsb |= DTB_DTF;
            }
            break;
        case DTO_WCO | DTO_SOB:
            if (wrd == (dir ? 0 : DT_BSIZE)) dt_seterr(u, DTB_TIM);
            else ev_schedule(EV_DT0 + u, DT_WSIZE * dt_ltime);
            break;
        }
        break;

    case FNC_WRIT:
        wrd = DT_LIN2WD(dt_pos[u]);
        switch (dt_substate) {
        case DTO_SOB:
            if (dtsb & DTB_DTF) { dt_seterr(u, DTB_TIM); return; }
            dt_substate = 0;
            /* fall through */
        case 0:
            M[DT_WC] = (M[DT_WC] + 1) & 07777;
            M[DT_CA] = (M[DT_CA] + 1) & 07777;
            /* fall through */
        case DTO_WCO:
            ma = DTB_GETMEX(dtsb) | M[DT_CA];
            dat = dt_substate ? 0 : (ma < PDP8_MEMSIZE ? M[ma] : 0);
            if (dir) dat = dt_comobv(dat);
            dt_block(u, blk)[wrd] = (uint16_t)dat;
            dtc_dirty = true;
            if (M[DT_WC] == 0) dt_substate = DTO_WCO;
            if (wrd != (dir ? 0 : DT_BSIZE - 1)) ev_schedule(EV_DT0 + u, DT_WSIZE * dt_ltime);
            else {
                dt_substate |= DTO_SOB;
                ev_schedule(EV_DT0 + u, ((2 * DT_HTLIN) + DT_WSIZE) * dt_ltime);
                if ((dtsa & DTA_MODE) == 0 || M[DT_WC] == 0) dtsb |= DTB_DTF;
            }
            break;
        case DTO_WCO | DTO_SOB:
            dt_schedez(u, dir);
            break;
        }
        break;

    case FNC_RALL:
        switch (dt_substate) {
        case 0: case DTO_SOB:
            if (dtsb & DTB_DTF) { dt_seterr(u, DTB_TIM); return; }
            relpos = DT_LIN2OF(dt_pos[u]);
            M[DT_WC] = (M[DT_WC] + 1) & 07777;
            M[DT_CA] = (M[DT_CA] + 1) & 07777;
            if (M[DT_WC] == 0) dt_substate = DTO_WCO;
            ma = DTB_GETMEX(dtsb) | M[DT_CA];
            if (relpos >= DT_HTLIN && relpos < DT_LPERB - DT_HTLIN) {
                wrd = DT_LIN2WD(dt_pos[u]);
                dat = dt_block(u, blk)[wrd];
                if (dir) dat = dt_comobv(dat);
            } else dat = dt_gethdr(u, blk, relpos, dir);
            ev_schedule(EV_DT0 + u, DT_WSIZE * dt_ltime);
            dt_store(ma, dat);
            if ((dtsa & DTA_MODE) == 0 || dt_substate == DTO_WCO) dtsb |= DTB_DTF;
            break;
        case DTO_WCO: case DTO_WCO | DTO_SOB:
            dt_schedez(u, dir);
            break;
        }
        break;

    case FNC_WALL:
        switch (dt_substate) {
        case 0: case DTO_SOB:
            if (dtsb & DTB_DTF) { dt_seterr(u, DTB_TIM); return; }
            relpos = DT_LIN2OF(dt_pos[u]);
            M[DT_WC] = (M[DT_WC] + 1) & 07777;
            M[DT_CA] = (M[DT_CA] + 1) & 07777;
            ma = DTB_GETMEX(dtsb) | M[DT_CA];
            if (relpos >= DT_HTLIN && relpos < DT_LPERB - DT_HTLIN) {
                dat = ma < PDP8_MEMSIZE ? M[ma] : 0;
                if (dir) dat = dt_comobv(dat);
                wrd = DT_LIN2WD(dt_pos[u]);
                dt_block(u, blk)[wrd] = (uint16_t)dat;
                dtc_dirty = true;
            }
            ev_schedule(EV_DT0 + u, DT_WSIZE * dt_ltime);
            if (M[DT_WC] == 0) dt_substate = DTO_WCO;
            if ((dtsa & DTA_MODE) == 0 || M[DT_WC] == 0) dtsb |= DTB_DTF;
            break;
        case DTO_WCO: case DTO_WCO | DTO_SOB:
            dt_schedez(u, dir);
            break;
        }
        break;

    default:
        dt_seterr(u, DTB_SEL);
        break;
    }
    dt_updint();
}

static uint32_t iot_dt76(uint32_t ir, uint32_t ac)
{
    uint32_t pulse = ir & 7, old = dtsa;
    if (pulse & 1) ac |= dtsa;                                   /* DTRA */
    if (pulse & 6) {
        if (pulse & 2) dtsa = 0;                                 /* DTCA */
        if (pulse & 4) {                                         /* DTXA */
            if ((ac & DTA_CERF) == 0) dtsb &= ~DTB_ALLERR;
            if ((ac & DTA_CDTF) == 0) dtsb &= ~DTB_DTF;
            dtsa ^= ac & DTA_RW;
            ac = 0;
        }
        if ((old ^ dtsa) & (07u << DTA_V_UNIT)) dt_deselect(old);
        int u = DTA_GETUNIT(dtsa);
        int fnc = DTA_GETFNC(dtsa);
        bool wp = host_dt_readonly(u);
        if (fnc >= FNC_WMRK || ((fnc == FNC_WALL || fnc == FNC_WRIT) && wp))
            dt_seterr(u, DTB_SEL);
        else dt_newsa(dtsa);
        dt_updint();
    }
    return ac;
}

static uint32_t iot_dt77(uint32_t ir, uint32_t ac, bool *skip)
{
    uint32_t pulse = ir & 7;
    if ((pulse & 1) && (dtsb & (DTB_ERF | DTB_DTF))) *skip = true;   /* DTSF */
    if (pulse & 2) ac |= dtsb;                                       /* DTRB */
    if (pulse & 4) { dtsb = (dtsb & ~DTB_MEX) | (ac & DTB_MEX); ac = 0; }  /* DTLB */
    return ac;
}

static void dt_reset(bool caf)
{
    for (int u = 0; u < DT_NUMDR; u++) {
        if (caf) {
            int prev = DTS_GETMOT(dt_state[u]);
            if ((prev & ~DTS_DIR) > DTS_DECF) {
                if (dt_setpos(u)) continue;
                ev_schedule(EV_DT0 + u, dt_dctime);
                DTS_SETSTA(u, DTS_DECF | (prev & DTS_DIR), 0);
            }
        } else {
            ev_time[EV_DT0 + u] = 0;
            dt_state[u] = 0;
            dt_lastt[u] = pdp8.icount;
            dt_pos[u] = DT_EZLIN;         /* as just mounted: beyond the leader */
            dt_offreel[u] = false;
        }
    }
    dtsa = dtsb = 0;
    dt_updint();
}

/* ---------------- RX8E / RX28 floppy (device 75) ----------------
 * After SIMH's pdp8_rx.c.  RX01 single density (256,256-byte images) on the
 * RX8E; with the RX28 (pdp8_rx_set_rx28) also RX02 double density (512,512
 * bytes), chosen per drive by the image size.  Sectors go straight to the
 * host; nothing is buffered beyond the controller's own sector buffer.
 */
#define RX_NUMTR        77
#define RX_NUMSC        26
#define RX_NUMBY        128
#define RX2_NUMBY       256
#define RX_SIZE         (RX_NUMTR * RX_NUMSC * RX_NUMBY)
#define RX2_SIZE        (RX_NUMTR * RX_NUMSC * RX2_NUMBY)

enum { RX_IDLE, RX_CMD8, RX_RWDS, RX_RWDT, RX_RWXFR, RX_FILL, RX_EMPTY, RX_SDCNF, RX_SDXFR,
       RX_CMD_COMPLETE, RX_INIT_COMPLETE };

#define RXCS_FILL       0
#define RXCS_EMPTY      1
#define RXCS_WRITE      2
#define RXCS_READ       3
#define RXCS_SDEN       4
#define RXCS_RXES       5
#define RXCS_WRDEL      6
#define RXCS_ECODE      7
#define RXCS_DRV        0020
#define RXCS_MODE       0100
#define RXCS_DEN        0400
#define RXCS_GETFNC(x)  (((x) >> 1) & 7)

#define RXES_ID         0004
#define RXES_RX02       0010
#define RXES_DERR       0020
#define RXES_DEN        0040
#define RXES_DD         0100
#define RXES_DRDY       0200

static bool     rx_28;
static int      rx_tr, rx_err, rx_state, rx_bptr;
static uint32_t rx_csr, rx_dbr, rx_esr, rx_ecode, rx_track, rx_sector;
static uint32_t rx_trk[2];
static uint8_t  rx_buf[RX2_NUMBY];
static const uint32_t rx_cwait = 100, rx_swait = 10, rx_xwait = 1;

void pdp8_rx_set_rx28(bool on) { rx_28 = on; }

static bool rx_den(int u) { return rx_28 && host_rx_bytes(u) > RX_SIZE; }

static uint32_t rx_read_dbr(uint32_t ac) { return (rx_csr & RXCS_MODE) ? (ac | (rx_dbr & 0377)) : rx_dbr; }

/* SIMH's sim_activate leaves an already scheduled unit alone */
static void rx_activate(int u, uint32_t d) { if (!ev_active(EV_RX0 + u)) ev_schedule(EV_RX0 + u, d); }

static void rx_done(uint32_t esr_flags, int new_ecode)
{
    int drv = (rx_csr & RXCS_DRV) ? 1 : 0;
    rx_state = RX_IDLE;
    dev_done |= INT_RX;
    int_update();
    rx_esr = (rx_esr | esr_flags) & ~(RXES_DRDY | RXES_RX02 | RXES_DEN);
    if (rx_28) rx_esr |= RXES_RX02;
    if (host_rx_present(drv)) {
        rx_esr |= RXES_DRDY;
        if (rx_den(drv)) rx_esr |= RXES_DEN;
    }
    if (new_ecode > 0) rx_err = 1;
    if (new_ecode < 0) return;
    rx_ecode = (uint32_t)new_ecode;
    rx_dbr = rx_esr;
}

static void rx_cmd(void)
{
    int drv = (rx_csr & RXCS_DRV) ? 1 : 0;
    switch (RXCS_GETFNC(rx_csr)) {
    case RXCS_FILL:
        rx_state = RX_FILL; rx_tr = 1; rx_esr &= RXES_ID; break;
    case RXCS_EMPTY:
        rx_state = RX_EMPTY; rx_esr &= RXES_ID; rx_activate(drv, rx_xwait); break;
    case RXCS_READ: case RXCS_WRITE: case RXCS_WRDEL:
        rx_state = RX_RWDS; rx_tr = 1; rx_esr &= RXES_ID; break;
    case RXCS_SDEN:
        if (rx_28) { rx_state = RX_SDCNF; rx_tr = 1; rx_esr &= RXES_ID; break; }
        /* fall through */
    default:
        rx_state = RX_CMD_COMPLETE; rx_activate(drv, rx_cwait); break;
    }
}

static void rx_reset(void);

static uint32_t iot_rx(uint32_t ir, uint32_t ac, bool *skip)
{
    int drv = (rx_csr & RXCS_DRV) ? 1 : 0;
    switch (ir & 7) {
    case 1:                                                      /* LCD */
        if (rx_state != RX_IDLE) return ac;
        dev_done &= ~INT_RX; int_req &= ~INT_RX;
        rx_tr = rx_err = 0;
        rx_bptr = 0;
        if (rx_28 && (ac & RXCS_MODE)) {
            rx_dbr = rx_csr = ac & 0377;
            rx_tr = 1;
            rx_state = RX_CMD8;
        } else {
            rx_dbr = rx_csr = ac;
            rx_cmd();
        }
        return 0;
    case 2:                                                      /* XDR */
        switch (rx_state & 017) {
        case RX_EMPTY:
            rx_activate(drv, rx_xwait);
            return rx_read_dbr(ac);
        case RX_CMD8:
            rx_dbr = ac & 0377;
            rx_csr = (rx_csr & 0377) | ((ac & 017) << 8);
            rx_cmd();
            break;
        case RX_RWDS: case RX_RWDT: case RX_FILL: case RX_SDCNF:
            rx_dbr = ac;
            rx_activate(drv, rx_xwait);
            break;
        default:
            return rx_read_dbr(ac);
        }
        break;
    case 3: if (rx_tr) { rx_tr = 0; *skip = true; } break;      /* STR */
    case 4: if (rx_err) { rx_err = 0; *skip = true; } break;    /* SER */
    case 5:                                                      /* SDN */
        if (dev_done & INT_RX) { dev_done &= ~INT_RX; int_req &= ~INT_RX; *skip = true; }
        break;
    case 6:                                                      /* INTR */
        if (ac & 1) int_enable |= INT_RX; else int_enable &= ~INT_RX;
        int_update();
        break;
    case 7: rx_reset(); break;                                   /* INIT */
    }
    return ac;
}

#define PTR12(x) ((((x) + (x) + (x)) >> 1))

static void rx_svc(int u)
{
    int bps = rx_den(u) ? RX2_NUMBY : RX_NUMBY, wps = bps / 2, func = RXCS_GETFNC(rx_csr), byptr;
    uint32_t da;
    switch (rx_state) {
    case RX_IDLE:
        return;
    case RX_EMPTY:
        if (rx_csr & RXCS_MODE) {
            if (rx_bptr >= bps) { rx_done(0, 0); break; }
            rx_dbr = rx_buf[rx_bptr];
        } else {
            byptr = PTR12(rx_bptr);
            if (rx_bptr >= wps) { rx_done(0, 0); break; }
            rx_dbr = (rx_bptr & 1) ? (uint32_t)(((rx_buf[byptr] & 017) << 8) | rx_buf[byptr + 1])
                                   : (uint32_t)((rx_buf[byptr] << 4) | ((rx_buf[byptr + 1] >> 4) & 017));
        }
        rx_bptr++;
        rx_tr = 1;
        break;
    case RX_FILL:
        if (rx_csr & RXCS_MODE) {
            rx_buf[rx_bptr++] = (uint8_t)rx_dbr;
            if (rx_bptr < bps) rx_tr = 1; else rx_done(0, 0);
        } else {
            byptr = PTR12(rx_bptr);
            if (rx_bptr & 1) {
                rx_buf[byptr] = (uint8_t)((rx_buf[byptr] & 0360) | ((rx_dbr >> 8) & 017));
                rx_buf[byptr + 1] = (uint8_t)(rx_dbr & 0377);
            } else {
                rx_buf[byptr] = (uint8_t)((rx_dbr >> 4) & 0377);
                rx_buf[byptr + 1] = (uint8_t)((rx_dbr & 017) << 4);
            }
            rx_bptr++;
            if (rx_bptr < wps) rx_tr = 1;
            else {
                for (int i = PTR12(wps); i < bps; i++) rx_buf[i] = 0;
                rx_done(0, 0);
            }
        }
        break;
    case RX_RWDS:
        rx_sector = rx_dbr & 0177;
        rx_tr = 1;
        rx_state = RX_RWDT;
        return;
    case RX_RWDT: {
        rx_track = rx_dbr & 0377;
        rx_state = RX_RWXFR;
        int32_t d = (int32_t)rx_track - (int32_t)rx_trk[u];
        rx_activate(u, rx_swait * (uint32_t)(d < 0 ? -d : d));
        return;
    }
    case RX_RWXFR:
        if (!host_rx_present(u)) { rx_done(0, 0110); return; }
        if (rx_track >= RX_NUMTR) { rx_done(0, 0040); break; }
        rx_trk[u] = rx_track;
        if (rx_sector == 0 || rx_sector > RX_NUMSC) { rx_done(0, 0070); break; }
        if (rx_28 && (rx_den(u) != ((rx_csr & RXCS_DEN) != 0))) { rx_done(RXES_DERR, 0240); break; }
        da = ((rx_track * RX_NUMSC) + (rx_sector - 1)) * (uint32_t)bps;
        if (func == RXCS_WRDEL) rx_esr |= RXES_DD;
        if (func == RXCS_READ) {
            if (!host_rx_read(u, da, rx_buf, (uint32_t)bps)) memset(rx_buf, 0, (size_t)bps);
        } else {
            if (host_rx_readonly(u)) { rx_done(0, 0100); break; }
            host_rx_write(u, da, rx_buf, (uint32_t)bps);
        }
        rx_done(0, 0);
        break;
    case RX_SDCNF:
        if ((rx_dbr & 0377) != 0111) { rx_done(0, 0250); break; }
        rx_state = RX_SDXFR;
        rx_activate(u, rx_cwait * 100);
        break;
    case RX_SDXFR:                                               /* format: erase, set density */
        if (!host_rx_format(u, (rx_csr & RXCS_DEN) ? RX2_SIZE : RX_SIZE)) { rx_done(0, 0100); break; }
        rx_done(0, 0);
        break;
    case RX_CMD_COMPLETE:
        if (func == RXCS_ECODE) { rx_dbr = rx_ecode; rx_done(0, -1); }
        else if (rx_28) {
            rx_esr &= ~RXES_DERR;
            if (rx_den(u) != ((rx_csr & RXCS_DEN) != 0)) rx_done(RXES_DERR, 0240);
            else rx_done(0, 0);
        } else rx_done(0, 0);
        break;
    case RX_INIT_COMPLETE:
        rx_trk[0] = 1;
        rx_trk[1] = 0;
        if (!host_rx_present(0)) { rx_done(RXES_ID, 0010); break; }
        da = (1 * RX_NUMSC) * (uint32_t)bps;                    /* track 1, sector 1 */
        if (!host_rx_read(0, da, rx_buf, (uint32_t)bps)) memset(rx_buf, 0, (size_t)bps);
        rx_done(RXES_ID, 0);
        if (!host_rx_present(1)) rx_ecode = 0020;
        break;
    }
}

static void rx_reset(void)
{
    rx_dbr = rx_csr = 0;
    rx_esr = rx_ecode = 0;
    rx_tr = rx_err = 0;
    rx_track = rx_sector = 0;
    rx_state = RX_IDLE;
    dev_done &= ~INT_RX;
    int_req &= ~INT_RX;
    int_enable &= ~INT_RX;
    ev_time[EV_RX1] = 0;
    if (host_rx_present(0)) {
        rx_state = RX_INIT_COMPLETE;
        int32_t d = 1 - (int32_t)rx_trk[0];
        rx_activate(0, rx_swait * (uint32_t)(d < 0 ? -d : d));
    } else rx_done(rx_esr | RXES_ID, 0010);
}

/* ---------------- TSC8-75 (device 36) ----------------
 * The time-share board ETOS needs on top of the KM8-E's user mode: in user
 * mode it records the trapped instruction (IOT, HLT/OSR, JMP, JMS) and, when
 * enabled, interrupts on JMP and JMS so the monitor can follow field changes.
 * After SIMH's pdp8_tsc.c (Bernhard Baehr's description of the board).
 */
static bool     tsc_present;
static uint32_t tsc_ir, tsc_pc;
static bool     tsc_cdf, tsc_enb;

void pdp8_set_tsc(bool on) { tsc_present = on; }

static uint32_t iot_tsc(uint32_t ir, uint32_t ac, bool *skip)
{
    switch (ir & 7) {
    case 0: tsc_enb = false; int_req &= ~INT_TSC; break;                     /* ETDS */
    case 1: if (int_req & INT_TSC) *skip = true; break;                      /* ESKP */
    case 2: int_req &= ~INT_TSC; break;                                      /* ECTF */
    case 3: ac |= (tsc_ir >> 3) & 7; if (tsc_cdf) *skip = true; tsc_cdf = false; break; /* ECDF */
    case 4: return tsc_pc;                                                   /* ERTB */
    case 5:                                                                  /* ESME */
        if (tsc_cdf && ((tsc_ir & 070) >> 3) == (pdp8.sf & 7)) { *skip = true; tsc_cdf = false; }
        break;
    case 6: return tsc_ir;                                                   /* ERIOT */
    case 7: tsc_enb = true; break;                                           /* ETEN */
    }
    return ac;
}

static void tsc_reset(void)
{
    tsc_ir = tsc_pc = 0; tsc_cdf = tsc_enb = false;
    int_req &= ~INT_TSC;
}

/* ---------------- device reset (CAF / power-on) ---------------- */
static void devices_reset(bool caf)
{
    tti_buf = 0; tto_buf = 0; lpt_buf = 0;
    dev_done = 0;
    int_req &= ~INT_DEV_ENABLE;
    int_enable = INT_INIT_ENABLE;              /* TTI/TTO/LPT enabled, clock disabled */
    ev_time[EV_TTO] = ev_time[EV_LPT] = ev_time[EV_PTR] = ev_time[EV_PTP] = 0;
    ptr_buf = ptp_buf = 0;
    rk_reset();
    ttx_reset();
    rf_reset();
    tsc_reset();
    dt_reset(caf);
    rx_reset();
    if (!ev_active(EV_POLL)) ev_schedule(EV_POLL, POLL_INTERVAL);
}

void pdp8_reset(void)
{
    memset(&pdp8, 0, sizeof pdp8);
    int_req = 0;
    memset(ev_time, 0, sizeof ev_time);
    ev_next = 1;
    devices_reset(false);
    pdp8_rf_invalidate();
    clk_last_ms = host_millis();
}

void pdp8_boot_rk(int unit)
{
    static const uint16_t boot[] = { 06007, 06744, 01032, 06746, 06743, 01032, 05031, 00000 };
    pdp8_reset();
    for (unsigned i = 0; i < sizeof boot / sizeof boot[0]; i++) M[023 + i] = boot[i];
    M[032] = (uint16_t)((unit & 3) << 1);
    pdp8.pc = 023;
}

void pdp8_boot_dt(void)                    /* TC08 bootstrap (SIMH's), from unit 0 */
{
    static const uint16_t boot[] = {
        07600, 01216, 04210, 01217, 03620, 01222, 04210, 05600, 00000, 06766,
        03621, 06771, 05213, 05610, 00600, 07577, 07755, 07754, 00220 };
    pdp8_reset();
    dt_pos[0] = DT_EZLIN;
    for (unsigned i = 0; i < sizeof boot / sizeof boot[0]; i++) M[0200 + i] = boot[i];
    pdp8.pc = 0200;
}

void pdp8_boot_rx(int unit)                /* RX8E / RX28 bootstrap (SIMH's) */
{
    static const uint16_t boot1[] = {
        06755, 05022, 07126, 01060, 06751, 07201, 04053, 04053, 07104, 06755,
        05054, 06754, 07450, 07610, 05046, 07402, 07402, 07402, 07402, 07402,
        06751, 04053, 03002, 02050, 05047, 00000, 06753, 05033, 06752, 05453,
        07024, 06030 };
    static const uint16_t boot2[] = {
        01061, 01046, 00060, 03061, 07327, 01061, 06751, 07201, 04053, 04053,
        07004, 06755, 05054, 06754, 07450, 05020, 01061, 06751, 01061, 00046,
        01032, 03060, 00360, 04053, 03002, 02050, 05047, 00000, 06753, 05033,
        06752, 05453, 00420, 00020 };
    pdp8_reset();
    if (rx_28) {
        for (unsigned i = 0; i < sizeof boot2 / sizeof boot2[0]; i++) M[020 + i] = boot2[i];
        pdp8.pc = 033;
    } else {
        for (unsigned i = 0; i < sizeof boot1 / sizeof boot1[0]; i++) M[022 + i] = boot1[i];
        M[060] = unit ? 07024 : 07004;
        pdp8.pc = 022;
    }
}

/* ---------------- event service ---------------- */
static void service_events(void)
{
    uint64_t now = pdp8.icount;
    for (int i = 0; i < EV_COUNT; i++) {
        if (ev_time[i] && ev_time[i] <= now) {
            ev_time[i] = 0;
            switch (i) {
            case EV_TTO: tto_svc(); break;
            case EV_LPT: lpt_svc(); break;
            case EV_RK0: case EV_RK1: case EV_RK2: case EV_RK3: rk_svc(i - EV_RK0); break;
            case EV_TTOX0: case EV_TTOX1: case EV_TTOX2: case EV_TTOX3: ttox_svc(i - EV_TTOX0); break;
            case EV_RF: rf_svc(); break;
            case EV_PTR: ptr_svc(); break;
            case EV_PTP: ptp_svc(); break;
            case EV_PCELL: pcell_svc(); break;
            case EV_DT0: case EV_DT1: case EV_DT2: case EV_DT3:
            case EV_DT4: case EV_DT5: case EV_DT6: case EV_DT7: dt_svc(i - EV_DT0); break;
            case EV_RX0: case EV_RX1: rx_svc(i - EV_RX0); break;
            case EV_POLL: {
                tti_poll();
                ttx_poll();
                uint32_t ms = host_millis();
                if ((uint32_t)(ms - clk_last_ms) >= 17) {   /* ~60 Hz */
                    clk_last_ms = ms;
                    pdp8_clock_tick();
                }
                ev_time[EV_POLL] = now + POLL_INTERVAL;
                break;
            }
            }
        }
    }
    ev_recalc();
}

/* ---------------- EAE (group 3 OPR) ---------------- */
static inline uint32_t eae_ind_addr(void)
{
    /* mode B memory operand: word after instruction is a pointer (field DF) */
    uint32_t ma = pdp8.ifld | pdp8.pc;
    if ((ma & 07770) != 00010) return pdp8.df | M[ma];
    M[ma] = (M[ma] + 1) & 07777;
    return pdp8.df | M[ma];
}

static void opr_group3(uint32_t ir)
{
    uint32_t temp = pdp8.mq;
    int32_t  st;
    uint32_t ma;
    uint32_t *LAC = &pdp8.lac;

    if (ir & 0200) *LAC &= 010000;                       /* CLA */
    if (ir & 0020) { pdp8.mq = *LAC & 07777; *LAC &= 010000; }  /* MQL */
    if (ir & 0100) *LAC |= temp;                         /* MQA */

    if (ir == 07431) { pdp8.emode = 1; return; }         /* SWAB */
    if (ir == 07447) { pdp8.emode = 0; pdp8.gtf = 0; return; } /* SWBA */
    if (pdp8.emode == 0) pdp8.gtf = 0;

    switch ((ir >> 1) & 027) {
    case 020: *LAC |= pdp8.sc; break;                    /* SCA */
    case 000: break;
    case 021:                                            /* DAD (B) */
        if (pdp8.emode) {
            ma = eae_ind_addr();
            pdp8.mq = pdp8.mq + M[ma];
            ma = pdp8.df | ((ma + 1) & 07777);
            *LAC = (*LAC & 07777) + M[ma] + (pdp8.mq >> 12);
            pdp8.mq &= 07777;
            pdp8.pc = (pdp8.pc + 1) & 07777;
            break;
        }
        *LAC |= pdp8.sc;
        /* fall through */
    case 001:                                            /* ACS (B) / SCL (A) */
        if (pdp8.emode) { pdp8.sc = *LAC & 037; *LAC &= 010000; }
        else { pdp8.sc = (~M[pdp8.ifld | pdp8.pc]) & 037; pdp8.pc = (pdp8.pc + 1) & 07777; }
        break;
    case 022:                                            /* DST (B) */
        if (pdp8.emode) {
            ma = eae_ind_addr();
            M[ma] = pdp8.mq & 07777;
            ma = pdp8.df | ((ma + 1) & 07777);
            M[ma] = *LAC & 07777;
            pdp8.pc = (pdp8.pc + 1) & 07777;
            break;
        }
        *LAC |= pdp8.sc;
        /* fall through */
    case 002:                                            /* MUY */
        ma = pdp8.emode ? eae_ind_addr() : (pdp8.ifld | pdp8.pc);
        temp = pdp8.mq * M[ma] + (*LAC & 07777);
        *LAC = (temp >> 12) & 07777;
        pdp8.mq = temp & 07777;
        pdp8.pc = (pdp8.pc + 1) & 07777;
        pdp8.sc = 014;
        break;
    case 023:                                            /* SWBA (B) handled above */
        if (pdp8.emode) break;
        *LAC |= pdp8.sc;
        /* fall through */
    case 003:                                            /* DVI */
        ma = pdp8.emode ? eae_ind_addr() : (pdp8.ifld | pdp8.pc);
        if ((*LAC & 07777) >= M[ma]) {
            *LAC |= 010000;
            pdp8.mq = ((pdp8.mq << 1) + 1) & 07777;
            pdp8.sc = 0;
        } else {
            temp = ((*LAC & 07777) << 12) | pdp8.mq;
            pdp8.mq = temp / M[ma];
            *LAC = temp % M[ma];
            pdp8.sc = 015;
        }
        pdp8.pc = (pdp8.pc + 1) & 07777;
        break;
    case 024:                                            /* DPSZ (B) */
        if (pdp8.emode) {
            if (((*LAC | pdp8.mq) & 07777) == 0) pdp8.pc = (pdp8.pc + 1) & 07777;
            break;
        }
        *LAC |= pdp8.sc;
        /* fall through */
    case 004:                                            /* NMI */
        temp = (*LAC << 12) | pdp8.mq;
        for (pdp8.sc = 0; (temp & 017777777) != 0 &&
             (temp & 040000000) == ((temp << 1) & 040000000); pdp8.sc++)
            temp <<= 1;
        *LAC = (temp >> 12) & 017777;
        pdp8.mq = temp & 07777;
        if (pdp8.emode && (*LAC & 07777) == 04000 && pdp8.mq == 0) *LAC &= 010000;
        break;
    case 025:                                            /* DPIC (B) */
        if (pdp8.emode) {
            temp = (*LAC + 1) & 07777;
            *LAC = pdp8.mq + (temp == 0);
            pdp8.mq = temp;
            break;
        }
        *LAC |= pdp8.sc;
        /* fall through */
    case 005:                                            /* SHL */
        pdp8.sc = (M[pdp8.ifld | pdp8.pc] & 037) + (pdp8.emode ^ 1);
        if (pdp8.sc > 25) temp = 0;
        else temp = ((*LAC << 12) | pdp8.mq) << pdp8.sc;
        *LAC = (temp >> 12) & 017777;
        pdp8.mq = temp & 07777;
        pdp8.pc = (pdp8.pc + 1) & 07777;
        pdp8.sc = pdp8.emode ? 037 : 0;
        break;
    case 026:                                            /* DCM (B) */
        if (pdp8.emode) {
            temp = (-(int32_t)*LAC) & 07777;
            *LAC = (pdp8.mq ^ 07777) + (temp == 0);
            pdp8.mq = temp;
            break;
        }
        *LAC |= pdp8.sc;
        /* fall through */
    case 006:                                            /* ASR */
        pdp8.sc = (M[pdp8.ifld | pdp8.pc] & 037) + (pdp8.emode ^ 1);
        st = (int32_t)(((*LAC & 07777) << 12) | pdp8.mq);
        if (*LAC & 04000) st |= ~037777777;
        if (pdp8.emode && pdp8.sc != 0) pdp8.gtf = (st >> (pdp8.sc - 1)) & 1;
        if (pdp8.sc > 25) st = (*LAC & 04000) ? -1 : 0;
        else st = st >> pdp8.sc;
        *LAC = ((uint32_t)st >> 12) & 017777;
        pdp8.mq = (uint32_t)st & 07777;
        pdp8.pc = (pdp8.pc + 1) & 07777;
        pdp8.sc = pdp8.emode ? 037 : 0;
        break;
    case 027:                                            /* SAM (B) */
        if (pdp8.emode) {
            temp = *LAC & 07777;
            *LAC = pdp8.mq + (temp ^ 07777) + 1;
            pdp8.gtf = (temp <= pdp8.mq) ^ ((temp ^ pdp8.mq) >> 11);
            break;
        }
        *LAC |= pdp8.sc;
        /* fall through */
    case 007:                                            /* LSR */
        pdp8.sc = (M[pdp8.ifld | pdp8.pc] & 037) + (pdp8.emode ^ 1);
        temp = ((*LAC & 07777) << 12) | pdp8.mq;
        if (pdp8.emode && pdp8.sc != 0) pdp8.gtf = (temp >> (pdp8.sc - 1)) & 1;
        if (pdp8.sc > 24) temp = 0;
        else temp >>= pdp8.sc;
        *LAC = (temp >> 12) & 07777;
        pdp8.mq = temp & 07777;
        pdp8.pc = (pdp8.pc + 1) & 07777;
        pdp8.sc = pdp8.emode ? 037 : 0;
        break;
    }
}

/* ---------------- IOT dispatch ---------------- */
static void do_iot(uint32_t ir)
{
    if (pdp8.uf) {                                        /* user mode trap */
        int_req |= INT_UF;
        tsc_ir = ir; tsc_cdf = (ir & 07707) == 06201;     /* TSC8-75 records it */
        return;
    }

    uint32_t dev = (ir >> 3) & 077, pulse = ir & 7;
    uint32_t ac = pdp8.lac & 07777;
    bool skip = false;

    switch (dev) {
    case 000:
        switch (pulse) {
        case 0: if (pdp8.ion) skip = true; pdp8.ion = false; break;          /* SKON */
        case 1: pdp8.ion = true; pdp8.ion_delay = true; break;               /* ION */
        case 2: pdp8.ion = false; break;                                     /* IOF */
        case 3: if (int_req) skip = true; break;                             /* SRQ */
        case 4:                                                              /* GTF */
            pdp8.lac = (pdp8.lac & 010000) | ((pdp8.lac & 010000) >> 1) |
                       (pdp8.gtf << 10) | ((int_req != 0) << 9) |
                       ((pdp8.ion ? 1u : 0u) << 7) | pdp8.sf;
            break;
        case 5:                                                              /* RTF */
            pdp8.gtf = (pdp8.lac & 02000) >> 10;
            pdp8.ub = (pdp8.lac & 0100) >> 6;
            pdp8.ib = (pdp8.lac & 0070) << 9;
            pdp8.df = (pdp8.lac & 0007) << 12;
            pdp8.lac = ((pdp8.lac & 04000) << 1) | ac;
            pdp8.ion = true;
            pdp8.cif_pending = true;
            break;
        case 6: if (pdp8.gtf) skip = true; break;                            /* SGT */
        case 7:                                                              /* CAF */
            pdp8_caf();
            break;
        }
        break;

    case 020: case 021: case 022: case 023:
    case 024: case 025: case 026: case 027:              /* KM8E memory extension */
        switch (pulse) {
        case 1: pdp8.df = (ir & 0070) << 9; break;                           /* CDF */
        case 2: pdp8.ib = (ir & 0070) << 9; pdp8.cif_pending = true; break;  /* CIF */
        case 3: pdp8.df = pdp8.ib = (ir & 0070) << 9; pdp8.cif_pending = true; break;
        case 4:
            switch (dev & 7) {
            case 0: int_req &= ~INT_UF; break;                               /* CINT */
            case 1: pdp8.lac |= pdp8.df >> 9; break;                         /* RDF */
            case 2: pdp8.lac |= pdp8.ifld >> 9; break;                       /* RIF */
            case 3: pdp8.lac |= pdp8.sf; break;                              /* RIB */
            case 4:                                                          /* RMF */
                pdp8.ub = (pdp8.sf & 0100) >> 6;
                pdp8.ib = (pdp8.sf & 0070) << 9;
                pdp8.df = (pdp8.sf & 0007) << 12;
                pdp8.cif_pending = true;
                break;
            case 5: if (int_req & INT_UF) skip = true; break;                /* SINT */
            case 6: pdp8.ub = 0; pdp8.cif_pending = true; break;             /* CUF */
            case 7: pdp8.ub = 1; pdp8.cif_pending = true; break;             /* SUF */
            }
            break;
        }
        break;

    case 003: pdp8.lac = (pdp8.lac & 010000) | (iot_tti(ir, ac, &skip) & 07777); break;
    case 004: pdp8.lac = (pdp8.lac & 010000) | (iot_tto(ir, ac, &skip) & 07777); break;
    case 001: pdp8.lac = (pdp8.lac & 010000) | (iot_ptr(ir, ac, &skip) & 07777); break;
    case 002: pdp8.lac = (pdp8.lac & 010000) | (iot_ptp(ir, ac, &skip) & 07777); break;
    case 013: pdp8.lac = (pdp8.lac & 010000) | (iot_clk(ir, ac, &skip) & 07777); break;
    case 066: pdp8.lac = (pdp8.lac & 010000) | (iot_lpt(ir, ac, &skip) & 07777); break;
    case 074: pdp8.lac = (pdp8.lac & 010000) | (iot_rk(ir, ac, &skip) & 07777); break;
    case 040: case 041: case 042: case 043: case 044: case 045: case 046: case 047:
        pdp8.lac = (pdp8.lac & 010000) | (iot_ttx(ir, ac, &skip) & 07777); break;
    case 075: pdp8.lac = (pdp8.lac & 010000) | (iot_rx(ir, ac, &skip) & 07777); break;
    case 076: pdp8.lac = (pdp8.lac & 010000) | (iot_dt76(ir, ac) & 07777); break;
    case 077: pdp8.lac = (pdp8.lac & 010000) | (iot_dt77(ir, ac, &skip) & 07777); break;
    case 036:
        if (tsc_present) pdp8.lac = (pdp8.lac & 010000) | (iot_tsc(ir, ac, &skip) & 07777);
        break;
    case 060: case 061: case 062: case 064:
        pdp8.lac = (pdp8.lac & 010000) | (iot_rf(ir, ac, &skip) & 07777); break;
    default: break;                                       /* no such device: no-op */
    }
    if (skip) pdp8.pc = (pdp8.pc + 1) & 07777;
}

/* ---------------- front panel ---------------- */
void pdp8_caf(void)
{
    pdp8.gtf = 0; pdp8.emode = 0;
    pdp8.ion = false; pdp8.ion_delay = false;
    int_req = 0;
    pdp8.lac = 0;
    devices_reset(true);
}

uint32_t pdp8_status_word(void)
{
    /* PDP-8/E STATUS: L, GT, INT BUS, NO INT, ION, USER, IF0-2, DF0-2 */
    return ((pdp8.lac >> 1) & 04000) | (pdp8.gtf << 10) | ((int_req != 0) << 9) |
           ((pdp8.cif_pending || pdp8.ion_delay) << 8) | ((pdp8.ion ? 1u : 0u) << 7) |
           (pdp8.uf << 6) | (pdp8.ifld >> 9) | (pdp8.df >> 12);
}

static bool lamps_on;
static uint32_t lamp_cd = 1, lamp_lfsr = 0xace1u;
static pdp8_lamps_t lamps;

static inline void lamp_bits(uint32_t *c, uint32_t v, int n)
{
    for (int i = 0; i < n; i++)
        c[i] += (v >> (n - 1 - i)) & 1;
}

static void lamps_sample(void)
{
    /* next sample in 32..63 instructions, jittered so it can't lock onto a loop */
    lamp_lfsr = (lamp_lfsr >> 1) ^ (-(lamp_lfsr & 1u) & 0xb400u);
    lamp_cd = 32 + (lamp_lfsr & 31);
    lamps.samples++;
    lamp_bits(lamps.ma, pdp8.ma & 077777, 15);
    lamp_bits(lamps.ac, pdp8.lac & 017777, 13);
    lamp_bits(lamps.mq, pdp8.mq, 12);
    lamp_bits(lamps.md, pdp8.md, 12);
    lamp_bits(lamps.ir, pdp8.ir, 12);
    lamp_bits(lamps.st, pdp8_status_word(), 12);
    lamps.ion += pdp8.ion;
    lamp_bits(lamps.pc, pdp8.pc & 07777, 12);
    lamp_bits(lamps.sc, pdp8.sc & 037, 5);
    lamp_bits(lamps.ifl, pdp8.ifld >> 12, 3);
    lamp_bits(lamps.dfl, pdp8.df >> 12, 3);
    uint32_t op = (pdp8.ir >> 9) & 7;
    lamps.op[op]++;
    lamps.fetch++;
    if (op < 6) {                                   /* memory reference */
        if (pdp8.ir & 0400) lamps.defer++;
        if (op != 5) lamps.exec++;                  /* JMP has no execute cycle */
    }
}

void pdp8_lamps_enable(bool on) { lamps_on = on; lamp_cd = 1; }

void pdp8_lamps_take(pdp8_lamps_t *out)
{
    *out = lamps;
    memset(&lamps, 0, sizeof lamps);
}

/* ---------------- main loop ---------------- */
uint32_t pdp8_run(uint32_t max)
{
    uint32_t n = 0;
    if (pdp8.halted) return 0;

    while (n < max) {
        uint32_t ma = pdp8.ifld | pdp8.pc;
        uint32_t ir = M[ma];
#ifdef PDP8_TRACE
        { extern uint32_t pdp8_trace[256][3]; extern uint32_t pdp8_trace_n;
          uint32_t *t = pdp8_trace[pdp8_trace_n++ & 255]; t[0] = ma; t[1] = ir; t[2] = pdp8.lac | (pdp8.df << 1); }
#endif
        uint32_t ea, mb;
        pdp8.pc = (pdp8.pc + 1) & 07777;
        pdp8.ir = ir;
        pdp8.ma = ma;
        pdp8.md = ir;
        bool ion_delay_was = pdp8.ion_delay;
        pdp8.ion_delay = false;

        switch (ir >> 9) {
        case 0: case 1: case 2: case 3:                    /* AND TAD ISZ DCA */
            ea = (ir & 0200) ? ((ma & 07600) | (ir & 0177)) : (ir & 0177);
            if (ir & 0400) {
                ea |= pdp8.ifld;
                mb = M[ea];
                if ((ea & 07770) == 00010) { mb = (mb + 1) & 07777; M[ea] = mb; }
                ea = pdp8.df | mb;
            } else ea |= pdp8.ifld;
            switch (ir >> 9) {
            case 0: pdp8.lac &= (M[ea] | 010000); break;
            case 1: pdp8.lac = (pdp8.lac + M[ea]) & 017777; break;
            case 2:
                mb = (M[ea] + 1) & 07777; M[ea] = mb;
                if (mb == 0) pdp8.pc = (pdp8.pc + 1) & 07777;
                break;
            case 3: M[ea] = pdp8.lac & 07777; pdp8.lac &= 010000; break;
            }
            pdp8.ma = ea;
            pdp8.md = M[ea];
            break;

        case 4:                                            /* JMS */
        case 5:                                            /* JMP */
            ea = (ir & 0200) ? ((ma & 07600) | (ir & 0177)) : (ir & 0177);
            if (ir & 0400) {
                uint32_t pa = pdp8.ifld | ea;
                mb = M[pa];
                if ((pa & 07770) == 00010) { mb = (mb + 1) & 07777; M[pa] = mb; }
                ea = mb;
            }
            if (pdp8.uf) {                             /* TSC8-75 */
                tsc_ir = ir; tsc_cdf = false;
                if (tsc_enb) {
                    tsc_pc = (pdp8.pc - 1) & 07777;
                    int_req |= INT_TSC;
                    if ((ir >> 9) == 4) {              /* trapped JMS: just go to target+1 */
                        pdp8.pc = (ea + 1) & 07777;
                        pdp8.ma = pdp8.ifld | ea;
                        pdp8.md = M[pdp8.ma];
                        break;
                    }
                }
            }
            pdp8.ifld = pdp8.ib;
            pdp8.uf = pdp8.ub;
            pdp8.cif_pending = false;
            if ((ir >> 9) == 4) {
                M[pdp8.ifld | ea] = (uint16_t)pdp8.pc;
                pdp8.pc = (ea + 1) & 07777;
            } else {
                pdp8.pc = ea;
            }
            pdp8.ma = pdp8.ifld | ea;
            pdp8.md = M[pdp8.ma];
            break;

        case 6:                                            /* IOT */
            do_iot(ir);
            break;

        case 7:                                            /* OPR */
            if (!(ir & 0400)) {                            /* group 1 */
                uint32_t L = pdp8.lac;
                if (ir & 0200) L &= 010000;
                if (ir & 0100) L &= 007777;
                if (ir & 0040) L ^= 007777;
                if (ir & 0020) L ^= 010000;
                if (ir & 0001) L = (L + 1) & 017777;
                switch (ir & 016) {
                case 002: L = (L & 010000) | ((L >> 6) & 077) | ((L & 077) << 6); break; /* BSW */
                case 004: L = ((L << 1) | (L >> 12)) & 017777; break;               /* RAL */
                case 006: L = ((L << 2) | (L >> 11)) & 017777; break;               /* RTL */
                case 010: L = ((L >> 1) | (L << 12)) & 017777; break;               /* RAR */
                case 012: L = ((L >> 2) | (L << 11)) & 017777; break;               /* RTR */
                case 014: L = L & (ir | 010000); break;
                case 016: L = (L & 010000) | (ma & 07600) | (ir & 0177); break;
                }
                pdp8.lac = L;
            } else if (!(ir & 1)) {                        /* group 2 */
                uint32_t L = pdp8.lac;
                bool cond;
                bool sma = ir & 0100, sza = ir & 0040, snl = ir & 0020;
                cond = (sma && (L & 04000)) || (sza && (L & 07777) == 0) || (snl && (L & 010000));
                if (ir & 0010) cond = !cond;               /* reverse sense */
                if (cond) pdp8.pc = (pdp8.pc + 1) & 07777;
                if (ir & 0200) pdp8.lac &= 010000;         /* CLA */
                if (ir & 06) {
                    if (pdp8.uf) { int_req |= INT_UF; tsc_ir = ir; tsc_cdf = false; }
                    else {
                        if (ir & 04) pdp8.lac |= pdp8.sr;  /* OSR */
                        if (ir & 02) {                     /* HLT */
                            pdp8.halted = true;
                            pdp8.halt_pc = pdp8.ifld | ((pdp8.pc - 1) & 07777);
                        }
                    }
                }
            } else {
                opr_group3(ir);
            }
            break;
        }

        n++;
        pdp8.icount++;
        if (lamps_on && --lamp_cd == 0) lamps_sample();
        if (pdp8.icount >= ev_next) service_events();

        /* interrupt? */
        if (pdp8.ion && !pdp8.ion_delay && !pdp8.cif_pending && int_req) {
            pdp8.ion = false;
            pdp8.sf = (pdp8.uf << 6) | (pdp8.ifld >> 9) | (pdp8.df >> 12);
            pdp8.ifld = pdp8.ib = pdp8.df = pdp8.uf = pdp8.ub = 0;
            M[0] = (uint16_t)pdp8.pc;
            pdp8.pc = 1;
        }
        (void)ion_delay_was;
        if (pdp8.halted) break;
    }
    return n;
}
