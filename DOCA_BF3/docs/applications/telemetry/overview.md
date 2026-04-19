# BF3 Telemetry — 수집 가능한 정보 총정리

## 개요

- BF3가 **하드웨어 경로에서 직접 관측**할 수 있는 정보를 레이어별로 정리
- DOCA SDK가 제공하는 telemetry API들은 **4개 레이어 + 1개 운영 레이어**로 분리되어 있음
- 본 문서의 모든 항목은 **host CPU를 거의 소모하지 않음** (HW path에서 카운팅/export)

**왜 중요한가**

- DTN boundary 관측의 실제 재료 — "무엇을 기록할 수 있는가"
- host OS와 독립된 관측 채널 → zero-trust 경계 보안 근거
- 기존 NetFlow/Prometheus 수집 스택과 **재발명 없이 연동**

---

## 레이어 지도

| Layer | 관측 대상 | 주요 헤더 | 샘플 위치 |
|-------|----------|----------|----------|
| **1. Flow-level** | 5-tuple per-entry 패킷·바이트 | `doca_flow.h` | `DOCA_BF3/projects/flow_common.{h,c}` |
| **2. Network Export** | NetFlow v9 / IPFIX 레코드 | `doca_telemetry_exporter_netflow.h` | `samples/doca_telemetry_exporter/telemetry_export_netflow/` |
| **3. Transport** | PCC counter · 재전송 히스토그램 | `doca_telemetry_pcc.h`, `doca_telemetry_adp_retx.h` | `samples/doca_telemetry/telemetry_pcc/`, `.../telemetry_adp_retx/` |
| **4. Hardware** | PCIe 링크·에러·지연 · 범용 diag counter | `doca_telemetry_pci.h`, `doca_telemetry_diag.h` | `samples/doca_telemetry/telemetry_pci/`, `.../telemetry_diag/` |
| **5. K8s 운영** | Layer 1~4 수집물의 DPUService 배포 | `doca-telemetry` helm chart | `docs/public/advanced-configuration/dpuservices/doca-telemetry-service.md` |

---

## Layer 1 — Flow-level counter (per 5-tuple)

### 수집 항목

- `total_pkts` — entry에 매칭된 총 패킷 수
- `total_bytes` — 누적 바이트
- Meter counter — rate limit 초과 bytes/pkts

### 코드 근거

- `DOCA_BF3/projects/flow_common.h:88-92` — 이미 프로젝트에 도입된 구조
  ```c
  struct flow_resources {
      uint32_t nr_counters;  /* number of counters to configure */
      uint32_t nr_meters;    /* number of traffic meters to configure */
  };
  ```
- 패턴
  - `doca_flow_pipe_cfg`에 `monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_COUNTER` 설정
  - `doca_flow_pipe_add_entry()` 시 entry별 counter 자동 할당
  - 주기적으로 `doca_flow_resource_query_entry()`로 `{total_pkts, total_bytes}` 조회

### 활용

- DTN 경계에 match pipe 설치 → 외부 peer 별 트래픽량 실시간 집계
- `dataplane.c:56`의 CPU-side `total_bytes_processed`와 bit-for-bit 교차검증 가능

### 예시

```
Pipe "ingress_monitor": match dst=10.34.20.4, dport∈{2811,1094}
  → entry{peer=10.34.60.5, xrootd} total_pkts=1,487,203 / total_bytes=2,187,540,210
  → entry{peer=10.34.60.2, gridftp} total_pkts=923,044 / total_bytes=1,352,110,888
```

---

## Layer 2 — NetFlow v9 export (RFC 3954)

### 수집 필드 (23개)

`samples/doca_telemetry_exporter/telemetry_export_netflow/telemetry_export_netflow_sample.c:106-151`에서 template에 등록되는 실제 필드:

| 카테고리 | 필드 | 의미 |
|---------|------|-----|
| Address | `IPV4_SRC_ADDR`, `IPV4_DST_ADDR`, `IPV6_SRC_ADDR`, `IPV6_DST_ADDR` | flow identity |
| Routing | `IPV4_NEXT_HOP`, `IPV6_NEXT_HOP`, `INPUT_SNMP`, `OUTPUT_SNMP` | 경로 추적, 인터페이스 index |
| L4 | `L4_SRC_PORT`, `L4_DST_PORT`, `PROTOCOL`, `TCP_FLAGS` | 서비스·connection 상태 |
| QoS | `SRC_TOS`, `SRC_AS`, `DST_AS`, `SRC_MASK`, `DST_MASK` | DSCP/AS/subnet 집계 |
| Volume | `IN_PKTS`, `IN_BYTES` | flow 볼륨 |
| Time | `FIRST_SWITCHED`, `LAST_SWITCHED` | flow duration (start/end timestamp) |
| Session | `CONNECTION_TRANSACTION_ID` | **session ID** — provenance/lineage 핵심 |
| App | `APPLICATION_NAME` | GridFTP/XRootD/Rucio 식별자 |

### Export 경로

- UDP NetFlow collector 직접 송신 (`telemetry_export_netflow_sample.c:222-223`, 기본 포트 9996)
- IPC를 통한 DOCA Telemetry Service 연동 (`:259-260`)
- 기존 수집 스택과 호환: `nfcapd`, `ntopng`, Grafana NetFlow 대시보드 등

### 활용

- draft의 "DTN Boundary Observer" — 이 API가 그대로 구현 수단
- 경계 flow의 빠짐없는 기록 → audit correctness 근거
- `CONNECTION_TRANSACTION_ID` = 연구 draft의 "session ID 캡처" 항목 1:1 매핑

---

## Layer 3 — Transport / protocol telemetry

### 3a. PCC counter — `doca_telemetry_pcc`

**출처**: `samples/doca_telemetry/telemetry_pcc/telemetry_pcc_sample.c:126-185`

- 카드 내 **algo slot별 congestion control counter** 추출
- 각 slot 독립 알고리즘 (BBR-like, DCQCN 등) 실행
- 수집
  - slot 메타데이터 — ID, major/minor version, algo info 문자열
  - slot별 활성화 상태 (`algo_en`, `counters_en`)
  - counter 배열 — RTT sample 수, cwnd 감소 횟수 등 (algo 정의)

**활용**

- host TCP stack보다 한 층 낮은 수준에서 congestion 반응 관측
- netem으로 loss 주입 시 slot counter 변화를 직접 검증

### 3b. Adaptive retransmission 히스토그램 — `doca_telemetry_adp_retx`

**출처**: `samples/doca_telemetry/telemetry_adp_retx/telemetry_adp_retx_sample.c:49-108, 239-275`

- **재전송 시간 간격 히스토그램** — nsec/usec/usec_100/msec bin 단위 분포
- 설정 가능: bin 개수, bin0/bin1 width, fixed/exponential mode
- `doca_telemetry_adp_retx_read_hist_bins()` 호출 한 번에 전체 덤프

**활용**

- 전송 중 재전송 분포 → congestion 반응 곡선
- netem profile(lan/metro/national/intl) × loss 조건 별 bin 분포 비교

**예시 출력** (`:236-274` 기반)

```
Bin 0 [   0 -  99]usec: 145
Bin 1 [ 100 - 199]usec: 892
Bin 2 [ 200 - 399]usec: 1,204
Bin 3 [ 400+    ]usec:    67   ← loss 주입 시 증가
```

---

## Layer 4 — Hardware-level

### 4a. PCIe link & error counters — `doca_telemetry_pci`

**출처**: `samples/doca_telemetry/telemetry_pci/telemetry_pci_sample.c:140-239`

세 그룹으로 분리 제공:

**Management info** (`:140-189`)
- `link_width_active`, `link_speed_active` — 실제 동작 중인 PCIe 대역폭
- `max_read_request_size`, `max_payload_size`
- `pwr_status`, `port_type`, `lane_reversal`
- `num_of_pfs`, `num_of_vfs`, `bdf0`
- 에러 플래그 — `correctable_error_detected`, `non_fatal_error_detected`, `fatal_error_detected`, `unsupported_request_detected`

**Perf counters group 1** (`:191-239`)
- TX overflow — `tx_overflow_buffer_pkt`, `tx_overflow_buffer_marked_pkt`
- RX/TX 에러 — `rx_errors`, `tx_errors`
- CRC — `crc_error_dllp`, `crc_error_tlp`
- Stall — `outbound_stalled_reads/writes` 및 events
- FEC — `fec_correctable_error_counter`, `fec_uncorrectable_error_counter`
- 링크 recovery — `l0_to_recovery`
- Bit Error Rate — `effective_ber`, `fber` (magnitude + coefficient)

**Latency histogram** (`:241-292`)
- bucket 단위 ns 분포
- `doca_telemetry_pci_get_latency_histogram_dimensions()`로 bucket_count/width 조회
- `doca_telemetry_pci_read_latency_histogram()`로 전체 dump

**활용**

- "DPU 내부 데이터 경로가 bottleneck이 아니다"를 BER/overflow/latency 증거로 입증
- reviewer의 "PCIe가 병목 아니냐" 반론 선제 방어

### 4b. 범용 diagnostic counters — `doca_telemetry_diag`

**출처**: `samples/doca_telemetry/telemetry_diag/telemetry_diag_sample.c:815-1029`

- **임의 data_id 리스트 기반 counter 샘플링** (JSON 입력)
- Sample mode
  - `SINGLE` — 한 번만 수집
  - `REPETITIVE` — 주기적 폴링 (`:667-724`)
  - `ON_DEMAND` — 수동 호출
- Sample period — nsec 단위 설정 (`:242, :826`)
- Output format 0/1/2 — per-sample timestamp 또는 묶음 timestamp
- 출력 — CSV 직접 dump (`:887-908`) → Grafana/Prometheus 바로 연동

**활용**

- 수집하고 싶은 임의 HW counter를 ns 단위로 샘플링
- 실험 중 "특정 구간에만 집중 수집" 같은 시나리오 구성

---

## Layer 5 — Cloud-native 배포 (DPUService)

**출처**: `docs/public/advanced-configuration/dpuservices/doca-telemetry-service.md`

DPF가 DPU cluster에 `doca-telemetry` Helm chart를 DPUService로 배포 → 위 Layer 1~4 수집물을 K8s-native로 orchestrate.

### 주요 설정

```yaml
spec:
  helmChart:
    values:
      configMapData:
        providers: "sysfs,ethtool"           # 수집 대상
        aggregator_providers: "prometheus_aggr"
        general:
          update: 1000                        # 샘플 주기 (ms)
          sync-time-limit: 10000              # 버퍼 회전 시간 (s)
        fluent:
          kafka:
            enable: 1
            brokers: "kafka-broker:9092"
            topics: "telemetry"
          influxdb: { enable: 0, ... }
          elasticsearch: { enable: 0, ... }
```

### 가능한 export 목적지

- Prometheus (scrape)
- Fluent Bit forward
- Kafka
- InfluxDB
- Elasticsearch

### 의미

- Draft의 "Collector — DTN host 수집 → Prometheus / Kafka → Grafana" 파이프라인이 이 레이어에서 **CRD 단 한 개**로 선언적 배포됨
- `DPUService` 리소스 하나로 telemetry 스택 전체 lifecycle 관리

---

## 통합 예시 — XRootD 단일 세션에서 나오는 관측 데이터

아래는 facility DTN ↔ external peer 사이 **한 개 XRootD 세션**(182초, 2GB 전송)에서 **5개 레이어가 동시에** 뽑아낼 수 있는 정보.

### Layer 2 — NetFlow record

```
src_addr_v4   = 10.34.60.5
dst_addr_v4   = 10.34.20.4
src_port      = 32451
dst_port      = 1094               (XRootD)
protocol      = 6 (TCP)
tcp_flags     = 0x1F               (SYN|ACK|PSH|FIN accumulated)
in_pkts       = 1,487,203
in_bytes      = 2,187,540,210      (~2GB)
first         = 1712345600
last          = 1712345782         (182초)
connection_transaction_id = 0x7f3a9c4b2e...
application_name = "XRootD"
```

### Layer 1 — doca_flow counter

```
pipe=ingress_monitor, entry=xrootd-session-A
  total_pkts  = 1,487,203
  total_bytes = 2,187,540,210
```

→ Layer 2 `in_pkts/in_bytes`와 일치 확인 → 누락 없음 증명

### Layer 3b — 재전송 히스토그램 (netem loss=0.1% 주입 시)

```
Bin 0 [   0 -   99]usec:    145
Bin 1 [ 100 -  199]usec:    892
Bin 2 [ 200 -  399]usec:  1,204
Bin 3 [ 400+     ]usec:     67
```

### Layer 3a — PCC slot counter

```
Slot 0: ID=0x1001 (BBR-like), ENABLED
  Counter 0: 523,110    - "rtt_samples"
  Counter 1:      89    - "cwnd_decreases"
```

### Layer 4a — PCIe latency histogram

```
[ 0]     0ns →   999ns : 12,483,021
[ 1]  1000ns →  1999ns :        421
[10+]                  :          3   ← PCIe 병목 없음
```

### 종합 해석

- **가시성 대칭**: Layer 1 HW counter와 Layer 2 NetFlow 수치 일치 → 경계에서 누락된 flow 없음
- **병목 위치 특정**: Layer 3 재전송 증가 + Layer 4 PCIe 낮은 latency → 병목은 네트워크 loss이지 DPU 아님
- **provenance 자동 기록**: `CONNECTION_TRANSACTION_ID`, timestamp, size가 flow 자체에 박혀 있음 → Bronze 레이어 enrichment에 그대로 활용

---

## 연구 pillar 와의 매핑

| draft 문장 | 구현 근거 (이 문서 레이어) |
|-----------|----------------------|
| "HW flow capture — DPU `doca_flow` per-5-tuple counter + mirror" | Layer 1 |
| "Telemetry export via `doca_telemetry_exporter`" | Layer 2 |
| "Collector — DTN host 수집 → Prometheus / Kafka → Grafana" | Layer 5 |
| "line-rate 관측" | Layer 1~4 전부 HW path |
| "Provenance/Lineage — source, timestamp, size, session ID" | Layer 2 필드 1:1 대응 |
| "host OS 독립 관측 (zero-trust)" | Layer 1/3/4는 HW pipeline 직접 수집, 커널 경로 미경유 |
| "audit correctness" | Layer 1 vs Layer 2 cross-check |

---

## 다음 단계

- **Transfer 실험**에 우선 필요한 레이어 — Layer 1 (bytes/pkts) + Layer 3b (retx 분포) + Layer 4a (PCIe 건전성)
- **Security Observer PoC**에 우선 필요한 레이어 — Layer 1 + Layer 2 (NetFlow export) + Layer 5 (DPUService로 Kafka 연동)
- **구현 진입점**
  - 최소 프로토타입: 사용자 dataplane에 Layer 1 counter 추가 → `doca_flow_resource_query_entry()` 주기 호출
  - 다음: Layer 2 sample 코드를 실제 doca-dev Pod에 빌드·실행해 NetFlow 레코드 덤프 확인
  - 최종: Layer 5 DPUService 배포해 cloud-native 파이프라인 완성

## 관련 문서

- [`../doca-flow/`](../doca-flow/) — DOCA Flow 애플리케이션 (Layer 1 counter 설치 위치)
- [`../offloading/mtu-ovs-setup.md`](../offloading/mtu-ovs-setup.md) — BF3 link 성능 trouble shooting
