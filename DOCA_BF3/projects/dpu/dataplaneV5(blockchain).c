/*
 * DPU-native HTTP Header Minimal Extractor + CPU SHA-256 + Merkle Aggregator
 * (TCP/IPv4, GRO/IP-frag) + DOCA Flow RSS (skeleton)
 *
 * 목표(프로토타입):
 *  - TCP 스트림에서 "HTTP 헤더 블록" 단위로만 인오더 재조립
 *  - 각 헤더 블록(EoH: \r\n\r\n 또는 \n\n)마다 최소 필드만 추출:
 *      * Request:  method, uri, host, x-amz-date(또는 date), x-amz-request-id(있으면)
 *      * Response: status_code, etag, x-amz-date(또는 date), x-amz-request-id
 *  - canonical string을 만들고 CPU(OpenSSL)로 SHA-256(leaf) 생성
 *  - 전역 Aggregator:
 *      * 1초 또는 1000 leaf마다 Merkle Root 계산
 *  - Dual Path:
 *      * Path A (Audit): leaf + 최소 필드 JSON line -> Log server (TCP)
 *      * Path B (Anchor): merkle_root + count JSON line -> Chain server (TCP)
 *
 * 주의:
 *  - BF3는 DOCA SHA 미지원(가속 엔진 없음) -> CPU SHA 사용이 맞음:contentReference[oaicite:3]{index=3}
 *  - send()는 blocking일 수 있어, 고부하 환경에선 non-blocking + ring/batch 권장(프로토타입에선 단순 구현)
 */

#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include <errno.h>

#include <openssl/evp.h>          /* CPU SHA-256 */

#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_ip_frag.h>
#include <rte_gro.h>

#include <doca_log.h>
#include <doca_flow.h>

#include "flow_common.h"

extern volatile bool force_quit;

DOCA_LOG_REGISTER(FLOW_APP_HDRMIN)

/* ===================== 튜너블 ===================== */

#define PACKET_BURST                   128
#define FRAG_BUCKETS                   64
#define FRAG_MAX_ENTRIES               256
#define FRAG_MAX_PER_BUCKET            4
#define FRAG_TIMEOUT_SEC               30

#define GRO_MAX_FLOW                   128
#define GRO_MAX_ITEMS_PER_FLOW         16

#define MAX_TCP_FLOWS                  512
#define TCP_FLOW_TIMEOUT_SEC           30

#define GRO_FLUSH_INTERVAL_CYCLES      (rte_get_tsc_hz() / 100)   /* 10ms */
#define CLEANUP_INTERVAL_CYCLES        (rte_get_tsc_hz() * 10)    /* 10s */

/* 헤더 누적 상한(한 TCP flow가 body까지 가기 전 헤더가 미친듯이 길어지는 걸 방지) */
#define MAX_HEADER_BYTES               (64 * 1024)
#define INIT_HDR_CAP                   2048

/* Aggregation */
#define AGG_MAX_LEAVES                 1000
#define AGG_INTERVAL_CYCLES            (rte_get_tsc_hz() * 1)      /* 1초 */

/* ===================== Dual-Path 전송 ===================== */

#define LOG_DST_IP      "10.38.36.32"
#define LOG_DST_PORT    8000

#define CHAIN_DST_IP    "10.38.36.32"
#define CHAIN_DST_PORT  8001

static int g_log_fd   = -1;   /* Path A */
static int g_chain_fd = -1;   /* Path B */

static int tcp_connect_once(int *out_fd, const char *dst_ip, uint16_t dst_port, const char *tag)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { DOCA_LOG_ERR("[%s] socket() failed", tag); return -1; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(dst_port);
    if (inet_pton(AF_INET, dst_ip, &sa.sin_addr) != 1) {
        DOCA_LOG_ERR("[%s] inet_pton failed", tag);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        DOCA_LOG_ERR("[%s] connect() failed (%s)", tag, strerror(errno));
        close(fd);
        return -1;
    }

    *out_fd = fd;
    DOCA_LOG_INFO("[%s] connected to %s:%u", tag, dst_ip, (unsigned)dst_port);
    return 0;
}

static void tcp_close_if_open(int *fd)
{
    if (*fd >= 0) { close(*fd); *fd = -1; }
}

static void tcp_send_all_or_close(int *fd, const uint8_t *p, size_t n, const char *tag)
{
    if (*fd < 0 || !p || n == 0) return;

    size_t left = n;
    while (left > 0) {
        ssize_t s = send(*fd, p, left, 0);
        if (s <= 0) {
            DOCA_LOG_ERR("[%s] send() failed -> close (%s)", tag, strerror(errno));
            tcp_close_if_open(fd);
            return;
        }
        p += s;
        left -= (size_t)s;
    }
}

/* ===================== 유틸 ===================== */

static inline uint64_t tsc_to_us(uint64_t tsc)
{
    return (tsc * 1000000ull) / rte_get_tsc_hz();
}

static inline bool is_request_line_prefix(const uint8_t *s, size_t n)
{
    if (n < 4) return false;

    if (n >= 4 && (!memcmp(s,"GET ",4)  || !memcmp(s,"get ",4)))  return true;
    if (n >= 4 && (!memcmp(s,"PUT ",4)  || !memcmp(s,"put ",4)))  return true;
    if (n >= 5 && (!memcmp(s,"POST ",5) || !memcmp(s,"post ",5))) return true;
    if (n >= 5 && (!memcmp(s,"HEAD ",5) || !memcmp(s,"head ",5))) return true;
    if (n >= 7 && (!memcmp(s,"DELETE ",7) || !memcmp(s,"delete ",7))) return true;
    if (n >= 8 && (!memcmp(s,"OPTIONS ",8) || !memcmp(s,"options ",8))) return true;
    if (n >= 6 && (!memcmp(s,"PATCH ",6) || !memcmp(s,"patch ",6))) return true;

    return false;
}

static inline bool is_status_line_prefix(const uint8_t *s, size_t n)
{
    if (n < 8) return false;
    if (!memcmp(s, "HTTP/1.1", 8) || !memcmp(s, "http/1.1", 8)) return true;
    if (!memcmp(s, "HTTP/1.0", 8) || !memcmp(s, "http/1.0", 8)) return true;
    if (!memcmp(s, "HTTP/2.0", 8) || !memcmp(s, "http/2.0", 8)) return true;
    return false;
}

static inline bool is_http_header_start(const uint8_t *s, size_t n)
{
    return is_request_line_prefix(s, n) || is_status_line_prefix(s, n);
}

/* 헤더 종료 구분자 탐색: \r\n\r\n 또는 \n\n */
static size_t find_eoh(const uint8_t *buf, size_t start, size_t len)
{
    size_t i = (start > 3) ? (start - 3) : 0;
    for (; i + 1 < len; i++) {
        if (i + 3 < len && buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2]=='\r' && buf[i+3]=='\n')
            return i + 4;
        if (buf[i]=='\n' && buf[i+1]=='\n')
            return i + 2;
    }
    return SIZE_MAX;
}

/* ===================== CPU SHA-256 ===================== */

/* OpenSSL one-shot SHA-256 */
static bool sha256_cpu(const uint8_t *data, size_t len, uint8_t out[32])
{
    unsigned int outlen = 0;
    int rc = EVP_Digest(data, len, out, &outlen, EVP_sha256(), NULL);
    return (rc == 1 && outlen == 32);
}

static void bin2hex(const uint8_t *in, size_t n, char *out, size_t cap)
{
    static const char *H="0123456789abcdef";
    size_t need = n*2 + 1;
    if (cap < need) { if (cap) out[0]=0; return; }
    for (size_t i=0;i<n;i++){
        out[i*2]   = H[in[i]>>4];
        out[i*2+1] = H[in[i]&0xF];
    }
    out[n*2]=0;
}

/* ===================== Minimal Extraction ===================== */
/*
 * 프로토타입 최소 필드:
 *  - Request: method, uri, host, ts(x-amz-date 우선, 없으면 date), req_id(있으면)
 *  - Response: status_code, etag, ts(x-amz-date 우선, 없으면 date), req_id(있으면)
 */

struct mini_meta {
    bool is_request;

    /* request */
    char method[8];
    char uri[384];
    char host[128];

    /* response */
    int  status_code;
    char etag[96];

    /* common */
    char ts[64];         /* X-Amz-Date 또는 Date */
    char req_id[96];     /* X-Amz-Request-Id */
};

/* case-insensitive char compare */
static inline char tolower_ascii(char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

/* line[0..len) 에서 "Key:" 형태로 시작하는지 검사 (대소문자 무시) */
static bool line_has_key(const uint8_t *line, size_t len, const char *key)
{
    size_t klen = strlen(key);
    if (len < klen + 1) return false;

    for (size_t i=0;i<klen;i++) {
        if (tolower_ascii((char)line[i]) != tolower_ascii(key[i])) return false;
    }

    /* key 다음은 ':' 이어야 함 (공백 허용은 안 하고, 보수적으로 ":" 바로 요구) */
    return (line[klen] == ':');
}

/* "Key: Value" 라인에서 Value 부분을 dest로 복사 (좌우 공백/CR 제거) */
static void copy_header_value(const uint8_t *line, size_t len, size_t keylen, char *dest, size_t cap)
{
    if (cap == 0) return;
    dest[0] = 0;

    size_t i = keylen + 1; /* skip "Key:" */
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;

    /* 끝쪽 CR/LF 제거 */
    size_t j = len;
    while (j > i && (line[j-1] == '\r' || line[j-1] == '\n' || line[j-1] == ' ' || line[j-1] == '\t'))
        j--;

    size_t vlen = (j > i) ? (j - i) : 0;
    if (vlen == 0) return;

    if (vlen >= cap) vlen = cap - 1;
    memcpy(dest, line + i, vlen);
    dest[vlen] = 0;
}

/* 첫 줄 파싱:
 *  - Request: "METHOD SP URI SP HTTP/..."
 *  - Response: "HTTP/... SP CODE ..."
 */
static void parse_first_line(const uint8_t *line, size_t len, struct mini_meta *m)
{
    if (is_status_line_prefix(line, len)) {
        m->is_request = false;
        m->status_code = 0;

        /* "HTTP/1.1 200 OK" -> 첫 공백 이후 숫자 */
        size_t i=0;
        while (i < len && line[i] != ' ') i++;
        while (i < len && line[i] == ' ') i++;
        /* atoi on bytes */
        int code = 0;
        while (i < len && line[i] >= '0' && line[i] <= '9') {
            code = code*10 + (line[i]-'0');
            i++;
        }
        m->status_code = code;
        return;
    }

    /* request line */
    m->is_request = true;

    /* METHOD */
    size_t i=0;
    while (i < len && line[i] != ' ' && i < sizeof(m->method)-1) {
        m->method[i] = (char)line[i];
        i++;
    }
    m->method[i] = 0;

    /* URI */
    while (i < len && line[i] == ' ') i++;
    size_t u=0;
    while (i < len && line[i] != ' ' && u < sizeof(m->uri)-1) {
        m->uri[u++] = (char)line[i++];
    }
    m->uri[u] = 0;
}

/* blk[0..blen) (EOH 포함)에서 최소 필드만 추출 (malloc 없이) */
static bool parse_http_block_minimal(const uint8_t *blk, size_t blen, struct mini_meta *m)
{
    memset(m, 0, sizeof(*m));

    /* 1) 라인 단위로 스캔 */
    size_t pos = 0;
    bool first = true;

    while (pos < blen) {
        /* line start = pos, line end = '\n' or end */
        size_t line_start = pos;
        while (pos < blen && blk[pos] != '\n') pos++;
        size_t line_end = pos;         /* '\n' 제외 */
        if (pos < blen && blk[pos] == '\n') pos++;

        /* 빈 줄(헤더 종료) */
        size_t line_len = (line_end > line_start) ? (line_end - line_start) : 0;

        /* CR 제거를 위해 line_len 조정 */
        while (line_len > 0 && blk[line_start + line_len - 1] == '\r')
            line_len--;

        if (line_len == 0) {
            /* empty line -> header end */
            break;
        }

        const uint8_t *line = &blk[line_start];

        if (first) {
            parse_first_line(line, line_len, m);
            first = false;
            continue;
        }

        /* 2) 관심 헤더만 추출 */
        if (line_has_key(line, line_len, "Host")) {
            copy_header_value(line, line_len, 4, m->host, sizeof(m->host));
        } else if (line_has_key(line, line_len, "ETag")) {
            copy_header_value(line, line_len, 4, m->etag, sizeof(m->etag));
        } else if (line_has_key(line, line_len, "X-Amz-Date")) {
            copy_header_value(line, line_len, 10, m->ts, sizeof(m->ts));
        } else if (line_has_key(line, line_len, "Date")) {
            /* X-Amz-Date가 없을 때만 Date 사용 */
            if (m->ts[0] == 0)
                copy_header_value(line, line_len, 4, m->ts, sizeof(m->ts));
        } else if (line_has_key(line, line_len, "X-Amz-Request-Id")) {
            copy_header_value(line, line_len, 16, m->req_id, sizeof(m->req_id));
        }
    }

    /* 첫 라인이 없으면 실패 */
    if (first) return false;

    /* 최소한의 식별자가 너무 없으면 false로 처리할 수도 있지만, 프로토타입에선 그대로 */
    return true;
}

/* canonical string: 필드 순서 고정(중요) */
static size_t build_canonical_minimal(const struct mini_meta *m, uint8_t *out, size_t cap)
{
    const char *ts   = m->ts[0] ? m->ts : "-";
    const char *rid  = m->req_id[0] ? m->req_id : "-";

    if (m->is_request) {
        const char *host = m->host[0] ? m->host : "-";
        const char *meth = m->method[0] ? m->method : "-";
        const char *uri  = m->uri[0] ? m->uri : "-";
        return (size_t)snprintf((char*)out, cap,
            "t=req|m=%s|u=%s|h=%s|ts=%s|rid=%s",
            meth, uri, host, ts, rid);
    } else {
        const char *etag = m->etag[0] ? m->etag : "-";
        return (size_t)snprintf((char*)out, cap,
            "t=resp|c=%d|etag=%s|ts=%s|rid=%s",
            m->status_code, etag, ts, rid);
    }
}

static bool make_leaf_hash_minimal(const struct mini_meta *m, uint8_t leaf[32])
{
    uint8_t canon[1024];
    size_t n = build_canonical_minimal(m, canon, sizeof(canon));
    if (n == 0 || n >= sizeof(canon)) return false;
    return sha256_cpu(canon, n, leaf);
}

/* ===================== Merkle Aggregator ===================== */

static uint8_t g_leaves[AGG_MAX_LEAVES][32];
static int g_leaf_cnt = 0;
static uint64_t g_last_flush_tsc = 0;

static bool sha256_2x32(const uint8_t a[32], const uint8_t b[32], uint8_t out[32])
{
    uint8_t tmp[64];
    memcpy(tmp, a, 32);
    memcpy(tmp + 32, b, 32);
    return sha256_cpu(tmp, 64, out);
}

static bool merkle_root(uint8_t (*leaves)[32], int cnt, uint8_t root[32])
{
    if (cnt <= 0) return false;

    /* 작업 버퍼(최대 1000) */
    uint8_t level[AGG_MAX_LEAVES][32];
    int n = cnt;
    for (int i=0;i<n;i++) memcpy(level[i], leaves[i], 32);

    while (n > 1) {
        int outn = 0;
        for (int i=0;i<n;i+=2) {
            const uint8_t *L = level[i];
            const uint8_t *R = (i+1<n) ? level[i+1] : level[i]; /* odd -> duplicate */
            if (!sha256_2x32(L, R, level[outn])) return false;
            outn++;
        }
        n = outn;
    }

    memcpy(root, level[0], 32);
    return true;
}

/* Path B: root 전송 (signature는 추후 확장) */
static void agg_flush_if_needed(bool force)
{
    uint64_t now = rte_get_tsc_cycles();

    if (!force) {
        if (g_leaf_cnt == 0) return;
        if (g_leaf_cnt < AGG_MAX_LEAVES && (now - g_last_flush_tsc) < AGG_INTERVAL_CYCLES)
            return;
    }

    uint8_t root[32];
    if (!merkle_root(g_leaves, g_leaf_cnt, root)) {
        DOCA_LOG_ERR("Merkle root failed (cnt=%d)", g_leaf_cnt);
        g_leaf_cnt = 0;
        g_last_flush_tsc = now;
        return;
    }

    char root_hex[65];
    bin2hex(root, 32, root_hex, sizeof(root_hex));

    /* timestamp는 wall-clock 대신 tsc_us로(프로토타입) */
    char msg[256];
    int ml = snprintf(msg, sizeof(msg),
                      "{\"root\":\"%s\",\"count\":%d,\"tsc_us\":%" PRIu64 "}\n",
                      root_hex, g_leaf_cnt, tsc_to_us(now));
    if (ml > 0) {
        tcp_send_all_or_close(&g_chain_fd, (const uint8_t*)msg, (size_t)ml, "CHAIN");
    }

    g_leaf_cnt = 0;
    g_last_flush_tsc = now;
}

static void agg_add_leaf(const uint8_t leaf[32])
{
    if (g_leaf_cnt < AGG_MAX_LEAVES) {
        memcpy(g_leaves[g_leaf_cnt++], leaf, 32);
    }
    agg_flush_if_needed(false);
}

/* Path A: leaf + 최소 필드 JSON 전송 */
static void send_audit_minimal(const struct mini_meta *m, const uint8_t leaf[32])
{
    if (g_log_fd < 0) return;

    char leaf_hex[65];
    bin2hex(leaf, 32, leaf_hex, sizeof(leaf_hex));

    char j[1024];
    if (m->is_request) {
        int jl = snprintf(j, sizeof(j),
            "{\"leaf\":\"%s\",\"type\":\"req\",\"m\":\"%s\",\"u\":\"%s\",\"h\":\"%s\",\"ts\":\"%s\",\"rid\":\"%s\"}\n",
            leaf_hex,
            m->method[0]?m->method:"-",
            m->uri[0]?m->uri:"-",
            m->host[0]?m->host:"-",
            m->ts[0]?m->ts:"-",
            m->req_id[0]?m->req_id:"-");
        if (jl > 0) tcp_send_all_or_close(&g_log_fd, (const uint8_t*)j, (size_t)jl, "LOG");
    } else {
        int jl = snprintf(j, sizeof(j),
            "{\"leaf\":\"%s\",\"type\":\"resp\",\"c\":%d,\"etag\":\"%s\",\"ts\":\"%s\",\"rid\":\"%s\"}\n",
            leaf_hex,
            m->status_code,
            m->etag[0]?m->etag:"-",
            m->ts[0]?m->ts:"-",
            m->req_id[0]?m->req_id:"-");
        if (jl > 0) tcp_send_all_or_close(&g_log_fd, (const uint8_t*)j, (size_t)jl, "LOG");
    }
}

/* ===================== TCP 흐름 상태 ===================== */

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

enum hdr_state_e {
    HDR_COLLECTING = 0,
    HDR_DONE = 1,
    HDR_FAILED = 2
};

struct tcp_hdr_flow {
    uint32_t src_ip, dst_ip;
    uint16_t src_port, dst_port;

    uint32_t expected_seq;
    bool initialized;

    uint64_t last_activity_tsc;

    struct ooo_queue ooo;

    uint8_t *hdr_buf;
    size_t   hdr_len;
    size_t   hdr_cap;
    size_t   hdr_scan_pos;

    size_t   hdr_block_start;  /* 현재 HTTP 블록 시작 오프셋 */

    enum hdr_state_e hdr_state;
    uint32_t headers_collected;
};

static struct tcp_hdr_flow g_flows[MAX_TCP_FLOWS];
static int g_active_flows = 0;

/* ===================== IP Reassembly / GRO ===================== */

static struct rte_ip_frag_tbl *frag_tbl = NULL;
static struct rte_ip_frag_death_row death_row;
static void *gro_ctx = NULL;
static struct rte_mempool *reassembly_pool = NULL;

/* forward */
static void process_segment_mbuf(struct rte_mbuf *m);

/* ===================== 흐름 테이블 ===================== */

static struct tcp_hdr_flow* find_flow(uint32_t s_ip, uint32_t d_ip, uint16_t s_port, uint16_t d_port)
{
    for (int i = 0; i < g_active_flows; i++) {
        struct tcp_hdr_flow *f = &g_flows[i];
        if (f->src_ip==s_ip && f->dst_ip==d_ip && f->src_port==s_port && f->dst_port==d_port)
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

    if (g_flows[idx].hdr_buf) {
        free(g_flows[idx].hdr_buf);
        g_flows[idx].hdr_buf = NULL;
    }

    if (idx != g_active_flows - 1)
        g_flows[idx] = g_flows[g_active_flows - 1];

    g_active_flows--;
}

static struct tcp_hdr_flow* get_or_create_flow(uint32_t s_ip, uint32_t d_ip, uint16_t s_port, uint16_t d_port)
{
    struct tcp_hdr_flow *f = find_flow(s_ip, d_ip, s_port, d_port);
    if (f) return f;

    if (g_active_flows >= MAX_TCP_FLOWS) {
        uint64_t now = rte_get_tsc_cycles();
        int oldest = -1;
        uint64_t oldest_age = 0;
        for (int i = 0; i < g_active_flows; i++) {
            uint64_t age = now - g_flows[i].last_activity_tsc;
            if (oldest == -1 || age > oldest_age) { oldest = i; oldest_age = age; }
        }
        if (oldest != -1) drop_flow_at_index(oldest);
    }
    if (g_active_flows >= MAX_TCP_FLOWS) return NULL;

    f = &g_flows[g_active_flows++];
    memset(f, 0, sizeof(*f));
    f->src_ip = s_ip; f->dst_ip = d_ip; f->src_port = s_port; f->dst_port = d_port;
    f->expected_seq = 0; f->initialized = false;
    f->last_activity_tsc = rte_get_tsc_cycles();
    f->ooo.head = NULL; f->ooo.count = 0;
    f->hdr_buf = NULL; f->hdr_len = 0; f->hdr_cap = 0; f->hdr_scan_pos = 0;
    f->hdr_block_start = 0;
    f->hdr_state = HDR_COLLECTING; f->headers_collected = 0;
    return f;
}

/* ===================== 헤더 버퍼 연산 ===================== */

static inline bool ensure_hdr_cap(struct tcp_hdr_flow *f, size_t need)
{
    if (need > MAX_HEADER_BYTES) return false;
    if (f->hdr_cap >= need) return true;

    size_t new_cap = f->hdr_cap ? f->hdr_cap : INIT_HDR_CAP;
    while (new_cap < need) {
        new_cap <<= 1;
        if (new_cap > MAX_HEADER_BYTES) { new_cap = MAX_HEADER_BYTES; break; }
    }
    uint8_t *nb = (uint8_t*)realloc(f->hdr_buf, new_cap);
    if (!nb) return false;
    f->hdr_buf = nb;
    f->hdr_cap = new_cap;
    return true;
}

/*
 * 핵심 변경점:
 *  - EOH를 찾을 때마다 "해당 블록만" 파싱 -> leaf -> (A) audit send + (B) agg push
 *  - 다음 바이트가 또 다른 HTTP 시작이면 연속 블록 수집
 *  - 아니면 바디 시작으로 보고 HDR_DONE + 버퍼 free(메모리 절약)
 */
static void append_bytes_to_header(struct tcp_hdr_flow *f, const uint8_t *data, uint32_t len)
{
    if (f->hdr_state != HDR_COLLECTING) return;

    size_t need = f->hdr_len + (size_t)len;
    if (need > MAX_HEADER_BYTES || !ensure_hdr_cap(f, need)) {
        f->hdr_state = HDR_FAILED;
        DOCA_LOG_WARN("HDR buffer exceeded/alloc failed");
        return;
    }

    memcpy(f->hdr_buf + f->hdr_len, data, len);
    f->hdr_len += len;

    while (f->hdr_state == HDR_COLLECTING) {
        size_t eoh = find_eoh(f->hdr_buf, f->hdr_scan_pos, f->hdr_len);
        if (eoh == SIZE_MAX) {
            /* 다음 append에서 EOH가 걸릴 수 있게 약간 backscan */
            if (f->hdr_scan_pos + 4 < f->hdr_len) f->hdr_scan_pos = f->hdr_len - 4;
            return;
        }

        /* 1) (hdr_block_start ~ eoh) 블록 추출 & 파싱 & leaf */
        if (eoh > f->hdr_block_start) {
            struct mini_meta meta;
            if (parse_http_block_minimal(f->hdr_buf + f->hdr_block_start, eoh - f->hdr_block_start, &meta)) {
                uint8_t leaf[32];
                if (make_leaf_hash_minimal(&meta, leaf)) {
                    send_audit_minimal(&meta, leaf);  /* Path A */
                    agg_add_leaf(leaf);               /* Aggregator (Path B root 대상) */
                }
            }
        }

        f->headers_collected++;

        /* 2) EOH 이후 다음이 또 다른 HTTP 시작인지 확인 */
        size_t next_pos = eoh;
        while (next_pos < f->hdr_len &&
               (f->hdr_buf[next_pos] == '\r' || f->hdr_buf[next_pos] == '\n'))
            next_pos++;

        /* 데이터 부족 -> 더 받아야 판단 가능 */
        if (next_pos >= f->hdr_len) {
            f->hdr_scan_pos = (eoh > 4) ? (eoh - 4) : 0;
            return;
        }

        size_t remain = f->hdr_len - next_pos;

        if (is_http_header_start(&f->hdr_buf[next_pos], remain)) {
            /* 연속 헤더 블록 */
            f->hdr_block_start = next_pos;
            f->hdr_scan_pos = next_pos;
            continue;
        }

        /* 바디 시작 */
        f->hdr_state = HDR_DONE;

        /* 바디까지는 이제 관심 없으니 버퍼 해제(메모리 절약) */
        if (f->hdr_buf) {
            free(f->hdr_buf);
            f->hdr_buf = NULL;
        }
        f->hdr_len = 0;
        f->hdr_cap = 0;
        f->hdr_scan_pos = 0;
        f->hdr_block_start = 0;
        return;
    }
}

/* OOO 삽입(오름차순) */
static void queue_ooo_segment(struct tcp_hdr_flow *f, uint32_t seq, const uint8_t *data, uint32_t len)
{
    if (len == 0) return;
    if (f->ooo.count >= 64) return;

    struct ooo_seg *seg = (struct ooo_seg*)malloc(sizeof(*seg));
    if (!seg) return;

    seg->data = (uint8_t*)malloc(len);
    if (!seg->data) { free(seg); return; }

    memcpy(seg->data, data, len);
    seg->seq = seq;
    seg->len = len;
    seg->next = NULL;

    struct ooo_seg **cur = &f->ooo.head;
    while (*cur && (*cur)->seq < seq) cur = &(*cur)->next;
    seg->next = *cur;
    *cur = seg;
    f->ooo.count++;
}

/* OOO 배출(연속 부분 흡수) */
static void drain_ooo_segments(struct tcp_hdr_flow *f)
{
    bool progressed = true;
    while (progressed && f->ooo.head && f->hdr_state == HDR_COLLECTING) {
        progressed = false;

        struct ooo_seg **cur = &f->ooo.head;
        while (*cur) {
            struct ooo_seg *s = *cur;
            uint32_t exp = f->expected_seq;

            if (s->seq <= exp && s->seq + s->len > exp) {
                uint32_t off = exp - s->seq;
                uint32_t take = s->len - off;
                append_bytes_to_header(f, s->data + off, take);
                f->expected_seq += take;
                *cur = s->next; free(s->data); free(s); f->ooo.count--;
                progressed = true;
                break;
            } else if (s->seq == exp) {
                append_bytes_to_header(f, s->data, s->len);
                f->expected_seq += s->len;
                *cur = s->next; free(s->data); free(s); f->ooo.count--;
                progressed = true;
                break;
            } else {
                cur = &(*cur)->next;
            }
        }
    }
}

/* ===================== IPv4 조각/GRO ===================== */

static bool is_ipv4_fragmented(struct rte_mbuf *m)
{
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    if (rte_be_to_cpu_16(eth->ether_type) != RTE_ETHER_TYPE_IPV4) return false;

    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
    uint16_t frag_off = rte_be_to_cpu_16(ip->fragment_offset);
    return (frag_off & RTE_IPV4_HDR_MF_FLAG) || (frag_off & RTE_IPV4_HDR_OFFSET_MASK);
}

static struct rte_mbuf* ip_reassemble(struct rte_mbuf *m)
{
    uint64_t ts = rte_rdtsc();
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    struct rte_ipv4_hdr *ip4 = (struct rte_ipv4_hdr *)(eth + 1);
    struct rte_mbuf *out = rte_ipv4_frag_reassemble_packet(frag_tbl, &death_row, m, ts, ip4);
    return out; /* out==NULL이면 내부 큐에 보관됨 */
}

/* ===================== 세그먼트 처리 ===================== */

static int extract_tcp_segment(struct rte_mbuf *m,
                               uint32_t *s_ip, uint32_t *d_ip,
                               uint16_t *s_port, uint16_t *d_port,
                               uint32_t *seq,
                               const uint8_t **payload, uint32_t *plen,
                               uint8_t *flags)
{
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    if (rte_be_to_cpu_16(eth->ether_type) != RTE_ETHER_TYPE_IPV4) return -1;

    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
    if (ip->next_proto_id != IPPROTO_TCP) return -1;

    uint16_t ip_hdr_len  = (ip->version_ihl & 0x0F) * 4;
    struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)((uint8_t *)ip + ip_hdr_len);
    uint16_t tcp_hdr_len = ((tcp->data_off >> 4) & 0xF) * 4;

    *s_ip   = rte_be_to_cpu_32(ip->src_addr);
    *d_ip   = rte_be_to_cpu_32(ip->dst_addr);
    *s_port = rte_be_to_cpu_16(tcp->src_port);
    *d_port = rte_be_to_cpu_16(tcp->dst_port);
    *seq    = rte_be_to_cpu_32(tcp->sent_seq);
    *flags  = tcp->tcp_flags;

    uint16_t ip_len = rte_be_to_cpu_16(ip->total_length);
    int hdr_total   = ip_hdr_len + tcp_hdr_len;
    *plen    = (hdr_total <= ip_len) ? (ip_len - hdr_total) : 0;
    *payload = (const uint8_t *)tcp + tcp_hdr_len;
    return 0;
}

static void process_segment_mbuf(struct rte_mbuf *m)
{
    uint32_t s_ip, d_ip, seq;
    uint16_t s_port, d_port;
    const uint8_t *payload;
    uint32_t plen;
    uint8_t flags;

    if (extract_tcp_segment(m, &s_ip, &d_ip, &s_port, &d_port, &seq, &payload, &plen, &flags) < 0) {
        rte_pktmbuf_free(m);
        return;
    }

    /* 순수 ACK(데이터 없음) -> 스킵 (FIN/RST 제외) */
    if (plen == 0 && !(flags & (RTE_TCP_FIN_FLAG | RTE_TCP_RST_FLAG))) {
        rte_pktmbuf_free(m);
        return;
    }

    struct tcp_hdr_flow *f = get_or_create_flow(s_ip, d_ip, s_port, d_port);
    if (!f) { rte_pktmbuf_free(m); return; }
    f->last_activity_tsc = rte_get_tsc_cycles();

    if (f->hdr_state != HDR_COLLECTING) {
        rte_pktmbuf_free(m);
        return;
    }

    /* 첫 페이로드에서 expected_seq 초기화 */
    if (!f->initialized && plen > 0) {
        f->expected_seq = seq;
        f->initialized = true;
    }

    if (plen > 0) {
        if (seq == f->expected_seq) {
            append_bytes_to_header(f, payload, plen);
            f->expected_seq += plen;
            drain_ooo_segments(f);
        } else if (seq > f->expected_seq) {
            queue_ooo_segment(f, seq, payload, plen);
        } else {
            uint32_t exp = f->expected_seq;
            if (seq + plen <= exp) {
                /* 완전 중복 */
            } else {
                /* 부분 겹침 */
                uint32_t off = exp - seq;
                append_bytes_to_header(f, payload + off, plen - off);
                f->expected_seq += (plen - off);
                drain_ooo_segments(f);
            }
        }
    }

    if (flags & (RTE_TCP_FIN_FLAG | RTE_TCP_RST_FLAG)) {
        f->hdr_state = HDR_DONE;
        /* 남은 leaf는 이미 EOH에서 생성됨. 여기서는 특별히 할 일 없음 */
    }

    rte_pktmbuf_free(m);
}

/* ===================== GRO 프런트 ===================== */

static void handle_one_packet(struct rte_mbuf *m)
{
    struct rte_mbuf *pm = m;

    if (is_ipv4_fragmented(m)) {
        struct rte_mbuf *reassembled = ip_reassemble(m);
        if (reassembled == NULL) return; /* 내부 큐에 쌓이는 중 */
        pm = reassembled;
    }

    struct rte_mbuf *in[1] = { pm };
    uint16_t outn = rte_gro_reassemble(in, 1, gro_ctx);
    for (uint16_t i = 0; i < outn; i++)
        process_segment_mbuf(in[i]);

    /* 주기적 GRO flush */
    static uint64_t last_flush = 0;
    uint64_t now = rte_get_tsc_cycles();
    if (now - last_flush > GRO_FLUSH_INTERVAL_CYCLES) {
        struct rte_mbuf *out[32];
        uint16_t nb = rte_gro_timeout_flush(gro_ctx, rte_get_tsc_hz() / 100, RTE_GRO_TCP_IPV4, out, 32);
        for (uint16_t i = 0; i < nb; i++)
            process_segment_mbuf(out[i]);
        last_flush = now;
    }
}

/* ===================== 정리/청소 ===================== */

static void cleanup_idle_flows(void)
{
    uint64_t now = rte_get_tsc_cycles();
    uint64_t timeout_cycles = (uint64_t)TCP_FLOW_TIMEOUT_SEC * rte_get_tsc_hz();

    int i = 0;
    while (i < g_active_flows) {
        struct tcp_hdr_flow *f = &g_flows[i];
        if (now - f->last_activity_tsc > timeout_cycles) {
            drop_flow_at_index(i);
        } else {
            i++;
        }
    }
}

/* ===================== 초기화/해제 ===================== */

static int init_reassembly_subsystems(void)
{
    uint32_t socket_id = rte_socket_id();

    reassembly_pool = rte_pktmbuf_pool_create("reassembly_pool",
                                              1024, 128, 0,
                                              RTE_MBUF_DEFAULT_BUF_SIZE,
                                              socket_id);
    if (!reassembly_pool) { DOCA_LOG_ERR("mpool create fail"); return -1; }

    uint64_t timeout_cycles = (uint64_t)FRAG_TIMEOUT_SEC * rte_get_tsc_hz();
    frag_tbl = rte_ip_frag_table_create(FRAG_BUCKETS, FRAG_MAX_ENTRIES, FRAG_MAX_PER_BUCKET,
                                        timeout_cycles, (int)socket_id);
    if (!frag_tbl) {
        DOCA_LOG_ERR("frag table create fail");
        rte_mempool_free(reassembly_pool); reassembly_pool = NULL;
        return -1;
    }
    memset(&death_row, 0, sizeof(death_row));

    struct rte_gro_param gp = {
        .gro_types = RTE_GRO_TCP_IPV4,
        .max_flow_num = GRO_MAX_FLOW,
        .max_item_per_flow = GRO_MAX_ITEMS_PER_FLOW,
        .socket_id = socket_id
    };
    gro_ctx = rte_gro_ctx_create(&gp);
    if (!gro_ctx) {
        DOCA_LOG_ERR("gro ctx create fail");
        rte_ip_frag_table_destroy(frag_tbl); frag_tbl = NULL;
        rte_mempool_free(reassembly_pool); reassembly_pool = NULL;
        return -1;
    }

    memset(g_flows, 0, sizeof(g_flows));
    g_active_flows = 0;

    /* aggregator init */
    g_leaf_cnt = 0;
    g_last_flush_tsc = rte_get_tsc_cycles();

    return 0;
}

static void cleanup_reassembly_subsystems(void)
{
    for (int i = 0; i < g_active_flows; i++) {
        free_ooo_queue(&g_flows[i].ooo);
        if (g_flows[i].hdr_buf) { free(g_flows[i].hdr_buf); g_flows[i].hdr_buf = NULL; }
    }
    g_active_flows = 0;

    if (gro_ctx) { rte_gro_ctx_destroy(gro_ctx); gro_ctx = NULL; }
    if (frag_tbl) { rte_ip_frag_table_destroy(frag_tbl); frag_tbl = NULL; }
    if (reassembly_pool) { rte_mempool_free(reassembly_pool); reassembly_pool = NULL; }
}

/* ===================== DOCA Flow: IPv4 RSS 파이프(스켈레톤) ===================== */

static doca_error_t create_rss_tcp_ipv4_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
    struct doca_flow_match match;
    memset(&match, 0, sizeof(match));

    struct doca_flow_actions actions;
    memset(&actions, 0, sizeof(actions));

    struct doca_flow_actions *actions_arr[1] = { &actions };

    struct doca_flow_fwd fwd, fwd_miss;
    memset(&fwd, 0, sizeof(fwd));
    memset(&fwd_miss, 0, sizeof(fwd_miss));

    struct doca_flow_pipe_cfg *cfg = NULL;
    uint16_t rss_queues[1] = {0};
    doca_error_t r;


    match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
    match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;


    match.outer.ip4.src_ip = 0xffffffff;


    actions.meta.pkt_meta = UINT32_MAX;

    r = doca_flow_pipe_cfg_create(&cfg, port);
    if (r != DOCA_SUCCESS) {
        DOCA_LOG_ERR("pipe_cfg_create: %s", doca_error_get_descr(r));
        return r;
    }

    r = set_flow_pipe_cfg(cfg, "RSS_IPv4_SRC_ONLY_FIXED", DOCA_FLOW_PIPE_BASIC, true);
    if (r != DOCA_SUCCESS) {
        DOCA_LOG_ERR("set_flow_pipe_cfg: %s", doca_error_get_descr(r));
        goto out_cfg;
    }

    /* match_mask를 NULL로 두는 implicit match 모드 */
    r = doca_flow_pipe_cfg_set_match(cfg, &match, NULL);
    if (r != DOCA_SUCCESS) {
        DOCA_LOG_ERR("pipe_cfg_set_match: %s", doca_error_get_descr(r));
        goto out_cfg;
    }


    r = doca_flow_pipe_cfg_set_actions(cfg, actions_arr, NULL, NULL, 1);
    if (r != DOCA_SUCCESS) {
        DOCA_LOG_ERR("pipe_cfg_set_actions: %s", doca_error_get_descr(r));
        goto out_cfg;
    }

    /* ===== RSS fwd 설정 =====
     * DOCA 3.1에서 RSS 설정이 리팩터링되었기 때문에,
     * (1) 현재 프로젝트 헤더/샘플이 구필드(rss_queues/num_of_queues...)를 쓰면 Legacy 블록 사용
     * (2) fwd.rss.* / fwd.rss_type을 요구하면 DOCA3.1 블록 사용
     * 릴리즈 노트: shared/non-shared 지정 필요:contentReference[oaicite:7]{index=7}
     */

    /* --- Legacy 스타일 --- */
    fwd.type = DOCA_FLOW_FWD_RSS;
    fwd.rss_queues = rss_queues;
    fwd.num_of_queues = 1;
    fwd.rss_inner_flags = DOCA_FLOW_RSS_IPV4;

    /* miss는 기존과 동일하게 DROP */
    fwd_miss.type = DOCA_FLOW_FWD_DROP;

    r = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, pipe);

out_cfg:
    doca_flow_pipe_cfg_destroy(cfg);
    return r;
}


static doca_error_t add_src_ip_entry_param(struct doca_flow_pipe *pipe, uint32_t be_src_ip, struct entries_status *st)
{
    struct doca_flow_match mval = {0};
    struct doca_flow_actions act = {0};
    struct doca_flow_pipe_entry *entry;
    doca_error_t r;

    mval.outer.ip4.src_ip = be_src_ip;
    act.action_idx = 0;

    r = doca_flow_pipe_add_entry(0, pipe, &mval, &act, NULL, NULL, 0, st, &entry);
    if (r != DOCA_SUCCESS) {
        DOCA_LOG_ERR("pipe_add_entry: %s", doca_error_get_descr(r));
        return r;
    }
    return DOCA_SUCCESS;
}

/* ===================== RX 루프 ===================== */

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

    /* 주기적 root flush도 RX 루프에서 같이 체크(1초/1000개) */
    agg_flush_if_needed(false);
}

/* ===================== Entry ===================== */

doca_error_t doca(int nb_queues)
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

    if (init_reassembly_subsystems() != 0) {
        DOCA_LOG_ERR("reassembly subsystems init failed");
        return DOCA_ERROR_INITIALIZATION;
    }

    /* Dual-path sockets */
    if (tcp_connect_once(&g_log_fd, LOG_DST_IP, LOG_DST_PORT, "LOG") != 0 ||
        tcp_connect_once(&g_chain_fd, CHAIN_DST_IP, CHAIN_DST_PORT, "CHAIN") != 0) {
        DOCA_LOG_ERR("tcp_connect failed");
        tcp_close_if_open(&g_log_fd);
        tcp_close_if_open(&g_chain_fd);
        cleanup_reassembly_subsystems();
        return DOCA_ERROR_INITIALIZATION;
    }

    /* DOCA Flow init */
    r = init_doca_flow(nb_queues, "vnf,hws", &res, nr_shared);
    if (r != DOCA_SUCCESS) {
        DOCA_LOG_ERR("init_doca_flow: %s", doca_error_get_descr(r));
        tcp_close_if_open(&g_log_fd);
        tcp_close_if_open(&g_chain_fd);
        cleanup_reassembly_subsystems();
        return r;
    }

    r = init_doca_flow_ports(nb_ports, ports, true, devs);
    if (r != DOCA_SUCCESS) {
        DOCA_LOG_ERR("init_doca_flow_ports: %s", doca_error_get_descr(r));
        doca_flow_destroy();
        tcp_close_if_open(&g_log_fd);
        tcp_close_if_open(&g_chain_fd);
        cleanup_reassembly_subsystems();
        return r;
    }

    for (port_id = 0; port_id < nb_ports; port_id++) {
        memset(&st, 0, sizeof(st));

        r = create_rss_tcp_ipv4_pipe(ports[port_id], &pipe);
        if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("create_rss_tcp_ipv4_pipe failed"); goto stop_ports; }

        /* 예시 엔트리(환경에 맞게 변경) */
        r = add_src_ip_entry_param(pipe, BE_IPV4_ADDR(10,197,0,9), &st);
        if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("add_src_ip_entry_param failed"); goto stop_ports; }

        r = add_src_ip_entry_param(pipe, BE_IPV4_ADDR(10,197,0,11), &st);
        if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("add_src_ip_entry_param failed"); goto stop_ports; }

        int processed_total = 0;
        for (int tries = 0; tries < 8 && processed_total < 2; tries++) {
            r = doca_flow_entries_process(ports[port_id], 0, DEFAULT_TIMEOUT_US, 1);
            if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("entries_process: %s", doca_error_get_descr(r)); goto stop_ports; }
            processed_total += st.nb_processed;
            if (st.failure) { DOCA_LOG_ERR("entries_process failure"); goto stop_ports; }
            memset(&st, 0, sizeof(st));
        }
        if (processed_total < 2) { DOCA_LOG_ERR("entries insufficient (%d<2)", processed_total); r = DOCA_ERROR_UNKNOWN; goto stop_ports; }
    }

    DOCA_LOG_INFO("=== HTTP Minimal Extract + CPU SHA256 + Merkle (Prototype) ===");
    DOCA_LOG_INFO("Path A(LOG): leaf + minimal fields -> %s:%u", LOG_DST_IP, (unsigned)LOG_DST_PORT);
    DOCA_LOG_INFO("Path B(CHAIN): merkle root -> %s:%u", CHAIN_DST_IP, (unsigned)CHAIN_DST_PORT);
    DOCA_LOG_INFO("Flush: every %d leaves or ~1s", AGG_MAX_LEAVES);
    DOCA_LOG_INFO("==============================================================");

    while (!force_quit) {
        rx_loop_once(0);
        usleep(10000); /* 10ms */
    }

    /* 종료 시 남은 leaf flush */
    agg_flush_if_needed(true);

stop_ports:
    stop_doca_flow_ports(nb_ports, ports);
    doca_flow_destroy();
    tcp_close_if_open(&g_log_fd);
    tcp_close_if_open(&g_chain_fd);
    cleanup_reassembly_subsystems();
    return r;
}
