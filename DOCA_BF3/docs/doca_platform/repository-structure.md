---
title: "레포지토리 구조"
---

[TOC]

`doca-platform` 레포 디렉토리 구조 및 아키텍처 컴포넌트 매핑

## 최상위 구조

```
doca-platform/
├── api/              # CRD 타입 정의 (Go 구조체 + 생성 코드)
├── cmd/              # 바이너리 진입점 (서비스별 1개)
├── config/           # Kubernetes 배포용 Kustomize 매니페스트
├── deploy/           # Helm 차트 및 Helmfile 구성
├── docs/             # 문서
├── dpuservices/      # DPU 서비스 매니페스트 (HBN, Firefly, DTS 등)
├── hack/             # 빌드 스크립트 및 개발 도구
├── internal/         # 컨트롤러 구현체 (외부 import 불가)
├── pkg/              # 공유 라이브러리 (외부 import 가능)
├── test/             # 테스트 유틸리티, mock, e2e 헬퍼
└── third_party/      # 외부 의존성 (벤더링/핀 고정)
```

---

## `api/` — CRD API 정의

Kubernetes API 서버 등록 커스텀 리소스의 Go 타입 정의. 하위 디렉토리 = 별도 API 그룹

| API 그룹 | 경로 | 주요 리소스 |
|----------|------|------------|
| Provisioning | `api/provisioning/v1alpha1/` | `DPU`, `DPUNode`, `DPUDevice`, `DPUSet`, `DPUCluster`, `DPUFlavor`, `BFB`, `BlueField`, `DPUDiscovery`, `DPUNodeMaintenance` |
| DPU Service | `api/dpuservice/v1alpha1/` | `DPUService`, `DPUDeployment`, `DPUServiceCredentialRequest` |
| Service Chain | `api/servicechain/v1alpha1/` | `DPUServiceChain`, `DPUServiceInterface`, `DPUServiceIPAM` |
| VPC / 네트워킹 | `api/vpc/v1alpha1/` | `DPUVPC`, `DPUVirtualNetwork` |
| Storage | `api/storage/v1alpha1/` | `DPUVolume`, `DPUVolumeAttachment`, `DPUStoragePolicy` |
| Node Resources | `api/noderesources/v1alpha1/` | 노드 리소스 할당 타입 |
| Operator | `api/operator/v1alpha1/` | `DPFOperatorConfig` |
| gRPC (storage) | `api/grpc/nvidia/storage/plugins/v1/` | Protobuf 생성 스토리지 플러그인 인터페이스 |

---

## `cmd/` — 서비스 진입점

하위 디렉토리마다 배포 가능한 바이너리 `main.go` 1개

| 바이너리 | 경로 | 역할 |
|---------|------|------|
| `dpf-operator` | `cmd/dpf-operator/` | 메인 오퍼레이터 — 전체 DPF 서브시스템 설치/구성 |
| `provisioning-controller` | `cmd/provisioning-controller/` | DPU 하드웨어 라이프사이클 관리 (BFB 플래시, VF 구성, DPU 클러스터 조인) |
| `dpuservice` | `cmd/dpuservice/` | ArgoCD를 통한 DPUService/DPUDeployment 라이프사이클 관리 |
| `dpuagent` | `cmd/dpuagent/` | **DPU 위에서 실행** (arm64) — 로컬 DPU 상태 관리 |
| `hostagent` | `cmd/hostagent/` | 호스트 노드에서 실행 — 호스트 ↔ DPU 라이프사이클 브리지 |
| `dpudetector` | `cmd/dpudetector/` | NFD 플러그인 — BF3 하드웨어 감지 및 노드 레이블링 |
| `servicechainset` | `cmd/servicechainset/` | ServiceChainSet 객체를 DPU 클러스터로 동기화 |
| `sfc-controller` | `cmd/sfc-controller/` | 서비스 함수 체인용 OVS 플로우 프로그래밍 |
| `pod-ipam-injector` | `cmd/pod-ipam-injector/` | 웹훅 — Pod에 IPAM 어노테이션 주입 |
| `static-cluster-manager` | `cmd/static-cluster-manager/` | 정적(non-Kamaji) DPU 클러스터 매니저 |
| `kamaji-cluster-manager` | `cmd/kamaji-cluster-manager/` | Kamaji 기반 DPU 컨트롤 플레인 매니저 |
| `snap-node-driver` | `cmd/storage/snap-node-driver/` | 노드 측 SNAP 스토리지 드라이버 (arm64) |
| `snap-host-controller` | `cmd/storage/snap-host-controller/` | 호스트 측 SNAP 스토리지 컨트롤러 |
| `snap-csi-plugin` | `cmd/storage/snap-csi-plugin/` | SNAP 볼륨용 CSI 플러그인 |

---

## `config/` — Kustomize 매니페스트

컴포넌트별 Kubernetes 배포 매니페스트. 각 하위 디렉토리 → Kubebuilder 표준 레이아웃 (`default/`, `manager/`, `rbac/`, `webhook/`, `certmanager/`)

| 컴포넌트 | 경로 | 배포 대상 |
|---------|------|----------|
| DPF Operator | `config/operator-crds/` | CRD + 오퍼레이터 매니저 |
| Provisioning | `config/provisioning/` | Provisioning 컨트롤러, 웹훅, RBAC |
| DPU Service | `config/dpuservice/` | DPUService 컨트롤러, 웹훅 |
| SR-IOV Device Plugin | `config/nodesriovdeviceplugin/` | SR-IOV VF 관리 데몬셋 |
| Static Cluster Manager | `config/static-cluster-manager/` | 정적 클러스터 매니저 |
| Kamaji Cluster Manager | `config/kamaji-cluster-manager/` | Kamaji 기반 클러스터 매니저 |
| BFB Registry | `config/bfb_registry/` | BlueField 부트이미지 레지스트리 서비스 |
| DPU Detector | `config/dpu-detector/` | NFD 기반 DPU 감지 데몬셋 |

---

## `deploy/` — Helm & Helmfile

프로덕션 배포 구성.

```
deploy/
├── charts/
│   ├── dpf-operator/       # 전체 DPF 스택을 위한 umbrella Helm 차트
│   └── dpu-networking/     # 네트워킹 서브차트 (SR-IOV, SFC, OVS-CNI)
└── helmfiles/
    ├── prereqs.yaml        # 기반 의존성 (아래 참조)
    └── values/             # 릴리스별 Helm 값 오버라이드
```

**`prereqs.yaml` 설치 순서:**

| 릴리스 | 차트 | 버전 | 목적 |
|--------|------|------|------|
| cert-manager | jetstack/cert-manager | v1.19.3 | TLS 인증서 관리 |
| kamaji | oci://ghcr.io/nvidia/charts/kamaji | v1.2.0 | DPU 컨트롤 플레인 라이프사이클 |
| maintenance-operator | oci://ghcr.io/mellanox/maintenance-operator-chart | v0.3.0 | 노드 유지보수 오케스트레이션 |
| argo-cd | argoproj/argo-cd | v9.4.1 | DPU 서비스 GitOps 전달 |
| node-feature-discovery | node-feature-discovery/nfd | v0.18.3 | 하드웨어 레이블링 (DPU 감지) |
| local-path-provisioner | local-storage/local-path-provisioner | v0.0.34 | etcd용 로컬 노드 스토리지 |

---

## `dpuservices/` — DPU 서비스 매니페스트

각 DOCA 서비스용 즉시 사용 가능한 `DPUService` / `DPUDeployment` 매니페스트. 호스트 클러스터 적용 시 DPF → ArgoCD → DPU 클러스터 자동 동기화

| 서비스 | 경로 | 설명 |
|--------|------|------|
| HBN | `dpuservices/hbn/` | Host Bridge Networking — BGP unnumbered 라우팅; Helm 차트 `doca-hbn v1.0.5` |
| Firefly | (DPUDeployment) | OVN 오프로드 (DPDK); 브리지 `br-sfc`, `br-dpu`, `br-ovn`; `hw-offload=true` |
| DTS | `dpuservices/dts/` | DOCA Telemetry Service — 포트 9100 메트릭; Helm 차트 `doca-telemetry v1.23.4` |
| Blueman | `dpuservices/blueman/` | BlueField 관리 서비스; Helm 차트 `doca-blueman v1.0.8` |
| Storage | `dpuservices/storage/` | SNAP 기반 블록/FS/NFS 스토리지 (CSI 통합) |
| DummyDPUService | `dpuservices/dummydpuservice/` | 커스텀 DPU 서비스 개발을 위한 레퍼런스 템플릿 |

---

## `hack/` — 빌드 및 개발 도구

```
hack/
├── scripts/
│   ├── dpu-control-plane-setup.sh   # BF3 DPU 모드 전환 (DPU mode ↔ NIC mode, mlxconfig + IPMI)
│   ├── kind-install.sh              # MetalLB 포함 로컬 KinD 클러스터 생성
│   ├── deploy-helmfile.sh           # Helmfile 배포 래퍼 (로컬, OCI, Helm 레포 지원)
│   ├── docker-build.sh              # docker buildx 래퍼 (구조화된 JSON 로그 포함)
│   ├── log-collector.sh             # 클러스터/Pod 로그 수집 (디버깅용)
│   └── crd-validation.sh            # CRD 스키마 유효성 검사
└── tools/                           # 고정된 도구 버전 (controller-gen, mockgen 등)
```

---

## `internal/` — 컨트롤러 구현체

핵심 비즈니스 로직. 외부 Go 모듈 import 불가

| 패키지 | 경로 | 책임 |
|--------|------|------|
| Provisioning 컨트롤러 | `internal/provisioning/controllers/` | DPU, DPUSet, DPUCluster, BFB, DPUNodeMaintenance 리콘실러 |
| DPU agent | `internal/provisioning/dpuagent/` | DPU 위 에이전트 로직 (arm64) |
| DPU service 컨트롤러 | `internal/dpuservice/controllers/` | DPUService, DPUDeployment, DPUServiceCredentialRequest 리콘실러 |
| Service chain | `internal/dpuservicechain/` | DPUServiceChain, DPUServiceInterface, DPUServiceIPAM 리콘실러 |
| SFC 컨트롤러 | `internal/sfccontroller/` | 서비스 함수 체인용 OVS 플로우 프로그래밍 |
| SNAP storage | `internal/storage/snap/` | SNAP 호스트 컨트롤러 및 노드 드라이버 |
| Cluster manager (static) | `internal/clustermanager/static/` | 정적 DPU 클러스터 관리 |
| Cluster manager (Kamaji) | `internal/clustermanager/kamaji/` | Kamaji 기반 DPU 클러스터 관리 |
| Pod IPAM injector | `internal/pod-ipam-injector/` | IPAM 어노테이션 주입 어드미션 웹훅 |
| Operator | `internal/operator/` | DPFOperatorConfig 리콘실러 — 전체 서브시스템 부트스트랩 |
| SR-IOV device plugin | `internal/nodesriovdeviceplugin/` | VF 할당 및 라이프사이클 관리 |

---

## `pkg/` — 공유 라이브러리

여러 컨트롤러/바이너리 공통 패키지

| 패키지 | 경로 | 목적 |
|--------|------|------|
| `bfcfg` | `pkg/bfcfg/` | BlueField 하드웨어 구성 유틸리티 |
| `conditions` | `pkg/conditions/` | `metav1.Condition` 표준 헬퍼 |
| `dpucluster` | `pkg/dpucluster/` | DPU 클러스터 클라이언트 추상화 |
| `dpuselector` | `pkg/dpuselector/` | DPU 노드 셀렉션 / 레이블 매칭 |
| `ipallocator` | `pkg/ipallocator/` | DPU 서비스용 IP 범위 할당 |
| `openflow` | `pkg/openflow/` | OpenFlow 메시지 구성 |
| `ovsmodel` / `ovsutils` | `pkg/ovsmodel/`, `pkg/ovsutils/` | OVS OVSDB 모델 및 쿼리 유틸리티 |
| `vfmac` | `pkg/vfmac/` | Virtual Function MAC 주소 관리 |
| `health` | `pkg/health/` | Kubernetes 헬스체크 헬퍼 |
| `utils` | `pkg/utils/` | 범용 유틸리티 |

---

## `test/` — 테스트 인프라

| 디렉토리 | 목적 |
|----------|------|
| `test/e2e/` | End-to-end 테스트 (실행 중인 클러스터 필요) |
| `test/apivalidation/` | CRD 스키마 유효성 검사 테스트 |
| `test/mock/` | mockgen으로 생성된 mock 구현체 |
| `test/objects/` | 단위 테스트용 오브젝트 팩토리 |
| `test/utils/` | 공유 테스트 헬퍼 |

---

## 주요 빌드 변수 (Makefile)

| 변수 | 기본값 | 설명 |
|------|--------|------|
| `REGISTRY` | `example.com` | 컨테이너 이미지 레지스트리 |
| `TAG` | `v0.1.0` | 이미지 태그 |
| `ARCH` | `amd64` | 빌드 대상 아키텍처 (`amd64` 또는 `arm64`) |
| `HOST_ARCH` | `amd64` | 호스트 측 이미지 아키텍처 |
| `DPU_ARCH` | `arm64` | DPU 측 이미지 아키텍처 |

DPU 측(arm64) 이미지 빌드:

```bash
make ARCH=arm64 REGISTRY=<your-registry> TAG=<tag> docker-build-dpuservice
```
