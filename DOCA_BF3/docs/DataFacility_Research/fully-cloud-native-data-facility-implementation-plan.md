---
title: Fully Cloud Native Data Facility Implementation Plan
---

[TOC]

# 목적

- `fully-cloud-native-data-facility-research-draft.md` 와 별도로
- **지금 DPF / DOCA / 현재 클러스터 구조 기준으로**
- 처리 / 저장 / 공유 / 보안을 실제로 어떻게 구현할 수 있는지 정리

이 문서는 연구 아이디어 설명 문서가 아니라,

- 현재 무엇이 가능한지
- 무엇은 built-in으로 쓸 수 있는지
- 무엇은 custom DPUService가 필요한지
- 지금 구조에서 어떤 구현이 가장 현실적인지

를 정리하는 **구현 관점 문서**이다.

# 현재 구현 기반

## 이미 확보된 기반

현재 환경에서 이미 검증된 것

- Host cluster 위에 `DPF` 동작
- BF3 DPU provisioning 완료
- `DPUCluster` 생성 완료
- BF3가 tenant DPU cluster worker node로 join 완료
- DPU lifecycle 이 Kubernetes 기반으로 관리 가능

즉 현재는 “DPU를 어떻게 올릴까?” 단계가 아니라

> **올라간 DPU 위에 어떤 서비스를 어떤 방식으로 올릴 수 있을까?**

를 고민할 수 있는 단계이다.

## DPF를 어떻게 봐야 하나

현재 구조에서 DPF는 다음으로 보는 것이 가장 적절하다.

- DPU provisioning platform
- DPU lifecycle manager
- DPU cluster bootstrapper
- DPUService deployment substrate

즉 DPF는

- Iceberg 자체를 만들어주는 도구도 아니고
- Rucio/XRootD 자체를 자동으로 제공하는 도구도 아니지만

**DPU 위에 서비스를 cloud-native하게 배포하고 운영할 수 있게 해주는 기반**이다.

# 구현 패턴

현재 구조에서 가장 자연스러운 구현 패턴은 다음 두 층이다.

## 1. Host-side component

host cluster 또는 DTN host에서 담당

- 외부 시스템 연동
- workflow orchestration
- control-plane logic
- data lakehouse / transfer controller / gateway

예시

- Iceberg catalog / lakehouse stack
- Rucio gateway
- XRootD gateway
- ingest orchestrator
- CSI controller

## 2. DPU-side component

BF3 DPU 위에서 담당

- telemetry
- flow visibility
- filtering / isolation
- transfer assist
- pre-ingest processing
- storage path assist

이 층은 주로 **DPUService** 로 배포하는 것이 자연스럽다.

그림으로 보면:

```text
External System
  |
  | ingress / egress / transfer
  |
Host-side Service
  |
  +-- control / orchestration / application logic
  |
DPU-side Service
  |
  +-- telemetry / policy / filtering / preprocessing / path assist
  |
Data Facility Core
```

# 코드/문서 기준 구현 근거

## 1. DPUService 기반 확장 가능

근거

- `api/dpuservice/v1alpha1/`
- `internal/dpuservice/controllers/`
- `dpuservices/dummydpuservice/`

의미

- custom DPU app을 DPUService 형태로 올리는 것이 공식 확장 포인트

## 2. Built-in telemetry 서비스 존재

근거

- `dpuservices/dts/DPUService.yaml`
- `docs/public/advanced-configuration/dpuservices/doca-telemetry-service.md`

의미

- telemetry / visibility 계열은 지금도 바로 실험 가능

## 3. Storage 시나리오 예제 존재

근거

- `dpuservices/storage/`
- `dpuservices/storage/examples/scenarios/README.md`

의미

- host controller + DPU-side plugin 분리 구조가 이미 예제로 존재

## 4. Flow / offload / observability 구현 힌트 존재

근거

- `DOCA_BF3/applications/common/telemetry_exporter.c`
- `DOCA_BF3/samples/doca_flow/`
- `DOCA_BF3/samples/doca_compress/`
- `DOCA_BF3/docs/offloading/00-Set Mtu9000 & OVS_Offloading.md`

의미

- telemetry / flow / checksum / compression / offload 방향의 실험은 현실적

# 영역별 구현 방안

---

# 1. 처리 (Processing)

## 현재 구조에서의 위치

현재 구조에서 processing은

- DTN에만 DPU 존재
- compute node마다 DPU가 없음

이라는 제약 때문에,

**직접적인 HPC/AI compute acceleration** 보다는

- pre-processing
- data staging
- ingest 전 준비
- metadata extraction

같은 **간접 지원 역할**이 더 현실적이다.

## 현재 DPF 기반 구현 가능성

| 항목 | 판단 |
|---|---|
| 지금 DPF로 바로 사용 가능 | 낮음 |
| custom DPUService로 확장 가능 | 높음 |
| 현재 구조 적합도 | 중간 |

## 권장 기술

- `dpuservices/dummydpuservice/` 기반 custom service
- `DOCA_BF3/samples/doca_compress/`
- checksum / hash / lightweight content inspection 계열 primitive

## 구현 아이디어

### A. Pre-ingest preprocessing service

DPU에서 수행

- file header inspection
- checksum generation
- compression / decompression assist
- metadata extraction
- dataset class hint 생성

Host에서 수행

- 최종 ingest orchestration
- Iceberg write
- workflow scheduling

## 권장 구현 방식

```text
External ingest
  -> DTN host ingress service
  -> DPU preprocess-service (custom DPUService)
  -> metadata / checksum / classification result
  -> host-side ingest pipeline
```

## 현재 단계 평가

- 구현 가능
- 하지만 지금 문서에서 main contribution으로 과장하면 안 됨
- `processing support` 또는 `preparation layer` 로 두는 것이 적절

---

# 2. 저장 (Storage)

## 현재 구조에서의 위치

storage 축은 현재 구조와 잘 맞는다.

다만 핵심은:

- Iceberg 자체를 DPU 위에 직접 올리는 것보다
- **저장 전 ingest / landing / path assist**

에 DPU를 쓰는 것이 현실적이라는 점이다.

## 현재 DPF 기반 구현 가능성

| 항목 | 판단 |
|---|---|
| 지금 DPF로 바로 사용 가능 | 중간 |
| storage 관련 예제 존재 | 높음 |
| 현재 구조 적합도 | 높음 |

## 권장 기술

- `dpuservices/storage/`
- `DPUServiceCredentialRequest`
- host controller + DPU-side service split 구조
- `Iceberg` / lakehouse stack은 host cluster에 유지

## 구현 아이디어

### A. Iceberg ingest landing tier

DPU에서 수행

- incoming dataset landing
- ingest 전 정리 / 분류
- format validation
- metadata hint 생성

Host에서 수행

- Iceberg catalog
- object store / lakehouse stack
- final commit / metadata transaction

### B. Storage path assist

DPU에서 수행

- transfer-adjacent storage buffering
- pre-ingest staging
- storage path assist

## 권장 구현 방식

```text
External source
  -> DTN(DPU)
      -> landing / organize / validate
  -> host-side lakehouse ingest
      -> Iceberg / object store
```

## 현재 단계 평가

- sharing 다음으로 유망
- storage 자체보다 **storage ingest assist** 에 초점을 두는 것이 좋음

---

# 3. 공유 (Sharing)

## 현재 구조에서의 위치

현재 구조에서 가장 강한 축이다.

이유

- BF3가 DTN에 있음
- DTN은 boundary node
- 외부 데이터 ingest/export 경로가 여기로 집중됨

즉 DPU가 가장 직접적으로 가치를 만들기 쉬운 곳이

- transfer
- sharing
- export / ingest boundary

이다.

## 현재 DPF 기반 구현 가능성

| 항목 | 판단 |
|---|---|
| 지금 DPF가 직접 Globus/Rucio 제공 | 아님 |
| custom DPUService로 보조 계층 구현 | 가능 |
| 현재 구조 적합도 | 매우 높음 |

## 권장 기술

- Host side:
  - `Rucio`
  - `Globus`
  - `XRootD`
  - `GridFTP` 계열 endpoint / gateway
- DPU side:
  - telemetry
  - checksum / integrity assist
  - flow visibility
  - policy / filtering

## 구현 아이디어

### A. Rucio/XRootD boundary gateway model

Host에서 수행

- transfer control
- endpoint management
- external protocol termination 일부

DPU에서 수행

- telemetry / flow export
- checksum / integrity support
- selective crypto offload 검토
- transfer observability

### B. DTN transfer assist service

custom DPUService로 구현

- ingress/egress flow classification
- transfer telemetry
- policy-based filtering
- heavy flow detection

## 권장 구현 방식

```text
External transfer client
  -> Host-side gateway (Rucio / XRootD / transfer service)
  -> DPU transfer-assist-service
  -> Data facility ingress/egress path
```

## 현재 단계 평가

- 가장 먼저 PoC 해야 하는 축
- 가장 설득력 있는 결과가 나올 가능성이 큼

---

# 4. 보안 (Security)

## 한 문장 요약

> **host(DTN)와 DPU가 각자 잘하는 층위에서 역할을 분담하는 경계 보안 오프로딩 구조 — Cilium이 DPF cluster의 모든 ingress/egress를 DTN으로 강제 경유시키면, host는 K8s CRD 기반 선언적 관리와 복잡한 결정(authz/cert 검증)을, DPU는 HW flow pipeline의 line-rate 관측과 정책 집행을 맡는다.**

## 현재 구조에서의 위치

- 범위: **DTN / DPU boundary security**
- 전제: Cilium 정책으로 DPF cluster의 모든 ingress/egress가 DTN 강제 경유
- 2축: 관측(Observer) + 정책(Enforcer). 암호화는 Transfer 실험 합류

## 공통 전제 — 경로 강제 레이어 (Cilium)

### egress: CiliumEgressGatewayPolicy

```yaml
apiVersion: cilium.io/v2
kind: CiliumEgressGatewayPolicy
metadata:
  name: facility-egress-via-dtn
spec:
  selectors:
    - podSelector:
        matchLabels:
          datafacility: "true"
  destinationCIDRs:
    - "0.0.0.0/0"
  excludedCIDRs:
    - "10.32.0.0/12"   # 내부망 제외
  egressGateway:
    nodeSelector:
      matchLabels:
        datafacility/role: dtn
```

### ingress: MetalLB DTN-only advertise

```yaml
apiVersion: metallb.io/v1beta1
kind: L2Advertisement
metadata:
  name: dtn-only
spec:
  ipAddressPools: [facility-pool]
  nodeSelectors:
    - matchLabels:
        datafacility/role: dtn
```

### 검증 기준

- non-DTN worker `tcpdump` → 외부 트래픽 없음
- DTN `tcpdump` → 모든 경계 트래픽 보임
- cross-cluster 전송 시 DTN record coverage 100%

---

## A. DTN Boundary Observer (관측)

### 구현 근거 (repo 내 샘플)

| 용도 | 경로 |
|------|------|
| per-5-tuple HW counter | `DOCA_BF3/samples/doca_flow/flow_monitor_meter` |
| 경계 트래픽 샘플 mirror | `DOCA_BF3/samples/doca_flow/flow_mirror` |
| flow hash | `DOCA_BF3/samples/doca_flow/flow_entropy` |
| NetFlow v9 export | `DOCA_BF3/samples/doca_telemetry_exporter/telemetry_export_netflow` |
| custom schema export | `DOCA_BF3/samples/doca_telemetry_exporter/telemetry_export` |
| 참고 base service | `dpuservices/dts/DPUService.yaml` |

### 구성

```text
External / ScalexPOD peer
    ↓  (Cilium-forced DTN 경유)
DTN host ingress
    ↓
DPU flow pipeline
  ├─ per-flow HW counter (doca_flow monitor)
  └─ mirror → telemetry exporter
    ↓
DOCA Telemetry Exporter → NetFlow v9 → 10.34.20.4:2055
    ↓
DTN host: goflow2 (collector)
    ↓
Prometheus remote-write / Kafka
    ↓
Grafana: boundary visibility dashboard
```

### Custom DPUService — `boundary-observer`

```yaml
apiVersion: svc.dpu.nvidia.com/v1alpha1
kind: DPUService
metadata:
  name: boundary-observer
  namespace: dpf-operator-system
spec:
  helmChart:
    source:
      repoURL: harbor.tempnode-bf3.local/charts
      chart: boundary-observer
      version: 0.1.0
    values:
      export:
        collector: "10.34.20.4:2055"
        version: 9
        sampling: 1
      capture:
        fields: [srcIP, dstIP, proto, srcPort, dstPort, bytes, packets, startTs, endTs]
  serviceDaemonSet:
    labels:
      dpuservice.dpu.nvidia.com/name: boundary-observer
```

### CRD (사용자 API)

```yaml
apiVersion: security.datafacility.io/v1alpha1
kind: BoundaryObserver
metadata:
  name: default
spec:
  scope:
    direction: [ingress, egress]
    protocols: [tcp, udp]
  export:
    netflow:
      collector: "10.34.20.4:2055"
      version: 9
    sampling: 1
  capture:
    fields: [srcIP, dstIP, proto, srcPort, dstPort, bytes, packets, startTs, endTs]
status:
  activeFlows: 12345
  exportedRecords: 987654
```

### 검증 지표

| 지표 | 측정 방법 | 기대 |
|------|-----------|------|
| 완전성 | 합성 트래픽 / flow record 비율 | > 99.9% |
| CPU offload | Hubble vs Observer 동일 트래픽 CPU | DPU 경로 우세 |
| Bypass-proof | non-DTN 경유 시도 → 실패 검증 | 우회 0건 |
| cross-cluster | ScalexPOD A→B 전송 coverage | 100% |

---

## B. DTN Boundary Policy Enforcer (정책)

### 구현 근거 (repo 내 샘플)

| 용도 | 경로 |
|------|------|
| 5-tuple ACL 매칭 (allow/drop) | `DOCA_BF3/samples/doca_flow/flow_acl` |
| stateful connection tracking | `DOCA_BF3/samples/doca_flow/flow_ct_tcp`, `flow_ct_udp` |
| HW drop action | `DOCA_BF3/samples/doca_flow/flow_drop` |
| CIR/CBS rate limiter | `DOCA_BF3/samples/doca_flow/flow_monitor_meter`, `flow_shared_meter` |
| controller 참고 패턴 | `internal/dpuservice/controllers/` (DPF Kubebuilder) |

### DPU Enforcement Pipeline

```text
경계 진입 패킷
  ↓
[Stage 1] flow_acl             — 5-tuple 매칭, miss = drop
  ↓
[Stage 2] flow_ct              — stateful (established only)
  ↓
[Stage 3] flow_monitor_meter   — per-rule CIR/CBS
  ↓
[Stage 4] flow_mirror          → Observer pipeline
  ↓
cluster 내부 or DTN host app (Rucio / XRootD / Iceberg ingest)
```

### Controller (Host, Go — Kubebuilder)

- 프로젝트: `boundary-controller`
- watch: `BoundaryPolicy` CRD
- Reconcile loop
  1. CRD diff 감지
  2. 5-tuple + meter spec 계산
  3. DPU agent gRPC API 호출 (rule add/delete/update)
  4. status (installedRules, activeConnections, lastProgrammed) 업데이트

### DPU Agent (Custom DPUService)

- DPU 측 daemon, gRPC 서버
- controller rule 수신 → `doca_flow` API로 HW rule install
- per-rule counter 조회 → controller 보고
- 기존 DPF DPUService 패턴 (`dpuservices/dummydpuservice/` 참고)로 패키징

### CRD (사용자 API)

```yaml
apiVersion: security.datafacility.io/v1alpha1
kind: BoundaryPolicy
metadata:
  name: rucio-transfer-allow
spec:
  direction: ingress
  from:
    cidrBlocks: ["203.0.113.0/24"]     # known peer facility
  to:
    endpoints:
      - service: rucio-gateway
        namespace: dpf-data
        port: 8443
  action: allow
  rateLimit:
    cir: 10Gbps
    cbs: 1GB
  logging:
    linkObserver: default              # 관측 A와 연계
status:
  installedRules: 1
  activeConnections: 42
  lastProgrammed: "2026-04-17T12:00:00Z"
```

### Balanced Offloading

| 역할 | 위치 | 내용 |
|------|------|------|
| slow path (PDP) | DTN host | Rucio token 검증, TLS cert 검증, tenant authz |
| fast path (PEP) | DPU | 결정된 flow를 HW rule로 설치, 이후 host touch 없이 line-rate 처리 |

### 검증 지표

| 지표 | 측정 방법 | 기대 |
|------|-----------|------|
| Enforcement 정확성 | disallowed flow 주입 → drop 확인 | 100% drop |
| Program latency | CRD apply → DPU rule install | < 100ms |
| Rate limit 정확도 | iperf3 sweep vs CIR 설정 | 오차 < 5% |
| CPU vs Cilium | 동일 policy CiliumNetworkPolicy vs BoundaryPolicy | DPU 경로 < Cilium |
| Rule scale | 10 / 100 / 1000 rule programming latency | linear |

---

## 통합 / 적층

- Observer와 Enforcer는 같은 DPU flow pipeline에 순차 stage
- 단일 CRD group (`security.datafacility.io`) 하에 관리
- 구현 순서:
  1. Cilium 경로 강제 검증 (PoC 기초)
  2. Observer (관측 먼저)
  3. Enforcer (관측 기반 policy 설계)
  4. Transfer 실험 시 암호화 축 합류

## 구성요소 실행 위치 (상세 배치)

### 역할별 위치표

| 구성요소 | 위치 | 이유 |
|---|---|---|
| `BoundaryPolicy` / `BoundaryObserver` CRD | **host cluster API server** (etcd) | 사용자 `kubectl apply` 대상 |
| **Controller (operator)** | **DTN host** (`Deployment` + `nodeSelector: datafacility/role: dtn`, replicas 1) | DPU agent와 로컬 통신, BF3 단일 노드 |
| Slow-path decision logic | **DTN host** (controller 내부 or sidecar) | Rucio/XRootD 인증, cert 검증 등 |
| **DPU Agent** (gRPC 서버) | **DPU cluster** (custom DPUService DaemonSet) | `doca_flow` API는 DPU 안에서만 호출 가능 |
| **boundary-observer** DPUService | **DPU cluster** (DaemonSet) | HW counter / NetFlow exporter는 DPU 로컬 |
| Collector (goflow2) | **host cluster** (DTN host 추천) | DPU에서 오는 NetFlow UDP 수신 |
| Prometheus + Grafana | **host cluster** | 운영자 대시보드 |

### 배치도

```text
┌────── Host Cluster (Cilium) ──────────────────┐
│ [kube-apiserver] ← CRD 저장                    │
│    ↑ kubectl                                   │
│                                                │
│ ┌── DTN host (tempnode-bf3) ──────────────┐    │
│ │  [BoundaryController Pod] ← operator    │    │
│ │    ├ watch CRD                          │    │
│ │    ├ slow-path decision                 │    │
│ │    └ gRPC → DPU Agent                   │    │
│ │  [goflow2 + Prometheus + Grafana]       │    │
│ └──────┬──────────────────┬───────────────┘    │
└────────┼──────────────────┼────────────────────┘
         │ gRPC rules       │ NetFlow UDP
         ▼                  ▲
┌── DPU Cluster (BF3 Kamaji tenant) ─────────────┐
│ [DPU Agent DaemonSet]                          │
│   └ doca_flow API로 HW rule install            │
│ [boundary-observer DaemonSet]                  │
│   └ HW counter → NetFlow v9 송신               │
│ ──── DPU HW Pipeline ────                      │
│  [ACL] → [CT] → [Meter] → [Mirror] → fwd       │
└────────────────────────────────────────────────┘
```

### Controller 배포 형태

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: boundary-controller
  namespace: datafacility-security
spec:
  replicas: 1
  selector:
    matchLabels:
      app: boundary-controller
  template:
    metadata:
      labels:
        app: boundary-controller
    spec:
      nodeSelector:
        datafacility/role: dtn        # DTN host 고정
      serviceAccountName: boundary-controller
      containers:
        - name: controller
          image: harbor.tempnode-bf3.local/datafacility/boundary-controller:0.1.0
          args:
            - --leader-elect=false
            - --dpu-agent-endpoint=<DPU agent service endpoint>
```

### 두 plane의 명시적 분리

- **Decision plane (host)** — Kubernetes API + controller. declarative + 유연
- **Enforcement plane (DPU)** — HW flow pipeline. line-rate + bypass-proof
- Cilium(eBPF 단일 레이어)과 달리 **두 plane을 분리**
- 효과: host compromise 독립성 + HW offload 병존

### 기능별 단독/협업 구분

| 기능 | 작동 방식 |
|------|----------|
| **Observer** | **DPU 단독** — HW counter + NetFlow export가 DPU 안에서 완결 |
| **Enforcer** | **DPU + DTN 협업** — host = decision, DPU = enforcement |

### 미래 multi-DTN 확장

- DTN-DPU 쌍이 여러 개 되면 controller replicas 복수 + leader election (`--leader-elect=true`)
- 각 controller는 자기 DTN의 DPU agent와만 통신
- 정책은 global CRD, enforcement는 per-DPU 지역화

## 권장 구현 방식

```text
BoundaryPolicy / BoundaryObserver CRD
      ↓ (watch)
Host controller + slow-path decisions (DTN host)
      ↓ (gRPC)
DPU Agent (custom DPUService)
      ↓ (doca_flow API)
DPU HW pipeline: [ACL] → [CT] → [Meter] → [Mirror/Export]
      ↓
NetFlow / counters → collector → Prometheus/Kafka → Grafana
```

## 현재 단계 평가

- sharing과 함께 가장 강한 축
- 관측 축 — DTS + NetFlow exporter 조합으로 **즉시 시작 가능**
- 정책 축 — CRD + controller + DPU agent 커스텀 개발 필요
- 암호화 축 — Transfer 실험에서 합류 (psp_gateway / ipsec_security_gw 샘플 기반)

# 현재 구조 기준 구현 우선순위

## 1순위

### 공유 + 보안

이유

- 현재 DTN + BF3 구조와 가장 잘 맞음
- boundary 역할과 자연스럽게 연결됨
- DPU의 차별점을 가장 빨리 보여줄 수 있음

추천 조합

- host: `Rucio` 또는 `XRootD` gateway
- DPU: `DTS` + `transfer-assist-service`

## 2순위

### 저장

추천 조합

- host: Iceberg / lakehouse stack
- DPU: ingest preprocess / storage assist

## 3순위

### 처리

추천 조합

- DPU: preprocess / compress / checksum / metadata hint service
- host: real compute orchestration

# 구현 가능성 매트릭스

| 영역 | Built-in DPF 도움 | Custom DPUService 필요성 | 현재 구조 적합도 | 추천 우선순위 |
|---|---|---:|---:|---:|
| 처리 | 낮음 | 높음 | 중간 | 3 |
| 저장 | 중간 | 중간 | 높음 | 2 |
| 공유 | 낮음~중간 | 높음 | 매우 높음 | 1 |
| 보안 | 중간(DTS) | 중간~높음 | 매우 높음 | 1 |

# 추천 구현 로드맵

## Phase 1 — 보안/관측 기반 확보

- `doca-telemetry-service` 배포
- BF3 metrics / counters / visibility 확보
- Prometheus 또는 Kafka/Influx export 검토

## Phase 2 — 공유 PoC

- host side:
  - `Rucio` 또는 `XRootD`
- DPU side:
  - `transfer-assist-service`
  - telemetry / integrity / visibility

## Phase 3 — 저장 PoC

- host side:
  - Iceberg / lakehouse stack
- DPU side:
  - ingest organize / classify / validate service

## Phase 4 — 처리 보조 PoC

- compression / checksum / staging support
- metadata extraction

# 지금 강하게 주장해도 되는 것

- DPU lifecycle cloud-native management
- selective DTN-DPU deployment
- sharing/transfer boundary acceleration 가능성
- boundary security / telemetry / visibility
- storage ingest assist 가능성

# 지금 보수적으로 써야 하는 것

- full-cluster network offload
- OVN/full replacement
- facility-wide east-west security
- compute acceleration as current major contribution

# 잠정 결론

현재 코드와 클러스터 구조를 함께 보면, 가장 현실적인 구현 방향은 다음이다.

> **Host cluster는 Cilium 기반 일반 구조를 유지하고, DTN에 장착된 BF3를 DPF로 cloud-native하게 관리한 뒤, 그 위에 built-in DPUService와 custom DPUService를 조합하여 sharing/transfer와 boundary security를 먼저 구현하고, 이후 storage ingest assist와 processing support로 확장하는 방식**

즉 현재 구조에서

- 가장 강한 연구축은 `sharing + security`
- 다음 축은 `storage ingest assist`
- `processing` 은 보조적 / 간접적 역할

으로 보는 것이 가장 현실적이다.
