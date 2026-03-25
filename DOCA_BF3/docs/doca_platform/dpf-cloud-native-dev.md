---
title: "DPF Cloud-Native 개발 전략"
---

[TOC]

## 개요

목표: **BF3 DPU를 "특수 하드웨어"가 아닌 "Kubernetes 리소스"로 추상화**

- SSH 수동 관리 → `kubectl apply` 선언적 관리
- 크로스 컴파일 → DPU 위 네이티브 빌드
- 수동 배포 → ArgoCD GitOps 자동 sync

선행조건:
- Host cluster에 `cert-manager`, `Kamaji`, `maintenance-operator`, `DPF Operator`가 먼저 설치되어 있어야 함
- `tempnode-bf3`가 DPF 관리 대상으로 온보딩되고 DPU kubeconfig를 확보한 뒤에 이 개발 흐름을 적용하는 것이 자연스러움

---

## DPF 도입 이점

### 1. DPU 라이프사이클 자동화

**Before:**
```bash
ssh joon@tempnode-bf3 → ssh ubuntu@192.168.101.2
systemctl restart ovs-vswitchd
ovs-vsctl add-br br-sfc
# 반복, 멱등성 없음
```

**After:**
```yaml
apiVersion: provisioning.dpf.nvidia.com/v1alpha1
kind: DPUSet
metadata:
  name: bf3-pool
spec:
  nodeSelector:
    matchLabels:
      feature.node.kubernetes.io/dpu-enabled: "true"
  dpuTemplate:
    spec:
      dpuFlavor: hbn-firefly
      bfb: doca-3.2.0118
```

→ `kubectl apply` 하나로: BFB 플래시 → VF 생성 → DPU 클러스터 조인

---

### 2. DPU 서비스 GitOps 배포

```yaml
# HBN, DTS, Firefly 등 DPU 서비스 — Helm + ArgoCD 버전 관리
apiVersion: dpuservice.dpf.nvidia.com/v1alpha1
kind: DPUService
metadata:
  name: hbn
spec:
  helm:
    chart: doca-hbn
    version: v1.0.5
    # ArgoCD가 DPU 클러스터로 자동 sync
```

- 롤백: `helm rollback` 또는 Git revert
- 버전 히스토리: Git 기반

---

### 3. SR-IOV VF → Pod 직접 연결

현재 상태: `numvfs=0` (VF 미생성) — Pod는 Cilium overlay만 사용

DPF 적용 후:
```yaml
resources:
  limits:
    nvidia.com/bf_sf: "1"   # BF3 Scalable Function 직접 할당
```

- DPDK 기반 패킷 처리 — overlay 없이 라인레이트
- `enp175s0f0np0` 직접 사용 (MTU 9000)
- GPU(TITAN V) + BF3 네트워크 오프로드 동시 활용

---

### 관리 방식 비교

| 항목 | Before DPF | After DPF |
|------|------------|-----------|
| DPU 설정 | SSH 수동 | `kubectl apply` |
| 서비스 배포 | 직접 설치 | Helm + ArgoCD |
| VF 할당 | `echo 8 > sriov_numvfs` | 자동 (DPUSet) |
| 스케일 | BF3 1대씩 수동 | nodeSelector 자동 |
| 모니터링 | SSH + journalctl | Prometheus/Grafana |
| 버전 관리 | 없음 | Git 기반 롤백 |

---

## Cilium 기반 단일 BF3 노드 구성

### 완전 지원 — 구조

```
sandbox-1~4  →  일반 워크로드 (Cilium only)
tempnode-bf3 →  Cilium + BF3 DPU (DPF 관리 대상)
```

DPF는 노드 수 무관 — `nodeSelector`로 BF3 노드만 격리:

```yaml
nodeSelector:
  matchLabels:
    feature.node.kubernetes.io/dpu-enabled: "true"  # tempnode-bf3만
```

### Cilium과 공존

| 트래픽 유형 | 처리 주체 |
|------------|----------|
| Pod-to-Pod (일반) | Cilium overlay (기존 그대로) |
| tempnode-bf3 고성능 워크로드 | BF3 SR-IOV VF 직접 |
| DPU 서비스 (HBN/Firefly) | BF3 내부 OVS/DPDK |

→ Cilium 제거 없이 BF3 노드에서만 DPF 추가 레이어 동작

### 단일 BF3 활용 시나리오

```
tempnode-bf3 GPU 워크로드 (TITAN V)
  → SR-IOV VF로 고성능 네트워크 직접 할당
  → BF3가 패킷처리 오프로드 (호스트 CPU 부담 감소)
  → GPU는 연산에만 집중
```

---

## 기존 DOCA 애플리케이션 Cloud-Native 전환

### 이미지 재사용 가능 — 3가지 패턴

**패턴 A: 기존 이미지 그대로 DPUService 래핑**
```yaml
kind: DPUService
spec:
  helm:
    chart: my-doca-app        # 직접 만든 Helm 차트
  serviceConfiguration:
    resources:
    - name: my-doca-app
      requests:
        nvidia.com/bf_sf: "1"  # BF3 리소스 할당
```

**패턴 B: DummyDPUService 템플릿 활용**
```
dpuservices/dummydpuservice/  ← 레포 내 레퍼런스 존재
```
→ 복사 후 이미지만 교체

**패턴 C: DPU 클러스터 직접 배포 (DOCA Sample)**
```bash
kubectl --kubeconfig=<dpu-kubeconfig> apply -f doca-sample-pod.yaml
```

### 기존 개발 방식 vs DPF 방식

| 항목 | 기존 방식 | DPF 방식 |
|------|---------|---------|
| 배포 | SSH → `docker run` | `kubectl apply` |
| 이미지 | arm64 (재사용) | 동일 — 변경 없음 |
| 설정 | 수동 env 주입 | ConfigMap/Secret |
| 재시작 | SSH 수동 | K8s 자동 |
| 로그 | SSH + journalctl | `kubectl logs` |
| 버전 | 태그 직접 관리 | GitOps (ArgoCD) |

---

## Cloud-Native 개발 환경 — DPU-Native Dev Pod

### 목표

```
기존: Host (amd64) → 크로스컴파일 → arm64 이미지 → DPU 배포 → 테스트
신규: DPU Pod (arm64) → 네이티브 빌드 → Harbor push → DPUService 배포 → 즉시 테스트
```

### 아키텍처

```
Host Cluster
└── DPF Operator
    └── DPU Cluster (Kamaji, arm64)
        └── doca-dev Pod
            ├── [dev]     DOCA SDK + 빌드 도구 (meson, ninja, clang)
            └── [builder] Kaniko (이미지 빌드 → Harbor push)
                    ↓
              Harbor (클러스터 내 레지스트리)
                    ↓
              DPUService 배포 (ArgoCD auto-sync)
```

### Dev Pod 구성

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: doca-dev
spec:
  containers:
  - name: dev
    image: nvcr.io/nvidia/doca/doca_container_base:3.2.0-devel  # arm64
    resources:
      limits:
        nvidia.com/bf_sf: "1"    # BF3 직접 접근 (DPDK 테스트 가능)
    volumeMounts:
    - name: workspace
      mountPath: /workspace      # Rook-Ceph PVC (코드 영속성)

  - name: builder
    image: gcr.io/kaniko-project/executor  # DinD 없이 이미지 빌드
    # → Harbor push

  volumes:
  - name: workspace
    persistentVolumeClaim:
      claimName: doca-dev-workspace
```

### 개발 사이클

```
① git clone → /workspace (Rook-Ceph PVC)
        ↓
② 네이티브 arm64 빌드 (크로스컴파일 불필요)
   kubectl exec doca-dev -c dev -- meson setup build && ninja -C build
        ↓
③ 이미지 빌드 → Harbor push
   Kaniko: --destination harbor.cluster/doca/my-app:latest
        ↓
④ DPUService 이미지 태그 업데이트 → ArgoCD auto-sync
        ↓
⑤ 같은 BF3 하드웨어에서 즉시 실행/테스트
        ↓
⑥ 문제 있으면 → ②
```

### 주의 사항

| 항목 | 내용 |
|------|------|
| DPU 리소스 | BF3 RAM 제한 — 무거운 SDK 전체 빌드는 분리 고려 |
| 이미지 빌드 | DinD(privileged) 대신 **Kaniko** 권장 |
| 코드 영속성 | Rook-Ceph PVC 필수 (Pod 재시작 대비) |
| 레지스트리 | Harbor 인증 정보 확인 필요 |

### 이 방식의 이점

- **크로스컴파일 제거** — arm64 네이티브 빌드 속도/정확도 향상
- **하드웨어 즉시 테스트** — 빌드 환경 = 실행 환경
- **SSH 불필요** — 전체 플로우 `kubectl`로만 수행
- **GitOps 통합** — 코드 변경 → 자동 배포 파이프라인 가능
