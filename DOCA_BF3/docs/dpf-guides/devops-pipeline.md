---
title: "DPF DevOps 파이프라인 — 전체 개요"
---

# DPF DevOps 파이프라인

DOCA 애플리케이션의 **개발 → 빌드 → 배포** 전 과정을 K8s-native로 운영하는 파이프라인.

---

## 진행 상태

| Phase | 내용 | 상태 |
|-------|------|------|
| **Phase 0** | Harbor insecure-registry 설정 (모든 노드) | ✅ 완료 |
| **Phase 1** | node4 ↔ tempnode-bf3 NFS 연결 | ✅ 완료 |
| **Phase 2** | doca-dev DPUService 배포 + Mode B kubectl exec 개발 | 🔲 대기 |
| **Phase 3** | Mode A — Kaniko 이미지 빌드 + Harbor push | 🔲 대기 |
| **Phase 4** | `doca-prod` DPUService 배포 (운영) | 🔲 대기 |

---

## 전체 아키텍처

```
[node4 (10.34.20.8) — VSCode 작업 머신]
  /home/joon/doca-platform/DOCA_BF3/projects/  ← NFS 마운트 (tempnode-bf3)
    ├── host/        ← Host 앱 소스
    ├── dpu/         ← DPU 앱 소스
    ├── meson.build
    └── flow_common.*
    │
    │  NFS 마운트 (편집 = 직접 tempnode-bf3에 기록)
    ▼
[tempnode-bf3 (10.34.20.4) — NFS 서버]
  /home/joon/doca-platform/DOCA_BF3/projects/  ← 단일 원본
    └── NFS export → node4

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

[Host Cluster — doca-dev namespace]
  DPUService: doca-dev
    → DPF Operator가 DPU 클러스터에 DaemonSet 배포
    → nodeSelector: tempnode-bf3-mt25476000nu (단일 BF3 노드)

[DPU Cluster (arm64, Kamaji) — doca-dev namespace]
  Pod: doca-dev
  ├── Container: dev  [doca:3.2.0-devel, arm64]
  │   ├── /doca_devel  ← NFS 마운트 (DOCA_BF3/projects/)
  │   ├── privileged: true, hugepages
  │   └── kubectl exec → meson/ninja 빌드 (Mode B)
  └── Container: builder  [kaniko-executor]
      ├── /doca_devel  ← 동일 NFS 마운트 (빌드 결과물 공유)
      └── kubectl exec → Dockerfile 빌드 → Harbor push (Mode A)

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

[Harbor: 10.34.25.12:80]  ✅ K8s 파드에서 pull/push 가능
  └── doca/<app-name>:<tag>

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

[Host Cluster — doca-prod namespace]
  DPUService: <app-name>
    └── helmChart.values.image.tag: <version>
        → DPF 내장 ArgoCD → DPU 클러스터 auto-sync

[DPU Cluster — doca-prod namespace]
  Pod: <app-name>  ← 운영 이미지 실행
```

---

## 네트워크 경로 구분

| 트래픽 종류 | 경로 | 비고 |
|-------------|------|------|
| K8s 이미지 pull (Harbor) | br-dpu → 10.34.25.12 | Phase 0 완료로 가능 |
| K8s 제어 플레인 | br-dpu bridge | 관리 경로 |
| DOCA 앱 데이터 플레인 | PCIe → VF/SF | DPUServiceInterface CRD |
| DPU SSH 접속 | tempnode-bf3 → 192.168.101.2 | 관리 경로 |

---

## 두 가지 개발 워크플로우

### Mode B — 빠른 반복 개발 (직접 테스트)

```
kubectl exec doca-dev -c dev -n doca-dev -- bash
  → cd /doca_devel
  → meson setup /tmp/build && ninja -C /tmp/build
  → /tmp/build/my-doca-app  (BF3 하드웨어 위에서 직접 실행)
```

- 이미지 빌드 없이 바로 테스트
- DOCA SDK 샘플 코드, 커스텀 앱 모두 지원
- 재시작해도 소스코드 유지 (NFS)

### Mode A — 프로덕션 이미지 빌드

```
[dev 컨테이너]
  meson setup /tmp/build && ninja -C /tmp/build
  cp /tmp/build/my-app /doca_devel/build/  ← NFS에 저장

[builder 컨테이너 — Kaniko]
  kubectl exec doca-dev -c builder -n doca-dev -- \
    /kaniko/executor \
    --dockerfile=/doca_devel/Dockerfile \
    --context=dir:///doca_devel \
    --destination=10.34.25.12:80/doca/my-app:v0.1 \
    --insecure

[배포]
  kubectl patch dpuservice my-app -n doca-prod \
    --type=merge \
    -p '{"spec":{"helmChart":{"values":{"image":{"tag":"v0.1"}}}}}'
  → DPF 내장 ArgoCD가 DPU 클러스터에 자동 sync
```

---

## 네임스페이스 구조

```
Host Cluster                    DPU Cluster (Kamaji)
─────────────────               ─────────────────────
doca-dev                   →    doca-dev
  DPUService: doca-dev            DaemonSet: doca-dev
                                    (dev + builder 컨테이너)

doca-prod                  →    doca-prod
  DPUService: <app>               DaemonSet: <app>
                                    (운영 이미지)

dpf-operator-system        →    (DPF 내부 컴포넌트)
  DPF Operator
  내장 ArgoCD
```

> **주의:** DPUService의 host namespace가 DPU 클러스터 namespace로 1:1 매핑됨
> (`internal/argocd/utils.go`: `Destination.Namespace = dpuService.Namespace`)

---

## 이미지 종류

| 용도 | 이미지 | 아키텍처 |
|------|--------|----------|
| DPU 개발 | `nvcr.io/nvidia/doca/doca:3.2.0-devel-ubuntu24.04` | arm64 |
| Host 개발 | `nvcr.io/nvidia/doca/doca:3.2.0-devel-ubuntu24.04-host` | x86_64 |
| DPU 런타임 (배포) | `nvcr.io/nvidia/doca/doca:3.2.0-full-rt-ubuntu24.04` | arm64 |
| 빌더 | `gcr.io/kaniko-project/executor:latest` | arm64/x86_64 |

---

## 관련 문서

- [Phase 0 — Harbor insecure-registry 설정](./phase0-harbor-registry-setup.md) ✅
- [Phase 1 — NFS 서버 + node4 마운트](./phase1-nfs-node4-mount.md) ✅
- [Phase 2 — doca-dev DPUService 배포 (Mode B)](./phase2-doca-dev-dpuservice.md)
- Phase 3 — Kaniko 빌드 가이드 (작성 예정)
- Phase 4 — doca-prod 배포 가이드 (작성 예정)
- [스펙 원본](../../.omc/specs/deep-dive-doca-k8s-dev-deploy-pipeline.md)
