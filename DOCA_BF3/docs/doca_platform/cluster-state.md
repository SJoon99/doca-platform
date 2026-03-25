---
title: "클러스터 현황 및 DPF 사전 조사"
---

[TOC]

## 클러스터 노드

| 노드 | IP | 역할 | OS | Kubernetes |
|------|----|------|-----|-----------|
| node4 | 10.34.20.3 | control-plane | Ubuntu 24.04.2 | v1.34.3 |
| sandbox-1 | 10.34.20.5 | worker | Ubuntu 24.04.2 | v1.34.3 |
| sandbox-2 | 10.34.20.6 | worker | Ubuntu 24.04.2 | v1.34.3 |
| sandbox-3 | 10.34.20.7 | worker | Ubuntu 24.04.2 | v1.34.3 |
| sandbox-4 | 10.34.20.8 | worker | Ubuntu 24.04.2 | v1.34.3 |
| tempnode-bf3 | 10.34.20.4 | worker + BF3 DPU | Ubuntu 24.04.2 | v1.34.3 |

- CNI: Cilium (전 노드)
- CRI: containerd 2.1.6
- tempnode-bf3 Taint: `gpu=dedicated:NoSchedule`, `doca=dedicated:NoSchedule`

---

## tempnode-bf3 상세

### 호스트 측

| 항목 | 값 |
|------|-----|
| BF3 PCI | `af:00.0`, `af:00.1` (ConnectX-7), `af:00.2` (SoC Mgmt) |
| PF0 인터페이스 | `enp175s0f0np0` — IP `10.34.20.4/12`, MTU 9000 |
| PF1 인터페이스 | `enp175s0f1np1` — IP `10.50.0.10/24` |
| rshim (tmfifo) | `tmfifo_net0` — `192.168.101.1/24` |
| SR-IOV totalvfs | PF0: 16 / PF1: 16 (enp175), enp61: 32 |
| SR-IOV numvfs | **0** (VF 미생성) |
| GPU | NVIDIA TITAN V × 2 (CUDA 12.8, driver 570) |

### BF3 DPU 측 (192.168.101.2)

접속: `ssh joon@10.34.20.4` → `ssh ubuntu@192.168.101.2`

| 항목 | 값 |
|------|-----|
| 호스트명 | `bluefield-3` |
| OS | Ubuntu 24.04.3 LTS |
| 커널 | `6.8.0-1012-bluefield-64k` |
| OOB IP | `oob_net0: 10.34.20.200/12` |
| DOCA 버전 | 3.2.0118 |
| OVS | `p0`, `p1`, `pf0hpf`, `pf1hpf`, `en3f0pf0sf0`, `en3f1pf1sf0`, `ovsbr1`, `ovsbr2` |
| Docker/containerd | ✅ 설치됨 |
| kubectl | ❌ 미설치 |
| 실행중 DOCA 서비스 | `mlx_ipmid.service` (IPMI daemon) |

---

## DPF 설치 사전 요건 현황

| 컴포넌트 | 상태 | 비고 |
|---------|------|------|
| ArgoCD | ✅ 실행중 | `argocd` 네임스페이스, 전 Pod Running |
| Cilium | ✅ 실행중 | 전 노드 CNI |
| NFD | ✅ 레이블 부착됨 | `feature.node.kubernetes.io/*` 존재 |
| cert-manager | ❌ 미설치 | 네임스페이스만 존재, Pod 없음 |
| Kamaji | ❌ 미설치 | |
| maintenance-operator | ❌ 미설치 | |
| DPF Operator | ❌ 미설치 | |
| DPF CRDs | ❌ 없음 | `dpu*`, `dpf*`, `bluefield*` CRD 전무 |
| Multus | ✅ CRD 존재 | `network-attachment-definitions.k8s.cni.cncf.io` |
| local-path-provisioner | 미확인 | |

---

## 기존 설치된 주요 구성요소

- **Rook-Ceph** — 분산 스토리지 (sandbox-1, 2, 4, tempnode-bf3 OSD)
- **GPU Operator** — tempnode-bf3 GPU 관리
- **Harbor** — 컨테이너 레지스트리 (7d10h)
- **Keycloak** — 인증
- **Prometheus** — 모니터링
- **PostgreSQL (CNPG)** — DB

---

## DPF 설치를 위한 필요 작업

> 주의: 현재 클러스터에는 ArgoCD와 NFD가 이미 설치되어 있으므로, `deploy/helmfiles/prereqs.yaml` 전체를 한 번에 적용하지 말고 필요한 릴리스만 selector로 설치하는 편이 안전하다.

**1단계 — 스토리지/레지스트리 정보 확인**
```bash
kubectl get sc
kubectl get pods -A | grep -E 'harbor|registry'
```

- `local-path-provisioner` 설치 여부 또는 대체 StorageClass 확인
- Harbor 주소, 프로젝트, 이미지 pull/push 인증 정보 확인

**2단계 — cert-manager 설치**
```bash
./hack/scripts/deploy-helmfile.sh -f deploy/helmfiles/prereqs.yaml --selector app=cert-manager
```

**3단계 — Kamaji 설치**
```bash
./hack/scripts/deploy-helmfile.sh -f deploy/helmfiles/prereqs.yaml --selector app=kamaji
# 네임스페이스: dpf-operator-system
```

**4단계 — maintenance-operator 설치**
```bash
./hack/scripts/deploy-helmfile.sh -f deploy/helmfiles/prereqs.yaml --selector app=maintenance-operator
# 네임스페이스: dpf-operator-system
```

**5단계 — DPF Operator 설치**
```bash
helm install dpf-operator ./deploy/charts/dpf-operator -n dpf-operator-system --create-namespace
```

**6단계 — tempnode-bf3 레이블링**
```bash
kubectl label node tempnode-bf3 feature.node.kubernetes.io/dpu-enabled=true
```

**7단계 — DPU 클러스터/DPUSet 설계**

- `DPFOperatorConfig`, `DPUSet`, 필요 시 `DPUService`를 정의
- 단일 BF3 노드만 대상으로 `feature.node.kubernetes.io/dpu-enabled=true` 라벨을 사용
- 이 단계가 끝나야 DPU kubeconfig 위치와 실제 DPU 제어 plane이 명확해짐

**8단계 — SR-IOV VF 활성화 검토** (tempnode-bf3에서)
```bash
# 임시 수동 검증용 예시
echo 8 > /sys/class/net/enp175s0f0np0/device/sriov_numvfs
```

- 현재는 `numvfs=0` 상태이며, 장기적으로는 DPF 선언형 워크플로우로 관리하는 편이 더 적절함

---

## 미확인 사항

- DPU kubeconfig 생성 방식 및 저장 위치 (Kamaji + DPF Operator 설치 후 확인 필요)
- 기본 StorageClass 및 `local-path-provisioner` 실제 사용 여부
- BF3 DPU의 현재 mlxconfig 값 (mst 도구 미설치 상태)
- Harbor 레지스트리 주소 및 인증 정보
