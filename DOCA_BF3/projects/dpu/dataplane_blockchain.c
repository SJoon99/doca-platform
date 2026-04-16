/*
 * DPU-native Chunker/Sealer + Metadata Router (single-file skeleton) + Minimal Logging
 *
 * What this implements:
 *  - Stage-0: HW RSS fan-out for TCP/IPv4 (DOCA Flow pipe). (MARK per-flow is TODO; we fallback to SW hash.)
 *  - Stage-1: Per-flow TCP in-order reassembly into fixed 64KB chunks. Each chunk gets metadata + CRC32c "seal".
 *  - Stage-2: Metadata-driven router queues (high/normal/low). TODO hooks for Host DMA / Hairpin / GPU.
 *
 * Notes
 *  - This is a refactor toward a DPU-centric design. It processes *all* TCP/IPv4 uniformly.
 *  - Per-flow MARK learning and DMA/GPUNetIO calls are left as TODOs (ready places provided).
 *  - For brevity, error handling is pragmatic; extend for production.
 *
 * Minimal logging added:
 *  - On IPv4 fragment reassembly success: one INFO line.
 *  - On chunk seal: one INFO line with metadata + one INFO line with truncated hex dump of payload.
 */

 #include <string.h>
 #include <unistd.h>
 #include <netinet/in.h>
 #include <stdlib.h>
 #include <stdio.h>
 #include <stdbool.h>
 #include <inttypes.h>
 #include <sys/socket.h>
 #include <arpa/inet.h>
 
 #include <rte_ethdev.h>
 #include <rte_mbuf.h>
 #include <rte_ether.h>
 #include <rte_ip.h>
 #include <rte_tcp.h>
 #include <rte_ip_frag.h>
 #include <rte_gro.h>
 
 #include <doca_log.h>
 #include <doca_flow.h>
 #include <doca_bitfield.h>
 
 #include "flow_common.h"  /* Assumed available from DOCA samples (init_doca_flow, set_flow_pipe_cfg, etc.) */
 
 extern volatile bool force_quit;
 
 DOCA_LOG_REGISTER(FLOW_APP_CHUNKER);
 
 /* ===================== Tunables & Limits ===================== */
 
 #define PACKET_BURST                   128
 
 /* Reassembly resources (optimized down) */
 #define FRAG_BUCKETS                   64
 #define FRAG_MAX_ENTRIES               256
 #define FRAG_MAX_PER_BUCKET            4
 #define FRAG_TIMEOUT_SEC               30
 
 #define GRO_MAX_FLOW                   128
 #define GRO_MAX_ITEMS_PER_FLOW         16
 
 /* Flow table & timeouts */
 #define MAX_TCP_FLOWS                  512
 #define TCP_FLOW_TIMEOUT_SEC           30
 
 /* Chunking */
 #define CHUNK_SIZE                     (16 * 1024) /* 16KB */
 #define MAX_OOO_SEGMENTS               64          /* per-flow out-of-order queue cap */
 
 /* Misc */
 #define GRO_FLUSH_INTERVAL_CYCLES      (rte_get_tsc_hz() / 100) /* 10ms */
 #define CLEANUP_INTERVAL_CYCLES        (rte_get_tsc_hz() * 10)  /* 10s */
 #define HEX_DUMP_BYTES                 64

 static int g_tcp_fd = -1; /* for future MARK socket option if needed */
 
 /* ===================== CRC32C (Castagnoli) Software ===================== */
 /* Bitwise, reflected, polynomial 0x1EDC6F41 (reflected 0x82F63B78) — compact & portable. */
 
 static inline uint32_t crc32c_sw(const uint8_t *buf, size_t len)
 {
     uint32_t crc = ~0u;
     for (size_t i = 0; i < len; i++) {
         crc ^= buf[i];
         for (int b = 0; b < 8; b++) {
             uint32_t mask = -(crc & 1u);
             crc = (crc >> 1) ^ (0x82F63B78u & mask);
         }
     }
     return ~crc;
 }

 static int tcp_connect_once(const char *dst_ip, uint16_t dst_port)
 {
    g_tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_tcp_fd < 0) {
        DOCA_LOG_ERR("socket() failed");
        return -1;
    }

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(dst_port);
    if (inet_pton(AF_INET, dst_ip, &sa.sin_addr) != 1) return -1;

    if (connect(g_tcp_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) return -1;

    return 0;
 }
 
 /* ===================== Metadata Router (Stage-2 Skeleton) ===================== */
 
 enum route_class {
     ROUTE_LOW = 0,
     ROUTE_NORMAL = 1,
     ROUTE_HIGH = 2,
 };
 
 struct chunk_metadata {
     /* 5-tuple (directional) */
     uint32_t src_ip;
     uint32_t dst_ip;
     uint16_t src_port;
     uint16_t dst_port;
 
     /* sequencing / indexing */
     uint32_t seq_start;    /* inclusive */
     uint32_t seq_end;      /* exclusive */
     uint32_t chunk_index;
 
     /* sizes & time */
     uint32_t payload_len;  /* actual valid bytes in chunk (<= CHUNK_SIZE) */
     uint64_t ts_us;        /* host TSC converted to microseconds */
 
     /* seal */
     uint32_t crc32c;
 };
 
 /* Placeholder classifier: TODO replace with real policy */
 static inline enum route_class classify_chunk(const struct chunk_metadata *m)
 {
     (void)m;
     /* Example: elevate HTTP-like ports — for now always NORMAL. */
     return ROUTE_NORMAL;
 }
 
 /* Placeholder dispatcher: TODO connect to Host DMA / Hairpin / GPU queues */
 static inline void dispatch_chunk(enum route_class cls,
                                   const struct chunk_metadata *m,
                                   const uint8_t *payload)
 {
    (void)cls; (void)m; (void)payload;

    if (m->chunk_index != 0)
            return;
    if (g_tcp_fd < 0){
        DOCA_LOG_ERR("tcp socket not connected");
        return;
    }

    size_t to_send = m->payload_len;
    const uint8_t *p = payload;
    while (to_send > 0) {
        ssize_t sent = send(g_tcp_fd, p, to_send, 0);
        if (sent <= 0) {
            DOCA_LOG_ERR("send() failed or connection closed");
            close(g_tcp_fd);
            g_tcp_fd = -1;
            return;
        }
        p += sent;
        to_send -= (size_t)sent;
    }
    DOCA_LOG_INFO("Dispatched chunk index=%u, bytes=%u to TCP socket", m->chunk_index, m->payload_len);
 }
 
 /* ===================== Per-flow TCP Chunking/Reassembly ===================== */
 
 struct ooo_seg {
     uint32_t seq;
     uint32_t len;
     uint8_t *data;
     struct ooo_seg *next;
 };
 
 struct ooo_queue {
     struct ooo_seg *head;
     int count;
 };
 
 struct tcp_chunk_flow {
     /* directional 5-tuple key */
     uint32_t src_ip, dst_ip;
     uint16_t src_port, dst_port;
 
     /* expected sequencing */
     uint32_t expected_seq;
     bool initialized;
 
     /* current chunk buffer */
     uint8_t  chunk[CHUNK_SIZE];
     uint32_t chunk_len;
     uint32_t chunk_index;
 
     /* recent activity */
     uint64_t last_activity_tsc;
 
     /* out-of-order queue */
     struct ooo_queue ooo;
 };
 
 static struct tcp_chunk_flow g_flows[MAX_TCP_FLOWS];
 static int g_active_flows = 0;
 
 /* ===================== IP Reassembly / GRO ===================== */
 
 static struct rte_ip_frag_tbl *frag_tbl = NULL;
 static struct rte_ip_frag_death_row death_row;
 static void *gro_ctx = NULL;
 static struct rte_mempool *reassembly_pool = NULL;
 
 /* ===================== Helpers ===================== */
 
 static inline uint64_t tsc_to_us(uint64_t tsc)
 {
     return (tsc * 1000000ull) / rte_get_tsc_hz();
 }
 
 static inline void hex_dump_trunc(const uint8_t *data, uint32_t len, char *out, size_t out_sz)
 {
     /* produce hex up to HEX_DUMP_BYTES, no spaces, ensure null-terminated */
     uint32_t n = len < HEX_DUMP_BYTES ? len : HEX_DUMP_BYTES;
     size_t need = (size_t)n * 2 + 1;
     if (out_sz < need) n = (out_sz - 1) / 2;
     for (uint32_t i = 0; i < n; i++)
         sprintf(out + (i * 2), "%02x", data[i]);
     out[n * 2] = '\0';
 }
 
 /* ===================== Flow Table ===================== */
 
 static struct tcp_chunk_flow* find_flow(uint32_t s_ip, uint32_t d_ip, uint16_t s_port, uint16_t d_port)
 {
     for (int i = 0; i < g_active_flows; i++) {
         struct tcp_chunk_flow *f = &g_flows[i];
         if (f->src_ip == s_ip && f->dst_ip == d_ip &&
             f->src_port == s_port && f->dst_port == d_port)
             return f;
     }
     return NULL;
 }
 
 static void free_ooo_queue(struct ooo_queue *q)
 {
     struct ooo_seg *cur = q->head;
     while (cur) {
         struct ooo_seg *n = cur->next;
         free(cur->data);
         free(cur);
         cur = n;
     }
     q->head = NULL;
     q->count = 0;
 }
 
 static void drop_flow_at_index(int idx)
 {
     if (idx < 0 || idx >= g_active_flows) return;
     free_ooo_queue(&g_flows[idx].ooo);
     if (idx != g_active_flows - 1)
         g_flows[idx] = g_flows[g_active_flows - 1];
     g_active_flows--;
 }
 
 static void seal_and_dispatch_chunk(struct tcp_chunk_flow *f)
 {
     if (f->chunk_len == 0) return;
 
     struct chunk_metadata meta = {0};
     meta.src_ip = f->src_ip;
     meta.dst_ip = f->dst_ip;
     meta.src_port = f->src_port;
     meta.dst_port = f->dst_port;
     meta.seq_end = f->expected_seq;
     meta.seq_start = meta.seq_end - f->chunk_len;
     meta.chunk_index = f->chunk_index;
     meta.payload_len = f->chunk_len;
     meta.ts_us = tsc_to_us(rte_get_tsc_cycles());
     meta.crc32c = crc32c_sw(f->chunk, f->chunk_len);
 
     /* Minimal logging: two lines */
     DOCA_LOG_INFO("SEALED chunk | flow: %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u id=%u bytes=%u seq=[%u..%u) crc32c=0x%08x timestamp=%" PRIu64,
                   (meta.src_ip >> 24) & 0xFF, (meta.src_ip >> 16) & 0xFF, (meta.src_ip >> 8) & 0xFF, meta.src_ip & 0xFF, meta.src_port,
                   (meta.dst_ip >> 24) & 0xFF, (meta.dst_ip >> 16) & 0xFF, (meta.dst_ip >> 8) & 0xFF, meta.dst_ip & 0xFF, meta.dst_port,
                   meta.chunk_index, meta.payload_len, meta.seq_start, meta.seq_end, meta.crc32c, meta.ts_us);
 
     char hexbuf[HEX_DUMP_BYTES * 2 + 1];
     hex_dump_trunc(f->chunk, f->chunk_len, hexbuf, sizeof(hexbuf));
     DOCA_LOG_INFO("SEALED payload hex preview(truncated %dB): %s", HEX_DUMP_BYTES, hexbuf);
 
     /* Stage-2 skeleton: classify + dispatch (TODO real queues) */
     enum route_class cls = classify_chunk(&meta);
     dispatch_chunk(cls, &meta, f->chunk);
 
     /* advance to next chunk */
     f->chunk_len = 0;
     f->chunk_index++;
 }
 
 static inline void append_bytes_to_chunk(struct tcp_chunk_flow *f, const uint8_t *data, uint32_t len)
 {
     uint32_t off = 0;
     while (off < len) {
         uint32_t space = CHUNK_SIZE - f->chunk_len;
         uint32_t take = (len - off < space) ? (len - off) : space;
         memcpy(f->chunk + f->chunk_len, data + off, take);
         f->chunk_len += take;
         off += take;
         if (f->chunk_len == CHUNK_SIZE)
             seal_and_dispatch_chunk(f);
     }
 }
 
 /* Insert OOO seg sorted by seq; cap length by available memory. */
 static void queue_ooo_segment(struct tcp_chunk_flow *f, uint32_t seq, const uint8_t *data, uint32_t len)
 {
     if (len == 0) return;
     if (f->ooo.count >= MAX_OOO_SEGMENTS) {
         /* Drop the tail-most (largest seq) by simple policy: insert only if "smallest" seq. */
         /* Simple behavior: drop incoming if full. */
         return;
     }
     struct ooo_seg *seg = (struct ooo_seg *)malloc(sizeof(*seg));
     if (!seg) return;
     seg->data = (uint8_t *)malloc(len);
     if (!seg->data) { free(seg); return; }
     memcpy(seg->data, data, len);
     seg->seq = seq;
     seg->len = len;
     seg->next = NULL;
 
     struct ooo_seg **cur = &f->ooo.head;
     while (*cur && (*cur)->seq < seq)
         cur = &(*cur)->next;
     seg->next = *cur;
     *cur = seg;
     f->ooo.count++;
 }
 
 /* Drain OOO queue if next contiguous segments are present (handles full/partial overlap). */
 static void drain_ooo_segments(struct tcp_chunk_flow *f)
 {
     bool progressed = true;
     while (progressed && f->ooo.head) {
         progressed = false;
         struct ooo_seg **cur = &f->ooo.head;
         while (*cur) {
             struct ooo_seg *s = *cur;
             uint32_t exp = f->expected_seq;
 
             if (s->seq <= exp && s->seq + s->len > exp) {
                 /* partial overlap forward */
                 uint32_t off = exp - s->seq;
                 uint32_t take = s->len - off;
                 append_bytes_to_chunk(f, s->data + off, take);
                 f->expected_seq += take;
 
                 *cur = s->next;
                 free(s->data); free(s);
                 f->ooo.count--;
                 progressed = true;
                 break;
             } else if (s->seq == exp) {
                 /* exact next */
                 append_bytes_to_chunk(f, s->data, s->len);
                 f->expected_seq += s->len;
 
                 *cur = s->next;
                 free(s->data); free(s);
                 f->ooo.count--;
                 progressed = true;
                 break;
             } else {
                 cur = &(*cur)->next;
             }
         }
     }
 }
 
 static struct tcp_chunk_flow* get_or_create_flow(uint32_t s_ip, uint32_t d_ip, uint16_t s_port, uint16_t d_port)
 {
     struct tcp_chunk_flow *f = find_flow(s_ip, d_ip, s_port, d_port);
     if (f) return f;
 
     /* Cleanup if full, then retry */
     if (g_active_flows >= MAX_TCP_FLOWS) {
         /* Lazy cleanup: drop oldest inactive flow (linear scan) */
         uint64_t now = rte_get_tsc_cycles();
         int oldest = -1;
         uint64_t oldest_age = 0;
         for (int i = 0; i < g_active_flows; i++) {
             uint64_t age = now - g_flows[i].last_activity_tsc;
             if (oldest == -1 || age > oldest_age) { oldest = i; oldest_age = age; }
         }
         if (oldest != -1) drop_flow_at_index(oldest);
     }
     if (g_active_flows >= MAX_TCP_FLOWS)
         return NULL;
 
     f = &g_flows[g_active_flows++];
     memset(f, 0, sizeof(*f));
     f->src_ip = s_ip; f->dst_ip = d_ip; f->src_port = s_port; f->dst_port = d_port;
     f->expected_seq = 0;
     f->initialized = false;
     f->chunk_len = 0;
     f->chunk_index = 0;
     f->last_activity_tsc = rte_get_tsc_cycles();
     f->ooo.head = NULL; f->ooo.count = 0;
     return f;
 }
 
 static void cleanup_idle_flows(void)
 {
     uint64_t now = rte_get_tsc_cycles();
     uint64_t timeout_cycles = (uint64_t)TCP_FLOW_TIMEOUT_SEC * rte_get_tsc_hz();
     int i = 0;
     while (i < g_active_flows) {
         struct tcp_chunk_flow *f = &g_flows[i];
         if (now - f->last_activity_tsc > timeout_cycles) {
             /* Flush partial chunk (if any) before drop */
             if (f->chunk_len > 0)
                 seal_and_dispatch_chunk(f);
             drop_flow_at_index(i);
         } else {
             i++;
         }
     }
 }
 
 /* ===================== IP/GRO Handling ===================== */
 
 static bool is_ipv4_fragmented(struct rte_mbuf *m)
 {
     struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
     if (rte_be_to_cpu_16(eth->ether_type) != RTE_ETHER_TYPE_IPV4)
         return false;
     struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
     uint16_t frag_off = rte_be_to_cpu_16(ip->fragment_offset);
     return (frag_off & RTE_IPV4_HDR_MF_FLAG) || (frag_off & RTE_IPV4_HDR_OFFSET_MASK);
 }
 
 static struct rte_mbuf* ip_reassemble(struct rte_mbuf *m)
 {
     uint64_t ts = rte_rdtsc();
     struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
     struct rte_ipv4_hdr *ip4 = (struct rte_ipv4_hdr *)(eth + 1);
 
     struct rte_mbuf *out = rte_ipv4_frag_reassemble_packet(
         frag_tbl, &death_row, m, ts, ip4);
 
     if (out) {
         DOCA_LOG_INFO("IPv4 fragments reassembled: %u bytes", rte_pktmbuf_pkt_len(out));
     }
     return out;
 }
 
 static void gro_timeout_flush_and_process(void);
 
 /* ===================== TCP Segment → Flow Feeding ===================== */
 
 static int extract_tcp_segment(struct rte_mbuf *m,
                                uint32_t *s_ip, uint32_t *d_ip,
                                uint16_t *s_port, uint16_t *d_port,
                                uint32_t *seq,
                                const uint8_t **payload, uint32_t *plen,
                                uint8_t *flags)
 {
     struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
     if (rte_be_to_cpu_16(eth->ether_type) != RTE_ETHER_TYPE_IPV4)
         return -1;
 
     struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
     if (ip->next_proto_id != IPPROTO_TCP)
         return -1;
 
     uint16_t ip_hdr_len = (ip->version_ihl & 0x0F) * 4;
     struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)((uint8_t *)ip + ip_hdr_len);
     uint16_t tcp_hdr_len = ((tcp->data_off >> 4) & 0xF) * 4;
 
     *s_ip = rte_be_to_cpu_32(ip->src_addr);
     *d_ip = rte_be_to_cpu_32(ip->dst_addr);
     *s_port = rte_be_to_cpu_16(tcp->src_port);
     *d_port = rte_be_to_cpu_16(tcp->dst_port);
     *seq = rte_be_to_cpu_32(tcp->sent_seq);
     *flags = tcp->tcp_flags;
 
     uint16_t ip_len = rte_be_to_cpu_16(ip->total_length);
     int hdr_total = ip_hdr_len + tcp_hdr_len;
     *plen = (hdr_total <= ip_len) ? (ip_len - hdr_total) : 0;
     *payload = (const uint8_t *)tcp + tcp_hdr_len;
     return 0;
 }
 
 static void process_segment_mbuf(struct rte_mbuf *m)
 {
     uint32_t s_ip, d_ip, seq;
     uint16_t s_port, d_port;
     const uint8_t *payload; uint32_t plen;
     uint8_t flags;
 
     if (extract_tcp_segment(m, &s_ip, &d_ip, &s_port, &d_port,
                             &seq, &payload, &plen, &flags) < 0) {
         rte_pktmbuf_free(m);
         return;
     }
 
     /* Ignore pure ACKs (no payload), but FIN/RST should still flush. */
     if (plen == 0 && !(flags & (RTE_TCP_FIN_FLAG | RTE_TCP_RST_FLAG))) {
         rte_pktmbuf_free(m);
         return;
     }
 
     struct tcp_chunk_flow *f = get_or_create_flow(s_ip, d_ip, s_port, d_port);
     if (!f) {
         rte_pktmbuf_free(m);
         return;
     }
     f->last_activity_tsc = rte_get_tsc_cycles();
 
     /* Initialize expected_seq on first payload */
     if (!f->initialized && plen > 0) {
         f->expected_seq = seq;
         f->initialized = true;
     }
 
     /* In-order / overlap handling */
     if (plen > 0) {
         if (seq == f->expected_seq) {
             append_bytes_to_chunk(f, payload, plen);
             f->expected_seq += plen;
             drain_ooo_segments(f);
         } else if (seq > f->expected_seq) {
             /* Out-of-order */
             queue_ooo_segment(f, seq, payload, plen);
         } else {
             /* seq < expected: overlap or duplicate */
             uint32_t exp = f->expected_seq;
             if (seq + plen <= exp) {
                 /* fully old, drop */
             } else {
                 /* partial overlap forward */
                 uint32_t off = exp - seq;
                 append_bytes_to_chunk(f, payload + off, plen - off);
                 f->expected_seq += (plen - off);
                 drain_ooo_segments(f);
             }
         }
     }
 
     /* FIN/RST: flush partial chunk */
     if (flags & (RTE_TCP_FIN_FLAG | RTE_TCP_RST_FLAG)) {
         if (f->chunk_len > 0)
             seal_and_dispatch_chunk(f);
     }
 
     rte_pktmbuf_free(m);
 }
 
 /* ===================== GRO Frontend ===================== */
 
 static void gro_timeout_flush_and_process(void)
 {
     struct rte_mbuf *out[32];
     uint16_t nb = rte_gro_timeout_flush(gro_ctx,
                                         rte_get_tsc_hz() / 100, /* 10ms */
                                         RTE_GRO_TCP_IPV4,
                                         out, 32);
     for (uint16_t i = 0; i < nb; i++)
         process_segment_mbuf(out[i]);
 }
 
 static void handle_one_packet(struct rte_mbuf *m)
 {
     struct rte_mbuf *pm = m;
 
     if (is_ipv4_fragmented(m)) {
         struct rte_mbuf *reassembled = ip_reassemble(m);
         if (reassembled == NULL) {
             /* Reassembly in progress; mbuf is queued internally. */
             return;
         }
         pm = reassembled;
     }
 
     struct rte_mbuf *in[1] = { pm };
     uint16_t outn = rte_gro_reassemble(in, 1, gro_ctx);
     for (uint16_t i = 0; i < outn; i++)
         process_segment_mbuf(in[i]);
 
     /* Periodic GRO flush */
     static uint64_t last_flush = 0;
     uint64_t now = rte_get_tsc_cycles();
     if (now - last_flush > GRO_FLUSH_INTERVAL_CYCLES) {
         gro_timeout_flush_and_process();
         last_flush = now;
     }
 }
 
 /* ===================== Init / Cleanup ===================== */
 
 static int init_reassembly_subsystems(void)
 {
     uint32_t socket_id = rte_socket_id();
 
     /* mbuf pool (for fragment reassembly paths if needed by library) */
     reassembly_pool = rte_pktmbuf_pool_create("reassembly_pool",
                                               1024, 128, 0,
                                               RTE_MBUF_DEFAULT_BUF_SIZE,
                                               socket_id);
     if (!reassembly_pool) {
         DOCA_LOG_ERR("Failed to create reassembly_pool");
         return -1;
     }
 
     /* IP fragment table */
     uint64_t timeout_cycles = (uint64_t)FRAG_TIMEOUT_SEC * rte_get_tsc_hz();
     frag_tbl = rte_ip_frag_table_create(FRAG_BUCKETS, FRAG_MAX_ENTRIES,
                                         FRAG_MAX_PER_BUCKET, timeout_cycles,
                                         (int)socket_id);
     if (!frag_tbl) {
         DOCA_LOG_ERR("Failed to create frag table");
         rte_mempool_free(reassembly_pool);
         reassembly_pool = NULL;
         return -1;
     }
     memset(&death_row, 0, sizeof(death_row));
 
     /* GRO context */
     struct rte_gro_param gp = {
         .gro_types = RTE_GRO_TCP_IPV4,
         .max_flow_num = GRO_MAX_FLOW,
         .max_item_per_flow = GRO_MAX_ITEMS_PER_FLOW,
         .socket_id = socket_id
     };
     gro_ctx = rte_gro_ctx_create(&gp);
     if (!gro_ctx) {
         DOCA_LOG_ERR("Failed to create GRO ctx");
         rte_ip_frag_table_destroy(frag_tbl);
         frag_tbl = NULL;
         rte_mempool_free(reassembly_pool);
         reassembly_pool = NULL;
         return -1;
     }
 
     /* flows */
     memset(g_flows, 0, sizeof(g_flows));
     g_active_flows = 0;
 
     return 0;
 }
 
 static void cleanup_reassembly_subsystems(void)
 {
     /* flush partial chunks before teardown */
     for (int i = 0; i < g_active_flows; i++) {
         if (g_flows[i].chunk_len > 0)
             seal_and_dispatch_chunk(&g_flows[i]);
         free_ooo_queue(&g_flows[i].ooo);
     }
     g_active_flows = 0;
 
     if (gro_ctx) {
         rte_gro_ctx_destroy(gro_ctx);
         gro_ctx = NULL;
     }
     if (frag_tbl) {
         rte_ip_frag_table_destroy(frag_tbl);
         frag_tbl = NULL;
     }
     if (reassembly_pool) {
         rte_mempool_free(reassembly_pool);
         reassembly_pool = NULL;
     }
 }
 
 /* ===================== DOCA Flow RSS Pipe (TCP/IPv4) ===================== */
 
 static doca_error_t create_rss_tcp_ipv4_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
    struct doca_flow_match match;
    struct doca_flow_actions actions, *actions_arr[NB_ACTIONS_ARR];
    struct doca_flow_fwd fwd, fwd_miss;
    struct doca_flow_pipe_cfg *cfg;
    uint16_t rss_queues[1];
    doca_error_t r;

    memset(&match, 0, sizeof(match));
    memset(&actions, 0, sizeof(actions));
    memset(&fwd, 0, sizeof(fwd));
    memset(&fwd_miss, 0, sizeof(fwd_miss));

    /* ✅ L3만 사용: IPv4 + src_ip만 매치 */
    match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
    match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;

    /* ❌ 아래 두 줄을 반드시 제거하세요 (protocol-only 유발) */
    /* match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP; */
    /* match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;     */

    /* ✅ src_ip만 마스크 켬(0xFFFFFFFF). 나머지는 0(와일드카드) */
    match.outer.ip4.src_ip = 0xffffffff;

    actions.meta.pkt_meta = UINT32_MAX;
    actions_arr[0] = &actions;

    r = doca_flow_pipe_cfg_create(&cfg, port);
    if (r != DOCA_SUCCESS) {
        DOCA_LOG_ERR("pipe_cfg_create: %s", doca_error_get_descr(r));
        return r;
    }
    r = set_flow_pipe_cfg(cfg, "RSS_IPv4_SRC_ONLY", DOCA_FLOW_PIPE_BASIC, true);
    if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("set_flow_pipe_cfg: %s", doca_error_get_descr(r)); goto out_cfg; }

    r = doca_flow_pipe_cfg_set_match(cfg, &match, NULL);
    if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("pipe_cfg_set_match: %s", doca_error_get_descr(r)); goto out_cfg; }

    r = doca_flow_pipe_cfg_set_actions(cfg, actions_arr, NULL, NULL, NB_ACTIONS_ARR);
    if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("pipe_cfg_set_actions: %s", doca_error_get_descr(r)); goto out_cfg; }

    rss_queues[0] = 0;

    /* hit → RSS */
    fwd.type = DOCA_FLOW_FWD_RSS;
    fwd.rss_queues = rss_queues;
    fwd.rss_inner_flags = DOCA_FLOW_RSS_IPV4;  
    fwd.num_of_queues = 1;

    /* miss → DROP */
    fwd_miss.type = DOCA_FLOW_FWD_DROP;

    r = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, pipe);

out_cfg:
    doca_flow_pipe_cfg_destroy(cfg);
    return r;
}
 
 static doca_error_t add_src_ip_entry(struct doca_flow_pipe *pipe, struct entries_status *st)
 {
     struct doca_flow_match mval;
     struct doca_flow_actions act;
     struct doca_flow_pipe_entry *entry;
     doca_error_t r;
 
     memset(&mval, 0, sizeof(mval));
     memset(&act, 0, sizeof(act));
 
     /* ★ 엔트리 '값'을 세팅: src_ip = 10.197.0.9  (big-endian 주의) */
     mval.outer.ip4.src_ip = BE_IPV4_ADDR(10, 197, 0, 9);
 
     act.action_idx = 0;
 
     r = doca_flow_pipe_add_entry(0, pipe, &mval, &act, NULL, NULL, 0, st, &entry);
     if (r != DOCA_SUCCESS) {
         DOCA_LOG_ERR("pipe_add_entry(src_ip=10.197.0.9) failed: %s", doca_error_get_descr(r));
         return r;
     }
     return DOCA_SUCCESS;
 }


 
 /* ===================== Main RX Loop ===================== */
 
 static void rx_loop_once(int ingress_port)
 {
     struct rte_mbuf *pkts[PACKET_BURST];
     uint16_t nb = rte_eth_rx_burst(ingress_port, 0, pkts, PACKET_BURST);
     for (uint16_t i = 0; i < nb; i++)
         handle_one_packet(pkts[i]);
 
     static uint64_t last_cleanup = 0;
     uint64_t now = rte_get_tsc_cycles();
     if (now - last_cleanup > CLEANUP_INTERVAL_CYCLES) {
         cleanup_idle_flows();
         last_cleanup = now;
     }
 }
 
 /* ===================== Entry ===================== */
 
 doca_error_t flow_rss_meta_with_app_buffering(int nb_queues)
 {
     const int nb_ports = 1;
     struct flow_resources res = {0};
     uint32_t nr_shared[SHARED_RESOURCE_NUM_VALUES] = {0};
     struct doca_flow_port *ports[nb_ports];
     struct doca_dev *devs[nb_ports];
     struct doca_flow_pipe *pipe;
     struct entries_status st;
     doca_error_t r;
     int port_id;
 
     /* Init chunker subsystems (IP reassembly + GRO + flow table) */
     if (init_reassembly_subsystems() != 0) {
         DOCA_LOG_ERR("Reassembly subsystems init failed");
         return DOCA_ERROR_INITIALIZATION;
     }

     if (tcp_connect_once("10.38.36.32", 8000) != 0){
        DOCA_LOG_ERR("tcp_connect_once failed");
        return DOCA_ERROR_INITIALIZATION;
     }
 
     /* DOCA Flow framework init */
     r = init_doca_flow(nb_queues, "vnf,hws", &res, nr_shared);
     if (r != DOCA_SUCCESS) {
         DOCA_LOG_ERR("init_doca_flow: %s", doca_error_get_descr(r));
         cleanup_reassembly_subsystems();
         return r;
     }
 
     r = init_doca_flow_ports(nb_ports, ports, true, devs);
     if (r != DOCA_SUCCESS) {
         DOCA_LOG_ERR("init_doca_flow_ports: %s", doca_error_get_descr(r));
         doca_flow_destroy();
         cleanup_reassembly_subsystems();
         return r;
     }
 
     for (port_id = 0; port_id < nb_ports; port_id++) {
         memset(&st, 0, sizeof(st));
 
         r = create_rss_tcp_ipv4_pipe(ports[port_id], &pipe);
         if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("create_rss_tcp_ipv4_pipe failed"); goto stop_ports; }
 
         r = add_src_ip_entry(pipe, &st);
         if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("add_src_ip_entry failed"); goto stop_ports; }
 
         r = doca_flow_entries_process(ports[port_id], 0, DEFAULT_TIMEOUT_US, 1);
         if (r != DOCA_SUCCESS || st.nb_processed != 1 || st.failure) {
             DOCA_LOG_ERR("entries_process failed: processed=%d, failure=%s",
                          st.nb_processed, st.failure ? "true" : "false");
             goto stop_ports;
         }
     }
 
     DOCA_LOG_INFO("=== DPU-native Chunker/Sealer started (TCP/IPv4) ===");
     DOCA_LOG_INFO("Stage-0 RSS fan-out active. Stage-1 chunking (64KB) with CRC32C seal. Stage-2 router: skeleton.");
     DOCA_LOG_INFO("====================================================");
 
     while (!force_quit) {
         rx_loop_once(0);
         usleep(10000);
     }
 
 stop_ports:
     stop_doca_flow_ports(nb_ports, ports);
     doca_flow_destroy();
     cleanup_reassembly_subsystems();
     return r;
 }
 