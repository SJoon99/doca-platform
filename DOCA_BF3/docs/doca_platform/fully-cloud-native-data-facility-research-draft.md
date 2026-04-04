---
title: Fully Cloud Native Data Facility Research Draft
---

[TOC]

# 목적

- 현재 구상 중인 연구 아이디어를 한 문서로 정리
- 이 연구가 **무엇을 주장하는지 / 무엇은 아직 주장하지 않는지** 명확히 분리
- 이후 제목, 초록, 기여점, 실험 계획으로 발전시키기 위한 기반 초안 작성

# 연구 주제

## 한 줄 주제

**Fully Cloud Native Data Facility**

- 처리
- 저장
- 공유
- 보안

을 모두 클라우드 네이티브하게 다루는 데이터 시설 아키텍처

## 핵심 아이디어

이 연구의 핵심은 단순히 Kubernetes 위에 데이터 시설을 올리는 것이 아니다.

핵심은:

- **DPU 자체를 cloud-native resource로 편입**
- 그리고 그 DPU를 데이터 시설의 boundary node에서
  - 외부 데이터 유입
  - 외부 데이터 반출
  - 저장 전 정리
  - 보안 / 관측 / 정책 집행
에 활용하는 것

즉 이 연구는

- “모든 노드에 DPU를 두는 full-cluster DPU architecture”
가 아니라
- **특정 DTN(Data Transfer Node)에 장착된 DPU를 cloud-native하게 운영하면서, 데이터 시설 전체의 ingest/export 및 boundary plane을 고도화하는 구조**

를 제안한다.

# 문제 정의

기존 데이터 시설은 보통 다음이 느슨하게 결합되거나 분리되어 운영된다.

- 처리 인프라
- 저장 인프라
- 공유/전송 인프라
- 보안 인프라

그리고 DPU가 있더라도 종종

- 고정 appliance
- 독립 장비
- 특수 네트워크 노드

처럼 취급되어, Kubernetes 운영 모델과 완전히 결합되지 못하는 경우가 많다.

이 연구가 던지는 질문은 다음과 같다.

> **DPU를 Kubernetes 기반 운영 모델 안으로 cloud-native resource로 편입시키고, 그 DPU를 장착한 DTN(DPU-equipped Data Transfer Node)을 선택적으로 배치하는 것만으로도 데이터 시설의 처리/저장/공유/보안 경계 계층을 효과적으로 고도화할 수 있는가?**

# 이 연구의 메인 claim

## 메인 claim

이 연구의 메인 claim은 다음이다.

> **DPU 자체를 cloud-native resource로 편입할 수 있다.**

여기서 현재 문서에서 말하는 `cloud-native resource` 의 의미는 우선 다음 수준이다.

- provisioning
- lifecycle management
- orchestration
- declarative management

즉 현재 단계의 기여는 **운영 관리 수준의 cloud-native integration** 이다.

## practical differentiator

이 연구의 실용적 차별점은 다음이다.

> **모든 host node에 DPU를 요구하지 않고도, DPU를 장착한 특정 DTN 노드만 선택적으로 추가하는 방식으로 기존 데이터 시설을 현실적으로 고도화할 수 있다.**

즉 이 연구는

- full-cluster DPU architecture를 요구하지 않음
- host cluster 전체 네트워크 구조를 DPU 중심으로 재설계하는 것을 전제로 하지 않음
- 기존 환경 위에 **선택적 DPU 도입** 으로 점진적 고도화를 시도함

# 현재 전제와 제약

## 현재 환경 전제

- host cluster는 일반 Kubernetes cluster
- host cluster networking은 `Cilium` 기반
- BF3 DPU는 host cluster의 특정 노드 하나에만 장착
- 그 노드는 `DTN(Data Transfer Node)` 역할
- 현재는 `DTN = data transfer dedicated node`

즉 구조상

- 모든 worker node에 DPU가 존재하지 않음
- DPU는 오직 `DTN(tempnode)` 에만 존재
- 따라서 DPU의 가장 강한 기여점은 cluster 내부 전체가 아니라 **외부-내부 경계(boundary)** 에 위치함

## 현재 전제가 가지는 의미

이 구조에서는 DPU의 역할이 자연스럽게 다음 쪽으로 집중된다.

- ingest / export
- transfer acceleration
- boundary security
- 저장 전 정리 / 분류 / 관측

반대로, 모든 compute node에 DPU가 있는 구조처럼

- cluster 내부 전체 data plane offload
- ubiquitous compute acceleration

을 바로 주장하기는 어렵다.

# 제안 아키텍처

## 큰 그림

```text
External World
  |
  |  ingest / export / large-scale transfer
  |
DTN with BF3 DPU
  |
  +-- transfer gateway
  +-- boundary security point
  +-- ingest pre-processing point
  +-- cloud-native DPU lifecycle target
  |
Host Kubernetes Cluster
  |
  +-- DPF-managed DPU provisioning
  +-- DPUCluster lifecycle management
  +-- tenant control-plane management
  |
Data Facility
  +-- Processing
  +-- Storage
  +-- Sharing
  +-- Security
```

## 현재 구현 기반

현재 실험 환경에서는 이미 다음이 검증되었다.

- host cluster 위에서 `DPF` 동작
- `Kamaji` 기반 `DPUCluster` 생성
- BF3 DPU provisioning 완료
- BF3가 tenant DPU cluster의 worker node로 join 완료

즉 이 연구는 아직 아이디어만 있는 상태가 아니라,

- **cloud-native DPU lifecycle**
- **DPU cluster 생성**
- **BF3 worker join**

까지는 이미 동작하는 기반 위에서 확장되는 연구이다.

참고 문서

- `DOCA_BF3/docs/doca_platform/dpf-current-cluster-and-dpu-topology.md`
- `DOCA_BF3/docs/doca_platform/dpf-current-cluster-topology-excalidraw.md`

# 왜 DPU를 cloud-native하게 다루는가

이 문서에서 “DPU를 cloud-native하게 만든다”는 뜻은 다음이다.

- DPU provisioning 이 선언적으로 수행됨
- DPU lifecycle 이 Kubernetes 리소스로 관리됨
- DPU가 별도 black-box 장비가 아니라 cluster-managed component가 됨
- DPU cluster / DPU worker / 관련 서비스가 orchestration 대상이 됨

즉 DPU를

- 단순 NIC / accelerator
가 아니라
- **데이터 시설의 운영 가능한 cloud-native execution substrate의 일부**

로 다루는 것

다만 현재 단계에서 이 의미는 우선

- 관리
- 배포
- lifecycle

수준이며,

향후 실제 DPU 애플리케이션이 배포된다면

- 실행 수준의 cloud-native DPU platform

으로 확장될 수 있다.

# 연구의 4개 축

## 1. Processing

### 현재 문제 인식

대규모 데이터 처리 환경

- HPC
- AI
- HPDA

에서 DPU가 항상 직접 compute acceleration을 제공하는 것은 아니다.

현재 구조처럼

- DPU가 DTN에만 존재
- compute node마다 DPU가 없는 경우

에는 DPU가 내부 계산 그 자체를 직접 가속하는 역할은 제한적이다.

### 현재 문서에서의 위치

따라서 현재 문서에서 processing 축은

- compute 자체의 직접 가속
보다는
- **compute 전에 필요한 data preparation / staging / pre-processing 지원**

쪽에 더 가깝다.

예상 가능한 기여

- 데이터 유입 시 사전 정리
- 전처리
- routing / classification
- staging
- metadata hint 생성

### 현재 판단

- processing은 중요한 축이지만
- 현재 구조에서는 **주력 contribution** 으로 바로 내세우기보다
- **보조적 / 간접적 기여 가능성** 으로 두는 것이 더 정확하다

## 2. Storage

### 방향

`Iceberg` 기반 데이터 레이크하우스 구축

핵심 아이디어

- 외부에서 대용량 데이터 유입
- DTN(BF3 DPU)에서 1차 정리/분류/검사
- 그 뒤 Data Lakehouse 저장 계층으로 반입

### 연구 질문

> DTN(DPU)를 저장 전 ingest 정리 계층으로 활용하면 데이터 레이크하우스를 더 안정적이고 쉽게 구축할 수 있는가?

### 예상 가능한 역할

- ingest endpoint
- landing zone pre-processing
- metadata enrichment
- dataset organization
- partitioning hint
- format / manifest 정리 보조

### 현재 판단

- storage 축은 현재 구조와 잘 맞는다
- DTN 중심 아키텍처와 자연스럽게 연결된다
- sharing 다음으로 강한 연구 축이 될 가능성이 높다

## 3. Sharing

### 방향

대규모 데이터셋 공유 / 전송

후보 스택

- `Globus`
- `Rucio`

후보 프로토콜

- `GridFTP`
- `XRootD`

### 왜 현재 구조와 잘 맞는가

이 축은 현재 구조에서 DPU가 가장 직접적으로 기여할 가능성이 높은 영역이다.

예상 가능한 기여

- transfer endpoint 가속
- transfer path optimization
- encryption offload
- integrity verification
- checksum handling
- large-scale ingress / egress support

### 연구 질문

> 대규모 scientific data transfer 경로에서 DPU를 boundary transfer accelerator로 활용할 수 있는가?

### 현재 판단

- sharing / transfer는 현재 구조와 가장 잘 맞는 **주력 실험축**

## 4. Security

### security의 위치

이 문서에서 security는

- 단순 부가 기능이 아니라
- **Data Facility의 4번째 pillar**

로 둔다.

다만 현재 문서에서 security의 main scope는

- **DTN / DPU boundary security**

이다.

즉 security를 시설 전체의 완전한 runtime security로 주장하지 않고,

- 외부-내부 경계에서
- DPU가 가장 잘 할 수 있는 security function

에 집중한다.

### security에 포함되는 것

- telemetry / flow visibility
- ingress / egress policy enforcement
- DPU-based isolation
- filtering

### security에 일부러 포함하지 않는 것

아래 항목은 현재 문서에서 security의 main scope로 주장하지 않는다.

- facility-wide east-west security
- cluster 내부 전체 마이크로세그멘테이션
- 모든 workload runtime security

### research question

> DTN 경계에 장착된 DPU를 활용해 데이터 시설 boundary security를 cloud-native하게 강화할 수 있는가?

### 현재 판단

- security는 현재 구조에서 “가장 자연스러운 4번째 축”
- sharing과 강하게 연결되지만, 동일 축으로 합치기보다는 별도 pillar로 유지하는 것이 더 명확하다

# 이 연구가 주장하지 않는 것

## explicit non-goals

현재 문서는 아래를 main claim으로 삼지 않는다.

### 1. full-cluster DPU architecture

- 모든 host node에 DPU를 두는 구조
- DPU가 cluster 전역에 깔린 전제

### 2. OVN / DOCA 기반 full network replacement

- 일반 CNI 기반 host cluster를 전부 DPU-aware network stack으로 치환하는 방식
- cluster 전체 data plane replacement

이 문서의 방향은 이와 다르다.

- full replacement가 특정 환경에서는 유효할 수 있음을 부정하지 않음
- 다만 본 연구는 **selective DTN-DPU architecture** 에 초점을 둔다

### 3. facility-wide east-west security를 현재 실험 범위로 주장하는 것

- 내부 모든 service-to-service traffic 보안
- cluster 전체 runtime segmentation

은 현재 문서의 main scope가 아님

# 현재 contribution 과 future extension

## current contribution

현재 문서에서 가장 강하게 주장할 수 있는 것은:

- DPU를 cloud-native resource로 편입
- DPU lifecycle / provisioning / orchestration을 Kubernetes 운영 모델 안에 통합
- DTN boundary node 중심으로 ingest / export / transfer / security 기능을 강화

## future extension

향후 확장 가능한 방향

- execution-level DPU applications
- facility-wide security
- 더 넓은 processing acceleration
- multiple DPU / multiple DTN architecture

즉 현재 문서는

- “완전한 최종 아키텍처 완성”
보다
- **현실적인 현재 기여와 설득력 있는 확장 경로**

를 같이 제시한다.

# 왜 일반 Cilium 기반 host cluster를 유지하는가

현재 host cluster는 `Cilium` 기반 일반 CNI 구조를 유지한다.

이유

- DPU가 모든 노드에 장착된 구조가 아님
- 오직 `DTN(tempnode)` 에만 BF3 존재
- 따라서 cluster 전체를 DPU 중심 네트워크 모델로 바꾸는 것은 현재 목적과 맞지 않음
- 일반 cluster는 일반 cluster대로 유지하고
- DPU는 boundary node / DTN 역할에서 집중 활용하는 것이 더 현실적

즉 이 연구는

- “모든 노드를 DPU cluster로 치환하는 연구”가 아니라
- “일반 host cluster 위에서 selective DTN-DPU를 통해 data facility boundary를 고도화하는 연구”

이다.

# 현재 DPU의 위치와 의미

## 현재 의미

`DPU-equipped DTN`

- 데이터 시설 내부와 외부를 잇는 경계 노드
- ingest / export / transfer / pre-storage organization의 중심

즉 현재 구조에서 BF3는

- 내부 compute accelerator
라기보다
- **boundary data accelerator / transfer-security-organization node**

에 더 가깝다.

## 현재 해석

따라서 이 연구의 현실적인 메시지는 다음이다.

> DPU는 데이터 시설 내부 모든 연산을 가속하는 장치가 아니라, 데이터 시설의 boundary plane에서 transfer / ingest / security / organization을 cloud-native하게 담당하는 핵심 가속 자원이다.

# 연구 질문

## RQ1

단일 DTN에 장착된 DPU만으로도 데이터 시설의 ingest / export 경계에서 의미 있는 cloud-native 가치를 만들 수 있는가?

## RQ2

Rucio / Globus / GridFTP / XRootD 와 같은 대규모 데이터 공유 스택에서 DPU는 어떤 기능을 실제로 오프로딩할 수 있는가?

## RQ3

Iceberg 기반 데이터 레이크하우스 구축 시, DPU를 ingest 전 정리 계층으로 사용하면 운영 및 성능 측면에서 어떤 이점이 있는가?

## RQ4

현재 구조에서 DPU는 processing 직접 가속보다 data preparation / transfer orchestration / boundary security 에 더 적합한가?

# 평가 항목 초안

## sharing / transfer

- throughput
- CPU 사용률 감소
- transfer latency
- checksum / crypto 처리 비용 변화
- ingest / export 동시 처리량

## storage / ingest

- 데이터 유입 후 정리 시간
- metadata 생성 시간
- 데이터 적재 파이프라인 단순화 정도
- 운영 복잡도 감소

## processing support

- staging 시간 감소
- preprocessing 시간 감소
- compute job 시작 전 준비 비용 감소

## security

- flow visibility 수준
- policy enforcement 가능 범위
- isolation / filtering 적용 가능성
- boundary security observability 향상 여부

## cloud-native 운영성

- 선언적 배포 가능 여부
- 재현성
- 장애 복구 용이성
- DPU lifecycle 자동화 수준

# 현재 한계

- DPU가 1개뿐
- DPU가 DTN 한 곳에만 존재
- compute node에 DPU가 없음
- 내부 분산 compute acceleration은 제한적
- 일부 protocol / offload 기능은 실제 BF3 + DOCA capability 검증이 더 필요
- 현재 DPU 활용은 boundary-centric architecture에 가깝다

# 잠정 결론

현재 가장 설득력 있는 방향은 다음과 같다.

- **main claim**
  - DPU를 cloud-native resource로 편입
- **practical differentiator**
  - full-cluster DPU를 요구하지 않고, 특정 DTN 노드에만 DPU를 선택적으로 배치
- **main functional focus**
  - sharing / transfer
  - storage ingest preparation
  - boundary security
- **processing**
  - 직접 compute acceleration보다 간접적 data preparation support 관점

즉 이 연구의 중심은

> **DPU-enabled DTN을 통해, 일반 host cluster 위에서 데이터 시설의 ingest / export / transfer / boundary security plane을 cloud-native하게 고도화하는 것**

이다.

# 다음 단계

## 1. 제목 다듬기

- 논문 제목 수준 한 줄
- subtitle 가능 여부 검토

## 2. 초록형 요약 만들기

- 문제
- 접근
- 차별점
- 기대 효과

## 3. 실험축 우선순위 정하기

추천 순서

1. Sharing / Transfer
2. Storage / Ingest
3. Security
4. Processing support

## 4. capability mapping

BF3/DOCA가 실제로 할 수 있는 기능을 분류

- telemetry
- flow visibility
- policy enforcement support
- crypto / checksum / integrity support
- transfer acceleration

## 5. PoC 범위 정의

예시

- `Rucio + XRootD + DTN(BF3)`
- `Iceberg ingest pipeline + DTN preprocessing`
- `boundary telemetry / flow visibility prototype`

# 한 줄 요약

이 연구는 **일반 Cilium 기반 host cluster 위에, DTN에 장착된 BF3 DPU를 cloud-native하게 관리하고, 이를 데이터 시설의 처리/저장/공유/보안 중 특히 ingest/export와 boundary plane에 활용하는 fully cloud-native data facility architecture** 를 제안한다.
