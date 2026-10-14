/*
 * sshsrv.c - SSH sessions as PDP-8 terminal lines (see sshsrv.h).
 */
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "sshsrv.h"
#include "ssh.h"

#define RING 2048
#define LOGIN_TIMEOUT_MS 60000

typedef struct {
    bool       used;
    int        sock;
    int        line;                 /* -1 until attached */
    bool       last_cr;
    uint32_t   started, ending, last_tx;          /* ending: when we closed the shell (0 = not) */
    uint32_t   end_at;                   /* close the shell at this time (0 = not) */
    bool       logged_out;               /* the system hung up (TSS/8 LOGOUT, MULTOS/8 LOGOFF, ETOS LOGOUT) */
    char       end_why[40];
    uint16_t   head, tail, count;    /* PDP-8 output waiting for the client */
    uint8_t    ring[RING];
    ssh_conn_t ssh;
} session_t;

static session_t sess[SSHSRV_MAX];
static int owner[SSHSRV_LINES] = { -1, -1, -1, -1, -1 };
static bool ready;

/* When a client goes away without LOGOUT, log its TSS/8 job out, as a modem
 * hang-up would have needed an operator to: ^B S stops any program, then
 * LOGOUT.  The pauses let the monitor act on each step. */
typedef struct { const char *keys; uint16_t wait_ms; } hstep_t;
static const hstep_t hangup_tss[] = {
    { "\x02", 300 }, { "S", 500 }, { "\r", 300 }, { "LOGOUT\r", 2000 }, { NULL, 0 },
};
/* MULTOS/8: Ctrl-C back to the (virtual) OS/8 monitor, then its LOGOFF
 * program: from the job's own disk if it has a copy (recommended: COPY
 * J2:<LOGOFF.SV etc. before starting MULTOS), else from J1:, the system area */
static const hstep_t hangup_multos[] = {
    { "\x03", 500 }, { "\x03", 500 }, { "R LOGOFF\r", 1500 }, { "RUN J1:LOGOFF\r", 2000 }, { NULL, 0 },
};
/* ETOS: Ctrl-C stops the program, ^V S <CR> gets the ETOS monitor's '!',
 * where LOGOUT ends the job */
static const hstep_t hangup_etos[] = {
    { "\x03", 500 }, { "\x03", 500 }, { "\x16S\r", 800 }, { "LOGOUT\r", 2000 }, { NULL, 0 },
};
static const hstep_t *hangup_script(int sys)
{
    return sys == SSHP_SYS_TSS ? hangup_tss : sys == SSHP_SYS_MULTOS ? hangup_multos :
           sys == SSHP_SYS_ETOS ? hangup_etos : NULL;
}
static int line_count(int sys)               /* terminal lines besides the console */
{
    if (sys == SSHP_SYS_TSS) return SSHSRV_LINES - 1;
    if (sys == SSHP_SYS_MULTOS || sys == SSHP_SYS_ETOS) {
        int n = sshp_rk_lines();
        return n < 0 ? 0 : n > SSHSRV_LINES - 1 ? SSHSRV_LINES - 1 : n;
    }
    return 0;
}
static const char *sys_label(int sys)
{
    return sys == SSHP_SYS_MULTOS ? "MULTOS/8" : sys == SSHP_SYS_ETOS ? "ETOS" : "TSS/8";
}
/* ETOS's LOGOUT says "JOB n LOGGED OUT ON CONSOLE n" */
static const char etos_bye[] = "LOGGED OUT ON CONSOLE";
static uint8_t bye_pos[SSHSRV_LINES];
static int8_t   hang_step[SSHSRV_LINES] = { -1, -1, -1, -1, -1 };
static uint32_t hang_next[SSHSRV_LINES];

void sshsrv_init(const uint8_t seed[32], const char *password)
{
    ssh_server_init(seed, password);
    for (int i = 0; i < SSHSRV_LINES; i++) owner[i] = -1;
    ready = true;
}

bool sshsrv_line_busy(int line)
{
    return line >= 0 && line < SSHSRV_LINES && (owner[line] >= 0 || hang_step[line] >= 0);
}

int sshsrv_sessions(void)
{
    int n = 0;
    for (int i = 0; i < SSHSRV_MAX; i++) n += sess[i].used;
    return n;
}

static void end_session(session_t *s, const char *why);

void sshsrv_line_out(int line, uint8_t c)
{
    if (line < 0 || line >= SSHSRV_LINES || owner[line] < 0) return;
    session_t *s = &sess[owner[line]];
    int sys = sshp_system();
    if (line > 0 && ((c == 004 && sys == SSHP_SYS_TSS) ||          /* TSS/8 LOGOUT ends with EOT */
                     (c == 003 && sys == SSHP_SYS_MULTOS))) {      /* MULTOS/8 LOGOFF prints ETXs */
        s->logged_out = true;
        end_session(s, "logged out");
        return;
    }
    if (line > 0 && sys == SSHP_SYS_ETOS) {
        uint8_t ch = c & 0177;
        if (ch == (uint8_t)etos_bye[bye_pos[line]]) {
            if (!etos_bye[++bye_pos[line]]) {
                bye_pos[line] = 0;
                s->logged_out = true;
                end_session(s, "logged out");       /* closes after the rest of the line */
            }
        } else bye_pos[line] = ch == (uint8_t)etos_bye[0];
    }
    if (s->count == RING) return;                    /* client too slow: drop */
    s->ring[s->head] = c;
    s->head = (uint16_t)((s->head + 1) % RING);
    s->count++;
}

static void text(session_t *s, const char *t)
{
    ssh_write(&s->ssh, (const uint8_t *)t, strlen(t));
}

static const char *line_name(int l)
{
    static const char *n[] = { "the console", "line K01", "line K02", "line K03", "line K04" };
    return n[l];
}

/* pick a line for a session that just started its shell; -1 = refused (message sent) */
static int attach(session_t *s, int idx)
{
    char u[SSH_USER_MAX + 1];
    const char *user = ssh_user(&s->ssh);
    size_t i = 0;
    for (; user[i] && i < SSH_USER_MAX; i++) u[i] = (char)tolower((unsigned char)user[i]);
    u[i] = 0;

    int sys = sshp_system(), want = -1;
    if (!strcmp(u, "console") || !strcmp(u, "k00")) want = 0;
    else if (u[0] == 'k' && u[1] == '0' && u[2] >= '1' && u[2] <= '4' && !u[3]) want = u[2] - '0';

    char msg[110];
    int nl = line_count(sys);
    if (sys == SSHP_SYS_NONE) { text(s, "\r\nNo PDP-8 system is running yet.\r\n"); return -1; }
    if (want > nl) {
        if (nl == 0) text(s, "\r\nNo time-sharing system is running, so only the console is available (user k00).\r\n");
        else {
            snprintf(msg, sizeof msg, "\r\n%s here has terminal lines K01-K%02d only.\r\n", sys_label(sys), nl);
            text(s, msg);
        }
        return -1;
    }
    if (want < 0) {
        if (nl > 0) {
            for (int l = 1; l <= nl; l++)
                if (owner[l] < 0 && hang_step[l] < 0 && sshp_line_free(l)) { want = l; break; }
            if (want < 0) {
                snprintf(msg, sizeof msg, "\r\nAll %s terminal lines are in use.\r\n", sys_label(sys));
                text(s, msg);
                return -1;
            }
        } else {
            want = 0;
        }
    }
    if (hang_step[want] >= 0) {
        snprintf(msg, sizeof msg, "\r\nThe PDP-8's %s is logging out its last user; try again in a moment.\r\n",
                 line_name(want));
        text(s, msg);
        return -1;
    }
    if (owner[want] >= 0 || !sshp_line_free(want)) {
        snprintf(msg, sizeof msg, "\r\nThe PDP-8's %s is already in use.\r\n", line_name(want));
        text(s, msg);
        return -1;
    }
    owner[want] = idx;
    snprintf(msg, sizeof msg, "\r\n[PDP-8 on Fruit Jam: connected to %s%s%s%s]\r\n", line_name(want),
             nl ? (want ? " of " : " (") : " (OS/8)", nl ? sys_label(sys) : "", nl ? (want ? "" : " operator)") : "");
    text(s, msg);
    return want;
}

/* Close the shell after a short pause, so the client shows the last message first. */
static void end_session(session_t *s, const char *why)
{
    if (s->end_at || s->ending) return;
    snprintf(s->end_why, sizeof s->end_why, "%s", why);
    s->end_at = (sshp_millis() + 300) | 1;
}

static void finish(session_t *s, int idx)
{
    sshp_close(s->sock);
    for (int l = 0; l < SSHSRV_LINES; l++) {
        if (owner[l] != idx) continue;
        owner[l] = -1;
        if (l > 0 && !s->logged_out && hangup_script(sshp_system())) {
            hang_step[l] = 0;
            hang_next[l] = sshp_millis();
        }
    }
    s->used = false;
}

static void hangups(uint32_t now)
{
    for (int l = 1; l < SSHSRV_LINES; l++) {
        if (hang_step[l] < 0 || (int32_t)(now - hang_next[l]) < 0) continue;
        const hstep_t *h = hangup_script(sshp_system());
        if (!h || !h[hang_step[l]].keys) { hang_step[l] = -1; continue; }
        const char *k = h[hang_step[l]].keys;
        if (sshp_line_room(l) < (int)strlen(k)) continue;
        while (*k) sshp_line_key(l, (uint8_t)*k++);
        hang_next[l] = now + h[hang_step[l]].wait_ms;
        hang_step[l]++;
    }
}

void sshsrv_drop_all(const char *why)
{
    for (int i = 0; i < SSHSRV_MAX; i++) {
        if (!sess[i].used) continue;
        if (ssh_shell_ready(&sess[i].ssh)) { text(&sess[i], "\r\n["); text(&sess[i], why); text(&sess[i], "]\r\n"); }
        end_session(&sess[i], why);
    }
}

static void flush_out(session_t *s, int *dead)
{
    const uint8_t *p;
    size_t n = ssh_out_peek(&s->ssh, &p);
    if (!n) return;
    int k = sshp_send(s->sock, p, (int)n);
    if (k < 0) { *dead = 1; return; }
    if (k > 0) s->last_tx = sshp_millis();
    ssh_out_consume(&s->ssh, (size_t)k);
}

void sshsrv_poll(void)
{
    if (!ready) return;
    uint32_t now = sshp_millis();

    /* new connections */
    for (;;) {
        int idx = -1;
        for (int i = 0; i < SSHSRV_MAX; i++) if (!sess[i].used) { idx = i; break; }
        int sock = sshp_accept();
        if (sock < 0) break;
        if (idx < 0) {                               /* full: say so the SSH way and hang up */
            static const char busy[] = "SSH-2.0-PDP8_FruitJam_1.0\r\n";
            sshp_send(sock, (const uint8_t *)busy, (int)sizeof busy - 1);
            sshp_close(sock);
            continue;
        }
        session_t *s = &sess[idx];
        s->used = true;
        s->sock = sock;
        s->line = -1;
        s->last_cr = false;
        s->started = now;
        s->last_tx = now;
        s->ending = 0;
        s->end_at = 0;
        s->logged_out = false;
        s->head = s->tail = s->count = 0;
        ssh_start(&s->ssh);
    }

    hangups(now);
    int sys = sshp_system();
    for (int i = 0; i < SSHSRV_MAX; i++) {
        session_t *s = &sess[i];
        if (!s->used) continue;
        int dead = 0;

        /* network -> ssh */
        static uint8_t buf[512];                     /* static: small stacks on the Pico */
        for (int rounds = 0; rounds < 4; rounds++) {
            size_t room = ssh_in_room(&s->ssh);
            if (room == 0) break;
            int n = sshp_recv(s->sock, buf, (int)(room < sizeof buf ? room : sizeof buf));
            if (n < 0) { dead = 1; break; }
            if (n == 0) break;
            ssh_feed(&s->ssh, buf, (size_t)n);
            flush_out(s, &dead);
        }
        ssh_service(&s->ssh);

        if (!ssh_shell_ready(&s->ssh) && s->line == -1 && now - s->started > LOGIN_TIMEOUT_MS)
            ssh_disconnect(&s->ssh, "login timed out");

        if (ssh_shell_ready(&s->ssh)) {
            if (s->line == -1) {
                s->line = attach(s, i);
                if (s->line < 0) { end_session(s, "no terminal line"); s->line = -2; }
            }
            if (s->line >= 1 && s->line > line_count(sys)) {
                text(s, "\r\n[the time-sharing system has stopped]\r\n");
                end_session(s, "time-sharing stopped");
                owner[s->line] = -1;
                s->line = -2;
            }
        }
        if (s->line >= 0 && ssh_shell_ready(&s->ssh)) {
            /* keyboard -> PDP-8 line, as fast as the line takes it */
            int room = sshp_line_room(s->line);
            static uint8_t k[64];
            while (room > 0) {
                int n = ssh_read(&s->ssh, k, room < (int)sizeof k ? room : (int)sizeof k);
                if (n <= 0) break;
                for (int j = 0; j < n; j++) {
                    uint8_t c = k[j];
                    if (c == '\n' && s->last_cr) { s->last_cr = false; continue; }   /* CR LF -> CR */
                    s->last_cr = c == '\r';
                    if (c == '\n') c = '\r';
                    sshp_line_key(s->line, c);
                    room--;
                }
            }
            /* PDP-8 output -> client */
            while (s->count) {
                static uint8_t chunk[256];
                int n = 0;
                uint16_t t = s->tail;
                while (n < (int)sizeof chunk && n < s->count) { chunk[n++] = s->ring[t]; t = (uint16_t)((t + 1) % RING); }
                size_t took = ssh_write(&s->ssh, chunk, (size_t)n);
                s->tail = (uint16_t)((s->tail + took) % RING);
                s->count = (uint16_t)(s->count - took);
                if (took < (size_t)n) break;
            }
        }
        if (now - s->last_tx > 60000) { ssh_ping(&s->ssh); s->last_tx = now; }   /* is the client still there? */
        if (s->end_at && (int32_t)(now - s->end_at) >= 0) {
            ssh_end_session(&s->ssh, s->end_why);
            s->end_at = 0;
            s->ending = now | 1;
        }
        if (s->ending && now - s->ending > 3000) ssh_disconnect(&s->ssh, "closed");
        flush_out(s, &dead);
        if (dead || ssh_finished(&s->ssh)) finish(s, i);
    }
}
