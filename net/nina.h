/*
 * nina.h - driver for an ESP32 running Adafruit's nina-fw ("AirLift"), as on
 * the Fruit Jam's ESP32-C6.  The SPI command protocol follows nina-fw's
 * CommandHandler and Adafruit's ESP32SPI library.
 *
 * Each command is two SPI transactions: the command, then the reply.  Before
 * each, wait for the ESP's BUSY/READY line to go low, assert CS, and wait for
 * the ESP to raise it again as an acknowledgement.
 */
#ifndef NINA_H
#define NINA_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- hardware hooks (platform) ---- */
void     nina_hw_cs(bool asserted);          /* true = CS low */
bool     nina_hw_busy(void);                 /* BUSY/READY line high */
void     nina_hw_write(const uint8_t *buf, size_t n);
void     nina_hw_read(uint8_t *buf, size_t n);   /* clocks out 0xFF */
void     nina_hw_reset(bool asserted);       /* true = reset held low */
uint32_t nina_hw_us(void);

/* ---- constants ---- */
#define NINA_WL_IDLE        0
#define NINA_WL_NO_SSID     1
#define NINA_WL_CONNECTED   3
#define NINA_WL_FAILED      4
#define NINA_WL_LOST        5
#define NINA_WL_DISCONNECTED 6
#define NINA_NO_SOCKET      255
#define NINA_ESTABLISHED    4

bool nina_ok(void);                           /* last command completed */
int  nina_status(void);                       /* WL_* or -1 */
bool nina_join(const char *ssid, const char *pass);
bool nina_set_hostname(const char *name);
bool nina_ip(uint8_t ip[4]);
bool nina_fw_version(char *out, size_t n);
int  nina_get_socket(void);                   /* socket number or NINA_NO_SOCKET / -1 */
bool nina_start_server(uint16_t port, uint8_t sock);
int  nina_accept(uint8_t server_sock);        /* new client socket, NINA_NO_SOCKET, or -1 */
int  nina_available(uint8_t sock);            /* bytes waiting, or -1 */
int  nina_read(uint8_t sock, uint8_t *buf, uint16_t max);
int  nina_write(uint8_t sock, const uint8_t *buf, uint16_t n);
int  nina_client_state(uint8_t sock);         /* NINA_ESTABLISHED when connected */
bool nina_stop(uint8_t sock);

#endif
