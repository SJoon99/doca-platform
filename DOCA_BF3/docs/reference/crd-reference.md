---
title: "DPF CRD 레퍼런스 가이드"
---

[TOC]

## 개요

DPF(DOCA Platform Framework)는 Kubernetes CRD로 BF3 DPU를 선언적으로 관리. <br><br>
**CRD는 6개 API 그룹으로 구분:**

| API 그룹 | 역할 |
|---------|------|
| `provisioning.dpu.nvidia.com` | DPU 하드웨어 프로비저닝 |
| `svc.dpu.nvidia.com` | DPU 위 서비스 배포 |
| `operator.dpu.nvidia.com` | DPF Operator 동작 제어 |
| `vpc.dpu.nvidia.com` | 가상 네트워크/멀티테넌시 |
| `storage.dpu.nvidia.com` | DPU 스토리지 관리 |
| `noderesources.dpu.nvidia.com` | 호스트 노드 SR-IOV 설정 |

---

## 전체 리소스 관계도

```
DPUDeployment  ◀── 가장 상위, 모든 것을 오케스트레이션
├── BFB               DPU에 설치할 OS 이미지
├── DPUFlavor         DPU 시스템 설정 (펌웨어, OVS, SR-IOV)
├── DPUSet            BFB + Flavor를 노드에 적용
│   └── DPU           (자동 생성) 개별 DPU 프로비저닝 상태
├── DPUService        DPU 클러스터에 Helm 차트 배포
├── DPUServiceInterface   네트워크 인터페이스 정의
└── DPUServiceChain   서비스 간 트래픽 경로 정의

DPUCluster      DPU 전용 Kubernetes 클러스터 (Kamaji 또는 Static)
DPUNode         DPU를 가진 호스트 노드 설정
DPUDevice       DPU 하드웨어 인벤토리
DPUDiscovery    DPU 자동 검색 (Zero-Trust 모드)
DPFOperatorConfig  DPF Operator 전역 설정 (싱글톤)
```

---

## 1. Provisioning API

### 1.1 BFB

**역할:** DPU에 설치할 OS/펌웨어 번들 이미지 정의

**원리:**
- BFB(BlueField Bundle) = DPU 전용 OS 이미지 (Ubuntu + MLNX_OFED + DOCA 포함)
- DPF Operator가 BFB를 PVC에 다운로드해 두고, 프로비저닝 시 DPU에 플래시
- DPUSet이 이 BFB를 참조함

**언제 사용:**
- DPU에 특정 DOCA 버전 OS를 설치하거나 업그레이드할 때
- 여러 DPUSet이 같은 BFB를 공유할 때

```yaml
apiVersion: provisioning.dpu.nvidia.com/v1alpha1
kind: BFB
metadata:
  name: doca-3.2
  namespace: dpf-operator-system
spec:
  url: http://harbor.cluster.local/bfb/bf-bundle-ubuntu24.04-3.2.0_ubuntu-24.04_64k_prod.bfb
```

**핵심 필드:**
- `spec.url` — BFB 파일 다운로드 URL (HTTP/HTTPS)

---

### 1.2 DPUFlavor

**역할:** DPU의 시스템 수준 설정 템플릿 (커널, 펌웨어, OVS, 리소스 할당)

**원리:**
- BFB가 "어떤 OS"를 설치할지라면, DPUFlavor는 "어떻게 설정"할지를 정의
- 프로비저닝 시 단 한 번 적용됨 — **생성 후 변경 불가(Immutable)**
- `nvConfig`로 mlxconfig 값을 선언적으로 설정 (SR-IOV VF 개수, LINK_TYPE 등)
- `ovs`로 Open vSwitch 설정 스크립트를 주입

**언제 사용:**
- SR-IOV VF 개수를 설정할 때
- DPU 동작 모드(dpu / zero-trust) 를 지정할 때
- DPU 서비스용 CPU/메모리/SF 리소스를 예약할 때

```yaml
apiVersion: provisioning.dpu.nvidia.com/v1alpha1
kind: DPUFlavor
metadata:
  name: base-flavor
  namespace: dpf-operator-system
spec:
  dpuMode: dpu                     # dpu 또는 zero-trust
  grub:
    kernelParameters:
      - "iommu=pt"
  nvConfig:
    - device: "*"
      parameters:
        NUM_OF_VFS: "8"            # SR-IOV VF 8개
        SRIOV_EN: "1"              # SR-IOV 활성화
        PF_TOTAL_SF: "20"          # Scalable Function 20개
  ovs:
    rawConfigScript: |
      ovs-vsctl add-br br-hpf
  dpuResources:                    # DPU 전체 가용 리소스
    cpu: "16"
    memory: "32Gi"
    "nvidia.com/sf": "20"
  systemReservedResources:         # 시스템 예약 (나머지가 DPUService에 할당됨)
    cpu: "4"
    memory: "8Gi"
```

**핵심 필드:**
- `spec.dpuMode` — `dpu`(일반) 또는 `zero-trust`(격리 모드)
- `spec.nvConfig` — mlxconfig 파라미터 (SR-IOV, VF 수 등)
- `spec.ovs.rawConfigScript` — OVS 설정 스크립트
- `spec.dpuResources` — DPU 전체 리소스
- `spec.systemReservedResources` — 시스템 예약 리소스

---

### 1.3 DPUSet

**역할:** BFB + DPUFlavor를 선택된 노드의 DPU에 적용하는 롤링 업데이트 컨트롤러

**원리:**
- Kubernetes Deployment/DaemonSet과 유사한 패턴
- 노드 셀렉터로 대상 노드를 선택 → 각 노드마다 `DPU` 리소스 자동 생성
- 프로비저닝 전 노드 drain/taint → BFB 플래시 → 재부팅 → DPU 클러스터 조인
- 이미 원하는 BFB 버전이 설치된 경우 플래시 스킵 가능

**언제 사용:**
- 처음 DPU를 DPF 관리 하에 온보딩할 때
- BFB 버전을 업그레이드할 때
- DPUFlavor(설정)를 변경할 때 (기존 것 삭제 후 새로 생성)

```yaml
apiVersion: provisioning.dpu.nvidia.com/v1alpha1
kind: DPUSet
metadata:
  name: bf3-pool
  namespace: dpf-operator-system
spec:
  dpuNodeSelector:
    matchLabels:
      feature.node.kubernetes.io/dpu-enabled: "true"  # tempnode-bf3 대상
  strategy:
    type: RollingUpdate
    rollingUpdate:
      maxUnavailable: 1            # 동시에 최대 1개 노드 프로비저닝
  dpuTemplate:
    spec:
      bfb:
        name: doca-3.2
      dpuFlavor: base-flavor
      nodeEffect:
        taint: {}                  # 프로비저닝 중 노드 taint
      cluster:
        nodeLabels:
          role: dpu-worker
```

**핵심 필드:**
- `spec.dpuNodeSelector` — 대상 호스트 노드 선택 (tempnode-bf3)
- `spec.strategy` — OnDelete 또는 RollingUpdate
- `spec.dpuTemplate.spec.bfb.name` — 사용할 BFB 이름
- `spec.dpuTemplate.spec.dpuFlavor` — 사용할 DPUFlavor 이름
- `spec.dpuTemplate.spec.nodeEffect` — 프로비저닝 중 노드 처리 방식

**프로비저닝 단계:**
```
Initializing → Node Effect (drain/taint)
  → Pending → Prepare BFB → Config FW Parameters
  → OS Installing → DPU Cluster Config
  → Host Network Configuration → Ready
```

---

### 1.4 DPU (자동 생성)

**역할:** DPUSet이 개별 DPU마다 자동 생성하는 프로비저닝 상태 추적 리소스

**원리:** 직접 생성하지 않음 — DPUSet 컨트롤러가 자동 관리

**언제 확인:**
```bash
kubectl get dpu -n dpf-operator-system -w   # 프로비저닝 진행 상태 모니터링
kubectl describe dpu <name> -n dpf-operator-system
```

---

### 1.5 DPUCluster

**역할:** DPU 전용 Kubernetes 클러스터의 컨트롤 플레인 관리

**원리:**
- `kamaji` 타입 — Kamaji가 Host Cluster Pod으로 kube-apiserver 실행 (우리 환경)
- `static` 타입 — 이미 존재하는 클러스터를 kubeconfig로 연결
- DPF Operator가 DPUSet 온보딩 완료 시 자동 생성

**언제 사용:**
- 직접 생성보다는 자동 생성 후 상태 확인용
- static 타입: 이미 실행 중인 DPU 클러스터를 DPF에 연결할 때

```bash
kubectl get dpucluster -A              # 클러스터 목록
kubectl get dpucluster -o yaml -A      # 상세 상태
```

---

### 1.6 DPUNode

**역할:** DPU를 탑재한 호스트 노드의 노드 수준 설정 (재부팅 방법, DMS 통신)

**원리:**
- DPF Agent가 각 노드마다 DPUNode 리소스를 자동 생성
- 재부팅 방법 설정: gNOI(네트워크 기반), external(외부 스크립트), script(커스텀)
- DMS(Device Management Service)를 통해 DPU BMC와 통신

**언제 사용:**
- 재부팅 방법을 커스텀 스크립트로 바꿀 때
- DMS IP/포트를 수동 설정할 때

---

### 1.7 DPUDevice

**역할:** 개별 DPU 하드웨어 인벤토리 리소스

**원리:**
- DPU 검색 시 자동 생성(DPUDiscovery) 또는 수동 생성
- 시리얼 번호, BMC IP, PF 수, MAC 주소 등 하드웨어 정보 저장

**언제 확인:**
```bash
kubectl get dpudevice -A        # DPU 하드웨어 목록
```

---

### 1.8 DPUDiscovery

**역할:** IP 범위를 스캔해 DPU BMC를 자동 검색하고 DPUDevice 생성

**원리:**
- Zero-Trust 배포에서 BMC IP 범위를 지정하면 자동 스캔
- 발견된 DPU마다 DPUDevice 리소스 자동 생성
- `scanInterval`마다 반복 스캔

**언제 사용:**
- Zero-Trust 환경에서 DPU를 자동 검색할 때
- 우리 환경(Host-Trusted, tempnode-bf3 직접 접근)에서는 불필요

```yaml
apiVersion: provisioning.dpu.nvidia.com/v1alpha1
kind: DPUDiscovery
spec:
  ipRangeSpec:
    ipRange:
      startIP: 192.168.100.1
      endIP: 192.168.100.254
      port: 443
  scanInterval: 1h
  workers: 5
```

---

## 2. Service API

### 2.1 DPUService

**역할:** DPU 클러스터에 Helm 차트를 DaemonSet으로 배포

**원리:**
- Host Cluster에서 선언 → DPF Operator가 DPU 클러스터에 Helm 배포
- DaemonSet으로 배포되므로 모든 DPU 노드에서 실행됨
- `svc.dpu.nvidia.com/critical` 레이블 시 Pod 미실행 시 호스트 노드 taint
- `spec.paused: true`로 일시 중단 가능

**언제 사용:**
- DPU 위에서 실행할 앱(DOCA 앱, 네트워크 서비스, 개발 환경 등)을 배포할 때
- HBN(Host Based Networking), SFC 컨트롤러 등 DOCA 서비스 배포 시
- doca-dev 개발 환경 배포 시

```yaml
apiVersion: svc.dpu.nvidia.com/v1alpha1
kind: DPUService
metadata:
  name: doca-dev
  namespace: dpf-operator-system
spec:
  helmChart:
    source:
      repoURL: oci://harbor.cluster.local/charts
      chart: doca-dev
      version: "1.0.0"
    values:
      image: nvcr.io/nvidia/doca/doca_container_base:3.2.0-devel
      resources:
        limits:
          cpu: "4"
          memory: "8Gi"
  interfaces:
    - name: p0
  configPorts:
    - name: ssh
      port: 22
      protocol: TCP
  paused: false
```

**핵심 필드:**
- `spec.helmChart.source` — Helm 차트 위치
- `spec.helmChart.values` — Helm values 오버라이드
- `spec.interfaces` — 사용할 네트워크 인터페이스 (DPUServiceInterface 참조)
- `spec.configPorts` — 호스트 클러스터에 노출할 포트
- `spec.paused` — 일시 중단 여부

---

### 2.2 DPUServiceInterface

**역할:** DPUService가 사용하는 네트워크 인터페이스 정의

**원리:**
- DPU에는 여러 인터페이스가 있음: 물리 포트(p0/p1), PF, SF(Scalable Function), 서비스 네트워크
- 각 인터페이스 타입마다 설정 방법이 다름
- Host Cluster에서 선언 → DPU 클러스터의 `ServiceInterface` 리소스로 자동 동기화

**인터페이스 타입:**

| 타입 | 설명 | 사용 사례 |
|------|------|---------|
| `physical` | 물리 포트 (p0, p1) | WAN/업스트림 연결 |
| `pf` | Physical Function | 호스트 측 연결 |
| `sf` | Scalable Function | 고성능 DPU 서비스 |
| `service` | 서비스 간 가상 네트워크 | DPUService 간 통신 |

```yaml
apiVersion: svc.dpu.nvidia.com/v1alpha1
kind: DPUServiceInterface
metadata:
  name: p0-interface
  namespace: dpf-operator-system
spec:
  template:
    spec:
      template:
        spec:
          interfaceType: physical
          physical:
            interfaceName: p0
```

---

### 2.3 DPUServiceChain

**역할:** DPUServiceInterface 사이의 트래픽 경로(서비스 체이닝) 정의

**원리:**
- 트래픽이 어떤 인터페이스 → 어떤 서비스 → 어떤 인터페이스 순으로 흐를지 정의
- SFC(Service Function Chaining) 아키텍처 구현
- Host Cluster에서 선언 → DPU 클러스터의 `ServiceChain`으로 자동 동기화

**예시 흐름:**
```
물리 포트(p0) → DPUService A (방화벽) → DPUService B (로드밸런서) → 호스트 PF
```

**언제 사용:**
- 트래픽이 여러 DPUService를 거쳐야 할 때
- 인라인 처리(암호화, 방화벽, 모니터링) 파이프라인 구성 시

---

### 2.4 DPUServiceTemplate

**역할:** DPUService의 기본 설정 템플릿 (NVIDIA가 공개)

**원리:**
- NVIDIA가 검증된 서비스(HBN, Firefly 등)에 대해 공식 템플릿 제공
- 리소스 요구사항(CPU, 메모리, SR-IOV SF 수) 포함
- DPUDeployment에서 DPUServiceConfiguration과 함께 사용

**언제 사용:**
- DPUDeployment로 표준 DOCA 서비스 배포 시
- 직접 DPUService 작성 없이 공식 템플릿 활용 시

---

### 2.5 DPUServiceConfiguration

**역할:** DPUServiceTemplate의 사용자 정의 오버라이드

**원리:**
- Template(기본값) + Configuration(사용자 설정) 병합 → DPUService 생성
- Configuration이 Template보다 우선순위 높음

**언제 사용:**
- 표준 템플릿의 일부 값만 커스터마이징할 때
- 환경별 설정 분리 시

---

### 2.6 DPUDeployment

**역할:** BFB + DPUFlavor + DPUService + ServiceChain을 하나로 묶는 최상위 오케스트레이터

**원리:**
- DPUSet, DPUService, DPUServiceInterface, DPUServiceChain을 **자동 생성**
- 리소스 호환성 검증: DPUService가 DPU에 스케줄 가능한지 사전 체크
- 버전 호환성 검증: 각 컴포넌트 버전 요구사항 충족 여부 확인
- 의존성 순서 관리: 프로비저닝 완료 후 서비스 배포

**언제 사용:**
- 완전한 DPU 스택을 한 번에 선언적으로 배포할 때 (권장)
- 개별 BFB/DPUSet/DPUService를 직접 관리하기 복잡할 때

```yaml
apiVersion: svc.dpu.nvidia.com/v1alpha1
kind: DPUDeployment
metadata:
  name: bf3-full-stack
  namespace: dpf-operator-system
spec:
  bfb:
    name: doca-3.2
  flavor:
    name: base-flavor
  dpuNodeSelector:
    matchLabels:
      feature.node.kubernetes.io/dpu-enabled: "true"
  services:
    hbn:
      serviceTemplate: hbn-template
      serviceConfiguration: hbn-config
    firefly:
      serviceTemplate: firefly-template
```

---

### 2.7 DPUServiceIPAM

**역할:** DPUService용 IP 주소 풀 관리

**원리:**
- 서비스 네트워크 인터페이스에 IP를 자동 할당
- 멀티테넌시 환경에서 테넌트별 IP 범위 격리

**언제 사용:**
- DPUServiceInterface type=service에 IP 범위를 설정할 때
- 서비스 간 통신에 사설 IP 범위를 쓸 때

---

### 2.8 DPUServiceNAD

**역할:** DPU 클러스터 Pod의 보조 네트워크 인터페이스 (Multus CNI)

**원리:**
- Kubernetes의 NetworkAttachmentDefinition(NAD)을 DPU 클러스터에 동기화
- Pod에 여러 네트워크를 붙이기 위해 Multus CNI 활용

**언제 사용:**
- DPU Pod에 메인 CNI 외 추가 네트워크 인터페이스가 필요할 때

---

### 2.9 DPUServiceCredentialRequest

**역할:** DPU 클러스터 워크로드가 Host Cluster에 인증하기 위한 크리덴셜 관리

**원리:**
- DPU 클러스터 → Host Cluster 간 ServiceAccount 토큰 자동 발급
- 크로스 클러스터 API 접근 시 사용

**언제 사용:**
- DPU에서 실행 중인 서비스가 Host Cluster API에 접근해야 할 때

---

## 3. Operator API

### 3.1 DPFOperatorConfig

**역할:** DPF Operator 전체 동작을 제어하는 싱글톤 리소스

**원리:**
- 클러스터당 1개만 존재
- Kamaji / Static 클러스터 매니저 활성화 여부
- BFB 저장용 PVC 이름, 동시 프로비저닝 수, 네트워킹 MTU 설정
- 이미지 풀 시크릿 전역 설정

**언제 사용:**
- Kamaji 대신 Static 클러스터 매니저로 전환할 때
- BFB 저장 PVC를 변경할 때
- 네트워크 MTU를 조정할 때

```yaml
apiVersion: operator.dpu.nvidia.com/v1alpha1
kind: DPFOperatorConfig
metadata:
  name: dpfoperatorconfig
  namespace: dpf-operator-system
spec:
  provisioningController:
    bfbPVCName: bfb-pvc
    maxUnavailableDPUNodes: 1     # 동시 프로비저닝 최대 노드 수
  networking:
    controlPlaneMTU: 1500         # 관리 네트워크 MTU
    highSpeedMTU: 9000            # 고속 인터페이스 MTU
  imagePullSecrets:
    - name: nvcr-secret           # NGC 인증
```

---

## 4. VPC API

### 4.1 DPUVPC / DPUVirtualNetwork / IsolationClass

**역할:** DPU 환경의 가상 사설 클라우드 및 멀티테넌시 네트워크 격리

**원리:**
- DPUVPC — 테넌트별 가상 네트워크 공간
- DPUVirtualNetwork — VPC 내 가상 네트워크 정의
- IsolationClass — 트래픽 격리 정책 및 QoS 클래스

**언제 사용:**
- 여러 테넌트가 같은 DPU 인프라를 공유할 때
- 테넌트 트래픽 완전 격리가 필요할 때

---

## 5. Storage API

### 5.1 DPUStoragePolicy / DPUStorageVendor / DPUVolume / DPUVolumeAttachment

**역할:** DPU 환경의 스토리지 볼륨 관리

**원리:**
- DPUStoragePolicy — 스토리지 접근 정책 및 쿼터
- DPUStorageVendor — 벤더별 스토리지 백엔드 설정
- DPUVolume — DPU 워크로드용 퍼시스턴트 볼륨
- DPUVolumeAttachment — 볼륨을 DPU 노드에 마운트

**언제 사용:**
- doca-dev 등 퍼시스턴트 스토리지가 필요한 DPU 워크로드 배포 시

---

## 6. Node Resources API

### 6.1 NodeSriovDevicePluginConfig

**역할:** 호스트 노드의 SR-IOV 디바이스 플러그인 설정

**원리:**
- 호스트 노드에서 SR-IOV VF를 Kubernetes 리소스(e.g. `nvidia.com/bf_sf`)로 노출
- Kubernetes 스케줄러가 VF를 Pod에 할당할 수 있도록 등록

**언제 사용:**
- 호스트 노드 Pod에서 BF3 VF를 직접 사용할 때
- `resources.limits.nvidia.com/bf_sf: "1"` 같은 리소스 요청을 사용할 때

---

## 빠른 참조

### "내가 지금 하고 싶은 것"으로 찾기

| 하고 싶은 것 | 사용할 CRD |
|------------|-----------|
| DPU에 OS 설치 | `BFB` + `DPUFlavor` + `DPUSet` |
| DPU 앱 배포 | `DPUService` |
| DPU 앱 + 네트워킹 한 번에 | `DPUDeployment` |
| DPU 클러스터 상태 확인 | `DPUCluster` |
| 개별 DPU 프로비저닝 상태 | `DPU` |
| 네트워크 인터페이스 정의 | `DPUServiceInterface` |
| 서비스 간 트래픽 경로 | `DPUServiceChain` |
| Operator 전역 설정 | `DPFOperatorConfig` |
| DPU 자동 검색 (Zero-Trust) | `DPUDiscovery` |
| DPU 하드웨어 인벤토리 | `DPUDevice` |
| 호스트에서 VF 사용 | `NodeSriovDevicePluginConfig` |

### 우리 환경(Sandbox)에서 당장 필요한 것

```
1단계: BFB + DPUFlavor + DPUSet  → BF3 DPU를 DPF 관리 하에 온보딩
2단계: DPUService (doca-dev)      → DPU 위 개발 환경 배포
3단계: DPUServiceInterface        → 네트워크 인터페이스 연결 (필요 시)
```

### 자동 생성 리소스 (직접 생성하지 않음)

- `DPU` — DPUSet이 생성
- `ServiceChain` — DPUServiceChain이 생성
- `ServiceChainSet` — DPUServiceChain이 생성
- `ServiceInterface` — DPUServiceInterface가 생성
- `ServiceInterfaceSet` — DPUServiceInterface가 생성
- `DPUNode` — DPF Agent가 생성
- `DPUCluster` — DPF Operator가 온보딩 시 생성
