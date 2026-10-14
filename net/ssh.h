/*
 * ssh.h - a small SSH-2 server for terminal sessions, transport-agnostic.
 *
 * One algorithm set, the one modern OpenSSH, PuTTY etc. all speak:
 *   key exchange  curve25519-sha256 (+ @libssh.org alias), strict kex
 *   host key      ssh-ed25519
 *   cipher        chacha20-poly1305@openssh.com
 *   auth          password (one shared password)
 *   channel       one "session" with a pty and a shell
 *
 * The caller owns the TCP connection: it feeds received bytes in with
 * ssh_feed(), sends whatever ssh_out_peek() returns, and exchanges terminal
 * data with ssh_read()/ssh_write().  No dynamic memory; each connection is
 * a fixed ssh_conn_t.  Crypto: Monocypher (lib/monocypher) and sha256.c.
 */
#ifndef SSH_H
#define SSH_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "sha256.h"

#define SSH_IN_MAX   6144      /* largest packet accepted (OpenSSH KEXINIT ~1.5 KB) */
#define SSH_OUT_MAX  6144
#define SSH_APP_IN   512       /* terminal input buffer = channel window we grant */
#define SSH_USER_MAX 32

/* supplied by the platform: cryptographically strong random bytes */
void ssh_random(uint8_t *buf, size_t n);

typedef struct {
    int      state;
    /* transport */
    uint8_t  in[SSH_IN_MAX];
    size_t   in_len;
    uint8_t  out[SSH_OUT_MAX];
    size_t   out_len;
    uint32_t seq_in, seq_out;
    bool     enc_in, enc_out;
    uint8_t  k_in[64], k_out[64];
    /* key exchange */
    char     v_c[256];
    size_t   v_c_len;
    uint8_t  i_s[400];
    size_t   i_s_len;
    bool     kexinit_sent, kexinit_rcvd, ecdh_done, newkeys_sent, kex_done_once;
    bool     strict, ignore_next;
    sha256_ctx h;
    uint8_t  session_id[32];
    uint8_t  nk_in[64], nk_out[64];
    /* authentication */
    bool     authed;
    int      auth_fails;
    char     user[SSH_USER_MAX + 1];
    /* channel */
    bool     chan_open, shell, chan_closed;
    uint32_t peer_chan, peer_window, peer_maxpkt;
    uint32_t my_window, consumed;
    uint8_t  app[SSH_APP_IN];
    uint16_t app_head, app_tail, app_count;
    uint32_t term_w, term_h;
    char     close_msg[64];
} ssh_conn_t;

enum { SSH_ST_FREE = 0, SSH_ST_VERSION, SSH_ST_RUN, SSH_ST_CLOSING, SSH_ST_DEAD };

/* server-wide setup: host key seed (32 random bytes kept on storage) and password */
void ssh_server_init(const uint8_t hostkey_seed[32], const char *password);
void ssh_hostkey_fingerprint(char out[64]);        /* "SHA256:..." as OpenSSH shows it */

void   ssh_start(ssh_conn_t *c);                   /* new TCP connection accepted */
size_t ssh_in_room(const ssh_conn_t *c);
void   ssh_feed(ssh_conn_t *c, const uint8_t *data, size_t n);  /* n <= ssh_in_room() */
void   ssh_service(ssh_conn_t *c);                 /* call periodically */
size_t ssh_out_peek(ssh_conn_t *c, const uint8_t **p);
void   ssh_out_consume(ssh_conn_t *c, size_t n);

bool   ssh_shell_ready(const ssh_conn_t *c);       /* logged in, shell started */
const char *ssh_user(const ssh_conn_t *c);
int    ssh_read(ssh_conn_t *c, uint8_t *buf, int n);          /* terminal input */
size_t ssh_write(ssh_conn_t *c, const uint8_t *data, size_t n); /* returns bytes taken */
void   ssh_end_session(ssh_conn_t *c, const char *why); /* close the shell (client exits normally) */
void   ssh_disconnect(ssh_conn_t *c, const char *why);  /* drop the connection; then wait for ssh_finished */
void   ssh_ping(ssh_conn_t *c);                     /* harmless packet, to notice a vanished client */
bool   ssh_finished(const ssh_conn_t *c);          /* nothing more to send: close the socket */

#endif
