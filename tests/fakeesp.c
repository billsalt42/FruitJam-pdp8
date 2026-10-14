/*
 * fakeesp.c - a software ESP32 running nina-fw, for testing net/nina.c and
 * net/wifi.c on the desktop.  It implements the nina_hw_* hooks: SPI bytes,
 * CS, the BUSY/READY handshake line and reset, and answers the commands the
 * driver uses exactly as nina-fw's CommandHandler.cpp does (same byte
 * offsets, little-endian quirks and reply padding), backed by real TCP sockets.
 *
 * WiFi network "testnet" with password "testpass" joins about a second after
 * SET_PASSPHRASE; any other SSID gives WL_NO_SSID_AVAIL, a wrong password
 * WL_CONNECT_FAILED.  Every malformed command packet aborts the program.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "../net/nina.h"

#define MAX_SOCKETS 6
enum { NO_MODE = 255, TCP_MODE = 0 };

static uint8_t sock_type[MAX_SOCKETS] = { NO_MODE, NO_MODE, NO_MODE, NO_MODE, NO_MODE, NO_MODE };
static int     srv_fd[MAX_SOCKETS] = { -1, -1, -1, -1, -1, -1 };
static int     cli_fd[MAX_SOCKETS] = { -1, -1, -1, -1, -1, -1 };

static bool    cs, in_reset = true, ble_mode, listening;
static uint64_t boot_done_us, ready_at_us, join_at_us;
static int     wl_status = 0, join_result = 0;
static uint8_t cmd[4096], resp[4096];
static size_t  cmd_len, resp_len, resp_pos;
static bool    have_resp;                    /* next transaction is the reply */
static long    commands;

static uint64_t us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
uint32_t nina_hw_us(void) { return (uint32_t)us(); }

static void die(const char *why)
{
    fprintf(stderr, "\r\nFAKE ESP: %s (command %ld)\r\n", why, commands);
    for (size_t i = 0; i < cmd_len && i < 48; i++) fprintf(stderr, "%02X ", cmd[i]);
    fprintf(stderr, "\r\n");
    exit(3);
}

void nina_hw_reset(bool asserted)
{
    if (asserted) { in_reset = true; return; }
    if (in_reset) {
        in_reset = false;
        ble_mode = cs;                       /* nina-fw: CS low at boot selects Bluetooth mode */
        boot_done_us = us() + 300000;
        have_resp = false;
        wl_status = 0;
        for (int i = 0; i < MAX_SOCKETS; i++) {
            if (srv_fd[i] >= 0) close(srv_fd[i]);
            if (cli_fd[i] >= 0) close(cli_fd[i]);
            srv_fd[i] = cli_fd[i] = -1;
            sock_type[i] = NO_MODE;
        }
    }
}

/* READY/BUSY: low while a transaction is queued and waiting for CS; high once
 * CS falls, and while the command is being processed. */
bool nina_hw_busy(void)
{
    if (in_reset || ble_mode || us() < boot_done_us) return true;
    if (cs) return true;
    return us() < ready_at_us;
}

static bool connected(int fd)
{
    uint8_t b;
    ssize_t n = recv(fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n > 0) return true;
    if (n == 0) return false;
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

/* the handlers: offsets as in nina-fw CommandHandler.cpp */
static int handle(void)
{
    const uint8_t *c = cmd;
    uint8_t *r = resp;
    switch (c[1]) {
    case 0x11: {                                                     /* setPassPhrase */
        char ssid[33] = { 0 }, pass[65] = { 0 };
        memcpy(ssid, &c[4], c[3]);
        memcpy(pass, &c[5 + c[3]], c[4 + c[3]]);
        wl_status = 0;                                              /* WL_IDLE */
        join_at_us = us() + 1000000;
        join_result = strcmp(ssid, "testnet") ? NINA_WL_NO_SSID :
                      strcmp(pass, "testpass") ? NINA_WL_FAILED : NINA_WL_CONNECTED;
        r[2] = 1; r[3] = 1; r[4] = 1;
        return 6;
    }
    case 0x16:                                                       /* setHostname */
        r[2] = 1; r[3] = 1; r[4] = 1;
        return 6;
    case 0x20:                                                       /* getConnStatus */
        if (join_at_us && us() >= join_at_us) { wl_status = join_result; join_at_us = 0; }
        r[2] = 1; r[3] = 1; r[4] = (uint8_t)wl_status;
        return 6;
    case 0x21: {                                                     /* getIPaddr */
        uint8_t ip[4] = { 127, 0, 0, 1 }, mask[4] = { 255, 0, 0, 0 }, gw[4] = { 127, 0, 0, 1 };
        r[2] = 3;
        r[3] = 4; memcpy(&r[4], ip, 4);
        r[8] = 4; memcpy(&r[9], mask, 4);
        r[13] = 4; memcpy(&r[14], gw, 4);
        return 19;
    }
    case 0x37: {                                                     /* getFwVersion */
        static const char v[] = "3.3.0";
        r[2] = 1; r[3] = sizeof v; memcpy(&r[4], v, sizeof v);
        return 5 + sizeof v;
    }
    case 0x3F: {                                                     /* getSocket */
        uint8_t res = 255;
        for (int i = 0; i < MAX_SOCKETS; i++) if (sock_type[i] == NO_MODE) { res = (uint8_t)i; break; }
        r[2] = 1; r[3] = 1; r[4] = res;
        return 6;
    }
    case 0x28: {                                                     /* startServerTcp */
        if (c[2] != 3) die("startServerTcp: expected 3 params");
        uint16_t port = (uint16_t)(c[4] << 8 | c[5]);
        uint8_t s = c[7], type = c[9];
        r[2] = 1; r[3] = 1; r[4] = 0;
        if (type == TCP_MODE && s < MAX_SOCKETS) {
            int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
            if (bind(fd, (struct sockaddr *)&a, sizeof a) == 0 && listen(fd, 4) == 0) {
                fcntl(fd, F_SETFL, O_NONBLOCK);
                srv_fd[s] = fd;
                sock_type[s] = TCP_MODE;
                r[4] = 1;
                listening = true;
            } else close(fd);
        }
        return 6;
    }
    case 0x2B: {                                                     /* availDataTcp */
        uint8_t s = c[4];
        uint16_t avail = 0;
        if (s >= MAX_SOCKETS) die("availDataTcp: bad socket");
        if (sock_type[s] == TCP_MODE) {
            if (srv_fd[s] >= 0) {
                uint8_t acc = c[6];
                avail = 255;
                int fd = accept(srv_fd[s], NULL, NULL);
                if (fd >= 0) {
                    int one = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    fcntl(fd, F_SETFL, O_NONBLOCK);
                    for (int i = 0; i < MAX_SOCKETS; i++) {
                        if (!acc && i == s) continue;
                        if (sock_type[i] == NO_MODE) { sock_type[i] = TCP_MODE; cli_fd[i] = fd; avail = (uint16_t)i; break; }
                        if (acc) break;              /* accept branch only tries the first free slot */
                    }
                    if (avail == 255) close(fd);     /* no slot: the client object is dropped */
                }
            } else if (cli_fd[s] >= 0) {
                int n = 0;
                uint8_t tmp[4096];
                ssize_t k = recv(cli_fd[s], tmp, sizeof tmp, MSG_PEEK | MSG_DONTWAIT);
                if (k > 0) n = (int)k;
                avail = (uint16_t)n;
            }
        }
        r[2] = 1; r[3] = 2; memcpy(&r[4], &avail, 2);               /* host (little-endian) order */
        return 7;
    }
    case 0x45: {                                                     /* getDataBufTcp */
        uint8_t s = c[5];
        uint16_t len;
        memcpy(&len, &c[8], 2);
        if (c[3] != 0 || c[4] != 1 || c[6] != 0 || c[7] != 2) die("getDataBufTcp: bad parameter lengths");
        int got = 0;
        if (s < MAX_SOCKETS && sock_type[s] == TCP_MODE && cli_fd[s] >= 0) {
            ssize_t k = recv(cli_fd[s], &r[5], len, MSG_DONTWAIT);
            got = k > 0 ? (int)k : 0;
        }
        r[2] = 1; r[3] = (uint8_t)(got >> 8); r[4] = (uint8_t)got;
        return 6 + got;
    }
    case 0x44: {                                                     /* sendDataTcp */
        uint8_t s = c[5];
        uint16_t len = (uint16_t)(c[6] << 8 | c[7]);
        if (c[3] != 0 || c[4] != 1) die("sendDataTcp: bad socket parameter");
        uint16_t written = 0;
        if (s < MAX_SOCKETS && sock_type[s] == TCP_MODE && cli_fd[s] >= 0) {
            ssize_t k = send(cli_fd[s], &c[8], len, MSG_NOSIGNAL);
            written = k > 0 ? (uint16_t)k : 0;
        }
        r[2] = 1; r[3] = 2; memcpy(&r[4], &written, 2);
        return 7;
    }
    case 0x2F: {                                                     /* getClientStateTcp */
        uint8_t s = c[4];
        r[2] = 1; r[3] = 1;
        r[4] = (s < MAX_SOCKETS && sock_type[s] == TCP_MODE && cli_fd[s] >= 0 && connected(cli_fd[s])) ? 4 : 0;
        return 6;
    }
    case 0x2E: {                                                     /* stopClientTcp */
        uint8_t s = c[4];
        if (s < MAX_SOCKETS) {
            if (cli_fd[s] >= 0) close(cli_fd[s]);
            cli_fd[s] = -1;
            sock_type[s] = NO_MODE;
        }
        r[2] = 1; r[3] = 1; r[4] = 1;
        return 6;
    }
    default:
        return 0;
    }
}

/* check the packet is well formed: params, END_CMD in place, padded to 4 */
static void validate(void)
{
    if (cmd_len < 4 || cmd[0] != 0xE0) die("bad start");
    bool len16 = cmd[1] == 0x44 || cmd[1] == 0x45;
    size_t k = 3;
    for (int i = 0; i < cmd[2]; i++) {
        size_t l = cmd[k++];
        if (len16) l = l << 8 | cmd[k++];
        k += l;
        if (k >= cmd_len) die("parameter runs past the packet");
    }
    if (cmd[k] != 0xEE) die("END_CMD missing");
    if (cmd_len % 4) die("packet not padded to 4 bytes");
    for (size_t i = k + 1; i < cmd_len; i++) if (cmd[i]) die("non-zero padding");
}

void nina_hw_cs(bool asserted)
{
    if (asserted == cs) return;
    if (asserted && nina_hw_busy() && !in_reset) die("CS asserted while the ESP was busy");
    cs = asserted;
    if (asserted) {
        if (have_resp) resp_pos = 0;
        else cmd_len = 0;
        return;
    }
    if (in_reset) return;
    if (have_resp) {                         /* reply read: back to waiting for a command */
        have_resp = false;
        ready_at_us = us() + 30;
        return;
    }
    commands++;
    validate();
    memset(resp, 0, sizeof resp);
    int n = handle();
    if (n == 0) { resp[0] = 0xEF; resp[1] = 0; resp[2] = 0xEE; n = 3; }
    else { resp[0] = 0xE0; resp[1] = cmd[1] | 0x80; resp[n - 1] = 0xEE; }
    resp_len = (size_t)((n + 3) & ~3);
    have_resp = true;
    ready_at_us = us() + 50 + (commands % 7) * 40;     /* processing time */
}

void nina_hw_write(const uint8_t *buf, size_t n)
{
    if (!cs) die("SPI write without CS");
    if (have_resp) return;                   /* ESP ignores MOSI during its reply */
    if (cmd_len + n > sizeof cmd) die("command too long");
    memcpy(cmd + cmd_len, buf, n);
    cmd_len += n;
}

void nina_hw_read(uint8_t *buf, size_t n)
{
    if (!cs) die("SPI read without CS");
    for (size_t i = 0; i < n; i++) {
        if (have_resp && resp_pos < sizeof resp) buf[i] = resp[resp_pos++];
        else buf[i] = 0;
        if (!have_resp && cmd_len < sizeof cmd) cmd[cmd_len++] = 0xFF;
    }
}
