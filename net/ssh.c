/*
 * ssh.c - small SSH-2 server (see ssh.h).  RFC 4253/4252/4254, RFC 8731
 * (curve25519-sha256), RFC 8709 (ssh-ed25519), OpenSSH PROTOCOL.chacha20poly1305
 * and the strict key exchange extension (CVE-2023-48795 countermeasure).
 */
#include <string.h>
#include <stdio.h>
#include "ssh.h"
#include "../lib/monocypher/monocypher.h"
#include "../lib/monocypher/monocypher-ed25519.h"

#define VERSION_S "SSH-2.0-PDP8_FruitJam_1.0"

enum {
    MSG_DISCONNECT = 1, MSG_IGNORE = 2, MSG_UNIMPLEMENTED = 3, MSG_DEBUG = 4,
    MSG_SERVICE_REQUEST = 5, MSG_SERVICE_ACCEPT = 6, MSG_EXT_INFO = 7,
    MSG_KEXINIT = 20, MSG_NEWKEYS = 21, MSG_KEX_ECDH_INIT = 30, MSG_KEX_ECDH_REPLY = 31,
    MSG_USERAUTH_REQUEST = 50, MSG_USERAUTH_FAILURE = 51, MSG_USERAUTH_SUCCESS = 52,
    MSG_GLOBAL_REQUEST = 80, MSG_REQUEST_FAILURE = 82,
    MSG_CHANNEL_OPEN = 90, MSG_CHANNEL_OPEN_CONFIRMATION = 91, MSG_CHANNEL_OPEN_FAILURE = 92,
    MSG_CHANNEL_WINDOW_ADJUST = 93, MSG_CHANNEL_DATA = 94, MSG_CHANNEL_EXTENDED_DATA = 95,
    MSG_CHANNEL_EOF = 96, MSG_CHANNEL_CLOSE = 97, MSG_CHANNEL_REQUEST = 98,
    MSG_CHANNEL_SUCCESS = 99, MSG_CHANNEL_FAILURE = 100,
};
enum { DIS_PROTOCOL_ERROR = 2, DIS_KEX_FAILED = 3, DIS_MAC_ERROR = 5, DIS_SERVICE_NOT_AVAILABLE = 7,
       DIS_BY_APPLICATION = 11, DIS_TOO_MANY = 12, DIS_NO_MORE_AUTH = 14 };

#define OUT_RESERVE 1200         /* room kept free for protocol replies */
#define MAX_DATA    1024         /* largest CHANNEL_DATA we send */

static uint8_t host_sk[64], host_pk[32];
static char    server_password[65];

/* ------------------------------------------------------------ helpers */
typedef struct { uint8_t *p; size_t len, cap; bool err; } wbuf;
typedef struct { const uint8_t *p; size_t len, pos; bool err; } rbuf;

static void put(wbuf *b, const void *d, size_t n)
{
    if (b->err || b->len + n > b->cap) { b->err = true; return; }
    memcpy(b->p + b->len, d, n);
    b->len += n;
}
static void put_u8(wbuf *b, uint8_t v) { put(b, &v, 1); }
static void put_u32(wbuf *b, uint32_t v)
{
    uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    put(b, x, 4);
}
static void put_str(wbuf *b, const void *d, size_t n) { put_u32(b, (uint32_t)n); put(b, d, n); }
static void put_cstr(wbuf *b, const char *s) { put_str(b, s, strlen(s)); }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static uint8_t get_u8(rbuf *r)
{
    if (r->pos + 1 > r->len) { r->err = true; return 0; }
    return r->p[r->pos++];
}
static uint32_t get_u32(rbuf *r)
{
    if (r->pos + 4 > r->len) { r->err = true; return 0; }
    uint32_t v = be32(r->p + r->pos);
    r->pos += 4;
    return v;
}
static const uint8_t *get_str(rbuf *r, size_t *n)
{
    uint32_t l = get_u32(r);
    if (r->err || l > r->len - r->pos) { r->err = true; *n = 0; return (const uint8_t *)""; }
    const uint8_t *p = r->p + r->pos;
    r->pos += l;
    *n = l;
    return p;
}
static bool str_is(const uint8_t *p, size_t n, const char *s) { return n == strlen(s) && !memcmp(p, s, n); }

/* is name in the comma-separated list? */
static bool list_has(const uint8_t *list, size_t n, const char *name)
{
    size_t k = strlen(name), i = 0;
    while (i <= n) {
        size_t j = i;
        while (j < n && list[j] != ',') j++;
        if (j - i == k && !memcmp(list + i, name, k)) return true;
        i = j + 1;
    }
    return false;
}
/* first entry of a name-list equals name? */
static bool list_first_is(const uint8_t *list, size_t n, const char *name)
{
    size_t j = 0;
    while (j < n && list[j] != ',') j++;
    return j == strlen(name) && !memcmp(list, name, j);
}

static void hash_str(sha256_ctx *h, const void *d, size_t n)
{
    uint8_t l[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    sha256_update(h, l, 4);
    sha256_update(h, d, n);
}

/* shared secret as an SSH mpint (RFC 8731: the 32 bytes as a big-endian number) */
static size_t mpint32(const uint8_t k[32], uint8_t out[37])
{
    int i = 0;
    while (i < 32 && k[i] == 0) i++;
    size_t n = 32 - i, o = 4;
    if (n && (k[i] & 0x80)) { out[o++] = 0; }
    memcpy(out + o, k + i, n);
    size_t len = (o - 4) + n;
    out[0] = (uint8_t)(len >> 24); out[1] = (uint8_t)(len >> 16); out[2] = (uint8_t)(len >> 8); out[3] = (uint8_t)len;
    return 4 + len;
}

/* ------------------------------------------------------------ server setup */
void ssh_server_init(const uint8_t seed[32], const char *password)
{
    uint8_t s[32];
    memcpy(s, seed, 32);
    crypto_ed25519_key_pair(host_sk, host_pk, s);       /* wipes s */
    snprintf(server_password, sizeof server_password, "%s", password ? password : "");
}

static size_t hostkey_blob(uint8_t out[51])
{
    wbuf b = { out, 0, 51, false };
    put_cstr(&b, "ssh-ed25519");
    put_str(&b, host_pk, 32);
    return b.len;
}

void ssh_hostkey_fingerprint(char out[64])
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t blob[51], h[32];
    sha256(blob, hostkey_blob(blob), h);
    char *o = out + sprintf(out, "SHA256:");
    for (int i = 0; i < 32; i += 3) {
        uint32_t v = (uint32_t)h[i] << 16 | (i + 1 < 32 ? (uint32_t)h[i + 1] << 8 : 0) | (i + 2 < 32 ? h[i + 2] : 0);
        *o++ = b64[(v >> 18) & 63];
        *o++ = b64[(v >> 12) & 63];
        if (i + 1 < 32) *o++ = b64[(v >> 6) & 63];
        if (i + 2 < 32) *o++ = b64[v & 63];
    }
    *o = 0;
}

/* ------------------------------------------------------------ packets out */
static void fatal(ssh_conn_t *c, uint32_t reason, const char *why);
static bool in_kex(const ssh_conn_t *c);
static void chan_reply(ssh_conn_t *c, uint8_t type);

static bool send_packet(ssh_conn_t *c, const uint8_t *payload, size_t n)
{
    size_t aad = c->enc_out ? 4 : 0;
    size_t pad = 8 - ((4 - aad + 1 + n) % 8);
    if (pad < 4) pad += 8;
    size_t body = 1 + n + pad;                         /* padding_length + payload + padding */
    size_t total = 4 + body + (c->enc_out ? 16 : 0);
    if (c->out_len + total > SSH_OUT_MAX) return false;
    uint8_t *p = c->out + c->out_len;
    p[0] = (uint8_t)(body >> 24); p[1] = (uint8_t)(body >> 16); p[2] = (uint8_t)(body >> 8); p[3] = (uint8_t)body;
    p[4] = (uint8_t)pad;
    memcpy(p + 5, payload, n);
    ssh_random(p + 5 + n, pad);
    if (c->enc_out) {
        uint8_t nonce[8] = { 0, 0, 0, 0, (uint8_t)(c->seq_out >> 24), (uint8_t)(c->seq_out >> 16),
                             (uint8_t)(c->seq_out >> 8), (uint8_t)c->seq_out };
        uint8_t polykey[32] = { 0 };
        crypto_chacha20_djb(p, p, 4, c->k_out + 32, nonce, 0);          /* length: header key */
        crypto_chacha20_djb(polykey, polykey, 32, c->k_out, nonce, 0);
        crypto_chacha20_djb(p + 4, p + 4, body, c->k_out, nonce, 1);
        crypto_poly1305(p + 4 + body, p, 4 + body, polykey);
        crypto_wipe(polykey, sizeof polykey);
    }
    c->out_len += total;
    c->seq_out++;
    return true;
}

static void send_or_die(ssh_conn_t *c, wbuf *b)
{
    if (b->err || !send_packet(c, b->p, b->len)) {
        c->state = SSH_ST_DEAD;                        /* can't even report it */
        c->out_len = 0;
    }
}

static void fatal(ssh_conn_t *c, uint32_t reason, const char *why)
{
    if (c->state == SSH_ST_RUN) {
        uint8_t m[128];
        wbuf b = { m, 0, sizeof m, false };
        put_u8(&b, MSG_DISCONNECT);
        put_u32(&b, reason);
        put_cstr(&b, why);
        put_cstr(&b, "");
        send_packet(c, b.p, b.len);
    }
    snprintf(c->close_msg, sizeof c->close_msg, "%s", why);
    c->state = SSH_ST_CLOSING;
}

void ssh_end_session(ssh_conn_t *c, const char *why)
{
    snprintf(c->close_msg, sizeof c->close_msg, "%s", why);
    if (c->state == SSH_ST_RUN && c->chan_open && !c->chan_closed && !in_kex(c)) {
        uint8_t m[32];
        wbuf b = { m, 0, sizeof m, false };
        put_u8(&b, MSG_CHANNEL_REQUEST);
        put_u32(&b, c->peer_chan);
        put_cstr(&b, "exit-status");
        put_u8(&b, 0);
        put_u32(&b, 0);
        send_or_die(c, &b);
        chan_reply(c, MSG_CHANNEL_EOF);
        chan_reply(c, MSG_CHANNEL_CLOSE);
        c->chan_closed = true;                           /* client answers CLOSE, then we disconnect */
    } else {
        ssh_disconnect(c, why);
    }
}

void ssh_disconnect(ssh_conn_t *c, const char *why)
{
    if (c->state == SSH_ST_VERSION || c->state == SSH_ST_RUN) fatal(c, DIS_BY_APPLICATION, why);
}

/* ------------------------------------------------------------ key exchange */
static const char *KEX_ALGS = "curve25519-sha256,curve25519-sha256@libssh.org,kex-strict-s-v00@openssh.com";

static void send_kexinit(ssh_conn_t *c)
{
    wbuf b = { c->i_s, 0, sizeof c->i_s, false };
    uint8_t cookie[16];
    ssh_random(cookie, 16);
    put_u8(&b, MSG_KEXINIT);
    put(&b, cookie, 16);
    put_cstr(&b, KEX_ALGS);
    put_cstr(&b, "ssh-ed25519");
    put_cstr(&b, "chacha20-poly1305@openssh.com");
    put_cstr(&b, "chacha20-poly1305@openssh.com");
    put_cstr(&b, "hmac-sha2-256");
    put_cstr(&b, "hmac-sha2-256");
    put_cstr(&b, "none");
    put_cstr(&b, "none");
    put_cstr(&b, "");
    put_cstr(&b, "");
    put_u8(&b, 0);
    put_u32(&b, 0);
    c->i_s_len = b.len;
    send_or_die(c, &b);
    c->kexinit_sent = true;
}

static bool in_kex(const ssh_conn_t *c) { return c->kexinit_sent || c->kexinit_rcvd; }

static void on_kexinit(ssh_conn_t *c, const uint8_t *pl, size_t n)
{
    if (c->kexinit_rcvd) { fatal(c, DIS_PROTOCOL_ERROR, "unexpected KEXINIT"); return; }
    rbuf r = { pl, n, 17, false };                     /* skip type + cookie */
    size_t ln[10];
    const uint8_t *l[10];
    for (int i = 0; i < 10; i++) l[i] = get_str(&r, &ln[i]);
    bool follows = get_u8(&r) != 0;
    if (r.err) { fatal(c, DIS_PROTOCOL_ERROR, "bad KEXINIT"); return; }

    bool kex_ok = list_has(l[0], ln[0], "curve25519-sha256") || list_has(l[0], ln[0], "curve25519-sha256@libssh.org");
    if (!kex_ok || !list_has(l[1], ln[1], "ssh-ed25519") ||
        !list_has(l[2], ln[2], "chacha20-poly1305@openssh.com") ||
        !list_has(l[3], ln[3], "chacha20-poly1305@openssh.com") ||
        !list_has(l[6], ln[6], "none") || !list_has(l[7], ln[7], "none")) {
        fatal(c, DIS_KEX_FAILED, "no matching algorithms (need curve25519, ed25519, chacha20-poly1305)");
        return;
    }
    if (!c->kex_done_once) c->strict = list_has(l[0], ln[0], "kex-strict-c-v00@openssh.com");
    if (follows) {
        bool right = (list_first_is(l[0], ln[0], "curve25519-sha256") ||
                      list_first_is(l[0], ln[0], "curve25519-sha256@libssh.org")) &&
                     list_first_is(l[1], ln[1], "ssh-ed25519");
        c->ignore_next = !right;
    }
    c->kexinit_rcvd = true;
    if (!c->kexinit_sent) send_kexinit(c);               /* client-initiated rekey */

    sha256_init(&c->h);
    hash_str(&c->h, c->v_c, c->v_c_len);
    hash_str(&c->h, VERSION_S, strlen(VERSION_S));
    hash_str(&c->h, pl, n);
    hash_str(&c->h, c->i_s, c->i_s_len);
}

static void derive(ssh_conn_t *c, const uint8_t *kmp, size_t kmpn, const uint8_t H[32], char letter, uint8_t out[64])
{
    sha256_ctx h;
    sha256_init(&h);
    sha256_update(&h, kmp, kmpn);
    sha256_update(&h, H, 32);
    sha256_update(&h, &letter, 1);
    sha256_update(&h, c->session_id, 32);
    sha256_final(&h, out);
    sha256_init(&h);
    sha256_update(&h, kmp, kmpn);
    sha256_update(&h, H, 32);
    sha256_update(&h, out, 32);
    sha256_final(&h, out + 32);
}

static void on_ecdh_init(ssh_conn_t *c, const uint8_t *pl, size_t n)
{
    rbuf r = { pl, n, 1, false };
    size_t qn;
    const uint8_t *qc = get_str(&r, &qn);
    if (!c->kexinit_rcvd || c->ecdh_done || r.err || qn != 32) { fatal(c, DIS_KEX_FAILED, "bad ECDH_INIT"); return; }

    uint8_t sec[32], qs[32], k[32];
    ssh_random(sec, 32);
    crypto_x25519_public_key(qs, sec);
    crypto_x25519(k, sec, qc);
    crypto_wipe(sec, 32);
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= k[i];
    if (!acc) { fatal(c, DIS_KEX_FAILED, "bad curve25519 point"); return; }

    uint8_t ks[51], kmp[37], H[32];
    size_t ksn = hostkey_blob(ks), kmpn = mpint32(k, kmp);
    crypto_wipe(k, 32);
    hash_str(&c->h, ks, ksn);
    hash_str(&c->h, qc, 32);
    hash_str(&c->h, qs, 32);
    sha256_update(&c->h, kmp, kmpn);
    sha256_final(&c->h, H);
    if (!c->kex_done_once) memcpy(c->session_id, H, 32);

    static uint8_t sig[64];                          /* statics: keep the stack small (4 KB on a Pico) */
    crypto_ed25519_sign(sig, host_sk, H, 32);
    static uint8_t sigblob[4 + 11 + 4 + 64];
    wbuf sb = { sigblob, 0, sizeof sigblob, false };
    put_cstr(&sb, "ssh-ed25519");
    put_str(&sb, sig, 64);

    static uint8_t m[256];
    wbuf b = { m, 0, sizeof m, false };
    put_u8(&b, MSG_KEX_ECDH_REPLY);
    put_str(&b, ks, ksn);
    put_str(&b, qs, 32);
    put_str(&b, sigblob, sb.len);
    send_or_die(c, &b);

    derive(c, kmp, kmpn, H, 'C', c->nk_in);              /* client to server key */
    derive(c, kmp, kmpn, H, 'D', c->nk_out);             /* server to client key */
    crypto_wipe(kmp, sizeof kmp);
    c->ecdh_done = true;

    uint8_t nk = MSG_NEWKEYS;
    if (!send_packet(c, &nk, 1)) { c->state = SSH_ST_DEAD; return; }
    memcpy(c->k_out, c->nk_out, 64);
    c->enc_out = true;
    if (c->strict) c->seq_out = 0;
    c->newkeys_sent = true;
}

static void on_newkeys(ssh_conn_t *c)
{
    if (!c->newkeys_sent) { fatal(c, DIS_PROTOCOL_ERROR, "unexpected NEWKEYS"); return; }
    memcpy(c->k_in, c->nk_in, 64);
    crypto_wipe(c->nk_in, 64);
    crypto_wipe(c->nk_out, 64);
    c->enc_in = true;
    if (c->strict) c->seq_in = 0;
    c->kexinit_sent = c->kexinit_rcvd = c->ecdh_done = c->newkeys_sent = false;
    c->kex_done_once = true;
}

/* ------------------------------------------------------------ auth + channel */
static bool password_ok(const uint8_t *pw, size_t n)
{
    size_t want = strlen(server_password);
    uint8_t diff = (uint8_t)(n != want) | (uint8_t)(want == 0);
    for (size_t i = 0; i < want; i++) diff |= (uint8_t)(server_password[i] ^ (i < n ? pw[i] : 0));
    return diff == 0;
}

static void auth_failure(ssh_conn_t *c)
{
    uint8_t m[32];
    wbuf b = { m, 0, sizeof m, false };
    put_u8(&b, MSG_USERAUTH_FAILURE);
    put_cstr(&b, "password");
    put_u8(&b, 0);
    send_or_die(c, &b);
}

static void on_userauth(ssh_conn_t *c, const uint8_t *pl, size_t n)
{
    if (c->authed) return;                               /* RFC 4252: ignore once authenticated */
    rbuf r = { pl, n, 1, false };
    size_t un, sn, mn;
    const uint8_t *u = get_str(&r, &un), *svc = get_str(&r, &sn), *m = get_str(&r, &mn);
    if (r.err) { fatal(c, DIS_PROTOCOL_ERROR, "bad USERAUTH_REQUEST"); return; }
    if (!str_is(svc, sn, "ssh-connection")) { fatal(c, DIS_SERVICE_NOT_AVAILABLE, "unknown service"); return; }
    if (str_is(m, mn, "password")) {
        bool change = get_u8(&r) != 0;
        size_t pn;
        const uint8_t *pw = get_str(&r, &pn);
        if (!r.err && !change && password_ok(pw, pn)) {
            if (un > SSH_USER_MAX) un = SSH_USER_MAX;
            memcpy(c->user, u, un);
            c->user[un] = 0;
            c->authed = true;
            uint8_t ok = MSG_USERAUTH_SUCCESS;
            if (!send_packet(c, &ok, 1)) c->state = SSH_ST_DEAD;
            return;
        }
        if (++c->auth_fails >= 3) { fatal(c, DIS_NO_MORE_AUTH, "too many password attempts"); return; }
    }
    auth_failure(c);
}

static void chan_reply(ssh_conn_t *c, uint8_t type)
{
    uint8_t m[8];
    wbuf b = { m, 0, sizeof m, false };
    put_u8(&b, type);
    put_u32(&b, c->peer_chan);
    send_or_die(c, &b);
}

static void on_channel_open(ssh_conn_t *c, const uint8_t *pl, size_t n)
{
    rbuf r = { pl, n, 1, false };
    size_t tn;
    const uint8_t *t = get_str(&r, &tn);
    uint32_t sender = get_u32(&r), window = get_u32(&r), maxpkt = get_u32(&r);
    if (r.err) { fatal(c, DIS_PROTOCOL_ERROR, "bad CHANNEL_OPEN"); return; }
    uint8_t m[64];
    wbuf b = { m, 0, sizeof m, false };
    if (!c->authed || !str_is(t, tn, "session") || c->chan_open) {
        put_u8(&b, MSG_CHANNEL_OPEN_FAILURE);
        put_u32(&b, sender);
        put_u32(&b, c->chan_open ? 4 : 1);              /* resource shortage / prohibited */
        put_cstr(&b, c->chan_open ? "one session per connection" : "only sessions");
        put_cstr(&b, "");
        send_or_die(c, &b);
        return;
    }
    c->chan_open = true;
    c->peer_chan = sender;
    c->peer_window = window;
    c->peer_maxpkt = maxpkt;
    c->my_window = SSH_APP_IN;
    c->consumed = 0;
    put_u8(&b, MSG_CHANNEL_OPEN_CONFIRMATION);
    put_u32(&b, sender);
    put_u32(&b, 0);                                    /* our channel number */
    put_u32(&b, SSH_APP_IN);
    put_u32(&b, SSH_APP_IN);
    send_or_die(c, &b);
}

static void on_channel_request(ssh_conn_t *c, const uint8_t *pl, size_t n)
{
    rbuf r = { pl, n, 1, false };
    get_u32(&r);
    size_t tn;
    const uint8_t *t = get_str(&r, &tn);
    bool want = get_u8(&r) != 0;
    if (r.err || !c->chan_open) { fatal(c, DIS_PROTOCOL_ERROR, "bad CHANNEL_REQUEST"); return; }
    bool ok = false;
    if (str_is(t, tn, "pty-req")) {
        size_t en;
        get_str(&r, &en);                                /* TERM */
        c->term_w = get_u32(&r);
        c->term_h = get_u32(&r);
        ok = true;
    } else if (str_is(t, tn, "shell")) {
        ok = !c->shell;
        c->shell = true;
    } else if (str_is(t, tn, "window-change")) {
        c->term_w = get_u32(&r);
        c->term_h = get_u32(&r);
        ok = true;
    } else if (str_is(t, tn, "env")) {
        ok = true;                                       /* accepted and ignored */
    }
    if (want) chan_reply(c, ok ? MSG_CHANNEL_SUCCESS : MSG_CHANNEL_FAILURE);
}

static void on_channel_data(ssh_conn_t *c, const uint8_t *pl, size_t n)
{
    rbuf r = { pl, n, 1, false };
    get_u32(&r);
    size_t dn;
    const uint8_t *d = get_str(&r, &dn);
    if (r.err) { fatal(c, DIS_PROTOCOL_ERROR, "bad CHANNEL_DATA"); return; }
    if (dn > c->my_window) dn = c->my_window;            /* peer overran the window */
    c->my_window -= (uint32_t)dn;
    for (size_t i = 0; i < dn && c->app_count < SSH_APP_IN; i++) {
        c->app[c->app_head] = d[i];
        c->app_head = (uint16_t)((c->app_head + 1) % SSH_APP_IN);
        c->app_count++;
    }
}

static void on_channel_close(ssh_conn_t *c)
{
    if (c->chan_open && !c->chan_closed) {             /* client hung up */
        chan_reply(c, MSG_CHANNEL_CLOSE);
        c->chan_closed = true;
        fatal(c, DIS_BY_APPLICATION, "session closed");
        return;
    }
    fatal(c, DIS_BY_APPLICATION, c->close_msg[0] ? c->close_msg : "session closed");  /* we hung up */
}

/* ------------------------------------------------------------ dispatch */
static void unimplemented(ssh_conn_t *c, uint32_t seq)
{
    uint8_t m[8];
    wbuf b = { m, 0, sizeof m, false };
    put_u8(&b, MSG_UNIMPLEMENTED);
    put_u32(&b, seq);
    send_or_die(c, &b);
}

static void dispatch(ssh_conn_t *c, const uint8_t *pl, size_t n, uint32_t seq)
{
    if (n == 0) { fatal(c, DIS_PROTOCOL_ERROR, "empty packet"); return; }
    uint8_t t = pl[0];
    if (c->ignore_next) { c->ignore_next = false; if (t >= 30 && t <= 49) return; }

    /* during key exchange only transport messages; during the first one, strictly */
    bool kexmsg = t == MSG_KEXINIT || t == MSG_NEWKEYS || t == MSG_KEX_ECDH_INIT;
    if (in_kex(c) && !kexmsg) {
        if (c->strict && !c->kex_done_once) { fatal(c, DIS_PROTOCOL_ERROR, "strict kex violation"); return; }
        if (t >= 50 || t == MSG_SERVICE_REQUEST) { fatal(c, DIS_PROTOCOL_ERROR, "message during key exchange"); return; }
    }
    if (!c->kex_done_once && !kexmsg) {
        if (c->strict || (t != MSG_IGNORE && t != MSG_DEBUG && t != MSG_DISCONNECT)) {
            fatal(c, DIS_PROTOCOL_ERROR, "message before key exchange");
            return;
        }
    }

    switch (t) {
    case MSG_DISCONNECT: c->state = SSH_ST_CLOSING; snprintf(c->close_msg, sizeof c->close_msg, "client disconnected"); return;
    case MSG_IGNORE: case MSG_DEBUG: case MSG_UNIMPLEMENTED: case MSG_EXT_INFO: return;
    case MSG_KEXINIT: on_kexinit(c, pl, n); return;
    case MSG_KEX_ECDH_INIT: on_ecdh_init(c, pl, n); return;
    case MSG_NEWKEYS: on_newkeys(c); return;
    case MSG_SERVICE_REQUEST: {
        rbuf r = { pl, n, 1, false };
        size_t sn;
        const uint8_t *s = get_str(&r, &sn);
        if (r.err || !str_is(s, sn, "ssh-userauth")) { fatal(c, DIS_SERVICE_NOT_AVAILABLE, "unknown service"); return; }
        uint8_t m[24];
        wbuf b = { m, 0, sizeof m, false };
        put_u8(&b, MSG_SERVICE_ACCEPT);
        put_cstr(&b, "ssh-userauth");
        send_or_die(c, &b);
        return;
    }
    case MSG_USERAUTH_REQUEST: on_userauth(c, pl, n); return;
    default: break;
    }
    if (!c->authed) { fatal(c, DIS_PROTOCOL_ERROR, "not authenticated"); return; }
    switch (t) {
    case MSG_GLOBAL_REQUEST: {
        rbuf r = { pl, n, 1, false };
        size_t sn;
        get_str(&r, &sn);
        if (get_u8(&r)) { uint8_t f = MSG_REQUEST_FAILURE; if (!send_packet(c, &f, 1)) c->state = SSH_ST_DEAD; }
        return;
    }
    case MSG_CHANNEL_OPEN: on_channel_open(c, pl, n); return;
    case MSG_CHANNEL_REQUEST: on_channel_request(c, pl, n); return;
    case MSG_CHANNEL_DATA: on_channel_data(c, pl, n); return;
    case MSG_CHANNEL_EXTENDED_DATA: return;
    case MSG_CHANNEL_WINDOW_ADJUST: {
        rbuf r = { pl, n, 1, false };
        get_u32(&r);
        uint32_t add = get_u32(&r);
        uint64_t w = (uint64_t)c->peer_window + add;
        c->peer_window = w > 0xffffffffu ? 0xffffffffu : (uint32_t)w;
        return;
    }
    case MSG_CHANNEL_EOF: return;
    case MSG_CHANNEL_CLOSE: on_channel_close(c); return;
    default: unimplemented(c, seq); return;
    }
}

/* ------------------------------------------------------------ input */
static void consume_in(ssh_conn_t *c, size_t n)
{
    memmove(c->in, c->in + n, c->in_len - n);
    c->in_len -= n;
}

static void process(ssh_conn_t *c)
{
    while (c->state == SSH_ST_VERSION || c->state == SSH_ST_RUN) {
        if (c->state == SSH_ST_VERSION) {
            uint8_t *nl = memchr(c->in, '\n', c->in_len);
            if (!nl) {
                if (c->in_len > 255) fatal(c, DIS_PROTOCOL_ERROR, "no version string");
                return;
            }
            size_t len = (size_t)(nl - c->in);
            size_t l = len;
            if (l && c->in[l - 1] == '\r') l--;
            if (l >= 8 && (!memcmp(c->in, "SSH-2.0-", 8) || (l >= 9 && !memcmp(c->in, "SSH-1.99-", 9)))) {
                if (l > sizeof c->v_c) l = sizeof c->v_c;
                memcpy(c->v_c, c->in, l);
                c->v_c_len = l;
                c->state = SSH_ST_RUN;
            } else if (l >= 4 && !memcmp(c->in, "SSH-", 4)) {
                consume_in(c, len + 1);
                snprintf(c->close_msg, sizeof c->close_msg, "SSH protocol 1 client");
                c->state = SSH_ST_CLOSING;
                return;
            }
            consume_in(c, len + 1);
            continue;
        }
        /* binary packets; keep room for any reply before taking one */
        if (SSH_OUT_MAX - c->out_len < OUT_RESERVE) return;
        if (c->in_len < 4) return;
        uint32_t body;
        size_t maclen = c->enc_in ? 16 : 0;
        uint8_t nonce[8] = { 0, 0, 0, 0, (uint8_t)(c->seq_in >> 24), (uint8_t)(c->seq_in >> 16),
                             (uint8_t)(c->seq_in >> 8), (uint8_t)c->seq_in };
        if (c->enc_in) {
            uint8_t l[4];
            crypto_chacha20_djb(l, c->in, 4, c->k_in + 32, nonce, 0);
            body = be32(l);
        } else {
            body = be32(c->in);
        }
        if (body < 8 || body > SSH_IN_MAX - 4 - 16 || (body + (c->enc_in ? 0 : 4)) % 8) {
            fatal(c, DIS_PROTOCOL_ERROR, "bad packet length");
            return;
        }
        if (c->in_len < 4 + body + maclen) return;
        if (c->enc_in) {
            uint8_t polykey[32] = { 0 }, tag[16];
            crypto_chacha20_djb(polykey, polykey, 32, c->k_in, nonce, 0);
            crypto_poly1305(tag, c->in, 4 + body, polykey);
            crypto_wipe(polykey, sizeof polykey);
            if (crypto_verify16(tag, c->in + 4 + body)) { fatal(c, DIS_MAC_ERROR, "corrupt packet (MAC)"); return; }
            crypto_chacha20_djb(c->in + 4, c->in + 4, body, c->k_in, nonce, 1);
        }
        uint8_t padlen = c->in[4];
        if (padlen < 4 || (size_t)padlen + 1 > body) { fatal(c, DIS_PROTOCOL_ERROR, "bad padding"); return; }
        uint32_t seq = c->seq_in++;
        dispatch(c, c->in + 5, body - 1 - padlen, seq);
        consume_in(c, 4 + body + maclen);
    }
}

/* ------------------------------------------------------------ public API */
void ssh_start(ssh_conn_t *c)
{
    memset(c, 0, sizeof *c);
    c->state = SSH_ST_VERSION;
    memcpy(c->out, VERSION_S "\r\n", strlen(VERSION_S) + 2);
    c->out_len = strlen(VERSION_S) + 2;
    send_kexinit(c);
}

size_t ssh_in_room(const ssh_conn_t *c) { return SSH_IN_MAX - c->in_len; }

void ssh_feed(ssh_conn_t *c, const uint8_t *data, size_t n)
{
    if (c->state != SSH_ST_VERSION && c->state != SSH_ST_RUN) return;
    if (n > ssh_in_room(c)) n = ssh_in_room(c);
    memcpy(c->in + c->in_len, data, n);
    c->in_len += n;
    process(c);
}

void ssh_service(ssh_conn_t *c)
{
    process(c);
    /* give back window as the terminal consumes input */
    if (c->state == SSH_ST_RUN && c->chan_open && !c->chan_closed && !in_kex(c) &&
        c->consumed >= SSH_APP_IN / 2 && SSH_OUT_MAX - c->out_len > 32) {
        uint8_t m[12];
        wbuf b = { m, 0, sizeof m, false };
        put_u8(&b, MSG_CHANNEL_WINDOW_ADJUST);
        put_u32(&b, c->peer_chan);
        put_u32(&b, c->consumed);
        send_or_die(c, &b);
        c->my_window += c->consumed;
        c->consumed = 0;
    }
}

size_t ssh_out_peek(ssh_conn_t *c, const uint8_t **p) { *p = c->out; return c->out_len; }

void ssh_out_consume(ssh_conn_t *c, size_t n)
{
    if (n > c->out_len) n = c->out_len;
    memmove(c->out, c->out + n, c->out_len - n);
    c->out_len -= n;
    if (n) process(c);                                   /* may have been waiting for room */
}

bool ssh_shell_ready(const ssh_conn_t *c)
{
    return c->state == SSH_ST_RUN && c->authed && c->chan_open && c->shell && !c->chan_closed;
}

const char *ssh_user(const ssh_conn_t *c) { return c->user; }

int ssh_read(ssh_conn_t *c, uint8_t *buf, int n)
{
    int k = 0;
    while (k < n && c->app_count) {
        buf[k++] = c->app[c->app_tail];
        c->app_tail = (uint16_t)((c->app_tail + 1) % SSH_APP_IN);
        c->app_count--;
        c->consumed++;
    }
    return k;
}

size_t ssh_write(ssh_conn_t *c, const uint8_t *data, size_t n)
{
    size_t done = 0;
    while (done < n && ssh_shell_ready(c) && !in_kex(c)) {
        size_t k = n - done;
        if (k > MAX_DATA) k = MAX_DATA;
        if (k > c->peer_window) k = c->peer_window;
        if (c->peer_maxpkt && k > c->peer_maxpkt) k = c->peer_maxpkt;
        size_t room = SSH_OUT_MAX - c->out_len;
        if (room < OUT_RESERVE + 64) break;
        if (k > room - OUT_RESERVE - 64) k = room - OUT_RESERVE - 64;
        if (k == 0) break;
        static uint8_t m[MAX_DATA + 16];
        wbuf b = { m, 0, sizeof m, false };
        put_u8(&b, MSG_CHANNEL_DATA);
        put_u32(&b, c->peer_chan);
        put_str(&b, data + done, k);
        if (b.err || !send_packet(c, b.p, b.len)) break;
        c->peer_window -= (uint32_t)k;
        done += k;
    }
    return done;
}

void ssh_ping(ssh_conn_t *c)
{
    if (c->state != SSH_ST_RUN || !c->kex_done_once || in_kex(c) || SSH_OUT_MAX - c->out_len < OUT_RESERVE) return;
    uint8_t m[8];
    wbuf b = { m, 0, sizeof m, false };
    put_u8(&b, MSG_IGNORE);
    put_cstr(&b, "");
    send_or_die(c, &b);
}

bool ssh_finished(const ssh_conn_t *c)
{
    return c->state == SSH_ST_DEAD || (c->state == SSH_ST_CLOSING && c->out_len == 0);
}
