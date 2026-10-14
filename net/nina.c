/*
 * nina.c - nina-fw (Adafruit AirLift) SPI protocol, see nina.h.
 *
 * Packet:  E0 cmd nparams { len data }... EE, padded to a multiple of 4.
 * Reply:   E0 cmd|80 nparams { len data }... EE   (EF = error)
 * Lengths are one byte, or two (big-endian) for the data commands
 * SEND_DATA_TCP (44) and GET_DATABUF_TCP (45).
 */
#include <string.h>
#include "nina.h"

#define START_CMD 0xE0
#define END_CMD   0xEE
#define ERR_CMD   0xEF
#define REPLY     0x80

enum {
    SET_PASSPHRASE = 0x11, SET_HOSTNAME = 0x16,
    GET_CONN_STATUS = 0x20, GET_IPADDR = 0x21,
    START_SERVER_TCP = 0x28, AVAIL_DATA_TCP = 0x2B,
    STOP_CLIENT_TCP = 0x2E, GET_CLIENT_STATE_TCP = 0x2F,
    GET_FW_VERSION = 0x37, GET_SOCKET = 0x3F,
    SEND_DATA_TCP = 0x44, GET_DATABUF_TCP = 0x45,
};

typedef struct { const void *p; uint16_t n; } param_t;

static bool last_ok = true;
static uint8_t pkt[1100];

bool nina_ok(void) { return last_ok; }

static bool wait_busy(bool level, uint32_t timeout_us)
{
    uint32_t t0 = nina_hw_us();
    while (nina_hw_busy() != level)
        if (nina_hw_us() - t0 > timeout_us) return false;
    return true;
}

/* select the ESP: wait until it is ready, assert CS, wait for its acknowledgement */
static bool select(void)
{
    if (!wait_busy(false, 250000)) return false;          /* ESP normally ready within a few ms */
    nina_hw_cs(true);
    if (!wait_busy(true, 100000)) { nina_hw_cs(false); return false; }
    return true;
}

static uint8_t rd(void)
{
    uint8_t b;
    nina_hw_read(&b, 1);
    return b;
}

/* Send a command and parse the reply.  Reply parameters are packed one after
 * the other into out (each truncated to fit); lens[] gets their real lengths.
 * Returns the number of reply parameters, or -1. */
static int command(uint8_t cmd, const param_t *pa, int np, bool len16,
                   uint8_t *out, size_t out_max, uint16_t *lens, int max_params, bool rlen16)
{
    size_t k = 0;
    pkt[k++] = START_CMD;
    pkt[k++] = cmd & ~REPLY;
    pkt[k++] = (uint8_t)np;
    for (int i = 0; i < np; i++) {
        if (k + pa[i].n + 3 > sizeof pkt) { last_ok = false; return -1; }
        if (len16) pkt[k++] = (uint8_t)(pa[i].n >> 8);
        pkt[k++] = (uint8_t)pa[i].n;
        memcpy(pkt + k, pa[i].p, pa[i].n);
        k += pa[i].n;
    }
    pkt[k++] = END_CMD;
    while (k % 4) pkt[k++] = 0;

    if (!select()) { last_ok = false; return -1; }
    nina_hw_write(pkt, k);
    nina_hw_cs(false);

    if (!select()) { last_ok = false; return -1; }
    int n = -1;
    uint8_t b = 0;
    for (int tries = 0; tries < 16; tries++) {
        b = rd();
        if (b == START_CMD || b == ERR_CMD) break;
    }
    if (b == START_CMD && rd() == (cmd | REPLY)) {
        int nparams = rd();
        size_t used = 0;
        n = 0;
        for (int i = 0; i < nparams; i++) {
            uint16_t len = rd();
            if (rlen16) len = (uint16_t)((len << 8) | rd());
            for (uint16_t j = 0; j < len; j++) {
                uint8_t v = rd();
                if (used < out_max) out[used++] = v;
            }
            if (i < max_params) lens[i] = len;
            n++;
        }
        if (rd() != END_CMD) n = -1;
    }
    nina_hw_cs(false);
    last_ok = n >= 0;
    return n;
}

/* one-byte answer commands */
static int simple(uint8_t cmd, const param_t *pa, int np)
{
    uint8_t r[4];
    uint16_t l[1];
    int n = command(cmd, pa, np, false, r, sizeof r, l, 1, false);
    return n >= 1 && l[0] >= 1 ? r[0] : -1;
}

int nina_status(void) { return simple(GET_CONN_STATUS, NULL, 0); }

bool nina_join(const char *ssid, const char *pass)
{
    param_t p[2] = { { ssid, (uint16_t)strlen(ssid) }, { pass, (uint16_t)strlen(pass) } };
    return simple(SET_PASSPHRASE, p, 2) == 1;
}

bool nina_set_hostname(const char *name)
{
    param_t p[1] = { { name, (uint16_t)strlen(name) } };
    return simple(SET_HOSTNAME, p, 1) == 1;
}

bool nina_ip(uint8_t ip[4])
{
    static const uint8_t dummy = 0xFF;
    param_t p[1] = { { &dummy, 1 } };
    uint8_t r[12];
    uint16_t l[3];
    int n = command(GET_IPADDR, p, 1, false, r, sizeof r, l, 3, false);
    if (n < 1 || l[0] != 4) return false;
    memcpy(ip, r, 4);
    return true;
}

bool nina_fw_version(char *out, size_t sz)
{
    uint8_t r[32];
    uint16_t l[1];
    int n = command(GET_FW_VERSION, NULL, 0, false, r, sizeof r, l, 1, false);
    if (n < 1 || !sz) return false;
    size_t i = 0;
    for (; i + 1 < sz && i < l[0] && i < sizeof r && r[i]; i++) out[i] = (char)r[i];
    out[i] = 0;
    return true;
}

int nina_get_socket(void) { return simple(GET_SOCKET, NULL, 0); }

bool nina_start_server(uint16_t port, uint8_t sock)
{
    uint8_t pb[2] = { (uint8_t)(port >> 8), (uint8_t)port }, mode = 0;   /* TCP_MODE */
    param_t p[3] = { { pb, 2 }, { &sock, 1 }, { &mode, 1 } };
    return simple(START_SERVER_TCP, p, 3) == 1;
}

static int avail16(uint8_t sock, bool server)
{
    uint8_t zero = 0, r[4];
    uint16_t l[1];
    param_t p[2] = { { &sock, 1 }, { &zero, 1 } };   /* server: accept flag 0, as CircuitPython */
    int n = command(AVAIL_DATA_TCP, p, server ? 2 : 1, false, r, sizeof r, l, 1, false);
    if (n < 1 || l[0] != 2) return -1;
    return r[0] | r[1] << 8;                          /* little-endian */
}

int nina_accept(uint8_t server_sock) { return avail16(server_sock, true); }
int nina_available(uint8_t sock) { return avail16(sock, false); }

int nina_read(uint8_t sock, uint8_t *buf, uint16_t max)
{
    uint8_t sz[2] = { (uint8_t)max, (uint8_t)(max >> 8) };   /* little-endian */
    param_t p[2] = { { &sock, 1 }, { sz, 2 } };
    uint16_t l[1];
    int n = command(GET_DATABUF_TCP, p, 2, true, buf, max, l, 1, true);
    if (n < 1) return -1;
    return l[0] > max ? max : l[0];
}

int nina_write(uint8_t sock, const uint8_t *buf, uint16_t n)
{
    param_t p[2] = { { &sock, 1 }, { buf, n } };
    uint8_t r[4];
    uint16_t l[1];
    int k = command(SEND_DATA_TCP, p, 2, true, r, sizeof r, l, 1, false);
    if (k < 1) return -1;
    return l[0] >= 2 ? (r[0] | r[1] << 8) : r[0];
}

int nina_client_state(uint8_t sock)
{
    param_t p[1] = { { &sock, 1 } };
    return simple(GET_CLIENT_STATE_TCP, p, 1);
}

bool nina_stop(uint8_t sock)
{
    param_t p[1] = { { &sock, 1 } };
    return simple(STOP_CLIENT_TCP, p, 1) == 1;
}
