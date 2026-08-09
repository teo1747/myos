/* wget.c -- a real HTTP/HTTPS downloader for EmbLinkOS. Resolves a host, opens a
 * TCP connection, (for https) runs an AUTHENTICATED TLS 1.3 handshake via our own
 * libtls, sends an HTTP/1.0 GET, splits the response header from the body, and
 * writes the body to a file (or stdout). Where this OS's networking stack meets
 * its filesystem AND its TLS stack: "fetch a file from the internet and save it."
 *
 * Built on the BSD-sockets shim (embk_socket.h) over the native CAP_NETWORK
 * socket syscalls, libtls for https, plus newlib file I/O (CAP_FILESYSTEM to
 * write a file). https:// verifies the server certificate (chain -> a bundled
 * trust anchor, hostname, validity, CertificateVerify) -- it refuses on failure.
 *
 * usage: wget [-O outfile] http[s]://host[:port]/path
 * exit:  0 = 2xx downloaded;  1 usage  2 bad-url  3 resolve  4 socket
 *        5 connect  6 open-outfile  7 non-2xx status  8 TLS handshake failed
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "embk.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "embk_socket.h"
#include "tls.h"

static int parse_url(const char *url, char *host, int hostsz, int *port,
                     char *path, int pathsz, int *https) {
    const char *p = url;
    *https = 0;
    if (strncmp(p, "https://", 8) == 0) { *https = 1; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) p += 7;
    else return -1;
    int i = 0;
    while (*p && *p != ':' && *p != '/' && i < hostsz - 1) host[i++] = *p++;
    host[i] = 0;
    if (i == 0) return -1;
    *port = *https ? 443 : 80;
    if (*p == ':') { p++; *port = atoi(p); while (*p && *p != '/') p++; }
    if (*p == '/') { strncpy(path, p, pathsz - 1); path[pathsz - 1] = 0; }
    else strcpy(path, "/");
    return 0;
}

/* A transport: plain socket, or TLS over it. */
struct xport { int fd; struct tls_conn *tls; };
static long x_send(struct xport *x, const void *b, size_t n) {
    return x->tls ? tls_write(x->tls, b, n) : send(x->fd, b, n, 0);
}
static long x_recv(struct xport *x, void *b, size_t n) {
    return x->tls ? tls_read(x->tls, b, n) : recv(x->fd, b, n, 0);
}
static void x_close(struct xport *x) {
    if (x->tls) { tls_close(x->tls); free(x->tls); }   /* tls_close closes the fd */
    else close(x->fd);
}

int main(int argc, char **argv)
{
    const char *outfile = NULL, *url = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-O") && i + 1 < argc) outfile = argv[++i];
        else url = argv[i];
    }
    if (!url) { fprintf(stderr, "usage: wget [-O file] http[s]://host/path\n"); return 1; }

    char host[128], path[256]; int port, https;
    if (parse_url(url, host, sizeof host, &port, path, sizeof path, &https) < 0) {
        fprintf(stderr, "wget: bad URL '%s'\n", url); return 2;
    }

    uint64_t t_start = embk_uptime_ms(), t_ready = t_start;

    struct in_addr addr;
    if (emb_resolve(host, &addr) != 0) { fprintf(stderr, "wget: cannot resolve %s\n", host); return 3; }
    unsigned int h = ntohl(addr.s_addr);
    fprintf(stderr, "wget: %s -> %u.%u.%u.%u, %s GET %s\n", host,
            (h >> 24) & 255, (h >> 16) & 255, (h >> 8) & 255, h & 255,
            https ? "https" : "http", path);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { fprintf(stderr, "wget: socket failed (%d)\n", fd); return 4; }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons((unsigned short)port); sa.sin_addr = addr;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        fprintf(stderr, "wget: connect failed\n"); return 5;
    }

    struct xport x = { fd, NULL };
    if (https) {
        x.tls = malloc(sizeof(struct tls_conn));
        if (!x.tls) { fprintf(stderr, "wget: oom\n"); close(fd); return 8; }
        int r = tls_connect(x.tls, fd, host);
        if (r != 0) {
            fprintf(stderr, "wget: TLS handshake failed (rc=%d) -- certificate not trusted?\n", r);
            free(x.tls); close(fd); return 8;
        }
        fprintf(stderr, "wget: TLS 1.3 established -- server authenticated\n");
    }

    t_ready = embk_uptime_ms();     /* resolve + connect + handshake are done */

    char req[512];
    int rl = snprintf(req, sizeof req,
                      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: EmbLinkOS-wget\r\n"
                      "Connection: close\r\n\r\n", path, host);
    x_send(&x, req, (size_t)rl);

    int out = 1;                                   /* default: stdout */
    if (outfile) {
        out = open(outfile, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (out < 0) { fprintf(stderr, "wget: cannot open %s\n", outfile); x_close(&x); return 6; }
    }

    /* Read the stream; header ends at the first CRLFCRLF, rest is body. */
    static char hdr[4096];
    char buf[2048];
    int hlen = 0, header_done = 0, status = 0, body = 0;
    int stalls = 0;
    for (;;) {
        long n = x_recv(&x, buf, sizeof buf);
        /* Nothing yet is not nothing coming -- the same distinction the browser
         * needs, for the same reason: this loop cannot tell a slow server from
         * a finished one except by asking again. */
        if (n < 0 && errno == EAGAIN) { if (++stalls > 10) break; continue; }
        if (n <= 0) break;
        stalls = 0;
        int off = 0;
        if (!header_done) {
            for (int i = 0; i < n && !header_done; i++) {
                if (hlen < (int)sizeof(hdr) - 1) hdr[hlen++] = buf[i];
                if (hlen >= 4 && hdr[hlen-4]=='\r' && hdr[hlen-3]=='\n' &&
                                 hdr[hlen-2]=='\r' && hdr[hlen-1]=='\n') {
                    header_done = 1; off = i + 1;
                }
            }
            if (!header_done) continue;
            hdr[hlen] = 0;
            if (!strncmp(hdr, "HTTP/1.", 7)) status = atoi(hdr + 9);
        }
        int blen = (int)n - off;
        if (blen > 0) { write(out, buf + off, blen); body += blen; }
    }
    x_close(&x);
    if (outfile) close(out);

    /* THE RATE, not just the total. A downloader that reports only bytes cannot
     * answer the only question anyone asks when a page takes two minutes --
     * and the rate is the number that separates "the file is big" from "the
     * stack is slow". Split from the handshake, because on a small file the
     * handshake IS the download time. */
    unsigned long total_ms = (unsigned long)(embk_uptime_ms() - t_start);
    unsigned long xfer_ms  = (unsigned long)(embk_uptime_ms() - t_ready);
    fprintf(stderr, "wget: HTTP %d, %d bytes in %lu.%lus (%lu KB/s), setup %lu.%lus%s%s\n",
            status, body, xfer_ms / 1000, (xfer_ms % 1000) / 100,
            xfer_ms ? ((unsigned long)body * 1000UL / xfer_ms) / 1024 : 0,
            (total_ms - xfer_ms) / 1000, ((total_ms - xfer_ms) % 1000) / 100,
            outfile ? " -> " : "", outfile ? outfile : "");
    return (status / 100 == 2) ? 0 : 7;
}
