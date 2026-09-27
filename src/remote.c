/*
 * The front panel in a browser on the local network: an iPad, typically.
 *
 * A browser that opens the address printed at start gets the real face plate
 * (assets/panel/octatrack.svg, the same drawing the window's raster is made
 * from) with the live screen, lamps, held keys and crossfader, and plays it
 * with multitouch: hold a trig and turn a knob for a parameter lock, or FUNC
 * plus a key, with fingers rather than keyboard latches.
 *
 * The protocol follows the Gearmulator (Machinedrum/Monomachine) and digiemu
 * (Digitakt/Digitone) remote panels in shape, so the four behave alike:
 *
 *     GET /               assets/remote/index.html
 *     GET /panel.svg      assets/panel/octatrack.svg
 *     GET /elements.json  assets/panel/octatrack-elements.json
 *     GET /ws             WebSocket. The server sends
 *       'L <json>'  once: {"product", "lamps": [svg id, ...],
 *                   "keys": {svg id: key id, ...}}
 *       binary 'S', 1024 bytes of screen (128x64, 1 bit a pixel, row-major
 *                   from the top, MSB first), 8 key-group bytes (bit n of
 *                   byte g = key 8g+n held), the crossfader (wire value, 255
 *                   = far LEFT), the headphones pot (0-255), then r, g, b for
 *                   each lamp in the 'L' order (0, 0, 0 when dark)
 *     The page sends text:
 *       hello              send the layout and state now
 *       b <svg id> <1|0>   press / release a key
 *       p <svg id> <1|0>   push / let go of a knob's switch
 *       e <svg id> <n>     turn a knob n detents (signed; knob-phones is the
 *                          host's monitor volume, not the Octatrack's)
 *       x <0-255>          move the crossfader (wire value)
 *       r                  let go of everything this page holds
 *
 * Each page remembers what it holds and lets go of it when its connection
 * ends, and a page that stops answering pings (an iPad put to sleep holds the
 * socket half open) is dropped the same way, so a key is never left stuck
 * down. The panel's key state is one bit per key, not a count: a remote
 * release also lets go of the same key held in the window.
 *
 * There is no authentication: anyone who can reach the port can play the
 * panel. It is meant for a trusted home network; --no-remote turns it off.
 *
 * OCTA_REMOTE_LOG=FILE appends one timestamped line per connection event
 * (accepted, first bytes, request, websocket, dropped, done): what a browser
 * really did when a page is slow to open.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "emu.h"

#define PORTS_TRIED 10         /* the first port and the next nine */
#define MAX_CLIENTS 16
#define FRAME_MS 33            /* publish at most this often */
#define PING_MS 4000           /* ping a page this often */
#define DEAD_MS 12000          /* no reply for this long: the page is gone */
#define SEND_TIMEOUT_S 2       /* a page this far behind is dropped */
#define MAX_DETENTS 96         /* one message's turn */
#define MAX_MESSAGE 4096
#define MAX_REQUEST 8192
#define PHONES_STEP 0.05f      /* per detent, as the window's wheel */

#define STATE_BYTES (1 + W * H / 8 + 8 + 1 + 1 + PANEL_NLAMPS * 3)

typedef struct {
    int fd;
    pthread_mutex_t send_lock;
    uint64_t held;             /* keys this page holds; its own thread only */
    atomic_bool ws, ready, closed;
    _Atomic int64_t heard;
    uint8_t last[STATE_BYTES]; /* publisher only */
    bool has_last;
    int n;                     /* connection number, for the log */
    char peer[48];
    int64_t born;
} Client;

static struct {
    int listen_fd, port;
    atomic_bool running;
    pthread_t accept_thr, pub_thr;
    pthread_mutex_t lock;      /* the client list */
    Client *clients[MAX_CLIENTS];
    int nclients;
    char layout[4096];
    char url[96];
    FILE *log;                 /* OCTA_REMOTE_LOG=file, or NULL */
    int64_t t0;
    int seq;
} g = { .listen_fd = -1 };

/* One line per connection event, when OCTA_REMOTE_LOG names a file: what a
 * browser actually did, for "the page takes 20 s to open" on a machine that
 * is not this one. Milliseconds since the server started. */
static void rlog(const Client *c, const char *fmt, ...)
{
    va_list ap;

    if (!g.log) {
        return;
    }
    flockfile(g.log);
    fprintf(g.log, "%8.3f #%-4d %-21s ", (now_ms() - g.t0) / 1000.0,
            c ? c->n : 0, c ? c->peer : "");
    va_start(ap, fmt);
    vfprintf(g.log, fmt, ap);
    va_end(ap);
    fputc('\n', g.log);
    fflush(g.log);
    funlockfile(g.log);
}

/* ---- SHA-1 and base64, for the WebSocket handshake only ------------------ */
static uint32_t rol(uint32_t v, int bits) { return v << bits | v >> (32 - bits); }

static void sha1(const uint8_t *in, size_t n, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476,
                     0xC3D2E1F0};
    const size_t total = ((n + 8) / 64 + 1) * 64;
    uint8_t *msg = calloc(total, 1);
    const uint64_t bits = (uint64_t)n * 8;

    if (!msg) {
        memset(out, 0, 20);
        return;
    }
    memcpy(msg, in, n);
    msg[n] = 0x80;
    for (int i = 0; i < 8; i++) {
        msg[total - 1 - i] = (uint8_t)(bits >> (i * 8));
    }
    for (size_t chunk = 0; chunk < total; chunk += 64) {
        uint32_t w[80], a, b, c, d, e;

        for (int i = 0; i < 16; i++) {
            const uint8_t *p = msg + chunk + i * 4;

            w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                   (uint32_t)p[2] << 8 | p[3];
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k, t;

            if (i < 20)      { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d; k = 0xCA62C1D6; }
            t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    free(msg);
    for (int i = 0; i < 20; i++) {
        out[i] = (uint8_t)(h[i / 4] >> (24 - (i % 4) * 8));
    }
}

static void base64(const uint8_t *in, size_t n, char *out)
{
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;

    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 |
                           (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) |
                           (i + 2 < n ? in[i + 2] : 0);

        out[o++] = t[v >> 18 & 63];
        out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = 0;
}

/* -> Sec-WebSocket-Accept for a client's Sec-WebSocket-Key (RFC 6455). */
static void accept_key(const char *key, char out[32])
{
    char buf[128];
    uint8_t d[20];

    snprintf(buf, sizeof buf, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    sha1((const uint8_t *)buf, strlen(buf), d);
    base64(d, 20, out);
}

/* ---- sending -------------------------------------------------------------- */
static bool send_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;

    while (n) {
        const ssize_t w = send(fd, p, n, MSG_NOSIGNAL);

        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

/* Mark `c` gone; its own thread notices and cleans up. */
static void drop(Client *c)
{
    if (!atomic_exchange(&c->closed, true)) {
        shutdown(c->fd, SHUT_RDWR);
    }
}

/* One unmasked, unfragmented server frame. -> false (and dropped) on error. */
static bool ws_send(Client *c, int opcode, const void *data, size_t n)
{
    uint8_t head[10];
    size_t hn;
    bool ok;

    if (atomic_load(&c->closed)) {
        return false;
    }
    head[0] = (uint8_t)(0x80 | opcode);
    if (n < 126) {
        head[1] = (uint8_t)n;
        hn = 2;
    } else if (n < 1 << 16) {
        head[1] = 126;
        head[2] = (uint8_t)(n >> 8);
        head[3] = (uint8_t)n;
        hn = 4;
    } else {
        head[1] = 127;
        for (int i = 0; i < 8; i++) {
            head[2 + i] = (uint8_t)((uint64_t)n >> (56 - 8 * i));
        }
        hn = 10;
    }
    pthread_mutex_lock(&c->send_lock);
    ok = send_all(c->fd, head, hn) && (!n || send_all(c->fd, data, n));
    pthread_mutex_unlock(&c->send_lock);
    if (!ok) {
        drop(c);
    }
    return ok;
}

/* ---- input ---------------------------------------------------------------- */
static void hold(Client *c, int id, bool down)
{
    const uint64_t bit = 1ull << id;

    if (down && !(c->held & bit)) {
        c->held |= bit;
        panel_key(id, true);
    } else if (!down && (c->held & bit)) {
        c->held &= ~bit;
        panel_key(id, false);
    }
}

static void let_go(Client *c)
{
    for (int id = 0; id < 64; id++) {
        hold(c, id, false);
    }
}

static void handle(Client *c, char *text)
{
    char verb[8], name[32];
    long v;
    int n = sscanf(text, "%7s %31s %ld", verb, name, &v);

    if (n < 1) {
        return;
    }
    if (!strcmp(verb, "hello")) {
        atomic_store(&c->ready, false);    /* the publisher resends it all */
    } else if (!strcmp(verb, "r")) {
        let_go(c);
    } else if (!strcmp(verb, "b") && n == 3) {
        const int id = panel_svg_key(name);

        if (id >= 0) {
            hold(c, id, v != 0);
        }
    } else if (!strcmp(verb, "p") && n == 3) {
        const int enc = panel_svg_knob(name);

        if (enc >= 0 && enc < 7) {
            hold(c, 56 + enc, v != 0);         /* ENC1-7 switches */
        }
    } else if (!strcmp(verb, "e") && n == 3) {
        const int enc = panel_svg_knob(name);

        if (v < -MAX_DETENTS) v = -MAX_DETENTS;
        if (v > MAX_DETENTS) v = MAX_DETENTS;
        if (enc == 7) {
            float ph = audio_phones() + (float)v * PHONES_STEP;

            audio_set_phones(ph < 0 ? 0 : ph > 1 ? 1 : ph);
        } else if (enc >= 0) {
            panel_encoder(enc, (int)v);
        }
    } else if (!strcmp(verb, "x") && n == 2) {
        v = strtol(name, NULL, 10);
        panel_xfader(v < 0 ? 0 : v > 255 ? 255 : (int)v);
    }
}

/* ---- the WebSocket -------------------------------------------------------- */
/* -> exactly n bytes; waits through receive timeouts until the page is
 * dropped or the server stops. */
static bool recv_all(Client *c, uint8_t *buf, size_t n)
{
    while (n) {
        const ssize_t r = recv(c->fd, buf, n, 0);

        if (r > 0) {
            buf += r;
            n -= (size_t)r;
        } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                             errno == EINTR)) {
            if (atomic_load(&c->closed) || !atomic_load(&g.running)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

static void ws_loop(Client *c)
{
    uint8_t payload[MAX_MESSAGE + 1];

    atomic_store(&c->heard, now_ms());
    atomic_store(&c->ws, true);
    while (!atomic_load(&c->closed) && atomic_load(&g.running)) {
        struct pollfd p = { c->fd, POLLIN, 0 };
        uint8_t head[2], ext[8], mask[4];
        uint64_t len;
        int opcode;

        if (poll(&p, 1, 500) <= 0) {
            continue;
        }
        if (!recv_all(c, head, 2)) {
            break;
        }
        opcode = head[0] & 0x0F;
        len = head[1] & 0x7F;
        if (len == 126) {
            if (!recv_all(c, ext, 2)) break;
            len = (uint64_t)ext[0] << 8 | ext[1];
        } else if (len == 127) {
            if (!recv_all(c, ext, 8)) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = len << 8 | ext[i];
        }
        if (len > MAX_MESSAGE) {           /* nothing the page sends is near */
            break;
        }
        if (head[1] & 0x80) {
            if (!recv_all(c, mask, 4)) break;
        } else {
            memset(mask, 0, 4);
        }
        if (!recv_all(c, payload, (size_t)len)) {
            break;
        }
        for (uint64_t i = 0; i < len; i++) {
            payload[i] ^= mask[i & 3];
        }
        payload[len] = 0;
        atomic_store(&c->heard, now_ms());
        if (opcode == 0x8) {               /* close: echo it and go */
            ws_send(c, 0x8, payload, len < 2 ? len : 2);
            break;
        } else if (opcode == 0x9) {
            ws_send(c, 0xA, payload, (size_t)len);
        } else if (opcode == 0x1) {
            handle(c, (char *)payload);
        }
    }
}

/* ---- HTTP ----------------------------------------------------------------- */
static void http_error(Client *c, const char *status)
{
    char buf[256];
    const int n = snprintf(buf, sizeof buf,
                           "HTTP/1.1 %s\r\nContent-Type: text/plain\r\n"
                           "Content-Length: %zu\r\nConnection: close\r\n\r\n%s\n",
                           status, strlen(status) + 1, status);

    send_all(c->fd, buf, (size_t)n);
}

/* Files are read from the repo root, as the panel raster is: run octemu from
 * there. They are read per request so the page can be edited live. */
static void serve_file(Client *c, const char *path, const char *mime)
{
    FILE *f = fopen(path, "rb");
    char head[256];
    char *body;
    long n;

    if (!f) {
        fprintf(stderr, "octemu: remote: cannot read %s%s\n", path,
                strstr(path, "panel/") ? " (run 'make panel-svg')" : "");
        http_error(c, "404 Not Found");
        return;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    body = malloc(n > 0 ? (size_t)n : 1);
    if (!body || n < 0 || fread(body, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(body);
        http_error(c, "500 Internal Server Error");
        return;
    }
    fclose(f);
    snprintf(head, sizeof head,
             "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
             "Cache-Control: no-store\r\nConnection: close\r\n\r\n", mime, n);
    if (send_all(c->fd, head, strlen(head))) {
        send_all(c->fd, body, (size_t)n);
    }
    free(body);
}

/* -> the value of header `name` (lower case, with the colon) in `req`. */
static bool header(const char *req, const char *name, char *out, size_t cap)
{
    const char *p = req;
    const size_t nl = strlen(name);

    while ((p = strstr(p, "\r\n")) != NULL) {
        size_t n = 0;

        p += 2;
        if (strncasecmp(p, name, nl)) {
            continue;
        }
        p += nl;
        while (*p == ' ' || *p == '\t') p++;
        while (p[n] && p[n] != '\r' && n + 1 < cap) {
            out[n] = p[n];
            n++;
        }
        while (n && (out[n - 1] == ' ' || out[n - 1] == '\t')) n--;
        out[n] = 0;
        return true;
    }
    return false;
}

static void serve(Client *c)
{
    char req[MAX_REQUEST + 1], method[8], path[256], upgrade[64], key[64];
    size_t have = 0;
    const int64_t until = now_ms() + 5000;
    char *q;

    req[0] = 0;
    while (!strstr(req, "\r\n\r\n")) {     /* the request head */
        ssize_t r;

        if (have >= MAX_REQUEST || now_ms() > until ||
            !atomic_load(&g.running)) {
            rlog(c, "gave up on the request: %zu bytes in %lld ms", have,
                 (long long)(now_ms() - c->born));
            return;
        }
        r = recv(c->fd, req + have, MAX_REQUEST - have, 0);
        if (r > 0) {
            /* ☠ Not HTTP — a TLS hello (0x16) from Firefox's HTTPS-First,
             * typically: hang up NOW. Waiting out the deadline instead made
             * every https:// try cost 5 s, and the page ~20 s to open. */
            if (!have && !(req[0] >= 'A' && req[0] <= 'Z')) {
                rlog(c, "not HTTP (first byte 0x%02x%s): hung up",
                     (uint8_t)req[0], req[0] == 0x16 ? ", a TLS hello" : "");
                return;
            }
            if (!have) {
                rlog(c, "first bytes after %lld ms",
                     (long long)(now_ms() - c->born));
            }
            have += (size_t)r;
        } else if (!(r < 0 && (errno == EAGAIN || errno == EINTR))) {
            rlog(c, "closed by the browser before a request (%zu bytes)", have);
            return;
        }
        req[have] = 0;
    }
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        return;
    }
    if (!strncmp(path, "/note?", 6)) {     /* the page's own timings, logged */
        char *o = path;

        for (const char *s = path + 6; *s; s++) {    /* percent-decode */
            unsigned v;

            if (*s == '%' && sscanf(s + 1, "%2x", &v) == 1) {
                *o++ = (char)(v >= 0x20 ? v : '?');
                s += 2;
            } else {
                *o++ = *s;
            }
        }
        *o = 0;
        rlog(c, "page: %s", path);
        http_error(c, "200 OK");
        return;
    }
    if ((q = strchr(path, '?')) != NULL) {
        *q = 0;
    }
    while (path[0] == '/' && path[1] == '/') {    /* "//" from a pasted URL */
        memmove(path, path + 1, strlen(path));
    }
    rlog(c, "%s %s", method, path);
    if (strcmp(method, "GET")) {
        http_error(c, "405 Method Not Allowed");
    } else if (!strcmp(path, "/ws")) {
        char acc[32], resp[256];

        if (!header(req, "sec-websocket-key:", key, sizeof key) ||
            !header(req, "upgrade:", upgrade, sizeof upgrade) ||
            strcasecmp(upgrade, "websocket")) {
            http_error(c, "400 Bad Request");
            return;
        }
        accept_key(key, acc);
        /* HTTP/1.1, not 1.0: Firefox will not upgrade a 1.0 connection and
         * the page reconnects forever (digiemu found this). */
        snprintf(resp, sizeof resp,
                 "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                 "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
        if (send_all(c->fd, resp, strlen(resp))) {
            rlog(c, "websocket open");
            ws_loop(c);
        }
    } else if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        serve_file(c, "assets/remote/index.html", "text/html; charset=utf-8");
    } else if (!strcmp(path, "/panel.svg")) {
        serve_file(c, "assets/panel/octatrack.svg", "image/svg+xml");
    } else if (!strcmp(path, "/ping")) {        /* the page's is-it-up probe */
        static const char ok[] =
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
            "Content-Length: 3\r\nCache-Control: no-store\r\n"
            "Connection: close\r\n\r\nok\n";

        send_all(c->fd, ok, sizeof ok - 1);
    } else if (!strcmp(path, "/elements.json")) {
        serve_file(c, "assets/panel/octatrack-elements.json",
                   "application/json");
    } else {
        http_error(c, "404 Not Found");
    }
}

static void *client_thread(void *arg)
{
    Client *c = arg;

    serve(c);
    rlog(c, "done after %lld ms", (long long)(now_ms() - c->born));
    atomic_store(&c->closed, true);
    let_go(c);                         /* never leave a key stuck down */
    pthread_mutex_lock(&g.lock);
    for (int i = 0; i < g.nclients; i++) {
        if (g.clients[i] == c) {
            g.clients[i] = g.clients[--g.nclients];
            break;
        }
    }
    pthread_mutex_unlock(&g.lock);
    close(c->fd);
    pthread_mutex_destroy(&c->send_lock);
    free(c);
    return NULL;
}

/* ---- threads -------------------------------------------------------------- */
static void *accept_thread(void *arg)
{
    (void)arg;
    while (atomic_load(&g.running)) {
        struct pollfd p = { g.listen_fd, POLLIN, 0 };
        struct timeval rcv = { 0, 500000 };
        struct timeval snd = { SEND_TIMEOUT_S, 0 };
        pthread_attr_t attr;
        pthread_t t;
        Client *c;
        struct sockaddr_in pa;
        socklen_t pa_len;
        int fd;

        if (poll(&p, 1, 200) <= 0) {
            continue;
        }
        pa_len = sizeof pa;
        fd = accept(g.listen_fd, (struct sockaddr *)&pa, &pa_len);
        if (fd < 0) {
            continue;
        }
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof rcv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd);
        c = calloc(1, sizeof *c);
        pthread_mutex_lock(&g.lock);
        if (!c || g.nclients >= MAX_CLIENTS) {
            pthread_mutex_unlock(&g.lock);
            rlog(NULL, "refused a connection: %d already open", g.nclients);
            close(fd);
            free(c);
            continue;
        }
        c->fd = fd;
        c->n = ++g.seq;
        c->born = now_ms();
        inet_ntop(AF_INET, &pa.sin_addr, c->peer, sizeof c->peer);
        snprintf(c->peer + strlen(c->peer), sizeof c->peer - strlen(c->peer),
                 ":%d", ntohs(pa.sin_port));
        pthread_mutex_init(&c->send_lock, NULL);
        g.clients[g.nclients++] = c;
        pthread_mutex_unlock(&g.lock);
        rlog(c, "accepted (%d open)", g.nclients);
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &attr, client_thread, c)) {
            pthread_mutex_lock(&g.lock);
            g.clients[--g.nclients] = NULL;    /* it was the last one added */
            pthread_mutex_unlock(&g.lock);
            close(fd);
            pthread_mutex_destroy(&c->send_lock);
            free(c);
        }
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

static size_t state_frame(uint8_t *out)
{
    static PanelState p;               /* publisher thread only */
    size_t o = 0;
    const float ph = audio_phones();

    panel_snapshot(&p);
    out[o++] = 'S';
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x += 8) {
            uint8_t b = 0;

            for (int k = 0; k < 8; k++) {
                b = (uint8_t)(b << 1 | (p.fb[y][x + k] ? 1 : 0));
            }
            out[o++] = b;
        }
    }
    memcpy(out + o, p.keys, 8);
    o += 8;
    out[o++] = (uint8_t)p.xfader;
    out[o++] = (uint8_t)(ph <= 0 ? 0 : ph >= 1 ? 255 : ph * 255 + 0.5f);
    for (int k = 0; k < PANEL_NLAMPS; k++) {
        panel_lamp_rgb(&p, k, out + o);
        o += 3;
    }
    return o;
}

static void *publish_thread(void *arg)
{
    static uint8_t st[STATE_BYTES];
    int64_t last_ping = now_ms();

    (void)arg;
    while (atomic_load(&g.running)) {
        const int64_t now = now_ms();
        const bool ping = now - last_ping >= PING_MS;
        size_t n = 0;

        usleep(FRAME_MS * 1000);
        if (ping) {
            last_ping = now;
        }
        pthread_mutex_lock(&g.lock);
        for (int i = 0; i < g.nclients; i++) {
            Client *c = g.clients[i];

            if (!atomic_load(&c->ws) || atomic_load(&c->closed)) {
                continue;
            }
            if (now - atomic_load(&c->heard) > DEAD_MS) {
                fprintf(stderr, "octemu: remote: a page stopped answering; "
                        "letting go of its keys\n");
                rlog(c, "no reply for %d ms: dropped", DEAD_MS);
                drop(c);
                continue;
            }
            if (ping && !ws_send(c, 0x9, NULL, 0)) {
                continue;
            }
            if (!atomic_load(&c->ready)) {
                if (!ws_send(c, 0x1, g.layout, strlen(g.layout))) {
                    continue;
                }
                c->has_last = false;
                atomic_store(&c->ready, true);
            }
            if (!n) {
                n = state_frame(st);
            }
            if (!c->has_last || memcmp(st, c->last, n)) {
                if (ws_send(c, 0x2, st, n)) {
                    memcpy(c->last, st, n);
                    c->has_last = true;
                }
            }
        }
        pthread_mutex_unlock(&g.lock);
    }
    return NULL;
}

/* ---- lifecycle ------------------------------------------------------------ */
static void build_layout(void)
{
    size_t o = 0;
    const size_t cap = sizeof g.layout;

    o += (size_t)snprintf(g.layout + o, cap - o,
                          "L {\"product\":\"Octatrack\",\"lamps\":[");
    for (int k = 0; k < PANEL_NLAMPS && o < cap; k++) {
        o += (size_t)snprintf(g.layout + o, cap - o, "%s\"%s\"", k ? "," : "",
                              panel_lamps[k].svg);
    }
    if (o < cap) {
        o += (size_t)snprintf(g.layout + o, cap - o, "],\"keys\":{");
    }
    for (int id = 0, first = 1; id < 64 && o < cap; id++) {
        char buf[16];
        const char *svg = panel_key_svg(id, buf, sizeof buf);

        if (svg) {
            o += (size_t)snprintf(g.layout + o, cap - o, "%s\"%s\":%d",
                                  first ? "" : ",", svg, id);
            first = 0;
        }
    }
    if (o < cap) {
        snprintf(g.layout + o, cap - o, "}}");
    }
}

/* -> this machine's address on the local network, as best known. Connecting
 * a UDP socket sends nothing; it only makes the OS pick the interface it
 * would route by. */
static void lan_address(char *out, size_t cap)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(1) };
    socklen_t al = sizeof a;
    const int s = socket(AF_INET, SOCK_DGRAM, 0);

    snprintf(out, cap, "127.0.0.1");
    inet_pton(AF_INET, "10.255.255.255", &a.sin_addr);
    if (s >= 0 && !connect(s, (struct sockaddr *)&a, sizeof a) &&
        !getsockname(s, (struct sockaddr *)&a, &al)) {
        inet_ntop(AF_INET, &a.sin_addr, out, (socklen_t)cap);
    }
    if (s >= 0) {
        close(s);
    }
}

bool remote_start(int first_port)
{
    char addr[64];
    int port;

    pthread_mutex_init(&g.lock, NULL);
    build_layout();
    g.t0 = now_ms();
    if (getenv("OCTA_REMOTE_LOG") && *getenv("OCTA_REMOTE_LOG")) {
        g.log = fopen(getenv("OCTA_REMOTE_LOG"), "a");
        if (!g.log) {
            fprintf(stderr, "octemu: remote: cannot write %s\n",
                    getenv("OCTA_REMOTE_LOG"));
        }
    }
    for (port = first_port; port < first_port + PORTS_TRIED; port++) {
        struct sockaddr_in a = { .sin_family = AF_INET,
                                 .sin_port = htons((uint16_t)port),
                                 .sin_addr.s_addr = htonl(INADDR_ANY) };
        const int one = 1;
        const int fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd < 0) {
            break;
        }
        /* rebinding a port in TIME_WAIT after a restart needs it */
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (!bind(fd, (struct sockaddr *)&a, sizeof a) && !listen(fd, 8)) {
            g.listen_fd = fd;
            g.port = port;
            break;
        }
        close(fd);
    }
    if (g.listen_fd < 0) {
        fprintf(stderr, "octemu: remote: no free port in %d-%d\n",
                first_port, first_port + PORTS_TRIED - 1);
        return false;
    }
    atomic_store(&g.running, true);
    if (pthread_create(&g.accept_thr, NULL, accept_thread, NULL)) {
        atomic_store(&g.running, false);
        close(g.listen_fd);
        g.listen_fd = -1;
        return false;
    }
    if (pthread_create(&g.pub_thr, NULL, publish_thread, NULL)) {
        atomic_store(&g.running, false);
        pthread_join(g.accept_thr, NULL);
        close(g.listen_fd);
        g.listen_fd = -1;
        return false;
    }
    lan_address(addr, sizeof addr);
    snprintf(g.url, sizeof g.url, "http://%s:%d/", addr, g.port);
    fprintf(stderr, "octemu: remote panel at %s\n", g.url);
    return true;
}

const char *remote_url(void)
{
    return atomic_load(&g.running) ? g.url : NULL;
}

void remote_stop(void)
{
    if (!atomic_exchange(&g.running, false)) {
        return;
    }
    pthread_join(g.accept_thr, NULL);
    pthread_join(g.pub_thr, NULL);
    close(g.listen_fd);
    g.listen_fd = -1;
    pthread_mutex_lock(&g.lock);
    for (int i = 0; i < g.nclients; i++) {
        drop(g.clients[i]);
    }
    pthread_mutex_unlock(&g.lock);
    /* Their threads let go of their keys and free themselves; give them a
     * moment, so a key is released before QEMU is told to stop. */
    for (int i = 0; i < 50; i++) {
        int n;

        pthread_mutex_lock(&g.lock);
        n = g.nclients;
        pthread_mutex_unlock(&g.lock);
        if (!n) {
            break;
        }
        usleep(20000);
    }
}
