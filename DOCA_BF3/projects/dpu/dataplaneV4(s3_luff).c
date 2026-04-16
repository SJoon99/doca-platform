/*
 * DPU-native HTTP Header Aggregator (TCP/IPv4, GRO/IP-frag) + DOCA Flow RSS (skeleton)
 *  - 목표: 바디 시작 전 "모든 연속 HTTP 요청 헤더 블록"만 인오더 재조립
 *  - 기준: "\r\n\r\n" 또는 "\n\n" 으로 헤더 블록 종결 감지 → 다음 바이트가 요청라인인지 검사
 *  - 연속: 다음 바이트가 요청라인이면 계속 수집, 아니면 바디 시작으로 간주하고 중단
 *  - 예외: S3 스트리밍 "hex;chunk-signature=" 라인 → 바디 시작으로 간주
 *  - 전송: 재조립 완료된 헤더 바이트 전체를 단일 TCP 연결로 send()
 *  - 유지: IPv4 fragment reassembly, DPDK GRO, OOO 큐, DOCA Flow 초기화
 *  - 제거: regex 오프로딩, 청크/CRC/봉인/라우팅
 */

#include <string.h>         /* memset, memcpy, memcmp, strstr, strncasecmp */
#include <unistd.h>         /* usleep, close */
#include <netinet/in.h>     /* sockaddr_in, htons, inet_pton */
#include <stdlib.h>         /* malloc, realloc, free */
#include <stdio.h>          /* snprintf */
#include <stdbool.h>        /* bool */
#include <inttypes.h>       /* PRIu64 */

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

#include "flow_common.h"    /* init_doca_flow, init_doca_flow_ports, set_flow_pipe_cfg, stop_doca_flow_ports, DEFAULT_TIMEOUT_US, entries_status, etc. */

extern volatile bool force_quit;

DOCA_LOG_REGISTER(FLOW_APP_HDRAGG)

/* ===================== 튜너블 ===================== */

#define PACKET_BURST                   128          /* RX 버스트 크기 */
#define FRAG_BUCKETS                   64           /* IPv4 조각 테이블 버킷 */
#define FRAG_MAX_ENTRIES               256          /* IPv4 조각 엔트리 */
#define FRAG_MAX_PER_BUCKET            4            /* 버킷당 최대 연결 길이 */
#define FRAG_TIMEOUT_SEC               30           /* 조각 타임아웃(초) */

#define GRO_MAX_FLOW                   128          /* GRO 플로우 */
#define GRO_MAX_ITEMS_PER_FLOW         16           /* GRO 항목/플로우 */

#define MAX_TCP_FLOWS                  512          /* 동시 추적 플로우 수 */
#define TCP_FLOW_TIMEOUT_SEC           30           /* 비활성 플로우 타임아웃(초) */

#define GRO_FLUSH_INTERVAL_CYCLES      (rte_get_tsc_hz() / 100)  /* 10ms */
#define CLEANUP_INTERVAL_CYCLES        (rte_get_tsc_hz() * 10)   /* 10s */

#define MAX_HEADER_BYTES               (64 * 1024) /* 흐름별 헤더 누적 상한 */
#define INIT_HDR_CAP                   2048        /* 헤더 버퍼 초기 용량 */

/* ===================== 외부 TCP 전송(단일 연결) ===================== */
/*  - 목적: 헤더 재조립 완료 시 바이트 그대로 전송
 *  - 구성: 실행 시작 시 1회 connect → 실패 시 전체 앱 실패 처리
 *  - 주의: 전송 실패 시 연결 닫고 FD 무효화
 */

#define TX_DST_IP      "10.38.36.32"   /* 수신기 IP (환경에 맞게) */
#define TX_DST_PORT    8000            /* 수신기 포트 */

static int g_tx_fd = -1;               /* 송신용 TCP FD */

static int tcp_connect_once(const char *dst_ip, uint16_t dst_port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);            /* TCP 소켓 */
    if (fd < 0) { DOCA_LOG_ERR("socket() failed"); return -1; }

    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));   /* 목적지 소켓 주소 */
    sa.sin_family = AF_INET;
    sa.sin_port = htons(dst_port);
    if (inet_pton(AF_INET, dst_ip, &sa.sin_addr) != 1) { DOCA_LOG_ERR("inet_pton failed"); close(fd); return -1; }

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        DOCA_LOG_ERR("connect() failed");
        close(fd);
        return -1;
    }
    g_tx_fd = fd;                                        /* 전역 FD 보관 */
    DOCA_LOG_INFO("TX socket connected to %s:%u", dst_ip, (unsigned)dst_port);
    return 0;
}

static void tcp_close_if_open(void)
{
    if (g_tx_fd >= 0) { close(g_tx_fd); g_tx_fd = -1; }
}

/* 전체 바이트 전송 루프 */
static void tcp_send_all_or_close(const uint8_t *p, size_t n)
{
    if (g_tx_fd < 0 || !p || n == 0) return;
    size_t left = n;
    while (left > 0) {
        ssize_t s = send(g_tx_fd, p, left, 0);
        if (s <= 0) {
            DOCA_LOG_ERR("send() failed, closing TX socket");
            tcp_close_if_open();
            return;
        }
        p += s; left -= (size_t)s;
    }
}

/* ===================== 유틸 ===================== */

static inline uint64_t tsc_to_us(uint64_t tsc) { return (tsc * 1000000ull) / rte_get_tsc_hz(); }

/* 요청라인 접두 확인 (대소문자 허용) */
static inline bool is_request_line_prefix(const uint8_t *s, size_t n)
{
    /* 안전 길이 확인 */
    if (n < 3) return false;

    /* 공통 케이스 빠른 비교 (대문자/소문자 쌍) */
    /* "GET " */
    if (n >= 4 && (!memcmp(s,"GET ",4) || !memcmp(s,"get ",4))) return true;
    /* "PUT " */
    if (n >= 4 && (!memcmp(s,"PUT ",4) || !memcmp(s,"put ",4))) return true;
    /* "POST " */
    if (n >= 5 && (!memcmp(s,"POST ",5) || !memcmp(s,"post ",5))) return true;
    /* "HEAD " */
    if (n >= 5 && (!memcmp(s,"HEAD ",5) || !memcmp(s,"head ",5))) return true;
    /* "DELETE " */
    if (n >= 7 && (!memcmp(s,"DELETE ",7) || !memcmp(s,"delete ",7))) return true;
    /* "OPTIONS " */
    if (n >= 8 && (!memcmp(s,"OPTIONS ",8) || !memcmp(s,"options ",8))) return true;
    /* "PATCH " */
    if (n >= 6 && (!memcmp(s,"PATCH ",6) || !memcmp(s,"patch ",6))) return true;

    return false;
}

/* S3 스트리밍 청크 시그니처 라인 접두 확인
 *  - ^[0-9a-fA-F]+;chunk-signature=
 */
static inline bool is_chunk_signature_line_start(const uint8_t *s, size_t n)
{
    const char *tag = "chunk-signature=";                /* 고정 태그 */
    size_t taglen = strlen(tag);

    /* 앞부분 hex 1+ */
    size_t i = 0;
    while (i < n) {
        char c = (char)s[i];
        bool is_hex = (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');
        if (!is_hex) break;
        i++;
    }
    if (i == 0) return false;                            /* hex 미존재 */
    if (i >= n) return false;                            /* 다음 기호 부족 */
    if (s[i] != ';') return false;                       /* 구분자 ';' 필요 */
    if (i + 1 + taglen > n) return false;                /* 태그 길이 부족 */
    if (memcmp(&s[i+1], tag, taglen) != 0) return false; /* 태그 불일치 */
    return true;
}

/* 헤더 종료 구분자 탐색
 *  - \r\n\r\n 또는 \n\n
 *  - start 이전 3바이트까지 백스캔 여유
 *  - 발견 시: 종료 직후 오프셋 반환, 없으면 SIZE_MAX
 */
static size_t find_eoh(const uint8_t *buf, size_t start, size_t len)
{
    size_t i = (start > 3) ? (start - 3) : 0;
    for (; i + 1 < len; i++) {
        /* \r\n\r\n */
        if (i + 3 < len && buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2]=='\r' && buf[i+3]=='\n')
            return i + 4;
        /* \n\n */
        if (buf[i]=='\n' && buf[i+1]=='\n')
            return i + 2;
    }
    return SIZE_MAX;
}

/* EOH 이후 분류
 *  - NEXT_REQUEST: 다음 바이트가 요청라인 접두
 *  - BODY_START  : 요청라인이 아니거나 S3 청크 시그니처면 바디 시작
 *  - NEED_MORE   : 판별 불가(데이터 부족)
 */
enum post_eoh_type { NEXT_REQUEST=0, BODY_START=1, NEED_MORE=2 };

static enum post_eoh_type classify_post_eoh(const uint8_t *buf, size_t after, size_t len)
{
    if (after >= len) return NEED_MORE;

    /* CR/LF 스킵(방어) */
    size_t i = after;
    while (i < len && (buf[i]=='\r' || buf[i]=='\n')) i++;

    size_t remain = len - i;
    if (remain == 0) return NEED_MORE;

    /* 요청라인 접두 → 다음 헤더 블록 */
    if (is_request_line_prefix(&buf[i], remain)) return NEXT_REQUEST;

    /* S3 청크 시그니처 → 즉시 바디 시작 */
    if (is_chunk_signature_line_start(&buf[i], remain)) return BODY_START;

    /* 그 외 → 바디 시작으로 간주 */
    return BODY_START;
}

/* ===================== TCP 흐름 상태 ===================== */

struct ooo_seg {
    uint32_t seq;                  /* 세그먼트 시작 seq */
    uint32_t len;                  /* 길이 */
    uint8_t *data;                 /* 데이터 복사본 */
    struct ooo_seg *next;          /* 다음 */
};

struct ooo_queue {
    struct ooo_seg *head;          /* 헤드 */
    int count;                     /* 개수 */
};

enum hdr_state_e {
    HDR_COLLECTING = 0,            /* 헤더 수집 중 */
    HDR_DONE = 1,                  /* 헤더 완료(바디 시작) */
    HDR_FAILED = 2                 /* 비정상/상한 초과/FINRST */
};

struct tcp_hdr_flow {
    /* 방향성 5-튜플 */
    uint32_t src_ip, dst_ip;
    uint16_t src_port, dst_port;

    /* TCP 순서 제어 */
    uint32_t expected_seq;
    bool initialized;

    /* 최근 활동 TSC */
    uint64_t last_activity_tsc;

    /* OOO 큐 */
    struct ooo_queue ooo;

    /* 헤더 버퍼 */
    uint8_t *hdr_buf;              /* 누적 버퍼 */
    size_t   hdr_len;              /* 사용 길이 */
    size_t   hdr_cap;              /* 용량 */
    size_t   hdr_scan_pos;         /* EOH 스캔 시작 오프셋 */
    enum hdr_state_e hdr_state;    /* 상태 */
    uint32_t headers_collected;    /* 수집 완료 블록 수 */
    bool     sent_once;            /* 전송 여부(중복 방지) */
};

static struct tcp_hdr_flow g_flows[MAX_TCP_FLOWS];
static int g_active_flows = 0;

/* ===================== IP Reassembly / GRO ===================== */

static struct rte_ip_frag_tbl *frag_tbl = NULL;      /* IPv4 조각 테이블 */
static struct rte_ip_frag_death_row death_row;       /* 만료 큐 */
static void *gro_ctx = NULL;                         /* GRO 컨텍스트 */
static struct rte_mempool *reassembly_pool = NULL;   /* mbuf 풀 */
static void process_segment_mbuf(struct rte_mbuf *m); /* forward */
static enum post_eoh_type classify_post_eoh(const uint8_t *buf, size_t after, size_t len); /* EOH 후속 분류 결과 타입 */

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
    q->head = NULL; q->count = 0;
}

static void drop_flow_at_index(int idx)
{
    if (idx < 0 || idx >= g_active_flows) return;

    free_ooo_queue(&g_flows[idx].ooo);               /* OOO 해제 */

    if (g_flows[idx].hdr_buf) {                      /* 헤더 버퍼 해제 */
        free(g_flows[idx].hdr_buf);
        g_flows[idx].hdr_buf = NULL;
    }

    if (idx != g_active_flows - 1)                   /* 배열 압축 */
        g_flows[idx] = g_flows[g_active_flows - 1];

    g_active_flows--;
}

static struct tcp_hdr_flow* get_or_create_flow(uint32_t s_ip, uint32_t d_ip, uint16_t s_port, uint16_t d_port)
{
    struct tcp_hdr_flow *f = find_flow(s_ip, d_ip, s_port, d_port);
    if (f) return f;

    /* 여유 없으면 가장 오래된 비활성 제거 */
    if (g_active_flows >= MAX_TCP_FLOWS) {
        uint64_t now = rte_get_tsc_cycles();
        int oldest = -1; uint64_t oldest_age = 0;
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
    f->hdr_state = HDR_COLLECTING; f->headers_collected = 0;
    f->sent_once = false;
    return f;
}

/* ===================== 헤더 버퍼 연산 ===================== */

static inline bool ensure_hdr_cap(struct tcp_hdr_flow *f, size_t need)
{
    if (need > MAX_HEADER_BYTES) return false;        /* 상한 보호 */
    if (f->hdr_cap >= need) return true;

    size_t new_cap = f->hdr_cap ? f->hdr_cap : INIT_HDR_CAP;
    while (new_cap < need) {
        new_cap <<= 1;
        if (new_cap > MAX_HEADER_BYTES) { new_cap = MAX_HEADER_BYTES; break; }
    }
    uint8_t *nb = (uint8_t*)realloc(f->hdr_buf, new_cap);
    if (!nb) return false;

    f->hdr_buf = nb; f->hdr_cap = new_cap;
    return true;
}

static inline bool is_status_line_prefix(const uint8_t *s, size_t n)
{
    // "HTTP/" 접두 확인
    if (n < 8) return false;

    /* HTTP/1.1, HTTP/1.0, HTTP/2.0 */
    if (n >= 8 && !memcmp(s, "HTTP/1.1", 8)) return true;
    if (n >= 8 && !memcmp(s, "HTTP/1.0", 8)) return true;
    if (n >= 8 && !memcmp(s, "HTTP/2.0", 8)) return true;
    if (n >= 8 && !memcmp(s, "http/1.1", 8)) return true;  
    if (n >= 8 && !memcmp(s, "http/1.0", 8)) return true;

    return false;
}

static inline bool is_http_header_start(const uint8_t *s, size_t n)
{
    return is_request_line_prefix(s, n) || is_status_line_prefix(s, n);
}

/* 헤더 바이트 누적 + 다중 블록 처리 + 바디 시작 판정 + 전송 트리거 */
static void append_bytes_to_header(struct tcp_hdr_flow *f, const uint8_t *data, uint32_t len)
{
    if (f->hdr_state != HDR_COLLECTING) return;

    // 1. 버퍼 용량 확보
    size_t need = f->hdr_len + (size_t)len;
    if (need > MAX_HEADER_BYTES) {
        f->hdr_state = HDR_FAILED;
        DOCA_LOG_WARN("HDR.BUF exceed limit");
        return;
    }
    if (!ensure_hdr_cap(f, need)) {
        f->hdr_state = HDR_FAILED;
        return;
    }

    // 2. 데이터 추가
    memcpy(f->hdr_buf + f->hdr_len, data, len);
    f->hdr_len += len;

    // 3. EOH 탐색 (단순 루프)
    while (f->hdr_state == HDR_COLLECTING) {
        size_t eoh = find_eoh(f->hdr_buf, f->hdr_scan_pos, f->hdr_len);
        
        if (eoh == SIZE_MAX) {
            // EOH 없음 - 스캔 위치만 업데이트
            if (f->hdr_scan_pos + 4 < f->hdr_len) {
                f->hdr_scan_pos = f->hdr_len - 4;
            }
            return;
        }

        // 4. EOH 발견 - 다음 내용 확인
        f->headers_collected++;
        
        // CR/LF 스킵
        size_t next_pos = eoh;
        while (next_pos < f->hdr_len && 
               (f->hdr_buf[next_pos] == '\r' || f->hdr_buf[next_pos] == '\n')) {
            next_pos++;
        }

        // 데이터 부족
        if (next_pos >= f->hdr_len) {
            f->hdr_scan_pos = (eoh > 4) ? (eoh - 4) : 0;
            return;
        }

        size_t remain = f->hdr_len - next_pos;

        // 5. 다음 요청인지 확인
        if (is_http_header_start(&f->hdr_buf[next_pos], remain)) {
            f->hdr_scan_pos = eoh;
            continue;  // 다음 헤더 블록 계속
        }

        f->hdr_len = next_pos; // 바디 시작 전까지 자르기
        f->hdr_scan_pos = 0; // 스캔 위치 초기화
        
        DOCA_LOG_INFO("HDR.DONE %u.%u.%u.%u:%u->%u.%u.%u.%u:%u blocks=%u bytes=%zu",
                      (f->src_ip>>24)&0xFF,(f->src_ip>>16)&0xFF,
                      (f->src_ip>>8)&0xFF,f->src_ip&0xFF,f->src_port,
                      (f->dst_ip>>24)&0xFF,(f->dst_ip>>16)&0xFF,
                      (f->dst_ip>>8)&0xFF,f->dst_ip&0xFF,f->dst_port,
                      f->headers_collected, f->hdr_len);
        return;
    }
}

static void finalize_and_send_flow(struct tcp_hdr_flow *f, const char *reason)
{
    if (f->hdr_len > 0 && !f->sent_once && g_tx_fd >= 0) {
        DOCA_LOG_INFO("HDR.FINAL %s blocks=%u bytes=%zu",
                    reason, f->headers_collected, f->hdr_len);
        tcp_send_all_or_close(f->hdr_buf, f->hdr_len);
        f->sent_once = true;
    }
}

/* OOO 삽입(오름차순) */
static void queue_ooo_segment(struct tcp_hdr_flow *f, uint32_t seq, const uint8_t *data, uint32_t len)
{
    if (len == 0) return;
    if (f->ooo.count >= 64) return;                    /* 간단 제한 */
    struct ooo_seg *seg = (struct ooo_seg*)malloc(sizeof(*seg));
    if (!seg) return;
    seg->data = (uint8_t*)malloc(len);
    if (!seg->data) { free(seg); return; }
    memcpy(seg->data, data, len);
    seg->seq = seq; seg->len = len; seg->next = NULL;

    struct ooo_seg **cur = &f->ooo.head;
    while (*cur && (*cur)->seq < seq) cur = &(*cur)->next;
    seg->next = *cur; *cur = seg; f->ooo.count++;
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
                /* partial overlap forward */
                uint32_t off = exp - s->seq;
                uint32_t take = s->len - off;
                append_bytes_to_header(f, s->data + off, take);
                f->expected_seq += take;
                *cur = s->next; free(s->data); free(s); f->ooo.count--;
                progressed = true; break;
            } else if (s->seq == exp) {
                /* exact next */
                append_bytes_to_header(f, s->data, s->len);
                f->expected_seq += s->len;
                *cur = s->next; free(s->data); free(s); f->ooo.count--;
                progressed = true; break;
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
    if (out) DOCA_LOG_INFO("IP4.reassembled %u bytes", rte_pktmbuf_pkt_len(out));
    return out;
}

static void gro_timeout_flush_and_process(void)
{
    struct rte_mbuf *out[32];
    uint16_t nb = rte_gro_timeout_flush(gro_ctx, rte_get_tsc_hz() / 100, RTE_GRO_TCP_IPV4, out, 32);
    for (uint16_t i = 0; i < nb; i++) {
        process_segment_mbuf(out[i]);                   /* 미리 선언 필요 → 아래 static 선언 이동 */
    }
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

/* forward 선언 보정 */
static void gro_timeout_flush_and_process(void);

static void process_segment_mbuf(struct rte_mbuf *m)
{
    uint32_t s_ip, d_ip, seq;
    uint16_t s_port, d_port;
    const uint8_t *payload; uint32_t plen;
    uint8_t flags;

    if (extract_tcp_segment(m, &s_ip, &d_ip, &s_port, &d_port, &seq, &payload, &plen, &flags) < 0) {
        rte_pktmbuf_free(m); return;
    }

    /* 순수 ACK → 스킵 (FIN/RST 제외) */
    if (plen == 0 && !(flags & (RTE_TCP_FIN_FLAG | RTE_TCP_RST_FLAG))) { rte_pktmbuf_free(m); return; }

    struct tcp_hdr_flow *f = get_or_create_flow(s_ip, d_ip, s_port, d_port);
    if (!f) { rte_pktmbuf_free(m); return; }
    f->last_activity_tsc = rte_get_tsc_cycles();

    /* 헤더 완료/실패 → 추가 재조립 불필요 */
    if (f->hdr_state != HDR_COLLECTING) { rte_pktmbuf_free(m); return; }

    /* 첫 페이로드에서 expected_seq 초기화 */
    if (!f->initialized && plen > 0) { f->expected_seq = seq; f->initialized = true; }

    /* 인오더/OOO/겹침 처리 */
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
                /* 완전 중복 → 무시 */
            } else {
                uint32_t off = exp - seq;             /* 부분 겹침 */
                append_bytes_to_header(f, payload + off, plen - off);
                f->expected_seq += (plen - off);
                drain_ooo_segments(f);
            }
        }
    }

    if (flags & (RTE_TCP_FIN_FLAG | RTE_TCP_RST_FLAG)) {
        if (f->hdr_len > 0) {
            /* 헤더 데이터 있으면 전송 시도 */
            finalize_and_send_flow(f, "FIN/RST");
        } else {
            /* 헤더 없으면 실패 처리 */
            DOCA_LOG_WARN("HDR.FINRST no data %u.%u.%u.%u:%u->%u.%u.%u.%u:%u",
                          (f->src_ip>>24)&0xFF,(f->src_ip>>16)&0xFF,
                          (f->src_ip>>8)&0xFF,f->src_ip&0xFF,f->src_port,
                          (f->dst_ip>>24)&0xFF,(f->dst_ip>>16)&0xFF,
                          (f->dst_ip>>8)&0xFF,f->dst_ip&0xFF,f->dst_port);
        }
        f->hdr_state = HDR_DONE;
    }

    rte_pktmbuf_free(m);
}

/* ===================== GRO 프런트 ===================== */

static void handle_one_packet(struct rte_mbuf *m)
{
    struct rte_mbuf *pm = m;

    /* IPv4 조각 재조립 */
    if (is_ipv4_fragmented(m)) {
        struct rte_mbuf *reassembled = ip_reassemble(m);
        if (reassembled == NULL) return;               /* 내부 큐 저장 */
        pm = reassembled;
    }

    /* GRO 재조립 */
    struct rte_mbuf *in[1] = { pm };
    uint16_t outn = rte_gro_reassemble(in, 1, gro_ctx);
    for (uint16_t i = 0; i < outn; i++) process_segment_mbuf(in[i]);

    /* 주기적 GRO flush */
    static uint64_t last_flush = 0;
    uint64_t now = rte_get_tsc_cycles();
    if (now - last_flush > GRO_FLUSH_INTERVAL_CYCLES) {
        struct rte_mbuf *out[32];
        uint16_t nb = rte_gro_timeout_flush(gro_ctx, rte_get_tsc_hz() / 100, RTE_GRO_TCP_IPV4, out, 32);
        for (uint16_t i = 0; i < nb; i++) process_segment_mbuf(out[i]);
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
            finalize_and_send_flow(f, "TIMEOUT"); 
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

    /* mbuf 풀 */
    reassembly_pool = rte_pktmbuf_pool_create("reassembly_pool",
                                              1024, 128, 0,
                                              RTE_MBUF_DEFAULT_BUF_SIZE,
                                              socket_id);
    if (!reassembly_pool) { DOCA_LOG_ERR("mpool create fail"); return -1; }

    /* IPv4 조각 테이블 */
    uint64_t timeout_cycles = (uint64_t)FRAG_TIMEOUT_SEC * rte_get_tsc_hz();
    frag_tbl = rte_ip_frag_table_create(FRAG_BUCKETS, FRAG_MAX_ENTRIES, FRAG_MAX_PER_BUCKET, timeout_cycles, (int)socket_id);
    if (!frag_tbl) {
        DOCA_LOG_ERR("frag table create fail");
        rte_mempool_free(reassembly_pool); reassembly_pool = NULL;
        return -1;
    }
    memset(&death_row, 0, sizeof(death_row));

    /* GRO 컨텍스트 */
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

    /* 플로우 배열 초기화 */
    memset(g_flows, 0, sizeof(g_flows)); g_active_flows = 0;

    return 0;
}

static void cleanup_reassembly_subsystems(void)
{
    /* 플로우 정리 */
    for (int i = 0; i < g_active_flows; i++) {
        free_ooo_queue(&g_flows[i].ooo);
        if (g_flows[i].hdr_buf) { free(g_flows[i].hdr_buf); g_flows[i].hdr_buf = NULL; }
    }
    g_active_flows = 0;

    /* GRO/frag/mpool 파괴 */
    if (gro_ctx) { rte_gro_ctx_destroy(gro_ctx); gro_ctx = NULL; }
    if (frag_tbl) { rte_ip_frag_table_destroy(frag_tbl); frag_tbl = NULL; }
    if (reassembly_pool) { rte_mempool_free(reassembly_pool); reassembly_pool = NULL; }
}

/* ===================== DOCA Flow: IPv4 RSS 파이프(스켈레톤) ===================== */

static doca_error_t create_rss_tcp_ipv4_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
    struct doca_flow_match match; memset(&match, 0, sizeof(match));
    struct doca_flow_actions actions, *actions_arr[NB_ACTIONS_ARR]; memset(&actions,0,sizeof(actions));
    struct doca_flow_fwd fwd, fwd_miss; memset(&fwd,0,sizeof(fwd)); memset(&fwd_miss,0,sizeof(fwd_miss));
    struct doca_flow_pipe_cfg *cfg;
    uint16_t rss_queues[1];
    doca_error_t r;

    /* L3 IPv4 매치 (L4 TCP 메타 주석 처리 → L3 기반 RSS) */
    match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
    match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
    /* match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP; */
    /* match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;     */

    /* src_ip만 마스크 온(0xFFFFFFFF), 나머지 와일드카드 */
    match.outer.ip4.src_ip = 0xffffffff;

    /* actions: pkt_meta(데모) */
    actions.meta.pkt_meta = UINT32_MAX; actions_arr[0] = &actions;

    /* 파이프 CFG 생성 */
    r = doca_flow_pipe_cfg_create(&cfg, port);
    if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("pipe_cfg_create: %s", doca_error_get_descr(r)); return r; }

    /* BASIC + root 파이프 */
    r = set_flow_pipe_cfg(cfg, "RSS_IPv4_SRC_ONLY", DOCA_FLOW_PIPE_BASIC, true);
    if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("set_flow_pipe_cfg: %s", doca_error_get_descr(r)); goto out_cfg; }

    /* 매치/액션 설정 */
    r = doca_flow_pipe_cfg_set_match(cfg, &match, NULL);
    if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("pipe_cfg_set_match: %s", doca_error_get_descr(r)); goto out_cfg; }
    r = doca_flow_pipe_cfg_set_actions(cfg, actions_arr, NULL, NULL, NB_ACTIONS_ARR);
    if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("pipe_cfg_set_actions: %s", doca_error_get_descr(r)); goto out_cfg; }

    /* RSS 설정: queue 0 하나 */
    rss_queues[0] = 0;
    fwd.type = DOCA_FLOW_FWD_RSS; fwd.rss_queues = rss_queues; fwd.rss_inner_flags = DOCA_FLOW_RSS_IPV4; fwd.num_of_queues = 1;
    fwd_miss.type = DOCA_FLOW_FWD_DROP;

    /* 파이프 생성 */
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

    mval.outer.ip4.src_ip = be_src_ip;    /* BE_IPV4_ADDR(a,b,c,d) 사용 */
    act.action_idx = 0;                   /* actions_arr[0] */

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
    for (uint16_t i = 0; i < nb; i++) handle_one_packet(pkts[i]);

    static uint64_t last_cleanup = 0;
    uint64_t now = rte_get_tsc_cycles();
    if (now - last_cleanup > CLEANUP_INTERVAL_CYCLES) { cleanup_idle_flows(); last_cleanup = now; }
}

/* ===================== Entry ===================== */

doca_error_t flow_rss_meta_with_app_buffering(int nb_queues)
{
    const int nb_ports = 1;                        /* 데모: 포트 1개 */
    struct flow_resources res = {0};
    uint32_t nr_shared[SHARED_RESOURCE_NUM_VALUES] = {0};
    struct doca_flow_port *ports[nb_ports];
    struct doca_dev *devs[nb_ports];
    struct doca_flow_pipe *pipe;
    struct entries_status st;
    doca_error_t r;
    int port_id;

    /* 하위 서브시스템 초기화 (frag/GRO/flows) */
    if (init_reassembly_subsystems() != 0) {
        DOCA_LOG_ERR("reassembly subsystems init failed");
        return DOCA_ERROR_INITIALIZATION;
    }

    /* TX 소켓 1회 연결(필수) */
    if (tcp_connect_once(TX_DST_IP, TX_DST_PORT) != 0) {
        DOCA_LOG_ERR("tcp_connect_once failed");
        cleanup_reassembly_subsystems();
        return DOCA_ERROR_INITIALIZATION;
    }

    /* DOCA Flow 초기화 */
    r = init_doca_flow(nb_queues, "vnf,hws", &res, nr_shared);
    if (r != DOCA_SUCCESS) {
        DOCA_LOG_ERR("init_doca_flow: %s", doca_error_get_descr(r));
        tcp_close_if_open();
        cleanup_reassembly_subsystems();
        return r;
    }

    /* 포트 초기화 */
    r = init_doca_flow_ports(nb_ports, ports, true, devs);
    if (r != DOCA_SUCCESS) {
        DOCA_LOG_ERR("init_doca_flow_ports: %s", doca_error_get_descr(r));
        doca_flow_destroy();
        tcp_close_if_open();
        cleanup_reassembly_subsystems();
        return r;
    }

    /* 파이프 생성 + 엔트리 추가 */
    for (port_id = 0; port_id < nb_ports; port_id++) {
        memset(&st, 0, sizeof(st));

        r = create_rss_tcp_ipv4_pipe(ports[port_id], &pipe);
        if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("create_rss_tcp_ipv4_pipe failed"); goto stop_ports; }

        r = add_src_ip_entry_param(pipe, BE_IPV4_ADDR(10,197,0,9), &st);
        if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("add_src_ip_entry_param failed"); goto stop_ports; }

        r = add_src_ip_entry_param(pipe, BE_IPV4_ADDR(10,197,0,11), &st);
        if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("add_src_ip_entry_param failed"); goto stop_ports; }

        /* 엔트리 처리 완료 대기 (최소 2개) */
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

    /* 배너 */
    DOCA_LOG_INFO("=== HTTP Header Aggregator (header-only, no-regex) ===");
    DOCA_LOG_INFO("Stage-0: DOCA Flow RSS (IPv4 src_ip).");
    DOCA_LOG_INFO("Stage-1: Header reassembly until body; multi-headers supported.");
    DOCA_LOG_INFO("TX: send reassembled headers to %s:%u", TX_DST_IP, (unsigned)TX_DST_PORT);
    DOCA_LOG_INFO("======================================================");

    /* 메인 루프 */
    while (!force_quit) {
        rx_loop_once(0);
        usleep(10000); /* 10ms */
    }

stop_ports:
    stop_doca_flow_ports(nb_ports, ports);
    doca_flow_destroy();
    tcp_close_if_open();
    cleanup_reassembly_subsystems();
    return r;
}