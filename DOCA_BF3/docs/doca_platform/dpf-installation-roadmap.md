---
title: "DPF 설치 로드맵"
---

[TOC]

## 현재 상태 (Gap Analysis)

### 클러스터 컴포넌트 현황

| 컴포넌트 | 상태 | 비고 |
|---------|------|------|
| Cilium | ✅ 운영중 | 전 노드 CNI |
| ArgoCD | ✅ 운영중 | `argocd` 네임스페이스 |
| NFD | ✅ 운영중 | `feature.node.kubernetes.io/*` 레이블 부착 |
| Multus | ✅ CRD 존재 | |
| Rook-Ceph | ✅ 운영중 | sandbox-1, 2, 4, tempnode-bf3 OSD |
| Harbor | ✅ 운영중 | 클러스터 내 레지스트리 |
| GPU Operator | ✅ 운영중 | tempnode-bf3 TITAN V 관리 |
| Prometheus | ✅ 운영중 | |
| cert-manager | ❌ 미설치 | 네임스페이스만 존재, Pod 없음 |
| Kamaji | ❌ 미설치 | |
| maintenance-operator | ❌ 미설치 | |
| DPF Operator | ❌ 미설치 | DPF CRD 전무 |
| SR-IOV VF | ❌ 비활성 | numvfs=0 (totalvfs=16 가용) |
| DPU kubeconfig | ❌ 없음 | Kamaji 설치 후 생성됨 |

### 핵심 문제

```
현재: tempnode-bf3 BF3 DPU → SSH로만 접근 가능, K8s 리소스로 관리 불가
목표: BF3 DPU → kubectl로 선언적 관리 + DPU 클러스터에서 DOCA 앱 실행
```

---

## 최종 목표

```
[Host Cluster]
  └── DPF Operator
        └── DPUSet → tempnode-bf3 BF3 온보딩
              └── [DPU Cluster, arm64] (Kamaji 관리)
                    ├── doca-dev Pod (DOCA SDK + Kaniko)
                    │     → 네이티브 arm64 빌드 → Harbor push
                    └── DPUService (HBN, Firefly, DTS, 커스텀 앱)
                          → ArgoCD auto-sync
```

- SSH 불필요 — 전체 플로우 `kubectl`만으로
- 크로스컴파일 불필요 — BF3(arm64) 위에서 네이티브 빌드
- GitOps — ArgoCD로 버전 관리 및 롤백

---

## 컴포넌트별 설치 이유

### cert-manager

- **역할** — TLS 인증서 자동 발급/갱신
- **DPF에서 필요한 이유**
  - DPF Operator 웹훅 서버 인증서 자동 관리
  - Kamaji가 DPU 클러스터 API 서버 인증서 발급에 사용
- **없으면** — DPF Operator 웹훅 Pod `CrashLoopBackOff`, Kamaji TenantControlPlane 생성 불가

### Kamaji

- **역할** — Host Cluster Pod 안에서 DPU 측 Kubernetes 컨트롤 플레인 실행
- **DPF에서 필요한 이유**
  - BF3 DPU는 자체 K8s 컨트롤 플레인이 없음 → Kamaji가 `TenantControlPlane` CRD로 대신 생성
  - DPU kubeconfig 자동 생성 (DPUService 배포 대상)
- **없으면** — DPU 클러스터 자체 생성 불가 → `DPUService`, `doca-dev` Pod 배포 불가
- **아키텍처**
  ```
  Host Cluster Pod: kube-apiserver + etcd (DPU용)
    → DPU가 이 API 서버에 조인
    → DPU kubeconfig 자동 생성
  ```

### maintenance-operator

- **역할** — 노드 유지보수 시 워크로드 자동 드레인/코든 오케스트레이션
- **DPF에서 필요한 이유**
  - BFB(BlueField Boot Image) 플래시 전 tempnode-bf3 워크로드 안전하게 이동
  - DPU 온보딩/오프보딩 자동화
- **없으면** — BFB 플래시 중 tempnode-bf3 실행 중 Pod(GPU 워크로드 등) 강제 종료 위험

### DPF Operator

- **역할** — DPF 전체 서브시스템 오케스트레이션 (핵심 컴포넌트)
- **등록하는 CRD**
  ```
  DPUSet, DPU, DPUCluster, DPUFlavor, BFB
  DPUService, DPUDeployment, DPUServiceCredentialRequest
  DPUServiceChain, DPUServiceInterface
  DPFOperatorConfig
  ```
- **없으면** — 위 kubectl 명령 전부 불가, DPU 관련 선언형 관리 불가
- **설치 후** — provisioning-controller, dpuservice-controller, hostagent 등 자동 배포

### SR-IOV VF 활성화

- **역할** — BF3 물리 포트를 여러 개의 가상 함수(VF)로 분할, Pod에 직접 할당
- **현재 상태** — `numvfs=0` (VF 없음), Pod는 Cilium overlay만 사용
- **활성화 후 가능한 것**
  - DPDK 기반 패킷 처리 (라인레이트, μs 지연시간)
  - RDMA / GPUDirect (TITAN V ↔ BF3 직접 통신)
  - `nvidia.com/bf_sf: "1"` 리소스로 Pod에 SF 직접 할당
- **없으면** — BF3 하드웨어 가속 기능 전혀 미활용

---

## 단계별 설치 순서

> **주의:** ArgoCD, NFD 이미 설치됨 → `prereqs.yaml` 전체 적용 금지, `--selector`로 개별 설치

### 1단계 — 사전 확인

```bash
# StorageClass 확인 (Kamaji etcd용)
kubectl get sc

# Harbor 주소 확인
kubectl get svc -A | grep harbor

# tempnode-bf3 상태 확인
kubectl get node tempnode-bf3 --show-labels
```

- local-path-provisioner 또는 대체 StorageClass 필요
- Harbor 인증 정보(push 권한) 확인

---

### 2단계 — cert-manager 설치

```bash
./hack/scripts/deploy-helmfile.sh \
  -f deploy/helmfiles/prereqs.yaml \
  --selector app=cert-manager
```

확인:
```bash
kubectl get pods -n cert-manager
# cert-manager, cert-manager-cainjector, cert-manager-webhook 모두 Running
```

---

### 3단계 — Kamaji 설치

```bash
./hack/scripts/deploy-helmfile.sh \
  -f deploy/helmfiles/prereqs.yaml \
  --selector app=kamaji
```

확인:
```bash
kubectl get pods -n kamaji-system
kubectl get crd | grep tenantcontrolplane
```

---

### 4단계 — maintenance-operator 설치

```bash
./hack/scripts/deploy-helmfile.sh \
  -f deploy/helmfiles/prereqs.yaml \
  --selector app=maintenance-operator
```

확인:
```bash
kubectl get pods -n dpf-operator-system | grep maintenance
```

---

### 5단계 — DPF Operator 설치

```bash
helm install dpf-operator ./deploy/charts/dpf-operator \
  -n dpf-operator-system --create-namespace
```

확인:
```bash
kubectl get pods -n dpf-operator-system
kubectl get crds | grep -E "dpu|dpf|bluefield"
# 20+ CRD 등록됨
```

tempnode-bf3 레이블링:
```bash
kubectl label node tempnode-bf3 feature.node.kubernetes.io/dpu-enabled=true
```

---

### 6단계 — DPUSet 생성 → DPU 클러스터 온보딩

```yaml
# dpuset-bf3.yaml
apiVersion: provisioning.dpf.nvidia.com/v1alpha1
kind: DPUSet
metadata:
  name: bf3-pool
  namespace: dpf-operator-system
spec:
  nodeSelector:
    matchLabels:
      feature.node.kubernetes.io/dpu-enabled: "true"
  dpuTemplate:
    spec:
      dpuFlavor: hbn-firefly
      bfb: doca-3.2.0118
```

```bash
kubectl apply -f dpuset-bf3.yaml
```

확인:
```bash
kubectl get dpu -A
kubectl get dpucluster -A
# DPU kubeconfig Secret 자동 생성 확인
kubectl get secret -n dpf-operator-system | grep kubeconfig
```

---

### 7단계 — doca-dev DPUService 배포

```yaml
# dpuservice-doca-dev.yaml
apiVersion: dpuservice.dpf.nvidia.com/v1alpha1
kind: DPUService
metadata:
  name: doca-dev
spec:
  helm:
    chart: doca-dev
  serviceConfiguration:
    deploymentServiceName: doca-dev
    resources:
    - name: dev
      image: nvcr.io/nvidia/doca/doca_container_base:3.2.0-devel
      resources:
        limits:
          nvidia.com/bf_sf: "1"
    - name: builder
      image: gcr.io/kaniko-project/executor:latest
```

확인:
```bash
# DPU 클러스터에서 Pod 확인
kubectl --kubeconfig=<dpu-kubeconfig> get pods
```

---

### 8단계 — SR-IOV VF 활성화

DPF DPUFlavor에서 선언적으로 관리:
```yaml
nvConfig:
  NUM_OF_VFS: "8"
  SRIOV_EN: "1"
  PF_TOTAL_SF: "20"
```

임시 수동 확인:
```bash
# tempnode-bf3에서
cat /sys/class/net/enp175s0f0np0/device/sriov_numvfs  # 현재 0
cat /sys/class/net/enp175s0f0np0/device/sriov_totalvfs  # 16 가용
```

---

## 설치 완료 후 기대 상태

```
kubectl get dpucluster -A
  → tempnode-bf3 DPU 클러스터 Running

kubectl --kubeconfig=<dpu> get nodes
  → bluefield-3 (arm64) Ready

kubectl --kubeconfig=<dpu> get pods
  → doca-dev Running (개발 환경)

kubectl get dpuservice -A
  → hbn, dts, doca-dev Running
```

---

## 미확인 사항

| 항목 | 확인 방법 |
|------|----------|
| Harbor 주소/인증 | `kubectl get svc -A \| grep harbor` |
| StorageClass | `kubectl get sc` |
| DPU kubeconfig 위치 | DPF Operator 설치 후 Secret 확인 |
| BF3 mlxconfig 값 | `mst` 도구 설치 후 `mlxconfig -d ... q` |
| NGC 인증 (DOCA 이미지 pull) | `nvcr.io` 접근 가능 여부 확인 |
