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

## 현재 구조에서의 위치

보안은 현재 구조에서

- `facility-wide runtime security`
가 아니라
- **DTN / DPU boundary security**

로 두는 것이 가장 맞다.

## 현재 DPF 기반 구현 가능성

| 항목 | 판단 |
|---|---|
| telemetry built-in 서비스 존재 | 예 |
| active security enforcement built-in | 제한적 |
| custom DPUService 확장 가능 | 높음 |
| 현재 구조 적합도 | 매우 높음 |

## 권장 기술

### 바로 가능한 후보

- `dpuservices/dts/DPUService.yaml`
- DOCA Telemetry Service (DTS)

### 확장 후보

- `telemetry_exporter.c`
- `samples/doca_flow/`
- policy / filtering용 custom DPUService

## 구현 아이디어

### A. Security observability first

가장 먼저 할 것

- DTS 배포
- sysfs / ethtool counters 수집
- Prometheus endpoint 노출
- DTN 경계 traffic visibility 확보

### B. Flow visibility / export

custom DPUService

- NetFlow/IPFIX 유사 export
- ingress/egress flow telemetry
- suspicious / large flow detection

### C. Policy enforcement / filtering

custom DPUService

- ingress / egress policy enforcement
- ACL / filtering
- protocol-aware gatekeeping
- DPU-based isolation

## 권장 구현 방식

```text
External boundary traffic
  -> DTN
      -> DPU telemetry service
      -> DPU policy/filter service
      -> host-side gateway / application
```

## 현재 단계 평가

- sharing과 함께 가장 강한 축
- 특히 `DTS` 는 지금 바로 시작 가능한 built-in foothold
- active enforcement는 실제 data path 연동을 더 확인해야 함

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
