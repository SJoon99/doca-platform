# Fully Cloud-Native Data Facility — Related Work Survey (통합)

연구 주제 · **"Design of an Enhanced Fully Cloud-Native Data Facility via a DPU-Augmented Data Transfer Node Approach"**

작성일 · 2026-04-19 · 4 pillars 통합판 (Processing · Storage · Sharing · Security)

---

## Executive Summary

### 본 연구 한 줄 정의
> **일반 Cilium 기반 host cluster 위에서, BF3 DPU를 장착한 DTN 하나만으로 Data Facility의 처리/저장/공유/보안 경계 계층을 cloud-native하게 고도화**

### 조사 결과 핵심 요지
1. **정확히 같은 통합 연구는 존재하지 않음** — 4 pillar의 개별 구성 요소 연구는 풍부하나, 이를 "selective DTN-DPU" 하나로 묶은 학술 연구는 아직 공백
2. **각 pillar별 관련 선행 연구 성숙도 편차 큼**
   - Processing/Storage — 업계 리포트 중심, 학술 연구는 선별적
   - Sharing — 10년 이상 축적 (GridFTP 하드웨어 오프로딩 등 강력한 precedent)
   - Security — 2024–2025 가장 활발 (P4-perfSONAR, Cilium→DPU 오프로드 등)
3. **본 연구 positioning** — 각 pillar의 개별 연구를 **하나의 cloud-native DTN-centric 아키텍처로 통합**하는 시스템 레벨 기여

### 4 Pillar 커버리지 요약

| Pillar | 상태 (draft) | 관련 연구 풍부도 | 직접 선행 |
|--------|-----------|--------------|---------|
| Processing | Pending (정지) | ★★★☆☆ | DOCA UROM · BluesMPI · in-network aggregation |
| Storage | On-going (진행중) | ★★★★☆ | Iceberg lakehouse · NVMe-oF offload · ABoF (LANL) · LineFS / DPFS |
| Sharing | On-going (진행중) | ★★★★★ | Hardware-assisted GridFTP · SENSE/AutoGOLE · Rucio+FTS+XRootD |
| Security | Up-coming (진행예정) | ★★★★★ | P4-perfSONAR · Palladium · P4-NIDS · Cilium→DPU |

---

## 연구 주제 포지셔닝 — 여러 학술 영역의 교차점

```
      Processing (in-network compute)
             │
Storage ─────┼───── Sharing (DTN · GridFTP · Rucio)
             │
       Security (P4-perfSONAR · zero-trust)
             │
   ╔═════════▼═════════╗
   ║  Cloud-Native DPU ║  ← 본 연구의 통합 자리
   ║  (DPF · K8s CRD)  ║
   ╚═══════════════════╝
```

본 연구는 **4 pillar × cloud-native 관리 축**이 교차하는 지점. 각 축은 독립적으로 활발히 연구되지만 통합은 공백.

---

## Pillar 1 — Processing (처리)

### 상태: 본 연구에서 "Pending(정지)" — compute acceleration 직접 주장은 하지 않음

### 선행 연구

#### 학술 논문 · 프리프린트

**"A Survey on Heterogeneous Computing Using SmartNICs and Emerging Data Processing Units"** (arxiv 2504.03653, 2025, 확장판 v2)
- **관련성: ★★★★★** · DPU/SmartNIC 영역 전반 포괄
- 분류 체계
  - 하드웨어 진화 — NIC → Offload NIC → SmartNIC → DPU / on-path vs off-path
  - 프로그래밍 프레임워크 — DOCA, DPDK/SPDK, MPI, OpenMP, gRPC, P4
  - 병렬 모델 — 공유 메모리(OpenMP), 분산 메모리(MPI), SIMD, pipeline
- Covered 주요 시스템
  - **BluesMPI** — MPI collective DPU 오프로드, all-to-all 44% 개선
  - **iPipe** — actor 기반 load-balanced application offloading
  - **IO-TCP** — TCP stack host-SmartNIC 분할
  - **LineFS / DPFS / Hyperion** — 분산 파일시스템 DPU 오프로드
  - **Runway** — in-transit 데이터 compression/transformation
  - **PsPIN / DORM / DPDPU** — packet-processing / load-balancing 프레임워크
- 식별된 연구 gap
  - HPC offloading with DPU as low-power accelerator
  - 정교한 halo exchange
  - in-network runtime 소프트웨어 개발
  - 클러스터 관리 · 성능 디버깅 도구
  - 표준화 (벤더별 SDK 파편화)

**"OpenSHMEM Performance on Bluefield-3 Data Processing Units"** (ACM PEARC 2025)
- BF3에서 OpenSHMEM 분산 메모리 모델 성능 평가
- 본 연구 관련성 — 직접 매칭은 아니나 "BF3 학술적 성능 평가" 선례

**"DOCA UROM: A Vehicle for Offloading HPC and AI to DPUs"** (Springer 2025)
- UROM = Unified Resource and Offload Manager · NVIDIA 공식 프레임워크
- HPC/AI 병렬 태스크를 host → DPU로 **선언적 오프로드** 가능
- 본 연구와의 관련성 — DPU를 compute accelerator로 쓰는 공식 경로. 본 연구는 이 경로를 **사용하지 않음** (boundary-centric, in-network compute 제외)

**"Geospatial Filter and Refine Computations on NVidia Bluefield DPU"** (NSF PAR 10515880)
- BF3를 spatial 연산 오프로드 target으로 활용
- DPU가 query 데이터 filter → host가 refine 파이프라인
- 본 연구 관련성 — 간접적 (전처리 사례)

**"In-Network AllReduce Optimization with Virtual ..."** (ACM 2024) · **OmNICCL** (SIGCOMM NAIC 2024)
- SmartNIC에서 AllReduce/collective offload
- OmNICCL은 Direct Cache Access로 sparse AllReduce 7.24× 가속
- 본 연구 관련성 — 내부 compute에 DPU 활용 (본 연구는 경계 배치로 이 방향은 아님)

**"OptimusNIC: Offloading Optimizer State to SmartNICs"** (ACM 2025)
- DL training에서 optimizer state를 SmartNIC 메모리로 오프로드

#### 산업계 사례

- **LANL ABoF (Accelerated Box of Flash)** — LANL이 구축한 DPU 기반 스토리지 시스템. 기존 대비 최대 **30× 빠름**. in-storage computing + InfiniBand 가속
- **Georgia Tech × Sandia** — BF2 DPU로 molecular dynamics 알고리즘 20% 가속
- **LBNL** — SSCA1 workload (genomics, graph analytics, ML preprocessing)에 DPU preprocessing 활용

### 본 연구와의 관계

- 본 연구는 Processing pillar를 **Pending**으로 명시적 제외 — DPU가 DTN에만 있어 내부 compute 가속 제한
- 그러나 **data preparation / staging / pre-processing** 측면에서 일부 기여 가능성 열어둠
- 위 survey 시스템들은 "DPU를 compute용으로 사용"하는 패러다임 — 본 연구는 **"DPU를 boundary용으로 사용"** 패러다임 → 상호 보완적 다른 선택

### Processing 관점 Gap

> **Selective DTN-DPU 구조에서 compute pillar의 기여 한계/가능성을 정식화**한 연구는 없음 — 본 연구가 "Pending" 결정을 내린 이유 자체가 학술적 논의 가치 있음

---

## Pillar 2 — Storage (저장)

### 상태: 본 연구에서 "On-going(진행중)" — Iceberg lakehouse + DPU-assisted ingest

### 선행 연구

#### 데이터 레이크하우스 아키텍처 (추상 계층)

**Apache Iceberg + Cloud-Native 데이터 플랫폼**
- **Salesforce Data Cloud** — 4M tables · 50 PB Iceberg 운영 사례
- **Databricks Iceberg v3 Public Preview** (2025)
- **Springer Applied Sciences** "Building a modern data platform based on the data lakehouse architecture" (2025) · 학술적 Iceberg 기반 플랫폼 평가
- 3-layer 모델 — **Catalog · Metadata · Data** 계층
- 핵심 기능 — schema evolution · hidden partitioning · ACID · time travel
- 파이프라인 스택 — **K8s + Argo Workflows + Dremio + MinIO** 조합 사례 (Google Gist)

**Storage Optimizer / SNCE 패턴**
- Reactive Storage Native Change Event system — Iceberg table 변경 감지 시 optimization 트리거

#### DPU 기반 스토리지 오프로드

**NVIDIA BlueField-3 스토리지 기능** (공식)
- NVMe/TCP · NVMe-oF target 오프로드 (하드웨어 수준)
- **BlueField SNAP** — 원격 NVMe를 로컬처럼 노출
- 암호화 · 압축 · 가상화 · deduplication — HW offload
- SSD ↔ 네트워크 포트 직접 데이터 이동 (CPU bypass)

**"Accelerated Box of Flash (ABoF)"** (LANL)
- DPU + InfiniBand 기반 스토리지 시스템
- Linux 파일시스템의 성능 크리티컬 부분 가속
- 기존 대비 **최대 30× 빠름**
- 근접 연산으로 데이터 이동 최소화

**Supermicro JBOF** (BF3 DPU 탑재 all-flash 스토리지)
- DPU가 CPU + 메모리 + NIC 역할을 통합 대체
- NVMe-over-Fabric target 오프로드
- 네트워크-CPU 데이터 복사 제거

**Xinnor Disaggregated Storage on DPU** — 산업계 상용 구성 사례

**"BlueField-4 DPU and KV-Cache Context Memory Storage"** (CES 2026, chiplog.io)
- 차세대 BF4의 KV-Cache 특화 아키텍처 분석

#### 분산 파일시스템 · 스토리지 on DPU (survey 파생)

- **LineFS** — persistent memory 분산 파일시스템 on SmartNIC
- **DPFS** — host 파일시스템을 DPU로 가상화
- **SmartDS** — FPGA 프로토타입, 메시지 header/body 분리
- **Hyperion** — DPU-primary 분산 NVMe 스토리지
- **PEDAL · D2Comp** — 압축 가속

#### 데이터 Lineage · Provenance (Storage pillar와 연결)

**OpenTelemetry Data Lineage 논의** (github issue #3447)
- OTEL 스펙에서 data lineage/provenance 모델링 논의 진행 중
- 본 연구 관련성 — DPU에서 capture한 network metadata가 provenance 소스로 기여 가능

**"Data Lineage and Provenance for Trustworthy AI Pipelines"** (CodeEcstasy)
- AI 파이프라인 trustworthiness에 lineage 핵심
- 캡처 → 저장 → 분석 3단계 구조

### 본 연구와의 관계

- 본 연구는 **Iceberg lakehouse ingest layer**에 DPU를 배치하는 구조
  - Raw/Landing — DPU가 line-rate ingress, checksum, flow telemetry, 전송 메타데이터 캡처
  - Bronze *(lightweight)* — Host DTN이 format 변환, schema 적용, DPU 메타데이터 기반 provenance enrichment
- 기존 연구는 **"DPU가 스토리지 그 자체의 속도를 높임"**이 주류 (NVMe-oF, ABoF 등)
- 본 연구는 **"DPU가 스토리지로 가는 데이터의 경계 정리자 역할"** — 접근 각도가 다름

### Storage 관점 Gap

> **DPU-captured network metadata를 Iceberg manifest의 provenance 필드로 자동 주입**하는 구조는 학술적으로 탐색 안 됨. 본 연구가 새롭게 제시할 수 있는 지점

---

## Pillar 3 — Sharing (공유)

### 상태: 본 연구에서 "On-going(진행중)" — Rucio + GridFTP/XRootD 대상 프로토콜

### 선행 연구

#### 과학 데이터 관리 플랫폼

**"Rucio: Scientific Data Management"** (EPJ Research Infrastructures 2019, Springer)
- 원래 ATLAS 실험용 → LHC 전체 실험 + 다른 과학 커뮤니티로 확장
- **2024년 중반 기준 ATLAS 데이터 1 EB 이상 관리** (2018년 대비 2배)
- 120+ 데이터센터 글로벌 분산
- 연간 **4 EB 이상 데이터 접근/전송** 오케스트레이션

**"Data Management System Analysis for Distributed Computing Workloads"** (arxiv 2510.00828, 2025)
- **2025 facility-scale 분석** — **DTN over-utilization** 발견. 중간 크기 전송 증가 추세. **DTN 최적화의 지속적 중요성 강조**
- 본 연구와의 관련성 — DTN 최적화 수요를 학술적으로 확인한 **최신 근거**

**"Rucio: Scientific Data Management"** (arxiv 1902.09857) — 기술 논문 전문

**RTN-032: Panda/Rucio Multi-site Configuration** — Rubin Observatory의 multi-site 운영 기술 노트

#### 데이터 전송 프로토콜 · DTN 가속

**"Long-haul Secure Data Transfer using Hardware-assisted GridFTP"** (FGCS 2015, Argonne/MCS)
- **관련성: ★★★★★** · 본 연구와 가장 가까운 역사적 precedent (2015)
- **SmartNIC에 UDT + iWARP + OpenSSL 오프로드**
  - UDT XIO driver on GridFTP · UDT offload engine on card
  - iWARP XIO driver on SmartNIC iWARP stack
  - OpenSSL offload → TLS 라인레이트 유지하며 host CPU 미소모
- 평가 — 100ms latency까지 **라인레이트 유지 · 서버 활용률 절감**
- 본 연구와의 관계 — **"DPU가 GridFTP 가속"**이라는 거의 같은 기여. 2015년 SmartNIC 수준 → 본 연구는 2025년 BF3 수준으로 재평가
- 필수 인용 논문

**"Enhancement of GridFTP through Hardware Offloading"** (SC workshops, scinet.supercomputing.org)
- GridFTP 하드웨어 오프로드 초기 연구

**"Rearchitecting the TCP Stack for I/O-Offloaded Content Delivery"** (NSDI 2023, Harvard)
- IO-TCP — TCP stack I/O 오프로드 방식
- 본 연구 관련성 — 간접적 · "host TCP를 건드리지 않고 offload"의 방법론

#### SDN + 데이터 전송 통합

**"AutoGOLE/SENSE: End-to-End Network Services"** (SC23 NRE-014)
- ESnet + Caltech + 국제 연구망
- 과학 워크플로우를 위한 **end-to-end 지능형 네트워크 서비스 provisioning**
- LHC FTS, DOE Superfacility, BigData Express와 통합
- DTN을 network provisioning의 end-point로 취급

**"SC23 NRE-015: SENSE and Rucio/FTS/XRootD Interoperation"**
- **관련성: ★★★★★** · Rucio + FTS + XRootD + SENSE 통합 실증 사례
- SC23(최상위 HPC 컨퍼런스) Network Research Exhibition 데모
- 본 연구 관련성 — multi-system integration의 최신 레퍼런스. 본 연구의 DTN에 SENSE 통합 가능성 시사

**"Data Transfer and Network Services management for Domain Science Workflows"** (arxiv 2203.08280)
- SENSE + Rucio + FTS + XRootD 통합 아키텍처

#### Globus / DTN 표준 구성

- **Globus** · **fasterdata.es.net** · **GridFTP Wikipedia** · **NERSC GridFTP docs**
- ESGF COG DTN Setup — standard DTN 구성 매뉴얼
- **GridFTP: A Brief History of Fast File Transfer** (Globus 블로그) — 프로토콜 진화 역사

#### DPU TLS Offload (Sharing에 직접 기여)

**"DOCA TLS Offload Guide"** (NVIDIA Docs)
- BF3 **TLS 암/복호화 최대 400 Gb/s**
- kTLS (kernel-TLS)로 구현 — 애플리케이션별 라이브러리 의존 없음

### 본 연구와의 관계

- 본 연구의 Sharing pillar = **"Rucio + GridFTP/XRootD + BF3 DPU TLS/checksum offload + flow telemetry"**
- FGCS 2015 논문이 2015년 SmartNIC 수준에서 **정확히 이 기여를 달성함**
- 본 연구의 신규성
  - 현대 BF3 DPU의 **400 Gb/s** 급 성능 (당시는 10 GbE)
  - **Cloud-native 관리** (DPF로 DPU lifecycle)
  - **Rucio/XRootD 레벨 통합** (당시는 GridFTP 수준)
  - **Telemetry pillar와 결합** (당시는 transfer 성능만)

### Sharing 관점 Gap

> **현대 DPU(BF3) + Cloud-native 관리(DPF) + Rucio 통합**의 정량 평가는 부재. FGCS 2015의 재평가 + 확장이 본 연구 기여

---

## Pillar 4 — Security (보안)

### 상태: 본 연구에서 "Up-coming(진행예정)" — DTN Boundary Observer + Enforcer

*(이 섹션은 앞선 Security-only 보고서에서 조사한 내용을 요약 재수록. 4개 논문 심층 분석 원본은 섹션 [Top 심층 논문](#top-관련-논문-심층-요약)로 이동)*

### 핵심 선행 연구 요약

**Axis 4-A — Science DMZ observability**
- P4-perfSONAR (ScienceDirect 2025) — P4 switch 기반 science DMZ fine-grained 관측, 본 연구의 직접 precedent
- Enhancing perfSONAR with P4 PDP (SC '23 Workshops INDIS) — perfSONAR × P4 초기 연구

**Axis 4-B — DPU flow telemetry**
- P4-NIDS (arxiv 2411.17987) — line-rate 8M pps NetFlow + 데이터플레인 내 decision tree
- Pipeleon (SIGCOMM 2023) — SmartNIC P4 성능 튜닝

**Axis 4-C — DPU boundary enforcement**
- Palladium (arxiv 2505.11339) — DPU-enabled multi-tenant serverless, boundary enforcer 패턴

**Axis 4-D — eBPF vs DPU offload**
- Cilium CNI full offload to DPU (netdev 0x19) — Marvell Octeon 10 사례
- Marvell k8s-cni-offload (GitHub) — 오픈소스 CNI offload framework

**Axis 4-E — Zero-trust + observability 교차**
- Gigamon deep observability · Dynatrace zero-trust · PMC zero-trust survey

---

## 횡단 연구 — Cloud-Native DPU Management (4 pillar 공통 기반)

### NVIDIA DOCA Platform Framework (DPF)

- **GitHub: NVIDIA/doca-platform** — 본 연구 기반 프레임워크
- NVIDIA Technical Blog "Powering the Next Wave of DPU-Accelerated Cloud Infrastructures with DPF"
- DPF Documentation v25.07.0 / v25.1.0
- **Canonical Kubernetes × DPF** (2024) — Ubuntu 생태계 통합
- **Red Hat OpenShift × DPF** (2025-03) — RH 공식 통합 문서
- **Spectro Cloud Palette × BF3** — 상용 K8s 관리 + BF3 통합
- **BIG-IP Next for Kubernetes × BF3** (F5) — AI Cloud 인프라

### 산업 동향

- DPU 시장 2024년 $1.11B → 2034년 $4.44B (15% CAGR)
- 50% 클라우드 providers가 DPU 사용 · AI training의 35% DPU 오프로드
- Pensando SoC (16 ARM + P4 MPU 400 Gb/s) · Marvell Octeon · AMD (Pensando 인수) · NVIDIA BF

---

## Top 관련 논문 심층 요약 (8건)

### ★ 1. "A Survey on Heterogeneous Computing Using SmartNICs and Emerging DPUs"
- arxiv 2504.03653 (2025, v2)
- **관련성 ★★★★★** — 본 연구 전체 분야 개괄 참조
- Taxonomy, 30+ 시스템, gap 분석
- **필수 인용** — Related Work survey 섹션 기반

### ★ 2. "Enhancing visibility on a science DMZ with P4-perfSONAR" (Security)
- ScienceDirect 2025
- **관련성 ★★★★★** — Science DMZ fine-grained 관측 · P4 switch 버전
- 기여 — per-flow statistics, adaptive reporting rate, 5× overhead 감소
- 본 연구의 DPU-based 대응판 positioning

### ★ 3. "Long-haul Secure Data Transfer using Hardware-assisted GridFTP" (Sharing)
- FGCS 2015, Argonne/MCS
- **관련성 ★★★★★** — SmartNIC에 UDT/iWARP/OpenSSL 오프로드 · GridFTP 가속
- 100ms latency에서도 라인레이트 유지, host CPU 절감
- 본 연구의 10년 후 BF3 재평가 positioning

### ★ 4. "SC23 NRE SENSE and Rucio/FTS/XRootD Interoperation" (Sharing)
- SC23 Network Research Exhibition
- **관련성 ★★★★★** — 실제 multi-system integration 데모
- Rucio + FTS + XRootD + SENSE 통합 운영
- 본 연구의 DTN 통합 시 확장 가능 경로

### ★ 5. "P4-NIDS" (Security)
- arxiv 2411.17987 (2024)
- **관련성 ★★★★★** — line-rate 8M pps NetFlow + 데이터플레인 내 탐지
- F1=99.76% (UNSW-NB15), FlowStalker 대비 4× throughput
- 본 연구의 telemetry 방법론 template

### ★ 6. "Palladium: DPU-enabled Multi-Tenant Serverless Cloud" (Security)
- arxiv 2505.11339 (2025)
- **관련성 ★★★★★** — DPU boundary enforcer로 multi-tenant 격리
- Zero-copy RDMA + hardware-accelerated policy enforcement
- Serverless → Data Facility 도메인 확장 positioning

### ★ 7. "Accelerating eBPF Network Stack: Cilium CNI to DPU" (Security, Cross-pillar)
- netdev 0x19 conference talk
- **관련성 ★★★★★** — Cilium full offload to Marvell Octeon 10
- "every packet still burns host CPU" 근거 명시
- 본 연구의 **selective boundary offload** positioning

### ★ 8. "Data Management System Analysis for Distributed Computing Workloads" (Sharing)
- arxiv 2510.00828 (2025)
- **관련성 ★★★★☆** — 최신 facility-scale DTN 분석
- DTN over-utilization 발견, medium-size transfer 증가 추세
- 본 연구의 DTN 최적화 수요 학술 근거

---

## Gap Analysis — 통합

### 4 Pillar 각각의 Gap

| Pillar | 기존 연구로 해결됨 | 여전히 Gap · 본 연구 기여 가능 |
|--------|----------------|--------------------------|
| Processing | DPU compute offload 프레임워크 (UROM, BluesMPI) | **"Boundary-only DPU" 구조에서 compute 기여의 한계/가능성 정식화** |
| Storage | NVMe-oF offload, Iceberg 아키텍처 성숙 | **DPU-captured network metadata → Iceberg provenance 자동 주입** |
| Sharing | GridFTP 하드웨어 오프로드 (2015), SENSE/Rucio 통합 | **현대 BF3 + Cloud-native(DPF) + Rucio 통합 정량 재평가** |
| Security | P4-perfSONAR, P4-NIDS, Palladium | **DPU 기반 science DMZ + Cilium-enforced bypass-proof boundary** |

### 통합적 Gap — 본 연구의 유일무이 위치

> **"Selective DTN-DPU"로 4 pillar(처리 + 저장 + 공유 + 보안)을 한 번에 고도화하는 cloud-native 시스템 아키텍처**는 학술적으로 탐색되지 않은 통합 공백

- Processing/Storage/Sharing/Security는 각 pillar에서 따로 연구됨
- Cloud-native DPU 관리(DPF)는 산업계 프레임워크 수준, 학술 평가 부재
- **이 둘을 묶어 "한 DTN으로 전체 facility를 고도화"하는 주장**은 본 연구가 최초

---

## 본 연구의 신규성 3축 (통합 재확인)

### 1️⃣ **Selective DTN-DPU 패러다임**
- Full-cluster DPU 요구 ❌ · DTN 하나에만 배치
- Marvell netdev 0x19의 "Full offload" 대비 **현실적·점진적 도입 경로**

### 2️⃣ **5-layer DPU Telemetry 통합 파이프**
- flow counter(L1) + NetFlow v9(L2) + PCC/retx(L3) + PCI/diag(L4) + DPUService(L5)
- P4-NIDS, P4-perfSONAR가 각각 부분적으로 다룬 것을 **한 플랫폼(DPU + DPF)** 아래 통합

### 3️⃣ **Cilium-Enforced Bypass-Proof Boundary**
- `CiliumEgressGatewayPolicy` + `CiliumL2AnnouncementPolicy`로 강제 경유
- Cloud-native CRD로 선언적 관리
- "DPU가 빠르다"가 아니라 **"DPU가 우회 불가능한 choke point"**라는 주장

---

## 제안 — 연구 draft에 반영할 Related Work 섹션 (4 pillar 완전 커버)

```markdown
## Related Work

### Cloud-Native DPU Management
NVIDIA DOCA Platform Framework (DPF)[NVIDIA 2025]가 BlueField DPU의 K8s 통합 공식 프레임워크로 등장, Canonical · Red Hat OpenShift · Spectro Cloud 등이 통합 채택. 학술적 평가는 본 연구가 최초.

### Processing
In-network computing에서 DPU를 compute accelerator로 쓰는 방향(BluesMPI, iPipe 등 [Survey arxiv 2504.03653])이 주류. DOCA UROM[Springer 2025]이 공식 HPC/AI offload 프레임워크 제공. 본 연구는 **boundary-only 배치로 이 compute offload 경로를 명시적으로 제외** (Pending 선언), data preparation 관점만 부분 활용.

### Storage
Apache Iceberg 기반 data lakehouse가 cloud-native 표준으로 성숙[Salesforce, Databricks, Springer 2025]. DPU NVMe-oF offload는 BlueField 공식 기능[NVIDIA, LANL ABoF]. 본 연구는 **DPU-captured network metadata를 Iceberg provenance로 자동 주입**하는 ingest 경로를 새롭게 제안.

### Sharing / Transfer
Rucio[EPJ 2019, arxiv 2510.00828]가 1 EB급 ATLAS 데이터를 관리, XRootD/FTS와 통합. GridFTP 하드웨어 오프로드는 2015년부터 SmartNIC 수준에서 시도[FGCS 2015, UDT/iWARP/OpenSSL offload]. AutoGOLE/SENSE[SC23 NRE]가 SDN-기반 end-to-end DTN provisioning. 본 연구는 **현대 BF3 DPU + Cloud-native DPF 관리**로 2015년 GridFTP-offload 작업의 재평가 + Rucio/XRootD 레벨로 확장.

### Security / Observability
P4-perfSONAR[ScienceDirect 2025]가 science DMZ fine-grained 관측을 P4 switch로 달성. P4-NIDS[arxiv 2024]는 line-rate 8M pps NetFlow + 데이터플레인 탐지. Palladium[arxiv 2505.11339]이 DPU를 multi-tenant boundary enforcer로 활용. Marvell의 Cilium-full-offload[netdev 0x19]는 eBPF 스택 전체를 DPU로 이전. 본 연구는 이들과 다르게 **"selective DTN-DPU + Cilium-enforced bypass-proof boundary + 5-layer telemetry"**를 통합하여 cloud-native science data facility의 경계 보안 모델 제시.
```

---

## 다음 단계 제안

### 우선 정독할 논문 Top 5

| # | 논문 | Pillar | 이유 |
|---|------|--------|------|
| 1 | SmartNIC/DPU Survey (arxiv 2504.03653) | 횡단 | Related Work 전체 구조 기반 |
| 2 | Long-haul Hardware-assisted GridFTP (FGCS 2015) | Sharing | 본 연구의 10년전 precedent — 재평가 근거 |
| 3 | P4-perfSONAR (ScienceDirect 2025) | Security | 직접 경쟁 선행 — 차별화 근거 |
| 4 | SENSE/Rucio/FTS/XRootD Interoperation (SC23 NRE) | Sharing | multi-system 통합 사례 |
| 5 | Palladium (arxiv 2505.11339) | Security | 최신 DPU boundary enforcement |

### 추가 심층 조사 가치 있는 영역

- **LANL ABoF 기술 문서** — in-storage + in-network compute 통합 사례 (Storage pillar 보강)
- **ATLAS/CMS XRootD 운영 논문들** — facility-scale 실제 operating 근거 (Sharing pillar)
- **IRIS-HEP / OSG (Open Science Grid) 문서** — 실제 과학 커뮤니티 요구사항
- **DOCA TLS Offload Guide + 성능 평가 블로그** — BF3 TLS 400 Gb/s 검증용
- **Cilium 공식 성능 문서 (kube-proxy-replacement)** — 정량 비교 baseline
- **OpenTelemetry Data Lineage 스펙 논의** — Storage pillar provenance 연계

### 2차 WebFetch 재시도 필요 건 (이번 조사에서 403/303 실패)

- **Long-haul GridFTP (Argonne PDF 직접 링크)** — Science Direct 버전으로 우회 가능
- **DOCA UROM Springer 챕터** — SpringerLink 인증 필요 가능성, 저자 preprint 탐색 필요

---

## 전체 참고 링크 (Sources)

### 학술 논문 · 프리프린트

**횡단 Survey**
- [A Survey on Heterogeneous Computing Using SmartNICs and Emerging DPUs (arxiv 2504.03653)](https://arxiv.org/html/2504.03653v2)

**Processing**
- [OpenSHMEM Performance on Bluefield-3 DPUs (ACM PEARC 2025)](https://dl.acm.org/doi/10.1145/3708035.3736109)
- [DOCA UROM: A Vehicle for Offloading HPC and AI to DPUs (Springer 2025)](https://link.springer.com/chapter/10.1007/978-3-032-07612-0_31)
- [Geospatial Filter and Refine Computations on NVidia BlueField DPU (NSF PAR)](https://par.nsf.gov/biblio/10515880)
- [Disaggregated Memory with SmartNIC Offloading (arxiv 2410.02599)](https://arxiv.org/html/2410.02599v1)
- [Characterizing Off-path SmartNIC (OSDI 2023)](https://www.usenix.org/system/files/osdi23-wei-smartnic.pdf)
- [Plug & Offload: PnO-TCP (arxiv 2503.22930)](https://arxiv.org/html/2503.22930v1)
- [In-Network Memory Access (arxiv 2507.04001)](https://arxiv.org/html/2507.04001v1)
- [Efficient Network Systems Design for ML (MIT thesis 2025)](https://dspace.mit.edu/bitstream/handle/1721.1/164120/yang-mingrany-phd-eecs-2025-thesis.pdf)
- [OmNICCL: Zero-cost Sparse AllReduce (SIGCOMM NAIC 2024)](https://dl.acm.org/doi/10.1145/3672198.3673804)
- [In-Network AllReduce Optimization (ACM 2024)](https://dl.acm.org/doi/pdf/10.1145/3672198.3673800)
- [OptimusNIC: Offloading Optimizer State (ACM 2025)](https://dl.acm.org/doi/pdf/10.1145/3721146.3721960)
- [SmartNIC-Aided Control Plane for Distributed ML (USENIX ATC 2024)](https://www.usenix.org/system/files/atc24-xiao.pdf)

**Storage**
- [Inside Salesforce Data Cloud Open Lakehouse (Iceberg)](https://engineering.salesforce.com/inside-data-clouds-open-lakehouse-4m-tables-and-50pb-powered-by-apache-iceberg/)
- [Building modern data platform based on lakehouse (Springer 2025)](https://link.springer.com/article/10.1007/s42452-025-06545-w)
- [Deep Dive into Apache Iceberg Architecture (Medium 2026)](https://medium.com/snowflake/deep-dive-into-apache-iceberg-architecture-the-three-layers-that-power-your-lakehouse-83c03403e503)
- [2025 Guide to Architecting an Iceberg Lakehouse (Dremio)](https://medium.com/data-engineering-with-dremio/2025-guide-to-architecting-an-iceberg-lakehouse-9b19ed42c9de)
- [Supermicro JBOF with BF3 DPU](https://www.supermicro.com/solutions/Solution_Brief_JBOF-Petascale-NVIDIA-BlueField3-DPU.pdf)
- [Disaggregated Storage on DPU (Xinnor)](https://xinnor.io/solutions/disaggregated-storage-on-dpu/)
- [BF4 DPU and KV-Cache (CES 2026, chiplog)](https://www.chiplog.io/p/analysis-of-nvidias-bluefield-4-dpu)
- [OpenTelemetry Data Lineage Discussion (#3447)](https://github.com/open-telemetry/opentelemetry-specification/issues/3447)

**Sharing**
- [Rucio: Scientific Data Management (EPJ 2019)](https://link.springer.com/article/10.1007/s41781-019-0026-3)
- [Rucio arxiv 1902.09857](https://ar5iv.labs.arxiv.org/html/1902.09857)
- [Data Management System Analysis for Distributed Workloads (arxiv 2510.00828)](https://arxiv.org/html/2510.00828)
- [Data Transfer and Network Services management for Domain Science Workflows (arxiv 2203.08280)](https://arxiv.org/pdf/2203.08280)
- [SC23 NRE-015 SENSE/Rucio/FTS/XRootD](https://sc23.supercomputing.org/wp-content/uploads/2023/11/SC23-NRE-015-Final-Abstract-SENSE-Rucio-FTS-Interoperation-Tom-Lehman.pdf)
- [SC23 NRE-014 AutoGOLE/SENSE](https://sc23.supercomputing.org/wp-content/uploads/2023/11/SC23-NRE-014-Final-Abstract-AutoGOLE-SENSE-Tom-Lehman.pdf)
- [SENSE: Intelligent Network Services for Science Workflows (Caltech)](https://supercomputing.caltech.edu/index.php/2020/10/11/sense-intelligent-network-services-for-science-workflows/)
- [SENSE (ESnet slide deck)](https://www.es.net/assets/Uploads/76kkEi9wqPrVdrGK4bSFdg.pdf)
- [Long-haul Secure Data Transfer using Hardware-assisted GridFTP (FGCS 2015)](https://www.mcs.anl.gov/~kettimut/publications/FGCS15-Long-Haul.pdf)
- [Enhancement of GridFTP through Hardware Offloading (SC Workshops)](https://scinet.supercomputing.org/community/documents/92/pap104s3.pdf)
- [Rearchitecting TCP for I/O-Offloaded Content Delivery (NSDI 2023)](https://minlanyu.seas.harvard.edu/writeup/nsdi23-iotcp.pdf)
- [RTN-032 Panda/Rucio Multi-site (Rubin Observatory)](https://rtn-032.lsst.io/)
- [GridFTP NERSC docs](https://docs.nersc.gov/services/gridftp/)
- [Globus fasterdata ESnet](https://fasterdata.es.net/data-transfer-tools/globus/)

**Security (4 pillar)**
- [Enhancing visibility on science DMZ with P4-perfSONAR (ScienceDirect 2025)](https://www.sciencedirect.com/science/article/pii/S1084804525001602)
- [Enhancing perfSONAR using P4 PDP (SC23 Workshops INDIS)](https://dl.acm.org/doi/10.1145/3624062.3624596)
- [P4-NIDS (arxiv 2411.17987)](https://arxiv.org/html/2411.17987v1)
- [Palladium DPU Multi-Tenant Serverless (arxiv 2505.11339)](https://arxiv.org/pdf/2505.11339)
- [Pipeleon SmartNIC P4 Performance (SIGCOMM 2023)](https://www.cs.rice.edu/~eugeneng/papers/SIGCOMM23-Pipeleon.pdf)
- [Theory and Application of Zero Trust Security (PMC)](https://pmc.ncbi.nlm.nih.gov/articles/PMC10742574/)
- [perfSONAR in Science DMZ (NSRC)](https://learn.nsrc.org/science-dmz/perfsonar-in-the-science-dmz)
- [Science DMZ Performance Monitoring (ESnet fasterdata)](https://fasterdata.es.net/science-dmz/science-dmz-performance-monitoring/)
- [SmartNIC Security Survey (JNCA)](https://research.cec.sc.edu/files/cyberinfra/files/documents/jnca_smartnic_security_survey-revised-2.pdf)
- [SmartNIC-Based Secure Aggregation for FL (CEUR 2022)](https://ceur-ws.org/Vol-3344/paper12.pdf)

### 컨퍼런스 · 커뮤니티 세션
- [Accelerating eBPF Cilium CNI to DPU (netdev 0x19)](https://netdevconf.info/0x19/sessions/talk/accelerating-an-ebpf-network-stack-our-journey-in-completely-offloading-ebpf-based-cilium-cni-to-dpu.html)
- [HPC Researchers Seed In-Network Computing with NVIDIA DPUs (HPCwire/NVIDIA blog)](https://www.hpcwire.com/off-the-wire/hpc-researchers-seed-the-future-of-in-network-computing-with-nvidia-bluefield-dpus/)

### 산업계 공식 문서 · 리포지토리
- [NVIDIA/doca-platform (GitHub)](https://github.com/NVIDIA/doca-platform)
- [DPF v25.07.0 Docs](https://docs.nvidia.com/networking/display/dpf2507)
- [DPF v25.1.0 Docs](https://docs.nvidia.com/networking/display/dpf2571)
- [Powering the Next Wave of DPU-Accelerated Cloud Infrastructures (NVIDIA Blog)](https://developer.nvidia.com/blog/powering-the-next-wave-of-dpu-accelerated-cloud-infrastructures-with-nvidia-doca-platform-framework/)
- [BlueField Networking for HPC (NVIDIA)](https://www.nvidia.com/en-us/networking/products/data-processing-unit/hpc/)
- [BlueField-3 Datasheet](https://www.nvidia.com/content/dam/en-zz/Solutions/Data-Center/documents/datasheet-nvidia-bluefield-3-dpu.pdf)
- [Ushering In New Era of HPC with DPUs (NVIDIA Blog)](https://developer.nvidia.com/blog/ushering-in-a-new-era-of-hpc-and-supercomputing-performance-with-dpus/)
- [Offloading and Isolating Data Center Workloads (NVIDIA Blog)](https://developer.nvidia.com/blog/offloading-and-isolating-data-center-workloads-with-bluefield-dpu/)
- [DOCA TLS Offload Guide](https://docs.nvidia.com/doca/sdk/doca-tls-offload-guide/index.html)
- [Canonical K8s meets DPF (Canonical)](https://canonical.com/blog/canonical-kubernetes-meets-nvidia-doca-platform-framework-dpf-building-the-future-of-dpu-driven-infrastructure)
- [DPU-enabled networking with OpenShift and NVIDIA DPF (RH Developer 2025)](https://developers.redhat.com/articles/2025/03/20/dpu-enabled-networking-openshift-and-nvidia-dpf)
- [Cloud-native enablement of DPUs in OpenShift (RH)](https://www.redhat.com/en/blog/cloud-native-enablement-dpus-red-hat-openshift)
- [How Palette accelerates K8s with BF3 (Spectro Cloud)](https://www.spectrocloud.com/blog/how-palette-accelerates-kubernetes-clusters-with-nvidia-bluefield-3-dpus)
- [BIG-IP Next for Kubernetes on BF3 (F5)](https://www.f5.com/products/big-ip/next/kubernetes-on-nvidia-bluefield-dpu)
- [Marvell k8s-cni-offload (GitHub)](https://github.com/MarvellEmbeddedProcessors/k8s-cni-offload)
- [Cilium (GitHub)](https://github.com/cilium/cilium)
- [Cilium CNI Benchmark](https://docs.cilium.io/en/stable/operations/performance/benchmark/)
- [BlueField DPU for Storage (Simplyblock)](https://www.simplyblock.io/glossary/nvidia-bluefield-dpu/)

### 추가 배경 자료
- [SmartNICs and DPUs Explained 2025 (network-switch.com)](https://network-switch.com/blogs/networking/smart-nics-and-dpus-explained)
- [DPUs and SmartNICs: third pillar (Introl 2025)](https://introl.com/blog/dpus-smartnics-data-center-infrastructure-bluefield-pensando-2025)
- [SONiC's Next Home SmartNIC DPU (Packet Pushers)](https://packetpushers.net/blog/sonics-next-home-the-smartnic-data-processing-unit-dpu/)
- [Understanding BF3 DPU (FiberMall)](https://www.fibermall.com/blog/understand-nvidia-bluefield-3-dpu.htm)
- [Deep Observability for Zero Trust (Gigamon)](https://www.gigamon.com/campaigns/zero-trust.html)
- [Zero Trust and Observability (Dynatrace)](https://www.dynatrace.com/news/blog/us-government-guidance-zero-trust-architecture/)
- [GridFTP: Brief History of Fast File Transfer (Globus)](https://www.globus.org/blog/gridftp-a-brief-history-of-fast-file-transfer)
