---
title: "DPF Bootstrap — ArgoCD 형상관리 가이드"
---

[TOC]

## 개요

DPF(DOCA Platform Framework) 설치에 필요한 선행 컴포넌트들을 SandBox-Infra 레포의 ArgoCD App-of-Apps 패턴으로 관리한다.

```
[Root App-of-Apps]  argocd/values.yaml
  └── doca-platform (syncWave: 50)
        └── [Sub App-of-Apps]  argocd/apps/07-Doca-Platform/
              ├── cert-manager     (wave 2)
              ├── kamaji           (wave 3)
              ├── maintenance-operator (wave 4)
              ├── dpf-node-label   (wave 5)
              └── dpf-operator     (wave 6)
```

---

## 파일 구조

```
argocd/apps/07-Doca-Platform/
├── Chart.yaml                          — 서브 Helm 차트 정의
├── templates/
│   └── application-template.yaml      — Application 생성 템플릿 (CASE 1/2/3)
├── values.yaml                         — 하위 컴포넌트 Application 정의
├── cert-manager/values.yaml            — cert-manager Helm values (원본 기반)
├── kamaji/values.yaml                  — kamaji Helm values (원본 기반)
├── maintenance-operator/values.yaml    — maintenance-operator Helm values (원본 기반)
├── dpf-operator/values.yaml            — dpf-operator Helm values (원본 기반)
└── node-label-bf3/
    ├── kustomization.yaml
    ├── rbac.yaml                       — ServiceAccount + ClusterRole (nodes patch)
    └── job.yaml                        — PostSync Hook Job (tempnode-bf3 레이블)
```

---

## 템플릿 CASE 구분

`application-template.yaml`에 3가지 소스 패턴 지원:

| CASE | 조건 | 용도 |
|------|------|------|
| CASE 1 | `source.chart` 존재 | 외부 Helm/OCI 레지스트리 차트 + SandBox-Infra values |
| CASE 2 | `source.externalPath` 존재 | 외부 Git 레포 Helm 차트 + SandBox-Infra values |
| CASE 3 | `source.path` 존재 | SandBox-Infra 내부 Manifest/Kustomize |

---

## 컴포넌트별 Values 변경 내용

### cert-manager

- **차트:** `https://charts.jetstack.io` / `cert-manager` / `v1.19.3`
- **원본 values:** `helm show values cert-manager --repo https://charts.jetstack.io --version v1.19.3`

| 항목 | 원본 | 변경 | 이유 |
|------|------|------|------|
| `crds.enabled` | `false` | `true` | Helm이 CRD를 직접 설치하도록. 없으면 DPF Operator 웹훅 CRD 누락 |
| `startupapicheck.enabled` | `true` | `false` | ArgoCD 환경에서 post-install hook Job이 ArgoCD sync를 블록할 수 있음 |

---

### kamaji

- **차트:** `oci://ghcr.io/nvidia/charts/kamaji` / `1.2.0`
- **원본 values:** `helm show values oci://ghcr.io/nvidia/charts/kamaji --version 1.2.0`

| 항목 | 원본 | 변경 | 이유 |
|------|------|------|------|
| `image.repository` | `clastix/kamaji` | `ghcr.io/nvidia/kamaji` | 공식 upstream이 아닌 NVIDIA DPF 전용 fork 사용 |
| `image.tag` | `null` (chart appVersion) | `v1.34.0-25.9.3` | DPF prereqs.yaml 기준 검증된 버전 고정 |
| `kamaji-etcd.persistentVolumeClaim.storageClassName` | (미정의) | `rook-ceph-block-hot` | etcd 데이터 영속성. 클러스터에 설치된 Rook-Ceph StorageClass 사용 |

> **왜 NVIDIA fork?**
> `clastix/kamaji`는 범용 Kamaji이고, `ghcr.io/nvidia/kamaji`는 DPF가 관리하는 fork로 DPU 클러스터 TenantControlPlane 생성에 특화된 패치가 포함됨.

> **왜 affinity/tolerations는 수정 안 했나?**
> kamaji 기본값이 이미 `affinity: {}`, `tolerations: []`이므로 수정 불필요. 컨트롤 플레인 노드 선호는 기본 설정 없음.

---

### maintenance-operator

- **차트:** `oci://ghcr.io/mellanox/maintenance-operator-chart` / `0.3.0`
- **원본 values:** `helm show values oci://ghcr.io/mellanox/maintenance-operator-chart --version 0.3.0`

| 항목 | 원본 | 변경 | 이유 |
|------|------|------|------|
| `operator.tolerations` | control-plane NoSchedule taint 목록 | `[]` | Sandbox 클러스터 워커 노드에 스케줄링 가능하도록 |
| `operator.affinity` | control-plane preferred affinity | `{}` | 동상 |
| `operatorConfig.deploy` | `false` | `true` | `MaintenanceOperatorConfig` CR을 Helm이 자동 생성하도록. `false`면 수동으로 CR 별도 생성 필요 |
| `operatorConfig.maxParallelOperations` | `null` | `"60%"` | 동시에 유지보수 가능한 노드 상한. DPU 온보딩/BFB 플래시 중 워크로드 보호 |

---

### dpf-operator

- **차트:** `https://github.com/SJoon99/doca-platform.git` / `deploy/charts/dpf-operator` / `public-main` (CASE 2)
- **원본 values:** 레포 내 `deploy/charts/dpf-operator/values.yaml`

| 항목 | 원본 | 변경 | 이유 |
|------|------|------|------|
| `affinity` | `node-role.kubernetes.io/master` or `control-plane` requiredDuringScheduling | `{}` | Sandbox 워커 노드에 스케줄링. 컨트롤 플레인 전용 affinity 유지 시 Pending 상태 |
| `tolerations` | control-plane NoSchedule taint | `[]` | 동상 |
| `controllerManager.image.repository` | `""` | `""` (**TODO**) | Harbor에 DPF Operator 이미지 빌드/push 후 설정 필요 |
| `controllerManager.image.tag` | `""` | `""` (**TODO**) | 동상 |

> **⚠️ dpf-operator 이미지 설정 전까지 배포 불가**
> `repository`/`tag`가 비어있으면 Pod가 `ImagePullBackOff`. Harbor에 이미지 push 후 값 업데이트 필요.

---

### dpf-node-label (Kustomize)

tempnode-bf3에 DPF 레이블을 부착하는 one-shot Job.

```
feature.node.kubernetes.io/dpu-enabled=true
```

- **방식:** ArgoCD `PostSync` Hook + `BeforeHookCreation` 삭제 정책 → 매 sync마다 재실행 (idempotent, `--overwrite`)
- **RBAC:** `dpf-node-labeler` ServiceAccount → ClusterRole (`nodes: get, patch`)

---

## 사전 확인 항목

| 항목 | 확인 명령 | 비고 |
|------|-----------|------|
| `rook-ceph-block-hot` StorageClass 존재 | `kubectl get sc` | kamaji etcd PVC용 |
| ArgoCD에 OCI 레지스트리 등록 | ArgoCD UI → Settings → Repositories | `oci://ghcr.io/nvidia/charts`, `oci://ghcr.io/mellanox` |
| doca-platform 레포 접근 가능 | ArgoCD UI → Settings → Repositories | `https://github.com/SJoon99/doca-platform.git` |
| dpf-operator 이미지 준비 | `kubectl get pods -n dpf-operator-system` | 빌드 후 values 업데이트 |

---

## 잠재적 문제 및 대응

### cert-manager가 이미 부분 설치된 경우

```bash
# 네임스페이스만 있고 Pod 없는 상태
kubectl get ns cert-manager       # 존재
kubectl get pods -n cert-manager  # 없음
```

→ ArgoCD sync 전에 네임스페이스 수동 삭제 또는 그대로 sync (ArgoCD가 idempotent 처리).

---

### Kamaji etcd PVC Pending

```bash
kubectl get pvc -n dpf-operator-system
# STATUS: Pending
```

원인: `rook-ceph-block-hot` StorageClass 없거나 Ceph OSD 여유 없음.

대응:
```bash
kubectl get sc  # 사용 가능한 StorageClass 목록 확인
# values.yaml에서 storageClassName 수정 후 재sync
```

---

### OCI 레지스트리 인증 실패

ArgoCD가 `oci://ghcr.io/...` 차트를 pull하지 못하는 경우:

```
# ArgoCD UI → Settings → Repositories → + Connect Repo
# Type: Helm
# Repository URL: oci://ghcr.io/nvidia/charts
# (public이므로 인증 불필요)
```

또는 ArgoCD CLI:
```bash
argocd repo add oci://ghcr.io/nvidia/charts --type helm
argocd repo add oci://ghcr.io/mellanox --type helm
```

---

### dpf-operator ImagePullBackOff

`controllerManager.image.repository`/`tag` 미설정 시:

```bash
# dpf-operator 이미지 빌드 및 Harbor push 후
# argocd/apps/07-Doca-Platform/dpf-operator/values.yaml 수정:
#   controllerManager.image.repository: harbor.<domain>/dpf/dpf-operator
#   controllerManager.image.tag: v0.1.0
```

---

## 설치 완료 확인

```bash
# 모든 컴포넌트 Running 확인
kubectl get pods -n cert-manager
kubectl get pods -n dpf-operator-system

# DPF CRD 등록 확인
kubectl get crd | grep -E "dpu|dpf|dpuservice"

# tempnode-bf3 레이블 확인
kubectl get node tempnode-bf3 --show-labels | grep dpu-enabled
```
