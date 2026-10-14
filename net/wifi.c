/*
 * wifi.c - WiFi + SSH listener on a nina-fw coprocessor (see wifi.h).
 */
#include <stdio.h>
#include <string.h>
#include "wifi.h"
#include "nina.h"
#include "sshsrv.h"

enum { W_OFF, W_RESET, W_BOOT, W_JOIN, W_WAIT, W_UP, W_RETRY };

static int      state = W_OFF;
static uint32_t t_next, t_join;
static char     ssid[33], pass[65], host[33], fw[24];
static uint16_t port;
static int      server = -1;
static uint8_t  ip[4];
static char     status[96] = "WiFi off (set wifi_ssid in PDP8.CFG)";
static int      failures;
static uint32_t now;
static uint32_t state_check[16];

void wifi_start(const char *s, const char *p, const char *h, uint16_t pt)
{
    snprintf(ssid, sizeof ssid, "%s", s);
    snprintf(pass, sizeof pass, "%s", p);
    snprintf(host, sizeof host, "%s", h && *h ? h : "pdp8");
    port = pt ? pt : 22;
    state = W_RESET;
    t_next = 0;
    snprintf(status, sizeof status, "WiFi starting");
}

void wifi_stop(void)
{
    state = W_OFF;
    nina_hw_reset(true);
    snprintf(status, sizeof status, "WiFi off");
}

bool wifi_up(void) { return state == W_UP; }
const char *wifi_status(void) { return status; }
const char *wifi_firmware(void) { return fw; }

static void retry(uint32_t delay_ms, const char *why)
{
    snprintf(status, sizeof status, "%s; retrying", why);
    state = W_RETRY;
    t_next = now + delay_ms;
    server = -1;
}

void wifi_poll(uint32_t t)
{
    now = t;
    if (state == W_OFF || (int32_t)(now - t_next) < 0) return;
    switch (state) {
    case W_RESET:                                  /* CS high during reset, or it boots into BLE mode */
        nina_hw_cs(false);
        nina_hw_reset(true);
        t_next = now + 20;
        state = W_BOOT;
        break;
    case W_BOOT:
        nina_hw_reset(false);
        state = W_JOIN;
        t_next = now + 1000;                       /* boot time */
        break;
    case W_JOIN:
        if (!nina_fw_version(fw, sizeof fw)) {
            fw[0] = 0;
            if (++failures >= 3) { failures = 0; state = W_RESET; t_next = now + 30000; snprintf(status, sizeof status, "WiFi module not responding; retrying"); }
            else t_next = now + 1000;
            break;
        }
        failures = 0;
        nina_set_hostname(host);
        if (!nina_join(ssid, pass)) { retry(10000, "WiFi join command failed"); break; }
        snprintf(status, sizeof status, "joining WiFi network \"%s\"", ssid);
        state = W_WAIT;
        t_join = now;
        t_next = now + 500;
        break;
    case W_WAIT: {
        int st = nina_status();
        t_next = now + 500;
        if (st == NINA_WL_CONNECTED) {
            int sock = nina_get_socket();
            if (!nina_ip(ip) || sock < 0 || sock == NINA_NO_SOCKET || !nina_start_server(port, (uint8_t)sock)) {
                retry(10000, "could not start the SSH listener");
                break;
            }
            server = sock;
            state = W_UP;
            t_next = now + 3000;
            if (port == 22)
                snprintf(status, sizeof status, "WiFi \"%s\": ssh %u.%u.%u.%u", ssid, ip[0], ip[1], ip[2], ip[3]);
            else
                snprintf(status, sizeof status, "WiFi \"%s\": ssh -p %u %u.%u.%u.%u", ssid, port, ip[0], ip[1], ip[2], ip[3]);
        } else if (st < 0) {
            state = W_RESET;
            t_next = now + 5000;
            snprintf(status, sizeof status, "WiFi module stopped answering; resetting it");
        } else if (now - t_join > 30000) {
            retry(15000, st == NINA_WL_NO_SSID ? "WiFi network not found" :
                         st == NINA_WL_FAILED ? "WiFi password rejected or join failed" : "WiFi join timed out");
        }
        break;
    }
    case W_UP: {
        int st = nina_status();
        t_next = now + 3000;
        if (st != NINA_WL_CONNECTED) {
            sshsrv_drop_all("WiFi connection lost");
            if (st < 0) { state = W_RESET; t_next = now + 1000; snprintf(status, sizeof status, "WiFi module stopped answering; resetting it"); }
            else retry(5000, "WiFi connection lost");
        }
        break;
    }
    case W_RETRY:
        state = W_JOIN;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------ sshsrv network hooks */
int sshp_accept(void)
{
    if (state != W_UP || server < 0) return -1;
    int s = nina_accept((uint8_t)server);
    if (s < 0 || s == NINA_NO_SOCKET || s >= 16) return -1;
    state_check[s] = sshp_millis();
    return s;
}

int sshp_recv(int s, uint8_t *buf, int max)
{
    int a = nina_available((uint8_t)s);
    if (a < 0) return -1;
    if (a == 0) {
        uint32_t t = sshp_millis();
        if (t - state_check[s] >= 1000) {          /* closed by the client? */
            state_check[s] = t;
            int st = nina_client_state((uint8_t)s);
            if (st != NINA_ESTABLISHED) return -1;
        }
        return 0;
    }
    if (a > max) a = max;
    return nina_read((uint8_t)s, buf, (uint16_t)a);
}

int sshp_send(int s, const uint8_t *buf, int n)
{
    if (n > 1000) n = 1000;
    return nina_write((uint8_t)s, buf, (uint16_t)n);
}

void sshp_close(int s) { nina_stop((uint8_t)s); }
