/*
 * sshsrv.h - SSH sessions as PDP-8 terminals.
 *
 * Each SSH login is attached to a terminal line: under TSS/8 the first free
 * multiplexer line K01-K04 (or the one named by the user name: k01..k04,
 * k00/console for the console), under MULTOS/8 and ETOS likewise among the
 * lines they were built for; under plain OS/8 the console.
 *
 * The platform supplies the network (a listening TCP socket) and the
 * terminal lines through the sshp_* functions below.
 */
#ifndef SSHSRV_H
#define SSHSRV_H
#include <stdint.h>
#include <stdbool.h>

#define SSHSRV_MAX   5          /* simultaneous connections */
#define SSHSRV_LINES 5          /* 0 = console, 1-4 = K01-K04 */

enum { SSHP_SYS_NONE = 0, SSHP_SYS_OS8 = 1, SSHP_SYS_TSS = 2, SSHP_SYS_MULTOS = 3, SSHP_SYS_ETOS = 4 };
/* OS8: console only.  TSS: TSS/8 (DEC or UWM), lines K01-K04.  MULTOS: MULTOS/8
 * time-sharing OS/8, and ETOS, with sshp_rk_lines() terminal lines from K01 up. */

/* ---- platform ---- */
int      sshp_accept(void);                          /* new connection handle, or -1 */
int      sshp_recv(int s, uint8_t *buf, int max);    /* >0 bytes, 0 none yet, <0 closed */
int      sshp_send(int s, const uint8_t *buf, int n);/* bytes taken (may be 0), <0 error */
void     sshp_close(int s);
uint32_t sshp_millis(void);
int      sshp_system(void);
int      sshp_rk_lines(void);                        /* lines MULTOS/8 or ETOS was built for */
bool     sshp_line_free(int line);                   /* not in use by other means (USB port) */
int      sshp_line_room(int line);                   /* keystrokes the line can take now */
void     sshp_line_key(int line, uint8_t c);

/* ---- server ---- */
void sshsrv_init(const uint8_t hostkey_seed[32], const char *password);
void sshsrv_poll(void);                     /* call often (every few ms) */
void sshsrv_line_out(int line, uint8_t c);  /* character the PDP-8 printed on a line */
bool sshsrv_line_busy(int line);            /* an SSH session owns the line */
int  sshsrv_sessions(void);                 /* connections open */
void sshsrv_drop_all(const char *why);

#endif
