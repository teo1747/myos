/* TCP (RFC 793), M3 -- an active-open client, enough for the OS to make a real
 * connection: 3-way handshake, in-order data in/out, and a clean FIN close.
 *
 * Scope is deliberate. It is STOP-AND-WAIT (one unacked segment at a time) with
 * simple timeout retransmit, no congestion control, no out-of-order reassembly
 * (SLIRP/loopback deliver in order; anything else is dropped and re-ACKed so the
 * peer resends). Passive open (listen/accept), windows/pipelining, and the
 * ring-3 surface are later work. Like the rest of the stack it is synchronous:
 * the blocking calls drive virtio_net_poll() while they wait.
 *
 * State lives in a small TCB table; tcp_input() runs the state machine, the
 * net_tcp_* calls are the client API. Sequence math uses serial-number
 * comparison so it is correct across the 32-bit wrap. */

#include "net/net.h"
#include "include/kstring.h"
#include "include/kprintf.h"   /* cwnd ramp log */
#include "process/process.h"   /* schedule */

#define TCP_CONNS   4
/* 64 KB, not 16. This is the flow-control window the peer sees, and with four
 * connections it is 256 KB of kernel memory -- cheap next to what it buys: a
 * sender may keep four times as much in flight before it has to stop and wait
 * for us. */
#define TCP_RXBUF   65536
#define TCP_MSS     1400       /* conservative; avoids IP fragmentation */
#define SPIN_MAX    600000     /* poll iterations before a retransmit/timeout */
#define RETRIES     6
#define TCP_ACCEPT_MAX 8       /* pending established conns a LISTEN queues */

enum tcp_state {
    TCP_CLOSED = 0, TCP_SYN_SENT, TCP_ESTABLISHED,
    TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSING,
    TCP_CLOSE_WAIT, TCP_LAST_ACK, TCP_TIME_WAIT,
    TCP_LISTEN, TCP_SYN_RECEIVED,          /* server side (passive open) */
};

struct tcb {
    bool     in_use;
    int      state;
    uint32_t local_ip, remote_ip;
    uint16_t local_port, remote_port;
    uint32_t snd_una;      /* oldest unacked seq */
    uint32_t snd_nxt;      /* next seq to send */
    uint32_t rcv_nxt;      /* next seq we expect */
    uint32_t snd_wnd;      /* peer's advertised receive window (bytes we may keep in flight) */
    uint32_t adv_wnd;      /* what WE last advertised -- see the window update below */
    uint32_t cwnd;         /* congestion window (Tahoe slow-start / congestion avoidance) */
    uint32_t ssthresh;     /* slow-start threshold */
    bool     peer_fin;     /* saw the peer's FIN */
    bool     reset;        /* saw RST */
    uint8_t  rxbuf[TCP_RXBUF];
    uint32_t rx_len;
    /* server side: a LISTEN tcb queues established children here; a child points
     * back at its listener so it can enqueue itself on the final ACK. */
    int      accept_q[TCP_ACCEPT_MAX];
    int      accept_n;
    int      parent_listener;   /* child: index of its LISTEN tcb; else -1 */
};

static struct tcb tcbs[TCP_CONNS];

/* Serial-number comparison (RFC 1982): correct across the u32 wrap. */
#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) <  0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int32_t)((a) - (b)) >  0)
#define SEQ_GEQ(a, b) ((int32_t)((a) - (b)) >= 0)

static struct tcb *tcb_find(uint16_t local_port, uint32_t remote_ip, uint16_t remote_port) {
    for (int i = 0; i < TCP_CONNS; i++) {
        struct tcb *t = &tcbs[i];
        if (t->in_use && t->state != TCP_LISTEN && t->local_port == local_port &&
            t->remote_ip == remote_ip && t->remote_port == remote_port)
            return t;
    }
    return 0;
}

static int tcp_seg(struct tcb *t, uint8_t flags, uint32_t seq,
                   const uint8_t *data, uint32_t len);   /* defined below */

/* A LISTEN tcb bound to `local_port` (server side, passive open). */
static struct tcb *tcb_find_listener(uint16_t local_port) {
    for (int i = 0; i < TCP_CONNS; i++)
        if (tcbs[i].in_use && tcbs[i].state == TCP_LISTEN && tcbs[i].local_port == local_port)
            return &tcbs[i];
    return 0;
}

/* An incoming SYN reached a listener: spin up a child connection in
 * SYN_RECEIVED and answer with SYN-ACK. The child enqueues itself on the
 * listener's accept queue once the final ACK completes the handshake. */
static void tcp_accept_syn(struct tcb *lis, uint32_t src_ip, uint16_t sport, uint32_t syn_seq) {
    static uint32_t srv_isn = 0x50000000u;
    int idx = -1;
    for (int i = 0; i < TCP_CONNS; i++) if (!tcbs[i].in_use) { idx = i; break; }
    if (idx < 0) return;                          /* no room -- client resends its SYN */
    struct tcb *t = &tcbs[idx];
    memset(t, 0, sizeof(*t));
    t->in_use = true;
    t->local_ip = g_netif.ip;   t->local_port = lis->local_port;
    t->remote_ip = src_ip;      t->remote_port = sport;
    t->rcv_nxt = syn_seq + 1;                     /* SYN consumes one seq */
    srv_isn += 0x7C31;
    t->snd_una = t->snd_nxt = srv_isn;
    t->state = TCP_SYN_RECEIVED;
    t->parent_listener = (int)(lis - tcbs);
    tcp_seg(t, TCP_SYN | TCP_ACK, t->snd_una, 0, 0);
    t->snd_nxt = t->snd_una + 1;                  /* our SYN consumes one seq */
}

/* Send one segment carrying `flags` and `len` data bytes at sequence `seq`. A
 * pure ACK/FIN/SYN uses len 0. Window advertises our free receive room. */
static int tcp_seg(struct tcb *t, uint8_t flags, uint32_t seq,
                   const uint8_t *data, uint32_t len) {
    uint8_t seg[sizeof(struct tcp_hdr) + TCP_MSS];
    if (len > TCP_MSS) len = TCP_MSS;
    struct tcp_hdr *th = (struct tcp_hdr *)seg;
    memset(th, 0, sizeof(*th));
    th->src_port = htons(t->local_port);
    th->dst_port = htons(t->remote_port);
    th->seq      = htonl(seq);
    th->ack      = htonl(t->rcv_nxt);
    th->data_off = (sizeof(struct tcp_hdr) / 4) << 4;
    th->flags    = flags;
    uint32_t room = TCP_RXBUF - t->rx_len;
    if (room > 65535) room = 65535;
    th->window   = htons((uint16_t)room);
    t->adv_wnd   = room;              /* what the peer now believes it may send */
    if (len) memcpy(seg + sizeof(*th), data, len);
    th->checksum = net_l4_checksum(t->local_ip, t->remote_ip, IP_PROTO_TCP, seg, sizeof(*th) + len);
    return ip_output(t->local_ip, t->remote_ip, IP_PROTO_TCP, seg, sizeof(*th) + len);
}

/* Send a bare RST to the peer for a segment that matched no connection, so it
 * fails fast instead of retrying/timing out (RFC 793 "reset generation"). If the
 * offending segment carried an ACK, RST.seq = its ack; otherwise RST carries an
 * ACK of its sequence space (seq + data + SYN). */
static void tcp_send_rst(uint32_t peer_ip, uint16_t peer_port, uint16_t local_port,
                         uint8_t in_flags, uint32_t in_seq, uint32_t in_ack, uint32_t in_datalen) {
    struct tcp_hdr th;
    memset(&th, 0, sizeof th);
    th.src_port = htons(local_port);
    th.dst_port = htons(peer_port);
    th.data_off = (sizeof(struct tcp_hdr) / 4) << 4;
    if (in_flags & TCP_ACK) {
        th.seq   = htonl(in_ack);
        th.flags = TCP_RST;
    } else {
        th.seq   = 0;
        th.ack   = htonl(in_seq + in_datalen + ((in_flags & TCP_SYN) ? 1 : 0));
        th.flags = TCP_RST | TCP_ACK;
    }
    th.checksum = net_l4_checksum(g_netif.ip, peer_ip, IP_PROTO_TCP, &th, sizeof th);
    ip_output(g_netif.ip, peer_ip, IP_PROTO_TCP, &th, sizeof th);
}

void tcp_input(uint32_t src_ip, const uint8_t *seg, uint32_t seg_len) {
    if (seg_len < sizeof(struct tcp_hdr)) return;
    const struct tcp_hdr *th = (const struct tcp_hdr *)seg;
    uint32_t doff = (th->data_off >> 4) * 4;
    if (doff < sizeof(*th) || doff > seg_len) return;

    uint16_t sport = ntohs(th->src_port), dport = ntohs(th->dst_port);
    uint8_t  flags = th->flags;
    uint32_t seq = ntohl(th->seq), ack = ntohl(th->ack);
    const uint8_t *data = seg + doff;
    uint32_t datalen = seg_len - doff;

    struct tcb *t = tcb_find(dport, src_ip, sport);
    if (!t) {
        /* No established/handshaking connection for this 4-tuple. A bare SYN may
         * be a new client arriving at one of our listeners (passive open). */
        if ((flags & (TCP_SYN | TCP_ACK)) == TCP_SYN) {
            struct tcb *lis = tcb_find_listener(dport);
            if (lis) { tcp_accept_syn(lis, src_ip, sport, seq); return; }
        }
        /* Nothing here (closed port, or a stray segment). RST the peer so it
         * fails fast -- but never answer a RST with a RST. */
        if (!(flags & TCP_RST))
            tcp_send_rst(src_ip, sport, dport, flags, seq, ack, datalen);
        return;
    }

    if (flags & TCP_RST) { t->reset = true; t->state = TCP_CLOSED; return; }

    /* Advance snd_una on a valid ACK, and track the peer's advertised window so
     * the sender can keep multiple segments in flight (windowed throughput). */
    if ((flags & TCP_ACK) && SEQ_GT(ack, t->snd_una) && SEQ_LEQ(ack, t->snd_nxt))
        t->snd_una = ack;
    if (flags & TCP_ACK)
        t->snd_wnd = ntohs(th->window);

    switch (t->state) {
    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == t->snd_nxt) {
            t->rcv_nxt = seq + 1;         /* SYN consumes one sequence number */
            t->snd_una = ack;
            t->state = TCP_ESTABLISHED;
            tcp_seg(t, TCP_ACK, t->snd_nxt, 0, 0);   /* complete the handshake */
        }
        return;

    case TCP_SYN_RECEIVED:
        /* The client's ACK completes the passive handshake -> ESTABLISHED, and
         * the connection joins its listener's accept queue. */
        if (!((flags & TCP_ACK) && ack == t->snd_nxt)) return;
        t->state = TCP_ESTABLISHED;
        if (t->parent_listener >= 0) {
            struct tcb *lis = &tcbs[t->parent_listener];
            if (lis->in_use && lis->state == TCP_LISTEN && lis->accept_n < TCP_ACCEPT_MAX)
                lis->accept_q[lis->accept_n++] = (int)(t - tcbs);
        }
        /* fall through: this same segment may already carry request bytes. */
        /* fall through */
    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
        if (datalen && seq == t->rcv_nxt) {          /* in-order data */
            uint32_t room = TCP_RXBUF - t->rx_len;
            uint32_t k = datalen < room ? datalen : room;
            memcpy(t->rxbuf + t->rx_len, data, k);
            t->rx_len   += k;
            t->rcv_nxt  += k;
        } else if (datalen && SEQ_LT(seq, t->rcv_nxt)) {
            tcp_seg(t, TCP_ACK, t->snd_nxt, 0, 0);   /* duplicate: re-ACK */
        }
        if ((flags & TCP_FIN) && seq + datalen == t->rcv_nxt) {
            t->rcv_nxt += 1;                          /* FIN consumes one seq */
            t->peer_fin = true;
            if (t->state == TCP_ESTABLISHED)      t->state = TCP_CLOSE_WAIT;
            else if (t->state == TCP_FIN_WAIT_1)  t->state = TCP_CLOSING;
            else if (t->state == TCP_FIN_WAIT_2)  t->state = TCP_TIME_WAIT;
        }
        if (datalen || (flags & TCP_FIN))
            tcp_seg(t, TCP_ACK, t->snd_nxt, 0, 0);   /* ACK what we accepted */

        /* Our own FIN getting acked. */
        if (t->state == TCP_FIN_WAIT_1 && t->snd_una == t->snd_nxt) t->state = TCP_FIN_WAIT_2;
        if (t->state == TCP_CLOSING     && t->snd_una == t->snd_nxt) t->state = TCP_TIME_WAIT;
        return;

    case TCP_LAST_ACK:
        if (t->snd_una == t->snd_nxt) t->state = TCP_CLOSED;
        return;
    default:
        return;
    }
}

/* ---- blocking client API ------------------------------------------------ */
static bool poll_until(struct tcb *t, int want_state_reached) {
    uint64_t deadline = net_ticks() + NET_RTX_TICKS;   /* one SYN-retransmit interval */
    while (net_ticks() < deadline) {
        if (t->reset) return false;
        if (want_state_reached && t->state == want_state_reached) return true;
        net_wait();
    }
    return false;
}

/* Ephemeral port + ISN sources, shared by the blocking and non-blocking active
 * opens so the two never hand out the same local port. */
static uint16_t g_next_port = 40000;
static uint32_t g_next_isn  = 0x1000;

/* Non-blocking active open: allocate a TCB, send ONE SYN, and return the conn
 * index in SYN_SENT WITHOUT waiting. The RX kthread advances it to ESTABLISHED
 * when the SYN-ACK arrives; net_tcp_ready() reports when. -1 = no free TCB.
 * No retransmit -- fine on SLIRP (which does not drop); a lost SYN surfaces as a
 * socket that never becomes writable (select times out), the honest outcome. */
int net_tcp_connect_start(uint32_t dst_ip, uint16_t dst_port) {
    net_lock();
    int idx = -1;
    for (int i = 0; i < TCP_CONNS; i++) if (!tcbs[i].in_use) { idx = i; break; }
    if (idx < 0) { net_unlock(); return -1; }

    struct tcb *t = &tcbs[idx];
    memset(t, 0, sizeof(*t));
    t->in_use = true;
    t->local_ip = g_netif.ip;    t->remote_ip = dst_ip;
    t->local_port = g_next_port++; t->remote_port = dst_port;
    g_next_isn += 0x9E3F;                        /* bump the ISN each connection */
    t->snd_una = t->snd_nxt = g_next_isn;
    t->state = TCP_SYN_SENT;
    tcp_seg(t, TCP_SYN, t->snd_una, 0, 0);
    t->snd_nxt = t->snd_una + 1;                 /* SYN consumes one seq */
    net_unlock();
    return idx;
}

int net_tcp_connect(uint32_t dst_ip, uint16_t dst_port) {
    int idx = net_tcp_connect_start(dst_ip, dst_port);   /* sends the first SYN */
    if (idx < 0) return -1;

    net_lock();
    struct tcb *t = &tcbs[idx];
    int rc = -1;
    for (int r = 0; r < RETRIES; r++) {
        if (poll_until(t, TCP_ESTABLISHED)) { rc = idx; break; }
        if (t->reset) break;
        if (r + 1 < RETRIES) {                   /* retransmit SYN at the same ISN */
            tcp_seg(t, TCP_SYN, t->snd_una, 0, 0);
            t->snd_nxt = t->snd_una + 1;
        }
    }
    if (rc < 0) t->in_use = false;
    net_unlock();
    return rc;
}

/* Non-blocking receive: like net_tcp_recv but never waits.
 *   >0  bytes copied
 *    0  EOF (peer FIN / reset, no data left)
 *   -2  would block (connection live but no data buffered yet)
 *   -1  bad conn */

/* THE WINDOW UPDATE, and the reason a 1 MB page took two minutes.
 *
 * The window we advertise is `TCP_RXBUF - rx_len`, and it is only ever put on
 * the wire by tcp_seg -- which runs when a SEGMENT ARRIVES. So once the buffer
 * filled and we advertised zero, the sender stopped; because it stopped,
 * nothing arrived; because nothing arrived, we never sent the ACK that would
 * have told it the application had drained the buffer. The transfer then moved
 * only when the peer's persist timer fired, roughly twice a second, which is
 * precisely the ~8 KB/s this stack was getting on any transfer big enough to
 * fill the window -- over plain HTTP as well as TLS, which is what proved it
 * was not the crypto.
 *
 * So: after the application takes bytes out, if the room we now have is
 * meaningfully larger than what the peer last heard, send a pure ACK carrying
 * the new window. Gated on a MSS-sized improvement so a reader draining a few
 * bytes at a time cannot turn every read into a packet. */
static void tcp_window_update(struct tcb *t, uint32_t freed) {
    if (!freed || t->state != TCP_ESTABLISHED) return;
    uint32_t room = TCP_RXBUF - t->rx_len;
    if (room > 65535) room = 65535;
    /* Worth an announcement when the peer thinks it has less than a segment's
     * worth of room and we now have at least two. */
    if (t->adv_wnd < TCP_MSS && room >= 2 * TCP_MSS)
        tcp_seg(t, TCP_ACK, t->snd_nxt, 0, 0);
}

int net_tcp_recv_nb(int conn, void *buf, uint32_t cap) {
    if (conn < 0 || conn >= TCP_CONNS) return -1;
    net_lock();
    struct tcb *t = &tcbs[conn];
    if (!t->in_use) { net_unlock(); return -1; }

    if (t->rx_len == 0) {
        int r = (t->peer_fin || t->reset || t->state == TCP_CLOSED) ? 0 : -2;
        net_unlock();
        return r;
    }
    uint32_t k = t->rx_len < cap ? t->rx_len : cap;
    memcpy(buf, t->rxbuf, k);
    if (k < t->rx_len) memmove(t->rxbuf, t->rxbuf + k, t->rx_len - k);
    t->rx_len -= k;
    tcp_window_update(t, k);
    net_unlock();
    return (int)k;
}

/* Poll readiness WITHOUT waiting -- see net.h. Returns a TCP_RDY_* bitmask, or
 * -1 for a bad/closed conn. */
int net_tcp_ready(int conn) {
    if (conn < 0 || conn >= TCP_CONNS) return -1;
    net_lock();
    struct tcb *t = &tcbs[conn];
    if (!t->in_use) { net_unlock(); return -1; }

    int r = 0;
    /* Readable: buffered data, or a FIN/reset (recv would return 0/EOF, not block). */
    if (t->rx_len > 0 || t->peer_fin || t->reset || t->state == TCP_CLOSED)
        r |= TCP_RDY_READ;
    /* Writable: established (send never blocks here). */
    if (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT)
        r |= TCP_RDY_WRITE;
    /* A connect that resolved to reset/closed is reported writable+error so a
     * caller polling for connect completion wakes and learns the failure. */
    if (t->reset || t->state == TCP_CLOSED)
        r |= TCP_RDY_ERR | TCP_RDY_WRITE;
    net_unlock();
    return r;
}

/* Passive open: put a TCB into LISTEN on `port`. Returns the listen conn index
 * (a "listening socket") or -1. Non-blocking. */
int net_tcp_listen(uint16_t port) {
    net_lock();
    int idx = -1;
    for (int i = 0; i < TCP_CONNS; i++) if (!tcbs[i].in_use) { idx = i; break; }
    if (idx < 0) { net_unlock(); return -1; }
    struct tcb *t = &tcbs[idx];
    memset(t, 0, sizeof(*t));
    t->in_use = true;
    t->state = TCP_LISTEN;
    t->local_ip = g_netif.ip;
    t->local_port = port;
    t->parent_listener = -1;
    net_unlock();
    return idx;
}

/* Block until a connection has completed its handshake on this listener, then
 * hand back its conn index (an ESTABLISHED connection ready to read/write).
 * Returns -1 on a bad listener or if none arrived within the bound. */
int net_tcp_accept(int listen_conn) {
    if (listen_conn < 0 || listen_conn >= TCP_CONNS) return -1;
    net_lock();
    struct tcb *lis = &tcbs[listen_conn];
    if (!lis->in_use || lis->state != TCP_LISTEN) { net_unlock(); return -1; }

    int child = -1;
    uint64_t deadline = net_ticks() + NET_TMO_TICKS;
    while (child < 0 && net_ticks() < deadline) {
        if (lis->accept_n > 0) {
            child = lis->accept_q[0];
            for (int j = 1; j < lis->accept_n; j++) lis->accept_q[j - 1] = lis->accept_q[j];
            lis->accept_n--;
            break;
        }
        net_wait();
    }
    net_unlock();
    return child;
}

int net_tcp_send(int conn, const void *data, uint32_t len) {
    if (conn < 0 || conn >= TCP_CONNS) return -1;
    net_lock();
    struct tcb *t = &tcbs[conn];
    if (!t->in_use || (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT)) {
        net_unlock(); return -1;
    }

    /* WINDOWED + CONGESTION-CONTROLLED send. We window directly over the caller's
     * buffer (send is synchronous, so `data` stays valid). The bytes in flight are
     * capped by min(peer's advertised window, cwnd) -- cwnd is TCP Tahoe: it opens
     * exponentially (slow start) up to ssthresh, then linearly (congestion
     * avoidance), and on a retransmit timeout it drops to one MSS with ssthresh =
     * cwnd/2. On this lossless link cwnd just ramps to the flow-control window and
     * stays; the machinery is here for correctness on a real path. `snd_una ==
     * snd_nxt` at entry (the previous send drained), so base is snd_nxt. */
    const uint8_t *p = (const uint8_t *)data;
    uint32_t base = t->snd_nxt;
    int failed = 0, retransmits = 0;
    if (t->cwnd == 0) { t->cwnd = 4 * TCP_MSS; t->ssthresh = 65535; }   /* IW = 4*MSS */
    uint64_t last_progress = net_ticks();

    while (!failed) {
        if (t->snd_una - base >= len) break;            /* everything acked -> done */

        /* Effective window = min(peer receive window, congestion window). */
        uint32_t wnd = t->snd_wnd ? t->snd_wnd : TCP_MSS;
        if (t->cwnd < wnd) wnd = t->cwnd;
        for (;;) {
            uint32_t off = t->snd_nxt - base;
            uint32_t inflight = t->snd_nxt - t->snd_una;
            if (off >= len || inflight >= wnd) break;
            uint32_t chunk = len - off;
            if (chunk > TCP_MSS) chunk = TCP_MSS;
            if (inflight + chunk > wnd) chunk = wnd - inflight;
            if (chunk == 0) break;
            tcp_seg(t, TCP_PSH | TCP_ACK, t->snd_nxt, p + off, chunk);
            t->snd_nxt += chunk;
        }

        /* Sleep for ACK progress; RX delivery (or the 10 ms tick) wakes us, and
         * one drain can advance snd_una several segments at once. */
        uint32_t una_before = t->snd_una;
        net_wait();
        if (t->reset) { failed = 1; break; }
        if (t->snd_una != una_before) {                 /* forward progress */
            last_progress = net_ticks();
            if (t->cwnd < t->ssthresh) t->cwnd += TCP_MSS;               /* slow start */
            else t->cwnd += (TCP_MSS * TCP_MSS) / t->cwnd + 1;           /* congestion avoidance */
            if (t->cwnd > 65535) t->cwnd = 65535;
        } else if (net_ticks() - last_progress > NET_RTX_TICKS) {        /* timed out: Tahoe */
            if (++retransmits > RETRIES) { failed = 1; break; }
            t->ssthresh = t->cwnd / 2;
            if (t->ssthresh < 2 * TCP_MSS) t->ssthresh = 2 * TCP_MSS;
            t->cwnd = TCP_MSS;                          /* restart slow start */
            t->snd_nxt = t->snd_una;                    /* go-back-N: resend from snd_una */
            last_progress = net_ticks();
        }
    }
    if (len > 8192)
        kprintf("[tcp] sent %u bytes, cwnd ramped to %u (ssthresh %u)\n",
                (unsigned)len, (unsigned)t->cwnd, (unsigned)t->ssthresh);

    uint32_t done = t->snd_una - base;
    int ret = (failed && done == 0) ? -1 : (int)(done < len ? done : len);
    net_unlock();
    return ret;
}

int net_tcp_recv(int conn, void *buf, uint32_t cap) {
    if (conn < 0 || conn >= TCP_CONNS) return -1;
    net_lock();
    struct tcb *t = &tcbs[conn];
    if (!t->in_use) { net_unlock(); return -1; }

    uint64_t deadline = net_ticks() + NET_TMO_TICKS;
    int timed_out = 0;
    while (t->rx_len == 0) {
        if (t->peer_fin || t->reset || t->state == TCP_CLOSED) break;
        if (net_ticks() >= deadline) { timed_out = 1; break; }
        net_wait();
    }
    uint32_t k = t->rx_len < cap ? t->rx_len : cap;
    if (k) {
        memcpy(buf, t->rxbuf, k);
        if (k < t->rx_len) memmove(t->rxbuf, t->rxbuf + k, t->rx_len - k);
        t->rx_len -= k;
        tcp_window_update(t, k);
    }
    net_unlock();
    /* A TIMEOUT IS NOT AN END OF STREAM, and saying 0 for both was the bug that
     * made this browser unable to load a large page.
     *
     * Three seconds with nothing arriving used to return 0, which every layer
     * above reads as "the peer is done": io_recv_exact turned it into -1,
     * tls_read into -1, and the HTTP loop into "the response ended here" -- so
     * a 367KB page became whatever had arrived by the first gap, with status
     * 200 and no error anywhere. The document rendered, short, and nothing in
     * the system knew. It got worse the bigger the page, which is exactly
     * backwards from how a browser should fail.
     *
     * -2 is the code the non-blocking variant already uses for "nothing yet",
     * so the fd layer's mapping to EAGAIN is the one that was already there.
     * A caller mid-record has to retry; a caller between messages may stop. */
    if (!k && timed_out) return -2;
    return (int)k;                               /* 0 = EOF (peer FIN, no data left) */
}

/* Non-blocking teardown for the fd reap path (runs under g_sched_lock, where the
 * polling close() below cannot). Drops the TCB; the peer times its side out. */
void net_tcp_abort(int conn) {
    if (conn < 0 || conn >= TCP_CONNS) return;
    tcbs[conn].state = TCP_CLOSED;
    tcbs[conn].in_use = false;
}

void net_tcp_close(int conn) {
    if (conn < 0 || conn >= TCP_CONNS) return;
    net_lock();
    struct tcb *t = &tcbs[conn];
    if (!t->in_use) { net_unlock(); return; }

    if (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) {
        int last = (t->state == TCP_CLOSE_WAIT);
        uint32_t seq = t->snd_nxt;
        t->state = last ? TCP_LAST_ACK : TCP_FIN_WAIT_1;
        tcp_seg(t, TCP_FIN | TCP_ACK, seq, 0, 0);
        t->snd_nxt = seq + 1;                    /* FIN consumes one seq */
        /* Best-effort wait for the close to settle (not required for correctness
         * on our side; SLIRP tears down regardless). */
        uint64_t deadline = net_ticks() + NET_RTX_TICKS;
        while (net_ticks() < deadline) {
            if (t->state == TCP_CLOSED || t->state == TCP_TIME_WAIT ||
                t->state == TCP_FIN_WAIT_2 || t->reset) break;
            net_wait();
        }
    }
    t->in_use = false;
    net_unlock();
}
