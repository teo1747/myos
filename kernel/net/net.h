#ifndef __NET_H__
#define __NET_H__

/* EmbLinkOS networking, M1: virtio-net + Ethernet/ARP/IPv4/ICMP, enough for the
 * OS to ARP-resolve a host on its segment and complete an ICMP echo round trip.
 * The userspace surface (native CAP_NETWORK endpoint handles + a sockets shim
 * for ports) is M4 -- none of this file is exposed to ring 3 yet. IPs are held
 * in HOST byte order and converted at the wire boundary (htonl/ntohl). */

#include <stdint.h>
#include "include/types.h"   /* kernel bool */

/* ---- byte order (x86 is little-endian; the wire is big-endian) ---------- */
static inline uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

/* An IPv4 address in HOST order, e.g. 10.0.2.15 -> 0x0A00020F. */
#define IPV4(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* The four octets of a HOST-order IPv4 address, for kprintf("%u.%u.%u.%u"). */
#define IP_OCTETS(ip) \
    (uint8_t)((ip) >> 24), (uint8_t)((ip) >> 16), (uint8_t)((ip) >> 8), (uint8_t)(ip)

/* Read/write an IPv4 address (HOST order) as 4 wire-order bytes, and read a
 * big-endian 16-bit field. Shared so no module re-rolls its own. */
static inline uint32_t net_ip_rd(const uint8_t *b)   { return IPV4(b[0], b[1], b[2], b[3]); }
static inline void     net_ip_wr(uint32_t ip, uint8_t *b) { b[0]=ip>>24; b[1]=ip>>16; b[2]=ip>>8; b[3]=ip; }
static inline uint16_t net_rd16be(const uint8_t *b)  { return ((uint16_t)b[0] << 8) | b[1]; }

#define ETH_ALEN 6
#define ETH_HLEN 14
#define ETH_FRAME_MAX 1514            /* 14 header + 1500 MTU */

#define ETH_P_IP  0x0800
#define ETH_P_ARP 0x0806

struct eth_hdr {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;               /* network order */
} __attribute__((packed));

/* ---- ARP (IPv4 over Ethernet) ------------------------------------------- */
#define ARP_HTYPE_ETH 1
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

struct arp_pkt {
    uint16_t htype, ptype;            /* network order */
    uint8_t  hlen, plen;
    uint16_t oper;                    /* network order */
    uint8_t  sha[ETH_ALEN];
    uint8_t  spa[4];                  /* wire order (4 bytes) */
    uint8_t  tha[ETH_ALEN];
    uint8_t  tpa[4];
} __attribute__((packed));

/* ---- IPv4 --------------------------------------------------------------- */
#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define IP_PROTO_TCP  6

struct ip_hdr {
    uint8_t  ver_ihl;                 /* 0x45 = v4, 5 words */
    uint8_t  dscp;
    uint16_t total_len;               /* network order */
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;                     /* network order */
    uint32_t dst;                     /* network order */
} __attribute__((packed));

/* ---- ICMP --------------------------------------------------------------- */
#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0
#define ICMP_DEST_UNREACH 3
#define ICMP_PORT_UNREACH 3   /* code within DEST_UNREACH */

struct icmp_hdr {
    uint8_t  type, code;
    uint16_t checksum;
    uint16_t id, seq;                 /* network order */
} __attribute__((packed));

/* ---- UDP ---------------------------------------------------------------- */
struct udp_hdr {
    uint16_t src_port, dst_port;      /* network order */
    uint16_t len;                     /* network order: header + data */
    uint16_t checksum;
} __attribute__((packed));

/* ---- TCP ---------------------------------------------------------------- */
struct tcp_hdr {
    uint16_t src_port, dst_port;      /* network order */
    uint32_t seq;                     /* network order */
    uint32_t ack;                     /* network order */
    uint8_t  data_off;                /* high 4 bits = header length in 32-bit words */
    uint8_t  flags;                   /* TCP_* below */
    uint16_t window;                  /* network order */
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

/* ---- the single network interface (M1: one NIC, static config) ---------- */
struct netif {
    bool     up;
    bool     dhcp;                    /* true once a DHCP lease configured us */
    uint8_t  mac[ETH_ALEN];
    uint32_t ip;                      /* HOST order */
    uint32_t netmask;                 /* HOST order */
    uint32_t gateway;                 /* HOST order */
    uint32_t dns;                     /* HOST order (from DHCP option 6) */
};

extern struct netif g_netif;

/* Internet checksum (RFC 1071): ones-complement sum over `len` bytes. */
uint16_t net_checksum(const void *data, uint32_t len);

/* Transport (UDP/TCP) checksum: the RFC 768/793 pseudo-header (src, dst, proto,
 * length) folded together with the `seg` bytes. Returns the raw folded ~sum;
 * UDP applies its own "0 -> 0xFFFF" rule, TCP stores it as-is. */
uint16_t net_l4_checksum(uint32_t src_ip, uint32_t dst_ip, uint8_t proto,
                         const void *seg, uint32_t seg_len);

/* ---- stack entry points ------------------------------------------------- */
void net_init(void);                          /* bring up the NIC + static config */
void net_rx(const uint8_t *frame, uint32_t len);   /* driver -> stack (one Ethernet frame) */

/* eth: build+send a frame carrying `payload` to `dst_mac` with `ethertype`. */
int  net_tx_eth(const uint8_t dst_mac[ETH_ALEN], uint16_t ethertype,
                const void *payload, uint32_t len);

/* ARP: resolve `ip` (HOST order) to a MAC, sending a request and waiting up to
 * a bounded number of poll cycles. Returns true and fills mac on success. */
bool arp_resolve(uint32_t ip, uint8_t mac_out[ETH_ALEN]);

/* IPv4: send `payload` (an ICMP/UDP/... message) to `dst_ip` (HOST order). */
int  net_send_ip(uint32_t dst_ip, uint8_t proto, const void *payload, uint32_t len);

/* UDP: send a datagram. src_ip may be 0 (unconfigured, e.g. DHCP DISCOVER);
 * dst_ip 255.255.255.255 goes out as an Ethernet broadcast (no ARP). */
int  net_send_udp(uint32_t src_ip, uint16_t src_port,
                  uint32_t dst_ip, uint16_t dst_port,
                  const void *payload, uint32_t len);

/* DHCP: run DISCOVER/OFFER/REQUEST/ACK and, on success, configure g_netif
 * (ip/netmask/gateway/dns) and set g_netif.dhcp. Returns true on a lease. */
bool net_dhcp(void);

/* DNS: resolve `name` to an IPv4 address (HOST order) via the DHCP-learned
 * resolver (g_netif.dns), UDP/53. Returns true and fills *out_ip on the first
 * A record. Needs a reachable resolver (SLIRP forwards to the host's). */
bool net_resolve(const char *name, uint32_t *out_ip);

/* ICMP: send one echo request to `dst_ip` and wait for the matching reply.
 * Returns true on a reply (the M1 witness). */
bool net_ping(uint32_t dst_ip);

/* TCP client (M3): a blocking, stop-and-wait API that drives virtio_net_poll()
 * while it waits -- consistent with the rest of the synchronous stack. Returns
 * a small connection index (>= 0) or -1. Not the ring-3 surface (that is M4). */
int  net_tcp_connect(uint32_t dst_ip, uint16_t dst_port);   /* active open, blocks to ESTABLISHED */
int  net_tcp_connect_start(uint32_t dst_ip, uint16_t dst_port); /* non-blocking: send SYN, return conn (SYN_SENT) */
int  net_tcp_recv_nb(int conn, void *buf, uint32_t cap);    /* like recv but -2 = would-block (no data yet) */
/* Readiness for select()/poll on a socket, WITHOUT waiting:
 *   bit 0 (readable): data buffered, or peer FIN/reset seen (recv won't block)
 *   bit 1 (writable): connection is ESTABLISHED (or resolved to an error)
 *   bit 2 (error):    reset / closed abnormally
 * Returns a negative errno for a bad conn. */
int  net_tcp_ready(int conn);
#define TCP_RDY_READ  0x1
#define TCP_RDY_WRITE 0x2
#define TCP_RDY_ERR   0x4
int  net_tcp_listen(uint16_t port);                         /* passive open -> a listen conn index */
int  net_tcp_accept(int listen_conn);                       /* block for a client -> its conn index */
int  net_tcp_send(int conn, const void *data, uint32_t len);/* send + wait for ACK */
int  net_tcp_recv(int conn, void *buf, uint32_t cap);       /* bytes; 0 = peer FIN/EOF; -2 = timed out, still open; -1 = bad conn */
void net_tcp_close(int conn);                               /* FIN handshake, then free */
void net_tcp_abort(int conn);                               /* free the TCB WITHOUT blocking (reap path) */

/* Ring-3 UDP sockets (M5): a datagram socket with a bound local port and a small
 * receive queue. Returns a socket index (>= 0) or -1. Under the big net lock. */
int  net_udp_open(void);
int  net_udp_bind(int us, uint16_t port);
int  net_udp_sendto(int us, uint32_t dst_ip, uint16_t dst_port, const void *data, uint32_t len);
int  net_udp_recvfrom(int us, void *buf, uint32_t cap, uint32_t *src_ip, uint16_t *src_port);
void net_udp_close(int us);
void net_udp_abort(int us);                                 /* lock-free free (fd reap path) */

/* ---- internal cross-layer hooks (each module -> its neighbour) ----------
 * These are how the layers hand a packet up (input) or down (output) across
 * .c files; they are not part of any ring-3 surface. Ownership:
 *   ETH_BCAST, net_rx  -> net.c        arp_input       -> ethernet/arp.c
 *   ip_output/ip_input -> ip/ipv4.c    icmp_input      -> ip/icmp.c
 *   udp_input          -> udp/udp.c    udp_arm/collect -> udp/udp.c            */
extern const uint8_t ETH_BCAST[ETH_ALEN];

/* The big net lock (net.c): sleeping, owner-recursive. Taken by the public entry
 * points so all shared state (tcbs, arp cache, capture) is serialised across
 * cores. net_yield() is the blocking-wait step: it drains RX under the lock then
 * RELEASES across schedule() so a blocked client does not camp the whole stack. */
void net_lock(void);
void net_unlock(void);
void net_yield(void);      /* poll-drain (locked) + release + schedule + reacquire */

/* Event-driven RX: clients SLEEP instead of polling. net_wait() releases net_lock
 * and blocks until RX delivery or the 10 ms tick wakes it, then reacquires (a
 * depth-safe monitor wait). net_signal_rx() is called from the NIC ISR; net_tick()
 * from the LAPIC timer; net_ticks() is the 10 ms clock clients time out against. */
void net_wait(void);
void net_signal_rx(void);
void net_tick(void);
uint64_t net_ticks(void);

#define NET_TMO_TICKS  300   /* give-up timeout, 10 ms ticks -> ~3 s   */
#define NET_RTX_TICKS   50   /* retransmit interval, ~500 ms           */

/* Contention instrumentation (the EMBKFS rule: measure before splitting further).
 * `contended` = acquisitions that actually had to block on another owner. */
struct net_lockstat { uint64_t acquires, recursive, contended; };
void net_lockstat_get(struct net_lockstat *out);
void net_lockstat_reset(void);

void arp_input(const uint8_t *pkt, uint32_t len);                     /* eth demux -> ARP */
int  ip_output(uint32_t src_ip, uint32_t dst_ip, uint8_t proto,       /* UDP/ICMP -> IP  */
               const void *payload, uint32_t len);
void ip_input(const uint8_t *pkt, uint32_t len);                      /* eth demux -> IP */
void icmp_input(uint32_t src_ip, const uint8_t *msg, uint32_t len);   /* IP demux -> ICMP */
bool udp_input(uint32_t src_ip, const uint8_t *seg, uint32_t seg_len);/* IP demux -> UDP; true if delivered */
void tcp_input(uint32_t src_ip, const uint8_t *seg, uint32_t seg_len);/* IP demux -> TCP */

/* ICMP dest/port-unreachable back to `dst_ip`, quoting the offending datagram's
 * IP header + first 8 bytes (RFC 792). Sent by ip/ipv4.c when UDP has no socket. */
void icmp_send_unreachable(uint32_t dst_ip, const uint8_t *orig_ip_pkt, uint32_t orig_len);

/* UDP single-slot receive capture, driven by DHCP/DNS: arm on a local port,
 * then poll (bounded) for the reply. */
void udp_arm(uint16_t port);
int  udp_collect(uint8_t *out, uint32_t cap, uint32_t *from_ip);

/* ---- driver interface (virtio_net.c) ------------------------------------ */
bool virtio_net_init(uint8_t mac_out[ETH_ALEN]);   /* probe + bring up; fills MAC */
int  virtio_net_tx(const void *frame, uint32_t len);   /* send one Ethernet frame */
void virtio_net_poll(void);                        /* drain the RX ring -> net_rx() */

#endif /* __NET_H__ */
