/*
 * DPU-native Chunker/Sealer + Metadata Router (single-file skeleton) + Minimal Logging
 *
 * What this implements:
 *  - Stage-0: HW RSS fan-out for TCP/IPv4 (DOCA Flow pipe). (MARK per-flow is TODO; we fallback to SW hash.)
 *  - Stage-1: Per-flow TCP in-order reassembly into fixed 64KB chunks. Each chunk gets metadata + CRC32c "seal".
 *  - Stage-2: Metadata-driven router queues (high/normal/low). TODO hooks for Host DMA / Hairpin / GPU.
 *
 * Notes
 *  - This is a refactor toward a DPU-centric design. It processes *all* TCP/IPv5 uniformly.
 *  - Per-flow MARK learning and DMA/GPUNetIO calls are left as TODOs (ready places provided).
 *  - For brevity, error handling is pragmatic; extend for production.
 *
 * Minimal logging added:
 *  - On IPv4 fragment reassembly success: one INFO line.
 *  - On chunk seal: one INFO line with metadata + one INFO line with truncated hex dump of payload.
 */

 #include <string.h> // memset, memcpy, strnlen, strncpy
 #include <unistd.h> // usleep, close등 POSIX 기본 함수
 #include <netinet/in.h> // sockaddr_in, htons, ntohs 등 네트워크 관련 구조체 및 함수
 #include <stdlib.h> // malloc, free 등 표준 라이브러리 함수
 #include <stdio.h> 
 #include <stdbool.h> // bool 타입 지원
 #include <inttypes.h> // 정수형 포맷팅 지원(고정폭 정수형)
 
 #include <rte_ethdev.h> // DPDK 이더넷 장치 관련 함수
 #include <rte_mbuf.h> // DPDK 메모리 버퍼(mbuf) 관련 함수
 #include <rte_ether.h> // DPDK 이더넷 헤더 관련 함수
 #include <rte_ip.h> // DPDK IP 헤더 관련 함수
 #include <rte_tcp.h> // DPDK TCP 헤더 관련 함수
 #include <rte_ip_frag.h> // DPDK IP 조각화 관련 함수
 #include <rte_gro.h> // DPDK GRO(대량 수신 오프로드) 관련 함수
 #include <rte_regexdev.h> // DPDK 정규식 장치 관련 함수
 
 #include <doca_log.h>
 #include <doca_flow.h> // DOCA Flow 프레임워크 관련 함수
 #include <doca_bitfield.h> // DOCA 비트필드 관련 함수
 
 #include "flow_common.h"  /* Assumed available from DOCA samples (init_doca_flow, set_flow_pipe_cfg, etc.) */
 
 extern volatile bool force_quit;
 
 DOCA_LOG_REGISTER(FLOW_APP_CHUNKER);
 
 /* ===================== Tunables & Limits ===================== */
 
 #define PACKET_BURST                   128 // 한번에 RX 버스트로 가져올 최대 mbuf 수
 
 /* Reassembly resources (optimized down) */
 #define FRAG_BUCKETS                   64 // 조각 테이블 버킷 수
 #define FRAG_MAX_ENTRIES               256 // 최대 엔트리 수 
 #define FRAG_MAX_PER_BUCKET            4 // 버킷당 최대 체인 길이
 #define FRAG_TIMEOUT_SEC               3 // 조각 재조립 타임아웃

 #define GRO_MAX_FLOW                   128 // GRO 흐름 수
 #define GRO_MAX_ITEMS_PER_FLOW         16 // 흐름당 최대 아이템 수
 
 /* Flow table & timeouts */
 #define MAX_TCP_FLOWS                  512 // 최대 TCP 흐름 수
 #define TCP_FLOW_TIMEOUT_SEC           30 // 비활성 흐름 타임아웃
 
 /* Chunking */
 #define CHUNK_SIZE                     (8 * 1024) /* 4KB 청크 크기*/ 
 #define MAX_OOO_SEGMENTS               64          /* 흐름별 out-of-order 큐에 저장할 최대 세그먼트 수 */
 
 /* Misc */
 #define GRO_FLUSH_INTERVAL_CYCLES      (rte_get_tsc_hz() / 100) /* 10ms마다 GRO flush */ 
 #define CLEANUP_INTERVAL_CYCLES        (rte_get_tsc_hz() * 10)  /* 10s마다 흐름 청소*/
 #define HEX_DUMP_BYTES                 64 // 페이로드 헥스 덤프 바이트 수(미리보기)

static int g_tcp_fd = -1; /* for future MARK socket option if needed */ 

enum route_class { ROUTE_LOW = 0, ROUTE_NORMAL = 1, ROUTE_HIGH = 2, };

// TCP 소켓을 생성하고 지정된 IP와 포트에 연결하는 함수
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
 
 /* ===================== CRC32C (Castagnoli) Software ===================== */
 /* Bitwise, reflected, polynomial 0x1EDC6F41 (reflected 0x82F63B78) — compact & portable. */
 
 static inline uint32_t crc32c_sw(const uint8_t *buf, size_t len)
 {
     uint32_t crc = ~0u; // 초기 CRC 값 설정
     for (size_t i = 0; i < len; i++) {
         crc ^= buf[i]; // 입력 바이트와 CRC를 XOR
         for (int b = 0; b < 8; b++) {
             uint32_t mask = -(crc & 1u);
             crc = (crc >> 1) ^ (0x82F63B78u & mask);
         }
     }
     return ~crc; // 최종 반전
 }
 
 // 청크 메타데이터 구조체: 5-tuple, 시퀀스, 크기, 타임스탬프, CRC32c 포함
 // 제거 예정
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
 // 청크 메정데이터를 분류하는 함수(현재는 항상 NORMAL 반환) 
 // 제거 예정
 static inline enum route_class classify_chunk(const struct chunk_metadata *m)
 {
     (void)m;
     /* Example: elevate HTTP-like ports — for now always NORMAL. */
     return ROUTE_NORMAL;
 }
 
 /* Placeholder dispatcher: TODO connect to Host DMA / Hairpin / GPU queues */
 // 청크를 TCP 소켓으로 전송하는 함수(향후 제거 예정)
 static inline void dispatch_chunk(enum route_class cls,
                                   const struct chunk_metadata *m,
                                   const uint8_t *payload)
 {
     (void)cls; (void)m; (void)payload;

    if (m->chunk_index != 0)
            return;   // 첫 번째 chunk만 전송
    if (g_tcp_fd < 0){
        DOCA_LOG_ERR("tcp socket not connected");
        return;
    }

    // send() 로프를 사용하여 전체 청크를 TCP 소켓으로 전송
    size_t to_send = m->payload_len;
    const uint8_t *p = payload;
    while (to_send > 0) {
        ssize_t sent = send(g_tcp_fd, p, to_send, 0);  // TCP 소켓으로 전송
        if (sent <= 0) {
            DOCA_LOG_ERR("send() failed or connection closed");
            close(g_tcp_fd);
            g_tcp_fd = -1;
            return;
        }
        p += sent;
        to_send -= (size_t)sent;
    }
    DOCA_LOG_INFO("Dispatched chunk index=%u, bytes=%u to TCP socket",
                   m->chunk_index, m->payload_len);
 }
 
 /* ===================== Per-flow TCP Chunking/Reassembly ===================== */
 // out-of-odrder(OOO) 세그먼트 구조체
 struct ooo_seg {
     uint32_t seq; // 세그먼스 시작 시퀀스 번호
     uint32_t len; // 세그먼트 길이
     uint8_t *data; // 세그먼트 데이터 포인터 (복사 저장된 페이로드)
     struct ooo_seg *next; // 단순 연결 리스트
 };

 // out-of-order 큐 헤드/카운터 구조체
 struct ooo_queue {
     struct ooo_seg *head; // OOO 세그먼트 리스트 헤드
     int count; // OOO 세그먼트 수
 };
 
 // TCP 흐름 상태 구조체
 struct tcp_chunk_flow {
     /* directional 5-tuple key */
     uint32_t src_ip, dst_ip;
     uint16_t src_port, dst_port;
 
     /* expected sequencing */
     uint32_t expected_seq;
     bool initialized; // 초기 페이로드를 보고 expected_seq 설정했는지 여부
 
     /* current chunk buffer */
     uint8_t  chunk[CHUNK_SIZE];
     uint32_t chunk_len;
     uint32_t chunk_index;
 
     /* recent activity */
     // 최근 활동 타임스탬프(TSC)
     uint64_t last_activity_tsc;
 
     /* out-of-order queue */
     struct ooo_queue ooo;
 };
 
 // 고정 크기 흐름 배열(간단 구현)
 static struct tcp_chunk_flow g_flows[MAX_TCP_FLOWS];
 static int g_active_flows = 0;
 
 /* ===================== IP Reassembly / GRO ===================== */
 static struct rte_ip_frag_tbl *frag_tbl = NULL; // IPv4 조각 재조립 테이블
 static struct rte_ip_frag_death_row death_row; // 만료 조각 수거를 위한 데스 로우
 static void *gro_ctx = NULL; // GRO 컨텍스트 포인터
 static struct rte_mempool *reassembly_pool = NULL; // 재조립용 mbuf 풀
 
 // 유틸리티 
 // TSC를 마이크로초로 변환
 static inline uint64_t tsc_to_us(uint64_t tsc)
 {
     return (tsc * 1000000ull) / rte_get_tsc_hz();
 }
 
 // 바이너리 데이터를 헥스 문자열로 변환하여 출력 버퍼에 저장
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
 
 // 5-tuple로 흐름을 찾는 함수(src/dst IP, src/dst 포트가 동일해야 같은 플로우로 판단)
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
 
 // ooo_queue의 모든 세그먼트를 해제하는 함수
 // 왜 해제하는가? 흐름이 종료되거나 타임아웃될 때 메모리 누수를 방지하기 위해
 static void free_ooo_queue(struct ooo_queue *q)
 {
     struct ooo_seg *cur = q->head;
     while (cur) {
         struct ooo_seg *n = cur->next;
         free(cur->data);
         free(cur);
         cur = n; // 다음 세그먼트로 이동
     }
     q->head = NULL;
     q->count = 0;
 }
 
 // 인덱스로 흐름을 삭제하는 함수
 // 왜 삭제하는가? 흐름 테이블이 가득 찼을 때 오래된 흐름을 제거하여 새로운 흐름을 수용하기 위해
 // 끝 요소 당겨오기 방식으로 삭제
 static void drop_flow_at_index(int idx)
 {
     if (idx < 0 || idx >= g_active_flows) return;
     free_ooo_queue(&g_flows[idx].ooo);
     if (idx != g_active_flows - 1)
         g_flows[idx] = g_flows[g_active_flows - 1];
     g_active_flows--;
 }
 
 // 현재 chunk가 차 있으면 봉인(seal)하고 전송하는 함수
 static void seal_and_dispatch_chunk(struct tcp_chunk_flow *f)
 {
     if (f->chunk_len == 0) return;
 
     struct chunk_metadata meta = {0};
     meta.src_ip = f->src_ip;
     meta.dst_ip = f->dst_ip;
     meta.src_port = f->src_port;
     meta.dst_port = f->dst_port;
     meta.seq_end = f->expected_seq;    // 현재 예상 시퀀스 번호가 청크의 끝 시퀀스
     meta.seq_start = meta.seq_end - f->chunk_len; // 청크의 시작 시퀀스
     meta.chunk_index = f->chunk_index;
     meta.payload_len = f->chunk_len;
     meta.ts_us = tsc_to_us(rte_get_tsc_cycles());
     meta.crc32c = crc32c_sw(f->chunk, f->chunk_len);
 
     /* Minimal logging: two lines */
     DOCA_LOG_INFO("SEALED chunk | flow: %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u id=%u bytes=%u seq=[%u..%u) crc32c=0x%08x timestamp=%" PRIu64,
                   (meta.src_ip >> 24) & 0xFF, (meta.src_ip >> 16) & 0xFF, (meta.src_ip >> 8) & 0xFF, meta.src_ip & 0xFF, meta.src_port,
                   (meta.dst_ip >> 24) & 0xFF, (meta.dst_ip >> 16) & 0xFF, (meta.dst_ip >> 8) & 0xFF, meta.dst_ip & 0xFF, meta.dst_port,
                   meta.chunk_index, meta.payload_len, meta.seq_start, meta.seq_end, meta.crc32c, meta.ts_us);
    
     // 청크 페이로드의 헥스 미리보기(최대 HEX_DUMP_BYTES 바이트)
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
 
 // 현재 청크 버퍼에 len 바이트를 최대 chunk_size 까지 추가하는 함수, 꽉 차면 봉인 후 전송
  /*
    계획 (HTTP 헤더 기준):

    흐름별로 임시 버퍼 유지:
    각 TCP 흐름마다 HTTP 헤더를 임시로 저장할 버퍼를 둡니다.

    새 데이터가 오면 버퍼에 추가:
    패킷에서 받은 데이터를 해당 흐름의 버퍼에 이어붙입니다.

    버퍼에서 HTTP 헤더 끝("\r\n\r\n")이 있는지 검사:
    버퍼에 HTTP 헤더의 끝(구분자)을 찾습니다.

    발견하면 해당 부분까지 처리:
    헤더 끝까지의 데이터를 봉인/전송/파싱 등 원하는 방식으로 처리합니다.

    남은 데이터는 버퍼에 남김:
    헤더 이후의 데이터(바디 등)는 버퍼에 남겨둡니다.
 */
 static inline void append_bytes_to_chunk(struct tcp_chunk_flow *f, const uint8_t *data, uint32_t len)
 {
     uint32_t off = 0; // data에서 복사할 오프셋
     while (off < len) { // 아직 복사할 데이터가 남아있으면 반복
         uint32_t space = CHUNK_SIZE - f->chunk_len; // 청크 버퍼에 남은 공간
         uint32_t take = (len - off < space) ? (len - off) : space; // 복사할 바이트 수
         memcpy(f->chunk + f->chunk_len, data + off, take); // 청크 버퍼에 데이터 복사
         f->chunk_len += take; // 청크 길이 갱신
         off += take; // data 오프셋 갱신
         if (f->chunk_len == CHUNK_SIZE) // 청크가 꽉 찼으면
             seal_and_dispatch_chunk(f); // 봉인하고 전송
     }
 }
 
 /* Insert OOO seg sorted by seq; cap length by available memory. */
 // ooo_queue에 seq 기준으로 정렬하여 세그먼트를 삽입하는 함수
 // 왜 이렇게 하는가? out-of-order 세그먼트를 순서대로 처리하기 위해
 // 궁극적으로는 예외처리를 위한 코드
 static void queue_ooo_segment(struct tcp_chunk_flow *f, uint32_t seq, const uint8_t *data, uint32_t len)
 {
     if (len == 0) return;
     if (f->ooo.count >= MAX_OOO_SEGMENTS) { // 큐가 가득 찼으면
         /* Drop the tail-most (largest seq) by simple policy: insert only if "smallest" seq. */
         /* Simple behavior: drop incoming if full. */
         return; // 버림
     }
     struct ooo_seg *seg = (struct ooo_seg *)malloc(sizeof(*seg)); // 새 세그먼트 할당
     if (!seg) return;
     seg->data = (uint8_t *)malloc(len);
     if (!seg->data) { free(seg); return; }
     memcpy(seg->data, data, len); // 데이터 복사
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
 // ooo_queue에서 다음 연속 세그먼트가 있으면 처리하는 함수(부분 겹침도 처리)
 // 왜 필요한가? out-of-order 세그먼트를 순서대로 청크에 추가하기 위해
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
 
 // 기존 플로우를 찾거나, 부족하면 오래된 플로우 삭제 후 새 플로우를 생성하는 함수
 static struct tcp_chunk_flow* get_or_create_flow(uint32_t s_ip, uint32_t d_ip, uint16_t s_port, uint16_t d_port)
 {
     struct tcp_chunk_flow *f = find_flow(s_ip, d_ip, s_port, d_port);
     if (f) return f;
 
     /* Cleanup if full, then retry */
     // 플로우 테이블이 가득 찼으면 오래된 비활성 플로우를 삭제
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

     // 새 플로우 생성
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
 
 // 비활성 흐름을 청소하는 함수(타임아웃된 흐름 삭제)
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
 /*
    TCP 흐름별 청크 재조립 과정:

    1. 패킷 수신 및 파싱
    NIC에서 패킷을 받아 mbuf에 저장
    extract_tcp_segment()로 5-tuple, 시퀀스 번호, 페이로드 추출

    2. 순서 확인 및 처리
    각 흐름별로 expected_seq 관리
    시퀀스 번호가 맞으면 바로 청크 버퍼에 추가 (append_bytes_to_chunk())

    3. Out-of-Order(OOO) 처리
    시퀀스 번호가 예상과 다르면 queue_ooo_segment()로 OOO 큐에 저장
    OOO 큐는 시퀀스 번호 오름차순으로 정렬

    4. 재조립(drain)
    앞부분이 도착하면 drain_ooo_segments()가 OOO 큐를 순회하며
    expected_seq와 맞는 세그먼트를 찾아 청크에 추가
    부분 겹침도 처리
    처리된 세그먼트는 큐에서 제거, 메모리 해제
 */
 
 // mbuf가 IPv4 조각화된 패킷인지 확인하는 함수
 static bool is_ipv4_fragmented(struct rte_mbuf *m)
 {
     struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *); // 이더넷 헤더 포인터
     if (rte_be_to_cpu_16(eth->ether_type) != RTE_ETHER_TYPE_IPV4) // IPv4 아니면 false
         return false;
     struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1); // IPv4 헤더 포인터
     uint16_t frag_off = rte_be_to_cpu_16(ip->fragment_offset); // 조각 오프셋
     return (frag_off & RTE_IPV4_HDR_MF_FLAG) || (frag_off & RTE_IPV4_HDR_OFFSET_MASK); // 조각화 여부 반환
 }

 // IPv4 조각화된 패킷을 재조립하는 함수 
 static struct rte_mbuf* ip_reassemble(struct rte_mbuf *m)
 {
     uint64_t ts = rte_rdtsc(); // 현재 TSC 읽기
     struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *); // 이더넷 헤더 포인터
     struct rte_ipv4_hdr *ip4 = (struct rte_ipv4_hdr *)(eth + 1); // IPv4 헤더 포인터
 
     struct rte_mbuf *out = rte_ipv4_frag_reassemble_packet( // 재조립 시도
         frag_tbl, &death_row, m, ts, ip4);
 
     if (out) { // 재조립 성공
         DOCA_LOG_INFO("IPv4 fragments reassembled: %u bytes", rte_pktmbuf_pkt_len(out));
     }
     return out;
 }
 
 // GRO 타임아웃 플러시 및 처리 함수 선언
 // GRO? 대량 수신 오프로드(Large Receive Offload)로, 여러 작은 패킷을 하나의 큰 패킷으로 합치는 기술
 static void gro_timeout_flush_and_process(void);
 
 /* ===================== TCP Segment → Flow Feeding ===================== */
 
 // mbuf에서 5-tuple, 시퀀스, 페이로드 추출하는 함수
 static int extract_tcp_segment(struct rte_mbuf *m,
                                uint32_t *s_ip, uint32_t *d_ip,
                                uint16_t *s_port, uint16_t *d_port,
                                uint32_t *seq,
                                const uint8_t **payload, uint32_t *plen,
                                uint8_t *flags)
 {
     struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *); // 이더넷 헤더 포인터
     if (rte_be_to_cpu_16(eth->ether_type) != RTE_ETHER_TYPE_IPV4) // IPv4 아니면 -1 반환
         return -1;
 
     struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1); // IPv4 헤더 포인터
     if (ip->next_proto_id != IPPROTO_TCP) // TCP 아니면 -1 반환
         return -1;
 
     uint16_t ip_hdr_len = (ip->version_ihl & 0x0F) * 4; // IPv4 헤더 길이
     struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)((uint8_t *)ip + ip_hdr_len); // TCP 헤더 포인터
     uint16_t tcp_hdr_len = ((tcp->data_off >> 4) & 0xF) * 4; // TCP 헤더 길이
 
     // 5-tuple, 시퀀스, 페이로드 설정
     *s_ip = rte_be_to_cpu_32(ip->src_addr); 
     *d_ip = rte_be_to_cpu_32(ip->dst_addr); 
     *s_port = rte_be_to_cpu_16(tcp->src_port);
     *d_port = rte_be_to_cpu_16(tcp->dst_port);
     *seq = rte_be_to_cpu_32(tcp->sent_seq);
     *flags = tcp->tcp_flags;
 
     // 페이로드 위치 및 길이 설정
     uint16_t ip_len = rte_be_to_cpu_16(ip->total_length); // 전체 IP 패킷 길이 
     int hdr_total = ip_hdr_len + tcp_hdr_len; // IP + TCP 헤더 길이
     *plen = (hdr_total <= ip_len) ? (ip_len - hdr_total) : 0; // 페이로드 길이
     *payload = (const uint8_t *)tcp + tcp_hdr_len; // 페이로드 포인터
     return 0;
 }
 
 // mbuf를 처리하여 TCP 세그먼트를 추출하고 해당 흐름에 추가하는 함수
 static void process_segment_mbuf(struct rte_mbuf *m)
 {
     uint32_t s_ip, d_ip, seq;
     uint16_t s_port, d_port;
     const uint8_t *payload; uint32_t plen;
     uint8_t flags;
    
    // TCP 세그먼트 추출
     if (extract_tcp_segment(m, &s_ip, &d_ip, &s_port, &d_port,
                             &seq, &payload, &plen, &flags) < 0) {
         rte_pktmbuf_free(m);
         return;
     }
 
     /* Ignore pure ACKs (no payload), but FIN/RST should still flush. */
     // 순수 ACK(페이로드 없음)는 무시, FIN/RST는 청크 봉인 위해 처리
     if (plen == 0 && !(flags & (RTE_TCP_FIN_FLAG | RTE_TCP_RST_FLAG))) {
         rte_pktmbuf_free(m);
         return;
     }
    
    // 흐름 찾기 또는 생성
     struct tcp_chunk_flow *f = get_or_create_flow(s_ip, d_ip, s_port, d_port);
     if (!f) {
         rte_pktmbuf_free(m);
         return;
     }
     f->last_activity_tsc = rte_get_tsc_cycles(); // 최근 활동 타임스탬프 갱신
 
     /* Initialize expected_seq on first payload */
     // 첫 페이로드에서 expected_seq 초기화
     if (!f->initialized && plen > 0) {
         f->expected_seq = seq; // 첫 시퀀스를 예상 시퀀스로 설정
         f->initialized = true; // 초기화 완료 표시
     }
 
     /* In-order / overlap handling */
     if (plen > 0) { // 페이로드가 있으면
         if (seq == f->expected_seq) { // 순서대로 도착한 경우
             append_bytes_to_chunk(f, payload, plen);
             f->expected_seq += plen;
             drain_ooo_segments(f);
         } else if (seq > f->expected_seq) { // 순서대로 도착하지 않은 경우
             /* Out-of-order */
             queue_ooo_segment(f, seq, payload, plen);
         } else { // 순서대로 도착했거나 겹치는 경우
             /* seq < expected: overlap or duplicate */
             uint32_t exp = f->expected_seq; // 예상 시퀀스
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
     // FIN/RST 플래그가 있으면 현재 청크 봉인 및 전송
     // 왜? 연결 종료 신호이므로 남은 데이터를 처리하기 위해
     if (flags & (RTE_TCP_FIN_FLAG | RTE_TCP_RST_FLAG)) {
         if (f->chunk_len > 0)
             seal_and_dispatch_chunk(f);
     }
 
     rte_pktmbuf_free(m); // mbuf 해제, 왜 해제하는가? 메모리 누수 방지를 위해, 이미 필요한 데이터는 복사했으므로
 }
 
 /* ===================== GRO Frontend ===================== */
 // GRO 타임아웃 플러시 및 처리 함수 정의
 // 왜 필요한가? 일정 시간 동안 도착하지 않은 세그먼트를 강제로 처리하여 지연을 줄이기 위해 
 // gro 타임아웃으로 배출된 mbuf를 process_segment_mbuf()로 처리
 static void gro_timeout_flush_and_process(void)
 {
     struct rte_mbuf *out[32]; // 출력 mbuf 배열
     uint16_t nb = rte_gro_timeout_flush(gro_ctx,
                                         rte_get_tsc_hz() / 100, /* 10ms */
                                         RTE_GRO_TCP_IPV4,
                                         out, 32);
     for (uint16_t i = 0; i < nb; i++)
         process_segment_mbuf(out[i]);
 }
 
 // 단일 패킷 처리 함수 정의
 // 왜 필요한가? 각 패킷을 개별적으로 처리하여 IP 조각화 및 GRO를 적용하기 위해
 // (1) IPv4 조각화된 패킷이면 재조립 시도 -> (2) GRO 재조립 -> (3) 각 세그먼트를 흐름에 추가
 static void handle_one_packet(struct rte_mbuf *m)
 {
     struct rte_mbuf *pm = m;
    
    // IPv4 조각화된 패킷이면 재조립 시도(완료 전에는 내부 큐에서 잡아두고 null 반환)
     if (is_ipv4_fragmented(m)) {
         struct rte_mbuf *reassembled = ip_reassemble(m);
         if (reassembled == NULL) {
             /* Reassembly in progress; mbuf is queued internally. */
             // 재조립 진행 중, mbuf는 내부 큐에 보관됨
             return;
         }
         pm = reassembled; // 재조립된 패킷으로 교체
     }
 
     struct rte_mbuf *in[1] = { pm }; // GRO 입력 배열
     uint16_t outn = rte_gro_reassemble(in, 1, gro_ctx); // GRO 재조립
     for (uint16_t i = 0; i < outn; i++)
         process_segment_mbuf(in[i]);
 
     /* Periodic GRO flush */
     // 일정 주기로 GRO 타임아웃 플러시
     static uint64_t last_flush = 0;
     uint64_t now = rte_get_tsc_cycles();
     if (now - last_flush > GRO_FLUSH_INTERVAL_CYCLES) {
         gro_timeout_flush_and_process();
         last_flush = now;
     }
 }
 
 /* ===================== Init / Cleanup ===================== */
 
 // 재조립 서브시스템 초기화 함수 정의
 static int init_reassembly_subsystems(void)
 {
     uint32_t socket_id = rte_socket_id(); // 현재 소켓 ID
 
     /* mbuf pool (for fragment reassembly paths if needed by library) */
     // 재조립 경로에서 사용할 mbuf 풀 생성
     // 왜 필요한가? 조각화된 패킷을 재조립할 때 mbuf가 필요하기 때문에
     // 크게 할당 하여 메모리 부족 방지 (추가로 매모리 계속 할당 받는 것보다 미리 할당된 풀에서 가져오는 것이 성능에 유리)
     reassembly_pool = rte_pktmbuf_pool_create("reassembly_pool",
                                               1024, 128, 0,
                                               RTE_MBUF_DEFAULT_BUF_SIZE,
                                               socket_id);
     if (!reassembly_pool) {
         DOCA_LOG_ERR("Failed to create reassembly_pool");
         return -1;
     }
 
     /* IP fragment table */
     // IPv4 조각화 테이블 생성
     // 안에 뭐가 있나? 조각화된 패킷을 임시로 저장하는 해시 테이블
     // 어떻게 생긴 해시테이블? 버킷 수, 최대 엔트리 수, 버킷당 최대 엔트리 수, 타임아웃 주기 등으로 구성
     // 버킷의 의미? 해시 충돌을 줄이기 위해 여러 개의 버킷으로 나누어 저장
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
     // GRO 컨텍스트 생성
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
     // 플로우 테이블 초기화
     memset(g_flows, 0, sizeof(g_flows));
     g_active_flows = 0;
 
     return 0;
 }
 
 // 재조립 서브시스템 정리 함수 정의
 static void cleanup_reassembly_subsystems(void)
 {
     /* flush partial chunks before teardown */
     // 모든 활성 플로우에 대해 남은 청크 봉인 및 전송, OOO 큐 해제
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
 
// RSS TCP/IPv4 파이프 생성 함수 정의
/*
 * create_rss_tcp_ipv4_pipe():
 *  - 매치 기준: L3 = IPv4, 그리고 src_ip(마스크 0xFFFFFFFF)
 *  - 액션: pkt_meta 쓰기(여기선 전체 비트 허용), 실제 액션 인덱스 0
 *  - FWD: hit 시 RSS (여기선 queue 0 하나만), miss 시 DROP
 *  - 주의: L4 TCP로 제한하는 필드를 "주석으로 제거"하여 L3 기반 RSS만 사용(코멘트 참고)
 */
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

    /* L3만 사용: IPv4 + src_ip만 매치 */
    match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
    match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;

    /* 아래 두 줄을 반드시 제거(protocol-only 유발) */
    /* match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP; */
    /* match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;     */

    /* src_ip만 마스크 켬(0xFFFFFFFF). 나머지는 0(와일드카드) */
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
 
 // 소스 IP 기반 엔트리 추가 함수 정의
 // doca_flow 구조: 매치, 액션, 포워딩
 // 내가 정의한 entry 함수는 매치: src_ip, 액션: actions_arr[0], 포워딩: 없음(파이프 레벨에서 처리)
 // create함수에서 매치 조건에 src_ip 마스크 0xFFFFFFFF로 설정했으므로
 // 여기서는 src_ip만 설정하면 됨(나머지는 와일드카드)
 // 역할(doca_flow 구조에 매칭하여 비교): create 함수 = 파이프 생성, add 함수 = 엔트리(규칙?) 추가
 static doca_error_t add_src_ip_entry_param(struct doca_flow_pipe *pipe, uint32_t be_src_ip, struct entries_status *st)
 {
     struct doca_flow_match mval = {0};
     struct doca_flow_actions act = {0};
     struct doca_flow_pipe_entry *entry;
     doca_error_t r;

     mval.outer.ip4.src_ip = be_src_ip; // BE_IPV4_ADDR(a,b,c,d) 사용
     act.action_idx = 0; // actions_arr[0] in pipe creatio용
  
     r = doca_flow_pipe_add_entry(0, pipe, &mval, &act, NULL, NULL, 0, st, &entry);
     if (r != DOCA_SUCCESS) {
         DOCA_LOG_ERR("pipe_add_entry failed: %s", doca_error_get_descr(r));
         return r;
     }
     return DOCA_SUCCESS;
 }


 
 /* ===================== Main RX Loop ===================== */
 // 단일 RX 루프 반복 함수 정의
 // dpu 기준 rx가 아니라 파이프 기준 rx
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
 // 메인 함수 정의
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

         r = add_src_ip_entry_param(pipe, BE_IPV4_ADDR(10,197,0,9), &st); 
         if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("add_src_ip_entry_param failed"); goto stop_ports; }

         r = add_src_ip_entry_param(pipe, BE_IPV4_ADDR(10,197,0,11), &st);
         if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("add_src_ip_entry_param failed"); goto stop_ports; }

         int processed_total = 0;
         for (int tries = 0; tries < 8 && processed_total < 2; tries++) {
             r = doca_flow_entries_process(ports[port_id], 0, DEFAULT_TIMEOUT_US, 1);
             if (r != DOCA_SUCCESS) {
                DOCA_LOG_ERR("entry_process: %s", doca_error_get_descr(r));
                goto stop_ports;
             }
             processed_total += st.nb_processed;
             if (st.failure) {
                DOCA_LOG_ERR("entries_process failure flag set");
                goto stop_ports;
             }
             memset(&st, 0, sizeof(st));   
         }
         if (processed_total < 2) {
             DOCA_LOG_ERR("entry_process: insufficient processing (%d < 2)", processed_total);
             r = DOCA_ERROR_UNKNOWN;
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
 