/*
 * wifi.h - joins the WiFi network through the nina-fw coprocessor and runs
 * the SSH listening socket; also provides the network half of the sshsrv
 * platform hooks (sshp_accept/recv/send/close) on top of nina.c.
 */
#ifndef WIFI_H
#define WIFI_H
#include <stdint.h>
#include <stdbool.h>

void wifi_start(const char *ssid, const char *password, const char *hostname, uint16_t port);
void wifi_stop(void);
void wifi_poll(uint32_t now_ms);          /* call often; does a little work each time */
bool wifi_up(void);                       /* joined and listening */
const char *wifi_status(void);            /* one line for the menu */
const char *wifi_firmware(void);          /* nina-fw version, "" if unknown */

#endif
