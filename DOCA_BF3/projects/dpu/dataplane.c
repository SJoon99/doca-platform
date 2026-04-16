/*
 * Copyright (c) 2022 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
 *
 * HTTP TCP Reassembly and Header Analysis with DOCA Flow
 * - HTTP(80): IP Reassembly -> GRO -> TCP Flow Tracking -> HTTP Header Parsing  
 * - HTTPS(443) & SSH(22): Legacy port-based buffering with detailed logging
 */

 #include <string.h>
 #include <unistd.h>
 #include <netinet/in.h>
 #include <stdlib.h>
 
 #include <rte_ethdev.h>
 #include <rte_tcp.h>
 #include <rte_ip_frag.h>
 #include <rte_gro.h>
 
 #include <doca_log.h>
 #include <doca_flow.h>
 #include <doca_bitfield.h>
 
 #include "flow_common.h"
 
 extern volatile bool force_quit;
 
 DOCA_LOG_REGISTER(FLOW_APP_BUFFER);
 
 #define PACKET_BURST 128
 #define BUFFER_SIZE 30
 #define BUFFER_TIMEOUT_US 60000000
 #define MAX_PACKET_TYPES 3
 
 /* TCP Flow 관리 - 메모리 최적화 */
 #define MAX_TCP_FLOWS 512               /* 2048 → 512로 감소 */
 #define TCP_FLOW_TIMEOUT_SEC 30
 #define HTTP_ACCUMULATE_BUFFER_SIZE 8192    /* 16384 → 8192로 감소 */
 #define MAX_OUT_OF_ORDER_SEGMENTS 16       /* 32 → 16으로 감소 */
 
 // 기존 패킷 타입 정의 (443, 22포트용)
 enum packet_type {
     PACKET_TYPE_HTTP = 80,
     PACKET_TYPE_HTTPS = 443,
     PACKET_TYPE_SSH = 22,
     PACKET_TYPE_UNKNOWN = 0
 };
 
 // 기존 패킷 버퍼 구조체 (443, 22포트용) - 복원
 struct packet_buffer {
     struct rte_mbuf *packets[BUFFER_SIZE];
     uint32_t count;
     uint64_t last_flush_time;
     enum packet_type type;
     bool is_active;
     uint64_t total_packets_processed;
     uint64_t total_bytes_processed;
 };
 
 /* TCP 세그먼트 정보 */
 struct tcp_info {
     uint32_t src_ip, dst_ip;
     uint16_t src_port, dst_port;
     uint32_t seq_num;
     uint16_t payload_len;
     char *payload;
     uint8_t flags;
 };
 
 /* Out-of-order TCP 세그먼트 */
 struct tcp_segment {
     uint32_t seq_num;
     uint16_t payload_len;
     char payload[1460];  /* MSS 크기 */
     struct tcp_segment *next;
 };
 
 struct tcp_segment_queue {
     struct tcp_segment *head;
     int count;
 };
 
 /* HTTP TCP Flow 구조체 */
 struct tcp_http_flow {
     /* Flow 식별자 */
     uint32_t src_ip;
     uint32_t dst_ip;
     uint16_t src_port;
     uint16_t dst_port;
     
     /* TCP 상태 */
     uint32_t start_seq;
     uint32_t expected_seq;
     bool flow_established;
     
     /* 누적 버퍼 */
     char accumulate_buffer[HTTP_ACCUMULATE_BUFFER_SIZE];
     uint16_t buffer_len;
     
     /* HTTP 상태 */
     bool http_header_complete;
     uint16_t http_header_len;
     
     /* 타임스탬프 */
     uint64_t last_activity;
     
     /* Out-of-order 세그먼트 임시 저장 */
     struct tcp_segment_queue out_of_order_queue;
 };
 
 /* 전역 변수 */
 static struct packet_buffer packet_buffers[MAX_PACKET_TYPES];  // 기존 버퍼 (443, 22) - 복원
 static struct tcp_http_flow tcp_flows[MAX_TCP_FLOWS];          // HTTP TCP Flow 테이블
 static int active_flow_count = 0;
 
 /* IP 재조립 및 GRO */
 static struct rte_ip_frag_tbl *frag_tbl = NULL;
 static struct rte_ip_frag_death_row death_row;
 static void *gro_ctx = NULL;
 static struct rte_mempool *reassembly_pool = NULL;
 
 /* 함수 선언 */
 static int get_buffer_index(enum packet_type type);
 static void init_packet_buffers(void);  // 복원
 static void flush_buffer(struct packet_buffer *buffer);
 static void check_buffer_timeouts(void);  // 복원
 static void print_buffer_stats(void);
 static enum packet_type extract_packet_type(struct rte_mbuf *packet);
 static void add_packet_to_buffer(struct rte_mbuf *packet, enum packet_type type);
 static void process_packets_with_buffering(int ingress_port);
 
 /* HTTP TCP 재조립 함수들 */
 static int init_http_reassembly_with_gro(void);
 static void cleanup_http_reassembly_with_gro(void);
 static void process_http_packet_for_reassembly(struct rte_mbuf *packet);
 static bool is_ip_fragmented(struct rte_mbuf *packet);
 static struct rte_mbuf* reassemble_ip_packet(struct rte_mbuf *packet);
 static void flush_gro_table(void);  // 복원
 static void process_tcp_segment_for_http(struct rte_mbuf *packet);
 static int extract_tcp_info(struct rte_mbuf *packet, struct tcp_info *info);
 static struct tcp_http_flow* find_or_create_flow(struct tcp_info *tcp_info);
 static int process_tcp_segment(struct tcp_http_flow *flow, struct tcp_info *tcp_info);
 static int append_to_flow_buffer(struct tcp_http_flow *flow, char *data, uint16_t len);
 static void process_queued_segments(struct tcp_http_flow *flow);
 static void add_to_out_of_order_queue(struct tcp_http_flow *flow, struct tcp_info *tcp_info);
 
 /* 새로운 함수들 */
 static bool should_start_processing(struct tcp_http_flow *flow);
 static void start_ordered_processing(struct tcp_http_flow *flow);
 static uint32_t find_minimum_sequence(struct tcp_http_flow *flow);
 
 /* 정렬 처리를 시작할 조건 확인 */
 static bool should_start_processing(struct tcp_http_flow *flow)
 {
     /* 조건 1: 큐에 2개 이상의 세그먼트가 있음 */
     if (flow->out_of_order_queue.count >= 2) {
         DOCA_LOG_INFO("Queue has %d segments, starting processing", flow->out_of_order_queue.count);
         return true;
     }
     
     /* 조건 2: 첫 세그먼트가 들어온 후 100ms 이상 지남 */
     if (flow->out_of_order_queue.count >= 1) {
         uint64_t current_time = rte_get_tsc_cycles();
         uint64_t timeout_cycles = rte_get_tsc_hz() / 10; /* 100ms */
         
         if (current_time - flow->last_activity > timeout_cycles) {
             DOCA_LOG_INFO("Timeout reached with %d segments, starting processing", 
                          flow->out_of_order_queue.count);
             return true;
         }
     }
     
     return false;
 }
 
 /* 큐의 모든 세그먼트를 정렬하여 순서대로 처리 시작 */
 static void start_ordered_processing(struct tcp_http_flow *flow)
 {
     if (flow->out_of_order_queue.count == 0) {
         DOCA_LOG_WARN("No segments in queue to process");
         return;
     }
     
     /* 큐에서 최소 시퀀스 번호 찾기 */
     uint32_t min_seq = find_minimum_sequence(flow);
     if (min_seq == 0) {
         DOCA_LOG_WARN("Could not find minimum sequence number");
         return;
     }
     
     DOCA_LOG_INFO("Starting ordered processing from seq=%u (queue has %d segments)", 
                  min_seq, flow->out_of_order_queue.count);
     
     /* Flow 설정 */
     flow->expected_seq = min_seq;
     flow->flow_established = true;
     
     /* 큐에서 순서대로 처리 */
     process_queued_segments(flow);
     
     DOCA_LOG_INFO("Ordered processing completed, buffer length: %d bytes, queue remaining: %d", 
                  flow->buffer_len, flow->out_of_order_queue.count);
 }
 
 /* 새로운 함수: out-of-order 큐에서 최소 시퀀스 번호 찾기 */
 static uint32_t find_minimum_sequence(struct tcp_http_flow *flow)
 {
     struct tcp_segment *current = flow->out_of_order_queue.head;
     uint32_t min_seq = 0;
     
     while (current != NULL) {
         if (min_seq == 0 || current->seq_num < min_seq) {
             min_seq = current->seq_num;
         }
         current = current->next;
     }
     
     DOCA_LOG_DBG("Found minimum sequence in queue: %u (queue size: %d)", 
                  min_seq, flow->out_of_order_queue.count);
     return min_seq;
 }
 
 static void clear_out_of_order_queue(struct tcp_segment_queue *queue);
 static void check_http_header_complete(struct tcp_http_flow *flow);
 static void parse_complete_http_message(struct tcp_http_flow *flow);
 static void cleanup_old_flows(void);
 
 /* HTTP 헤더 파싱 함수들 */
 static void parse_http_request_line(char *header, uint16_t len);
 static void extract_url_from_request(char *line, int len);
 static void extract_status_from_response(char *line, int len);
 static void parse_http_header_fields(char *header, uint16_t len);
 static void print_http_body_sample(char *body, uint16_t body_len);
 static void reset_flow_for_next_message(struct tcp_http_flow *flow);
 static void flush_all_queued_segments(struct tcp_http_flow *flow);
 
 /* ===================== 기존 버퍼 관리 함수들 (443, 22포트용) - 복원 ===================== */
 
 static int get_buffer_index(enum packet_type type)
 {
     switch (type) {
         case PACKET_TYPE_HTTPS: return 0;
         case PACKET_TYPE_SSH:   return 1;
         default:                return -1;
     }
 }
 
 static void init_packet_buffers(void)  // 복원
 {
     int i;
     enum packet_type types[] = {PACKET_TYPE_HTTPS, PACKET_TYPE_SSH};
     
     for (i = 0; i < 2; i++) {  // HTTP는 제외하고 HTTPS, SSH만
         memset(&packet_buffers[i], 0, sizeof(struct packet_buffer));
         packet_buffers[i].type = types[i];
         packet_buffers[i].is_active = true;
         packet_buffers[i].last_flush_time = rte_get_tsc_cycles();
     }
     
     DOCA_LOG_INFO("Initialized %d legacy packet buffers (HTTPS, SSH)", 2);
 }
 
 static void check_buffer_timeouts(void)  // 복원
 {
     uint64_t current_time = rte_get_tsc_cycles();
     uint64_t timeout_cycles = BUFFER_TIMEOUT_US * rte_get_tsc_hz() / 1000000;
     
     for (int i = 0; i < 2; i++) {  // HTTPS, SSH만 체크
         struct packet_buffer *buffer = &packet_buffers[i];
         
         if (buffer->count > 0) {
             uint64_t elapsed = current_time - buffer->last_flush_time;
             
             if (elapsed > timeout_cycles) {
                 DOCA_LOG_INFO("Buffer timeout for type %d, flushing %d packets", 
                               buffer->type, buffer->count);
                 flush_buffer(buffer);
             }
         }
     }
 }
 
 static void print_buffer_stats(void)
 {
     static uint64_t last_print_time = 0;
     uint64_t current_time = rte_get_tsc_cycles();
     
     if (current_time - last_print_time > rte_get_tsc_hz() * 30) {
         DOCA_LOG_INFO("=== Legacy Buffer Status (HTTPS/SSH) ===");  // 복원
         for (int i = 0; i < 2; i++) {
             struct packet_buffer *buffer = &packet_buffers[i];
             if (buffer->is_active) {
                 DOCA_LOG_INFO("Buffer[%d] Type:%d Current:%d/%d Total_Processed:%lu", 
                               i, buffer->type, buffer->count, BUFFER_SIZE, 
                               buffer->total_packets_processed);
             }
         }
         DOCA_LOG_INFO("=========================================");
         
         /* TCP Flow 상태도 출력 - 더 상세하게 */
         DOCA_LOG_INFO("=== HTTP TCP Flow Status ===");
         DOCA_LOG_INFO("Active Flows: %d/%d", active_flow_count, MAX_TCP_FLOWS);
         int complete_flows = 0;
         int established_flows = 0;
         int queued_segments = 0;
         
         for (int i = 0; i < active_flow_count; i++) {
             if (tcp_flows[i].http_header_complete) complete_flows++;
             if (tcp_flows[i].flow_established) established_flows++;
             queued_segments += tcp_flows[i].out_of_order_queue.count;
         }
         
         DOCA_LOG_INFO("Flows with complete headers: %d", complete_flows);
         DOCA_LOG_INFO("Established flows: %d", established_flows);
         DOCA_LOG_INFO("Total queued segments: %d", queued_segments);
         DOCA_LOG_INFO("============================\n");
         
         last_print_time = current_time;
     }
 }
 
 static enum packet_type extract_packet_type(struct rte_mbuf *packet)
 {
     if (!rte_flow_dynf_metadata_avail()) {
         DOCA_LOG_WARN("Metadata not available for packet");
         return PACKET_TYPE_UNKNOWN;
     }
         
     uint32_t meta = *RTE_FLOW_DYNF_METADATA(packet);
     
     DOCA_LOG_DBG("Extracted packet metadata: %u, packet length: %d", 
                  meta, rte_pktmbuf_pkt_len(packet));
     
     switch (meta) {
         case 80:  
             DOCA_LOG_DBG("Packet classified as HTTP (port 80)");
             return PACKET_TYPE_HTTP;
         case 443: 
             DOCA_LOG_DBG("Packet classified as HTTPS (port 443)");
             return PACKET_TYPE_HTTPS;
         case 22:  
             DOCA_LOG_DBG("Packet classified as SSH (port 22)");
             return PACKET_TYPE_SSH;
         default:  
             DOCA_LOG_WARN("Unknown packet metadata: %u", meta);
             return PACKET_TYPE_UNKNOWN;
     }
 }
 
 static void flush_buffer(struct packet_buffer *buffer)
 {
     uint64_t total_bytes = 0;
     
     if (buffer->count == 0)
         return;
         
     DOCA_LOG_INFO("=== Flushing Legacy Buffer Type %d ===", buffer->type);
     DOCA_LOG_INFO("Buffer contains %d packets", buffer->count);
     
     for (uint32_t i = 0; i < buffer->count; i++) {
         struct rte_mbuf *pkt = buffer->packets[i];
         uint16_t pkt_len = rte_pktmbuf_data_len(pkt);
         total_bytes += pkt_len;
         
         if (rte_flow_dynf_metadata_avail()) {
             uint32_t meta = *RTE_FLOW_DYNF_METADATA(pkt);
             DOCA_LOG_INFO("[LEGACY_BATCH_%d][PKT_%d] Meta:%d Size:%d bytes", 
                           buffer->type, i, meta, pkt_len);
         }
         
         rte_pktmbuf_free(pkt);
     }
     
     buffer->total_packets_processed += buffer->count;
     buffer->total_bytes_processed += total_bytes;
     
     DOCA_LOG_INFO("Processed %d packets (%lu bytes) for legacy type %d", 
                   buffer->count, total_bytes, buffer->type);
     
     buffer->count = 0;
     buffer->last_flush_time = rte_get_tsc_cycles();
     
     DOCA_LOG_INFO("=== Legacy Buffer Flushed Successfully ===\n");
 }
 
 /* ===================== HTTP TCP 재조립 시스템 ===================== */
 
 static int init_http_reassembly_with_gro(void)
 {
     uint32_t socket_id = rte_socket_id();
     
     /* 재조립용 메모리 풀 생성 - 크기 최적화 */
     reassembly_pool = rte_pktmbuf_pool_create(
         "reassembly_pool",
         1024,    /* 메모리 풀 크기 - 4096 → 1024로 감소 */
         128,     /* 캐시 크기 - 256 → 128로 감소 */
         0,       /* private data 크기 */
         RTE_MBUF_DEFAULT_BUF_SIZE,
         socket_id
     );
     if (reassembly_pool == NULL) {
         DOCA_LOG_ERR("Failed to create reassembly memory pool");
         return -1;
     }
     
     /* IP 조각 테이블 생성 - 메모리 사용량 최적화 */
     uint64_t timeout_cycles = rte_get_tsc_hz() * 30; /* 30초 타임아웃 */
     frag_tbl = rte_ip_frag_table_create(
         64,             /* bucket_num - 1024 → 64로 감소 */
         256,            /* max_entries - 4096 → 256로 감소 */
         4,              /* max_entries_per_bucket - 16 → 4로 감소 */
         timeout_cycles, /* max_cycles */
         socket_id       /* socket_id (int) */
     );
     if (frag_tbl == NULL) {
         DOCA_LOG_ERR("Failed to create IP fragmentation table");
         rte_mempool_free(reassembly_pool);
         return -1;
     }
     
     /* Death row 초기화 - memset 사용 */
     memset(&death_row, 0, sizeof(struct rte_ip_frag_death_row));
     
     /* GRO 컨텍스트 생성 - 매개변수 최적화 */
     struct rte_gro_param gro_param = {
         .gro_types = RTE_GRO_TCP_IPV4,
         .max_flow_num = 128,        /* 1024 → 128로 감소 */
         .max_item_per_flow = 16,    /* 32 → 16으로 감소 */
         .socket_id = socket_id
     };
     
     gro_ctx = rte_gro_ctx_create(&gro_param);
     if (gro_ctx == NULL) {
         DOCA_LOG_ERR("Failed to create GRO context");
         rte_ip_frag_table_destroy(frag_tbl);
         rte_mempool_free(reassembly_pool);
         return -1;
     }
     
     /* TCP flow 테이블 초기화 */
     memset(tcp_flows, 0, sizeof(tcp_flows));
     active_flow_count = 0;
     
     DOCA_LOG_INFO("HTTP TCP reassembly with GRO initialized successfully");
     DOCA_LOG_INFO("Memory pool size: 1024, IP frag table: 64 buckets, 256 entries");
     DOCA_LOG_INFO("GRO: 128 flows, 16 items per flow");
     return 0;
 }
 
 static void cleanup_http_reassembly_with_gro(void)
 {
     /* 활성 TCP flow들 정리 */
     for (int i = 0; i < active_flow_count; i++) {
         clear_out_of_order_queue(&tcp_flows[i].out_of_order_queue);
     }
     active_flow_count = 0;
     
     /* GRO 컨텍스트 정리 */
     if (gro_ctx) {
         rte_gro_ctx_destroy(gro_ctx);
         gro_ctx = NULL;
     }
     
     /* IP 조각 테이블 정리 */
     if (frag_tbl) {
         rte_ip_frag_table_destroy(frag_tbl);
         frag_tbl = NULL;
     }
     
     /* 재조립 메모리 풀 정리 */
     if (reassembly_pool) {
         rte_mempool_free(reassembly_pool);
         reassembly_pool = NULL;
     }
     
     DOCA_LOG_INFO("HTTP TCP reassembly cleaned up");
 }
 
 static bool is_ip_fragmented(struct rte_mbuf *packet)
 {
     struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(packet, struct rte_ether_hdr *);
     
     if (rte_be_to_cpu_16(eth_hdr->ether_type) != RTE_ETHER_TYPE_IPV4)
         return false;
         
     struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
     uint16_t frag_off = rte_be_to_cpu_16(ip_hdr->fragment_offset);
     
     return (frag_off & RTE_IPV4_HDR_MF_FLAG) || (frag_off & RTE_IPV4_HDR_OFFSET_MASK);
 }
 
 static struct rte_mbuf* reassemble_ip_packet(struct rte_mbuf *packet)
 {
     uint64_t timestamp = rte_rdtsc();
     
     /* IP 헤더 추출 */
     struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(packet, struct rte_ether_hdr *);
     struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
     
     /* 최신 DPDK API 사용 - 5개 인자 필요 */
     struct rte_mbuf *reassembled = rte_ipv4_frag_reassemble_packet(
         frag_tbl, 
         &death_row,
         packet, 
         timestamp,
         ip_hdr      /* IP 헤더 포인터 추가 */
     );
     
     if (reassembled != NULL) {
         DOCA_LOG_DBG("IP packet reassembled: %d bytes", rte_pktmbuf_pkt_len(reassembled));
     }
     
     return reassembled;
 }
 
 static void flush_gro_table(void)  // 복원
 {
     struct rte_mbuf *flushed_packets[32];
     uint16_t nb_flushed;
     
     /* 최신 DPDK API 사용 - 5개 인자 순서 맞춤 */
     uint64_t timeout_cycles = rte_get_tsc_hz() / 100; /* 10ms 타임아웃 */
     nb_flushed = rte_gro_timeout_flush(
         gro_ctx,                /* ctx */
         timeout_cycles,         /* timeout_cycles */
         RTE_GRO_TCP_IPV4,      /* gro_types */
         flushed_packets,        /* out */
         32                      /* max_nb_out */
     );
     
     if (nb_flushed > 0) {
         DOCA_LOG_DBG("GRO timeout flush: %d packets", nb_flushed);
         
         for (uint16_t i = 0; i < nb_flushed; i++) {
             process_tcp_segment_for_http(flushed_packets[i]);
         }
     }
 }
 
 static void process_http_packet_for_reassembly(struct rte_mbuf *packet)
 {
     struct rte_mbuf *processed_packet = packet;
     
     /* 1단계: IP 조각 재조립 (필요한 경우만) */
     if (is_ip_fragmented(packet)) {
         DOCA_LOG_INFO("Processing fragmented IP packet");
         struct rte_mbuf *reassembled = reassemble_ip_packet(packet);
         if (reassembled == NULL) {
             DOCA_LOG_DBG("IP reassembly in progress, waiting for more fragments");
             return;  /* 재조립 진행 중 */
         }
         processed_packet = reassembled;
         DOCA_LOG_INFO("IP reassembly completed, packet size: %d bytes", 
                       rte_pktmbuf_pkt_len(processed_packet));
     }
     
     /* 2단계: GRO 처리 */
     struct rte_mbuf *gro_packets[1] = {processed_packet};
     uint16_t nb_gro = rte_gro_reassemble(gro_packets, 1, gro_ctx);
     
     if (nb_gro > 0) {
         DOCA_LOG_DBG("GRO processed %d packets", nb_gro);
         for (uint16_t i = 0; i < nb_gro; i++) {
             process_tcp_segment_for_http(gro_packets[i]);
         }
     }
     
     /* GRO 타임아웃 처리 */
     static uint64_t last_flush = 0;
     uint64_t current_time = rte_get_tsc_cycles();
     if (current_time - last_flush > rte_get_tsc_hz() / 100) {
         flush_gro_table();
         last_flush = current_time;
     }
 }
 
 static void flush_all_queued_segments(struct tcp_http_flow *flow)
 {
     struct tcp_segment *seg = flow->out_of_order_queue.head;
     int flushed_count = 0;
     
     while (seg) {
         append_to_flow_buffer(flow, seg->payload, seg->payload_len);
         flushed_count++;
         seg = seg->next;
     }
     
     if (flushed_count > 0) {
         DOCA_LOG_INFO("Flushed %d queued segments to flow buffer", flushed_count);
     }
     
     clear_out_of_order_queue(&flow->out_of_order_queue);
 }
 
 static int extract_tcp_info(struct rte_mbuf *packet, struct tcp_info *info)
 {
     struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(packet, struct rte_ether_hdr *);
     struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
     struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)((uint8_t *)ip_hdr + 
                                                          (ip_hdr->version_ihl & 0x0F) * 4);
     
     /* IP 정보 */
     info->src_ip = rte_be_to_cpu_32(ip_hdr->src_addr);
     info->dst_ip = rte_be_to_cpu_32(ip_hdr->dst_addr);
     
     /* TCP 정보 */
     info->src_port = rte_be_to_cpu_16(tcp_hdr->src_port);
     info->dst_port = rte_be_to_cpu_16(tcp_hdr->dst_port);
     info->seq_num = rte_be_to_cpu_32(tcp_hdr->sent_seq);
     info->flags = tcp_hdr->tcp_flags;
     
     /* TCP 페이로드 */
     uint8_t tcp_hdr_len = (tcp_hdr->data_off >> 4) * 4;
     info->payload = (char *)tcp_hdr + tcp_hdr_len;
     
     uint16_t ip_total_len = rte_be_to_cpu_16(ip_hdr->total_length);
     uint16_t ip_hdr_len = (ip_hdr->version_ihl & 0x0F) * 4;
     info->payload_len = ip_total_len - ip_hdr_len - tcp_hdr_len;
     
     return 0;
 }
 
 static struct tcp_http_flow* find_or_create_flow(struct tcp_info *tcp_info)
 {
     /* 기존 flow 검색 */
     for (int i = 0; i < active_flow_count; i++) {
         struct tcp_http_flow *flow = &tcp_flows[i];
         if ((flow->src_ip == tcp_info->src_ip && flow->dst_ip == tcp_info->dst_ip &&
              flow->src_port == tcp_info->src_port && flow->dst_port == tcp_info->dst_port) ||
             (flow->src_ip == tcp_info->dst_ip && flow->dst_ip == tcp_info->src_ip &&
              flow->src_port == tcp_info->dst_port && flow->dst_port == tcp_info->src_port)) {
             DOCA_LOG_DBG("Found existing TCP flow at index %d", i);
             return flow;
         }
     }
     
     /* 새 flow 생성 */
     if (active_flow_count >= MAX_TCP_FLOWS) {
         DOCA_LOG_WARN("TCP flow table full, cleaning up old flows");
         cleanup_old_flows();
         if (active_flow_count >= MAX_TCP_FLOWS) {
             DOCA_LOG_ERR("Cannot create new flow, table still full after cleanup");
             return NULL;
         }
     }
     
     struct tcp_http_flow *new_flow = &tcp_flows[active_flow_count++];
     memset(new_flow, 0, sizeof(struct tcp_http_flow));
     
     new_flow->src_ip = tcp_info->src_ip;
     new_flow->dst_ip = tcp_info->dst_ip;
     new_flow->src_port = tcp_info->src_port;
     new_flow->dst_port = tcp_info->dst_port;
     new_flow->expected_seq = 0;  /* 정렬 처리 시작할 때 설정됨 */
     new_flow->flow_established = false;  /* 정렬 처리 시작할 때 true로 변경 */
     new_flow->last_activity = rte_get_tsc_cycles();
     
     DOCA_LOG_INFO("Created new TCP flow #%d: %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d",
                   active_flow_count - 1,
                   (tcp_info->src_ip >> 24) & 0xFF, (tcp_info->src_ip >> 16) & 0xFF,
                   (tcp_info->src_ip >> 8) & 0xFF, tcp_info->src_ip & 0xFF, tcp_info->src_port,
                   (tcp_info->dst_ip >> 24) & 0xFF, (tcp_info->dst_ip >> 16) & 0xFF,
                   (tcp_info->dst_ip >> 8) & 0xFF, tcp_info->dst_ip & 0xFF, tcp_info->dst_port);
     
     return new_flow;
 }
 
 static int append_to_flow_buffer(struct tcp_http_flow *flow, char *data, uint16_t len)
 {
     if (flow->buffer_len + len > HTTP_ACCUMULATE_BUFFER_SIZE) {
         DOCA_LOG_WARN("Flow buffer overflow, resetting flow (current=%d + new=%d > max=%d)", 
                       flow->buffer_len, len, HTTP_ACCUMULATE_BUFFER_SIZE);
         flow->buffer_len = 0;
         return -1;
     }
     
     memcpy(flow->accumulate_buffer + flow->buffer_len, data, len);
     flow->buffer_len += len;
     
     DOCA_LOG_DBG("Appended %d bytes to flow buffer, total length now: %d bytes", 
                   len, flow->buffer_len);
     
     return 0;
 }
 
 static void add_to_out_of_order_queue(struct tcp_http_flow *flow, struct tcp_info *tcp_info)
 {
     if (tcp_info->payload_len > sizeof(((struct tcp_segment*)0)->payload)) {
         DOCA_LOG_WARN("TCP payload too large for queue: %d bytes", tcp_info->payload_len);
         return;
     }
     
     if (flow->out_of_order_queue.count >= MAX_OUT_OF_ORDER_SEGMENTS) {
         DOCA_LOG_WARN("Out-of-order queue full (%d segments), dropping segment", 
                       MAX_OUT_OF_ORDER_SEGMENTS);
         return;
     }
     
     struct tcp_segment *new_segment = malloc(sizeof(struct tcp_segment));
     if (new_segment == NULL) {
         DOCA_LOG_ERR("Failed to allocate TCP segment");
         return;
     }
     
     new_segment->seq_num = tcp_info->seq_num;
     new_segment->payload_len = tcp_info->payload_len;
     memcpy(new_segment->payload, tcp_info->payload, tcp_info->payload_len);
     new_segment->next = NULL;
     
     /* 시퀀스 번호 순서로 정렬 삽입 */
     struct tcp_segment **current = &flow->out_of_order_queue.head;
     while (*current != NULL && (*current)->seq_num < new_segment->seq_num) {
         current = &(*current)->next;
     }
     
     new_segment->next = *current;
     *current = new_segment;
     flow->out_of_order_queue.count++;
     
     DOCA_LOG_DBG("Added out-of-order segment: seq=%u, len=%d, queue_size=%d", 
                  tcp_info->seq_num, tcp_info->payload_len, flow->out_of_order_queue.count);
 }
 
 static void process_queued_segments(struct tcp_http_flow *flow)
 {
     struct tcp_segment_queue *queue = &flow->out_of_order_queue;
     bool found_next = true;
     int processed_count = 0;
     
     while (found_next && queue->head != NULL) {
         found_next = false;
         struct tcp_segment **current = &queue->head;
         
         while (*current != NULL) {
             if ((*current)->seq_num == flow->expected_seq) {
                 struct tcp_segment *segment = *current;
                 
                 if (append_to_flow_buffer(flow, segment->payload, segment->payload_len) == 0) {
                     flow->expected_seq += segment->payload_len;
                     
                     *current = segment->next;
                     free(segment);
                     queue->count--;
                     found_next = true;
                     processed_count++;
                     
                     DOCA_LOG_DBG("Processed queued segment: seq=%u", 
                                 flow->expected_seq - segment->payload_len);
                     break;
                 }
             }
             current = &(*current)->next;
         }
     }
     
     if (processed_count > 0) {
         DOCA_LOG_INFO("Processed %d queued segments, remaining in queue: %d", 
                       processed_count, queue->count);
     }
 }
 
 static int process_tcp_segment(struct tcp_http_flow *flow, struct tcp_info *tcp_info)
 {
     DOCA_LOG_INFO("Processing TCP segment: seq=%u, len=%d, flags=0x%02x, expected_seq=%u, established=%s, queue_count=%d", 
                  tcp_info->seq_num, tcp_info->payload_len, tcp_info->flags, flow->expected_seq,
                  flow->flow_established ? "true" : "false", flow->out_of_order_queue.count);
     
     if (flow->flow_established) {
         /* Flow가 확립된 후에는 모든 큐의 세그먼트를 flush하고 현재 세그먼트 처리 */
         flush_all_queued_segments(flow);
         append_to_flow_buffer(flow, tcp_info->payload, tcp_info->payload_len);
         flow->expected_seq = tcp_info->seq_num + tcp_info->payload_len;
         DOCA_LOG_INFO("Established flow: processed segment seq=%u, len=%d", 
                      tcp_info->seq_num, tcp_info->payload_len);
         return 0;
     }
 
     if (tcp_info->payload_len == 0) {
         DOCA_LOG_DBG("Skipping packet with no payload (flags=0x%02x)", tcp_info->flags);
         return 0;  /* ACK only 패킷 등 */
     }
 
     DOCA_LOG_INFO("TCP segment with payload: seq=%u, len=%d, expected_seq=%u, established=%s, queue_count=%d", 
                  tcp_info->seq_num, tcp_info->payload_len, flow->expected_seq,
                  flow->flow_established ? "true" : "false", flow->out_of_order_queue.count);
     
     // 페이로드 내용의 일부를 로그로 출력 (HTTP 요청 확인용)
     if (tcp_info->payload_len > 0) {
         char payload_sample[64];
         int sample_len = tcp_info->payload_len > 63 ? 63 : tcp_info->payload_len;
         memcpy(payload_sample, tcp_info->payload, sample_len);
         payload_sample[sample_len] = '\0';
         DOCA_LOG_INFO("Payload sample: %.60s", payload_sample);
     }
     
     /* Flow가 아직 설정되지 않은 경우 - 모든 세그먼트를 큐에만 저장 */
     if (!flow->flow_established) {
         DOCA_LOG_INFO("Flow not established, adding segment to queue (queue will have %d segments)", 
                      flow->out_of_order_queue.count + 1);
         add_to_out_of_order_queue(flow, tcp_info);
         
         /* 충분한 세그먼트가 모이거나 시간이 지나면 정렬 처리 시작 */
         if (should_start_processing(flow)) {
             DOCA_LOG_INFO("Starting delayed processing of queued segments");
             start_ordered_processing(flow);
         }
         return 0;
     }
     
     /* Flow가 설정된 후 정상 처리 */
     if (tcp_info->seq_num == flow->expected_seq) {
         if (append_to_flow_buffer(flow, tcp_info->payload, tcp_info->payload_len) == 0) {
             flow->expected_seq += tcp_info->payload_len;
             
             DOCA_LOG_INFO("TCP segment added in order: seq=%u, len=%d, next_expected=%u", 
                          tcp_info->seq_num, tcp_info->payload_len, flow->expected_seq);
             
             process_queued_segments(flow);
             return 0;
         }
     }
     /* Out-of-order 세그먼트인 경우 */
     else if (tcp_info->seq_num > flow->expected_seq) {
         DOCA_LOG_INFO("Out-of-order segment: expected=%u, got=%u, queuing", 
                      flow->expected_seq, tcp_info->seq_num);
         
         add_to_out_of_order_queue(flow, tcp_info);
         return 0;
     }
     /* 이미 받은 세그먼트 (재전송) */
     else {
         DOCA_LOG_INFO("Duplicate/past segment: seq=%u (expected=%u), dropping", 
                      tcp_info->seq_num, flow->expected_seq);
         return 0;
     }
     
     return -1;
 }
 
 static void process_tcp_segment_for_http(struct rte_mbuf *packet)
 {
     struct tcp_info tcp_info;
     if (extract_tcp_info(packet, &tcp_info) < 0) {
         DOCA_LOG_WARN("Failed to extract TCP info");
         rte_pktmbuf_free(packet);
         return;
     }
     
     DOCA_LOG_INFO("TCP segment: %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d, seq=%u, len=%d, flags=0x%02x",
                   (tcp_info.src_ip >> 24) & 0xFF, (tcp_info.src_ip >> 16) & 0xFF,
                   (tcp_info.src_ip >> 8) & 0xFF, tcp_info.src_ip & 0xFF, tcp_info.src_port,
                   (tcp_info.dst_ip >> 24) & 0xFF, (tcp_info.dst_ip >> 16) & 0xFF,
                   (tcp_info.dst_ip >> 8) & 0xFF, tcp_info.dst_ip & 0xFF, tcp_info.dst_port,
                   tcp_info.seq_num, tcp_info.payload_len, tcp_info.flags);
     
     struct tcp_http_flow *flow = find_or_create_flow(&tcp_info);
     if (flow->buffer_len == 0)
         flow->start_seq = tcp_info.seq_num;
     if (flow == NULL) {
         DOCA_LOG_WARN("Failed to find/create TCP flow");
         rte_pktmbuf_free(packet);
         return;
     }
     
     /* 활동 시간 업데이트 (타임아웃 계산용) */
     flow->last_activity = rte_get_tsc_cycles();
     
     if (process_tcp_segment(flow, &tcp_info) == 0) {
         DOCA_LOG_INFO("TCP segment processed successfully, flow buffer length: %d bytes", 
                       flow->buffer_len);
         
         if (!flow->http_header_complete) {
             check_http_header_complete(flow);
         }
         
         if (flow->http_header_complete) {
             DOCA_LOG_INFO("HTTP header complete! Parsing now...");
             parse_complete_http_message(flow);
         }
     } else {
         DOCA_LOG_WARN("Failed to process TCP segment");
     }
     
     rte_pktmbuf_free(packet);
 }
 
 static void clear_out_of_order_queue(struct tcp_segment_queue *queue)
 {
     struct tcp_segment *current = queue->head;
     int freed_count = 0;
     
     while (current != NULL) {
         struct tcp_segment *next = current->next;
         free(current);
         current = next;
         freed_count++;
     }
     
     if (freed_count > 0) {
         DOCA_LOG_DBG("Cleared %d segments from out-of-order queue", freed_count);
     }
     
     queue->head = NULL;
     queue->count = 0;
 }
 
 static void cleanup_old_flows(void)
 {
     uint64_t current_time = rte_get_tsc_cycles();
     uint64_t timeout_cycles = rte_get_tsc_hz() * TCP_FLOW_TIMEOUT_SEC;
     int cleaned_count = 0;
     
     for (int i = 0; i < active_flow_count; i++) {
         struct tcp_http_flow *flow = &tcp_flows[i];
         
         if (current_time - flow->last_activity > timeout_cycles) {
             DOCA_LOG_DBG("Cleaning up expired TCP flow: %d.%d.%d.%d:%d (inactive for %lu seconds)",
                           (flow->src_ip >> 24) & 0xFF, (flow->src_ip >> 16) & 0xFF,
                           (flow->src_ip >> 8) & 0xFF, flow->src_ip & 0xFF, flow->src_port,
                           (current_time - flow->last_activity) / rte_get_tsc_hz());
             
             clear_out_of_order_queue(&flow->out_of_order_queue);
             
             if (i < active_flow_count - 1) {
                 tcp_flows[i] = tcp_flows[active_flow_count - 1];
                 i--;
             }
             active_flow_count--;
             cleaned_count++;
         }
     }
     
     if (cleaned_count > 0) {
         DOCA_LOG_INFO("Cleaned up %d expired TCP flows, active flows: %d", 
                       cleaned_count, active_flow_count);
     }
 }
 
 /* ===================== HTTP 헤더 파싱 ===================== */
 
 static void check_http_header_complete(struct tcp_http_flow *flow)
 {
     if (flow->buffer_len < 4) {
         DOCA_LOG_DBG("Flow buffer too short for header check: %d bytes", flow->buffer_len);
         return;
     }
     
     DOCA_LOG_DBG("Checking HTTP header completion in %d bytes of data", flow->buffer_len);
     
     for (uint16_t i = 0; i <= flow->buffer_len - 4; i++) {
         if (flow->accumulate_buffer[i] == '\r' && 
             flow->accumulate_buffer[i+1] == '\n' &&
             flow->accumulate_buffer[i+2] == '\r' && 
             flow->accumulate_buffer[i+3] == '\n') {
             
             // 헤더 완성 플래그 
             flow->http_header_complete = true;
             flow->http_header_len = i + 4;
 
             // in-order 모드로 전환
             flow->flow_established = true;
             flow->expected_seq = flow->start_seq + flow->http_header_len;
 
             // out-of-order 큐에 쌓여 있던 페이로드 조각 한번 더 처리 
             process_queued_segments(flow);
             flush_all_queued_segments(flow);
             
             DOCA_LOG_INFO("=== HTTP Header Complete ===");
             DOCA_LOG_INFO("Flow: %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d",
                           (flow->src_ip >> 24) & 0xFF, (flow->src_ip >> 16) & 0xFF,
                           (flow->src_ip >> 8) & 0xFF, flow->src_ip & 0xFF, flow->src_port,
                           (flow->dst_ip >> 24) & 0xFF, (flow->dst_ip >> 16) & 0xFF,
                           (flow->dst_ip >> 8) & 0xFF, flow->dst_ip & 0xFF, flow->dst_port);
             DOCA_LOG_INFO("Header Length: %d bytes", flow->http_header_len - 4);
             DOCA_LOG_INFO("Total Buffer: %d bytes", flow->buffer_len);
             DOCA_LOG_INFO("Start seq: %u, Header end seq: %u", 
                           flow->start_seq, flow->expected_seq);
             break;
         }
     }
     
     if (!flow->http_header_complete) {
         DOCA_LOG_DBG("\\r\\n\\r\\n pattern not found yet in %d bytes", flow->buffer_len);
     }
 }
 
 static void parse_complete_http_message(struct tcp_http_flow *flow)
 {
     char *header = flow->accumulate_buffer;
     uint16_t header_len = flow->http_header_len - 4;
     
     DOCA_LOG_INFO("=== Parsing HTTP Message ===");
     parse_http_request_line(header, header_len);
     parse_http_header_fields(header, header_len);
     
     uint16_t body_len = flow->buffer_len - flow->http_header_len;
     if (body_len > 0) {
         DOCA_LOG_INFO("HTTP Body Present: %d bytes", body_len);
         char *body = flow->accumulate_buffer + flow->http_header_len;
         print_http_body_sample(body, body_len);
 
         memmove(flow->accumulate_buffer, body, body_len);
         flow->buffer_len = body_len;
         DOCA_LOG_INFO("Moved %d bytes of body data to buffer start", body_len);
     } else {
         flow->buffer_len = 0;
         DOCA_LOG_INFO("No body data remaining");
     }
     
     flow->http_header_complete = false;
     flow->http_header_len = 0;
     DOCA_LOG_INFO("=== HTTP Message Parsing Complete ===\n");
 }
 
 static void parse_http_request_line(char *header, uint16_t len)
 {
     char *line_end = NULL;
     for (int i = 0; i < len - 1; i++) {
         if (header[i] == '\r' && header[i+1] == '\n') {
             line_end = &header[i];
             break;
         }
     }
     
     if (line_end == NULL) return;
     
     int line_len = line_end - header;
     
     if (strncmp(header, "GET ", 4) == 0) {
         DOCA_LOG_INFO("HTTP GET Request: %.*s", line_len, header);
         extract_url_from_request(header, line_len);
     } 
     else if (strncmp(header, "POST ", 5) == 0) {
         DOCA_LOG_INFO("HTTP POST Request: %.*s", line_len, header);
         extract_url_from_request(header, line_len);
     }
     else if (strncmp(header, "PUT ", 4) == 0) {
         DOCA_LOG_INFO("HTTP PUT Request: %.*s", line_len, header);
         extract_url_from_request(header, line_len);
     }
     else if (strncmp(header, "DELETE ", 7) == 0) {
         DOCA_LOG_INFO("HTTP DELETE Request: %.*s", line_len, header);
         extract_url_from_request(header, line_len);
     }
     else if (strncmp(header, "HTTP/1.1 ", 9) == 0 || strncmp(header, "HTTP/1.0 ", 9) == 0) {
         DOCA_LOG_INFO("HTTP Response: %.*s", line_len, header);
         extract_status_from_response(header, line_len);
     }
     else {
         DOCA_LOG_INFO("HTTP Unknown: %.*s", line_len, header);
     }
 }
 
 static void extract_url_from_request(char *line, int len)
 {
     char *url_start = strchr(line, ' ');
     if (url_start && ++url_start < line + len) {
         char *url_end = strchr(url_start, ' ');
         if (url_end) {
             int url_len = url_end - url_start;
             DOCA_LOG_INFO("  → URL: %.*s", url_len, url_start);
         }
     }
 }
 
 static void extract_status_from_response(char *line, int len)
 {
     char *status_start = strchr(line, ' ');
     if (status_start && ++status_start < line + len) {
         char *status_end = strchr(status_start, ' ');
         if (status_end) {
             int status_len = status_end - status_start;
             DOCA_LOG_INFO("  → Status: %.*s", status_len, status_start);
         }
     }
 }
 
 static void parse_http_header_fields(char *header, uint16_t len)
 {
     char *current = header;
     char *end = header + len;
     int field_count = 0;
     
     /* 첫 번째 줄 건너뛰기 */
     while (current < end && !(*current == '\r' && *(current+1) == '\n')) {
         current++;
     }
     if (current < end) current += 2;
     
     DOCA_LOG_INFO("  HTTP Headers:");
     
     while (current < end) {
         char *line_start = current;
         char *line_end = current;
         
         while (line_end < end && !(*line_end == '\r' && *(line_end+1) == '\n')) {
             line_end++;
         }
         
         if (line_end >= end || line_end == line_start) break;
         
         int line_len = line_end - line_start;
         field_count++;
         
         if (strncasecmp(line_start, "Content-Type:", 13) == 0) {
             DOCA_LOG_INFO("    Content-Type:%.*s", line_len - 13, line_start + 13);
         }
         else if (strncasecmp(line_start, "Content-Length:", 15) == 0) {
             DOCA_LOG_INFO("    Content-Length:%.*s", line_len - 15, line_start + 15);
         }
         else if (strncasecmp(line_start, "Host:", 5) == 0) {
             DOCA_LOG_INFO("    Host:%.*s", line_len - 5, line_start + 5);
         }
         else if (strncasecmp(line_start, "User-Agent:", 11) == 0) {
             DOCA_LOG_INFO("    User-Agent:%.*s", line_len - 11, line_start + 11);
         }
         else if (strncasecmp(line_start, "Accept:", 7) == 0) {
             DOCA_LOG_INFO("    Accept:%.*s", line_len - 7, line_start + 7);
         }
         else if (strncasecmp(line_start, "Connection:", 11) == 0) {
             DOCA_LOG_INFO("    Connection:%.*s", line_len - 11, line_start + 11);
         }
         else {
             DOCA_LOG_DBG("    Other header: %.*s", line_len, line_start);
         }
         
         current = line_end + 2;
     }
     
     DOCA_LOG_INFO("  Total HTTP header fields: %d", field_count);
 }
 
 static void print_http_body_sample(char *body, uint16_t body_len)
 {
     int sample_len = (body_len > 64) ? 64 : body_len;
     
     DOCA_LOG_INFO("Body Sample (%d/%d bytes):", sample_len, body_len);
     
     bool is_text = true;
     for (int i = 0; i < sample_len; i++) {
         if (body[i] < 32 && body[i] != '\r' && body[i] != '\n' && body[i] != '\t') {
             is_text = false;
             break;
         }
     }
     
     if (is_text) {
         char text_sample[65] = {0};
         memcpy(text_sample, body, sample_len);
         DOCA_LOG_INFO("  Text: %s", text_sample);
     } else {
         char hex_str[129] = {0};
         for (int i = 0; i < sample_len && i < 32; i++) {
             sprintf(hex_str + i*2, "%02x", (unsigned char)body[i]);
         }
         DOCA_LOG_INFO("  Hex: %s", hex_str);
     }
 }
 
 static void reset_flow_for_next_message(struct tcp_http_flow *flow)
 {
     uint16_t remaining_len = flow->buffer_len - flow->http_header_len;
     
     if (remaining_len > 0) {
         memmove(flow->accumulate_buffer, 
                 flow->accumulate_buffer + flow->http_header_len, 
                 remaining_len);
         flow->buffer_len = remaining_len;
         DOCA_LOG_DBG("Flow has remaining data: %d bytes", remaining_len);
     } else {
         flow->buffer_len = 0;
     }
     
     flow->http_header_complete = false;
     flow->http_header_len = 0;
 }
 
 /* ===================== 메인 처리 로직 ===================== */
 
 static void add_packet_to_buffer(struct rte_mbuf *packet, enum packet_type type)
 {
     if (type == PACKET_TYPE_HTTP) {
         /* 80포트는 TCP 재조립 + HTTP 헤더 분석 */
         DOCA_LOG_INFO("HTTP packet detected, processing with TCP reassembly");
         process_http_packet_for_reassembly(packet);
     }
     else {
         /* 443, 22포트는 기존 방식 복원 */
         int buffer_idx = get_buffer_index(type);
         
         if (buffer_idx < 0) {
             DOCA_LOG_WARN("Unknown packet type %d, dropping packet", type);
             rte_pktmbuf_free(packet);
             return;
         }
 
         struct packet_buffer *buffer = &packet_buffers[buffer_idx];
 
         if (buffer->count >= BUFFER_SIZE) {
             DOCA_LOG_INFO("Buffer full for type %d, flushing...", type);
             flush_buffer(buffer);
         }
 
         buffer->packets[buffer->count] = packet;
         buffer->count++;
         
         DOCA_LOG_DBG("Added packet to legacy buffer[%d], count: %d/%d", 
                      buffer_idx, buffer->count, BUFFER_SIZE);
     }
 }
 
 static void process_packets_with_buffering(int ingress_port)
 {
     struct rte_mbuf *packets[PACKET_BURST];
     int queue_index = 0;
     int nb_packets;
     int i;
 
     nb_packets = rte_eth_rx_burst(ingress_port, queue_index, packets, PACKET_BURST);
 
     if (nb_packets) {
         DOCA_LOG_DBG("Port %d: Received %d packets", ingress_port, nb_packets);
     }
 
     for (i = 0; i < nb_packets; i++) {
         enum packet_type type = extract_packet_type(packets[i]);
         
         // 모든 패킷에 대한 기본 정보 로그
         DOCA_LOG_DBG("Processing packet %d: type=%d, length=%d", 
                      i, type, rte_pktmbuf_pkt_len(packets[i]));
         
         if (type != PACKET_TYPE_UNKNOWN) {
             add_packet_to_buffer(packets[i], type);
         } else {
             DOCA_LOG_WARN("Received packet with unknown metadata (length=%d), dropping", 
                          rte_pktmbuf_pkt_len(packets[i]));
             
             // Unknown 패킷에 대한 추가 디버깅 정보
             if (rte_pktmbuf_pkt_len(packets[i]) > 54) {  // 이더넷+IP+TCP 헤더보다 큰 경우
                 DOCA_LOG_WARN("Unknown packet appears to have payload, investigating...");
                 // 여기서 패킷 내용을 더 자세히 검사할 수 있음
             }
             
             rte_pktmbuf_free(packets[i]);
         }
     }
     
     check_buffer_timeouts();  // 복원
     
     /* TCP flow 정리 */
     static uint64_t last_cleanup = 0;
     uint64_t current_time = rte_get_tsc_cycles();
     if (current_time - last_cleanup > rte_get_tsc_hz() * 10) {
         cleanup_old_flows();
         last_cleanup = current_time;
     }
 }
 
 /* ===================== DOCA Flow 파이프라인 (DOCA 2.8 호환) ===================== */
 
 static doca_error_t create_rss_meta_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
 {
         struct doca_flow_match match;
         struct doca_flow_actions actions, *actions_arr[NB_ACTIONS_ARR];
         struct doca_flow_fwd fwd;
         struct doca_flow_fwd fwd_miss;
         struct doca_flow_pipe_cfg *pipe_cfg;
         uint16_t rss_queues[1];
         doca_error_t result;
 
         memset(&match, 0, sizeof(match));
         memset(&actions, 0, sizeof(actions));
         memset(&fwd, 0, sizeof(fwd));
         memset(&fwd_miss, 0, sizeof(fwd_miss));
 
         actions.meta.pkt_meta = UINT32_MAX;
         actions_arr[0] = &actions;
 
         match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_TCP;
         match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
         match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;
         match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
         match.outer.ip4.src_ip = 0xffffffff;
         match.outer.ip4.dst_ip = 0xffffffff;
         match.outer.tcp.l4_port.src_port = 0x0000;
         match.outer.tcp.l4_port.dst_port = 0xffff;
 
         result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
         if (result != DOCA_SUCCESS) {
                 DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
                 return result;
         }
         result = set_flow_pipe_cfg(pipe_cfg, "RSS_META_PIPE", DOCA_FLOW_PIPE_BASIC, true);
         if (result != DOCA_SUCCESS) {
                 DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
                 goto destroy_pipe_cfg;
         }
         result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
         if (result != DOCA_SUCCESS) {
                 DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
                 goto destroy_pipe_cfg;
         }
         result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, NB_ACTIONS_ARR);
         if (result != DOCA_SUCCESS) {
                 DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
                 goto destroy_pipe_cfg;
         }
 
         rss_queues[0] = 0;
         fwd.type = DOCA_FLOW_FWD_RSS;
         fwd.rss_queues = rss_queues;  // DOCA 2.8 방식
         fwd.rss_inner_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_TCP;
         fwd.num_of_queues = 1;  // DOCA 2.8 방식
 
         fwd_miss.type = DOCA_FLOW_FWD_DROP;
 
         result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe);
 destroy_pipe_cfg:
         doca_flow_pipe_cfg_destroy(pipe_cfg);
         return result;
 }
 
 static doca_error_t add_rss_meta_pipe_entries(struct doca_flow_pipe *pipe, struct entries_status *status)
 {
         struct doca_flow_match match;
         struct doca_flow_actions actions;
         struct doca_flow_pipe_entry *entry;
         doca_error_t result;
 
         doca_be32_t dst_ip_addr = BE_IPV4_ADDR(10, 197, 0, 11);
         doca_be32_t src_ip_addr = BE_IPV4_ADDR(10, 197, 0, 9);  // DOCA 2.8 환경에 맞춤
 
         /* HTTP entry - port 80, meta 80 */
         memset(&match, 0, sizeof(match));
         memset(&actions, 0, sizeof(actions));
 
         match.outer.ip4.dst_ip = dst_ip_addr;
         match.outer.ip4.src_ip = src_ip_addr;
         match.outer.tcp.l4_port.dst_port = rte_cpu_to_be_16(80);
 
         actions.meta.pkt_meta = DOCA_HTOBE32(80);
         actions.action_idx = 0;
 
         result = doca_flow_pipe_add_entry(0, pipe, &match, &actions, NULL, NULL, 0, status, &entry);
         if (result != DOCA_SUCCESS) {
                 DOCA_LOG_ERR("Failed to add HTTP entry: %s", doca_error_get_descr(result));
                 return result;
         }
         DOCA_LOG_INFO("Added HTTP entry (port 80 -> meta 80)");
 
         /* HTTPS entry - port 443, meta 443 */
         memset(&match, 0, sizeof(match));
         memset(&actions, 0, sizeof(actions));
 
         match.outer.ip4.dst_ip = dst_ip_addr;
         match.outer.ip4.src_ip = src_ip_addr;
         match.outer.tcp.l4_port.dst_port = rte_cpu_to_be_16(443);
 
         actions.meta.pkt_meta = DOCA_HTOBE32(443);
         actions.action_idx = 0;
 
         result = doca_flow_pipe_add_entry(0, pipe, &match, &actions, NULL, NULL, 0, status, &entry);
         if (result != DOCA_SUCCESS) {
                 DOCA_LOG_ERR("Failed to add HTTPS entry: %s", doca_error_get_descr(result));
                 return result;
         }
         DOCA_LOG_INFO("Added HTTPS entry (port 443 -> meta 443)");
 
         /* SSH entry - port 22, meta 22 */
         memset(&match, 0, sizeof(match));
         memset(&actions, 0, sizeof(actions));
 
         match.outer.ip4.dst_ip = dst_ip_addr;
         match.outer.ip4.src_ip = src_ip_addr;
         match.outer.tcp.l4_port.dst_port = rte_cpu_to_be_16(22);
 
         actions.meta.pkt_meta = DOCA_HTOBE32(22);
         actions.action_idx = 0;
 
         result = doca_flow_pipe_add_entry(0, pipe, &match, &actions, NULL, NULL, 0, status, &entry);
         if (result != DOCA_SUCCESS) {
                 DOCA_LOG_ERR("Failed to add SSH entry: %s", doca_error_get_descr(result));
                 return result;
         }
         DOCA_LOG_INFO("Added SSH entry (port 22 -> meta 22)");
 
         return DOCA_SUCCESS;
 }
 
 /* ===================== 메인 함수 ===================== */
 
 doca_error_t flow_rss_meta_with_app_buffering(int nb_queues)
 {
         const int nb_ports = 1;  
         struct flow_resources resource = {0};
         uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
         struct doca_flow_port *ports[nb_ports];
         struct doca_dev *dev_arr[nb_ports];
         struct doca_flow_pipe *pipe;
         struct entries_status status;
         const int num_of_entries = 3;
         doca_error_t result;
         int port_id;
 
         /* 기존 버퍼 초기화 (HTTPS, SSH용) - 복원 */
         init_packet_buffers();
         
         /* HTTP TCP 재조립 시스템 초기화 */
         if (init_http_reassembly_with_gro() < 0) {
                 DOCA_LOG_ERR("Failed to initialize HTTP TCP reassembly");
                 return DOCA_ERROR_INITIALIZATION;
         }
 
         /* DOCA Flow 프레임워크 초기화 */
         result = init_doca_flow(nb_queues, "vnf,hws", &resource, nr_shared_resources);
         if (result != DOCA_SUCCESS) {
                 DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
                 goto cleanup_reassembly;
         }
 
         // DOCA 2.8 호환 - actions_mem_size 제거
         result = init_doca_flow_ports(nb_ports, ports, true, dev_arr);
         if (result != DOCA_SUCCESS) {
                 DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
                 goto destroy_flow;
         }
 
         for (port_id = 0; port_id < nb_ports; port_id++) {
                 memset(&status, 0, sizeof(status));
 
                 result = create_rss_meta_pipe(ports[port_id], &pipe);
                 if (result != DOCA_SUCCESS) {
                         DOCA_LOG_ERR("Failed to create pipe: %s", doca_error_get_descr(result));
                         goto stop_ports;
                 }
 
                 result = add_rss_meta_pipe_entries(pipe, &status);
                 if (result != DOCA_SUCCESS) {
                         DOCA_LOG_ERR("Failed to add entries: %s", doca_error_get_descr(result));
                         goto stop_ports;
                 }
 
                 result = doca_flow_entries_process(ports[port_id], 0, DEFAULT_TIMEOUT_US, num_of_entries);
                 if (result != DOCA_SUCCESS || status.nb_processed != num_of_entries || status.failure) {
                         DOCA_LOG_ERR("Failed to process entries: processed=%d, expected=%d, failure=%s",
                                     status.nb_processed, num_of_entries, status.failure ? "true" : "false");
                         goto stop_ports;
                 }
         }
 
         DOCA_LOG_INFO("=== Enhanced HTTP TCP Reassembly System Started (DOCA 2.8 Compatible) ===");
         DOCA_LOG_INFO("HTTP(80):  IP Reassembly -> GRO -> TCP Flow Tracking -> HTTP Header Parsing");
         DOCA_LOG_INFO("HTTPS(443): Legacy port-based buffering with detailed logging");
         DOCA_LOG_INFO("SSH(22):   Legacy port-based buffering with detailed logging");
         DOCA_LOG_INFO("============================================================================");
 
         while (!force_quit) {
                 process_packets_with_buffering(0);
                 print_buffer_stats();
                 usleep(10000);
         }
 
 stop_ports:
         if (result == DOCA_SUCCESS)
                 stop_doca_flow_ports(nb_ports, ports);
 destroy_flow:
         doca_flow_destroy();
 cleanup_reassembly:
         cleanup_http_reassembly_with_gro();
         return result;
 }