---
title: "Phase 2 — doca-dev DPUService 배포 (Mode B 개발환경)"
status: "🟢 완료 — doca-dev Pod Running, NFS 마운트 확인"
---

# Phase 2: doca-dev DPUService 배포

## 목적

BF3 DPU 위에서 DOCA 앱을 직접 빌드/테스트할 수 있는 개발 컨테이너를 DPUService로 배포.

```
[node4 — VSCode]
  /home/joon/doca-platform/DOCA_BF3/projects/  ← NFS 마운트
    │  (편집 = tempnode-bf3에 직접 기록)
    ▼
[tempnode-bf3 — NFS 서버]
  /home/joon/doca-platform/DOCA_BF3/projects/
    │  NFS export → DPU
    ▼
[BF3 DPU — DPU cluster worker]
  Pod: doca-dev
    /doca_devel ← NFS 마운트
    → kubectl exec -c dev → meson/ninja 빌드
```

---

## 파일 구조

```
DOCA_BF3/
  helm/
    doca-dev/
      Chart.yaml
      values.yaml
      templates/
        daemonset.yaml
  gitops/
    doca-dev-dpuservice.yaml  ← Namespace + DPUService CR
```

---

## 실행 순서

### Step 1. Helm chart를 Harbor에 push

```bash
cd /home/joon/doca-platform

# chart 패키징
helm package DOCA_BF3/helm/doca-dev -d /tmp/

# Harbor OCI registry에 push
helm push /tmp/doca-dev-0.1.0.tgz oci://10.34.25.12:80/doca --insecure-skip-tls-verify
```

확인:
```bash
# push된 chart 목록 확인 (Harbor UI 또는 OCI)
curl -s http://10.34.25.12:80/v2/doca/doca-dev/tags/list
```

### Step 2. DPUService CR 적용 (host cluster)

```bash
kubectl apply -f DOCA_BF3/gitops/doca-dev-dpuservice.yaml
```

### Step 3. DPU cluster에서 pod 확인

```bash
# tenant kubeconfig 추출 (이미 있으면 생략)
kubectl get secret -n dpu-cplane-tenant1 dpu-cplane-tenant1-admin-kubeconfig \
  -o go-template='{{index .data "admin.conf" | base64decode}}' \
  > /tmp/dpu-cplane-tenant1-admin.conf

# DPU cluster에서 pod 확인
kubectl --kubeconfig /tmp/dpu-cplane-tenant1-admin.conf get pods -n doca-dev -o wide
```

정상 기대값:
- `doca-dev-<hash>` pod `1/1 Running`
- NODE: `tempnode-bf3-mt25476000nu`

### Step 4. 개발 컨테이너 접속 (Mode B)

```bash
# pod 이름 확인
POD=$(kubectl --kubeconfig /tmp/dpu-cplane-tenant1-admin.conf \
  get pods -n doca-dev -o jsonpath='{.items[0].metadata.name}')

# dev 컨테이너 접속
kubectl --kubeconfig /tmp/dpu-cplane-tenant1-admin.conf \
  exec -it -n doca-dev $POD -c dev -- bash
```

컨테이너 내부에서:
```bash
# NFS 마운트 확인
ls /doca_devel
# → dpu/  host/  flow_common.c  flow_common.h  meson.build

# DOCA 빌드 (예시)
cd /doca_devel
meson setup /tmp/build
ninja -C /tmp/build
```

---

## 2026-04-11 검증 기록

### 현재 판정

- Host cluster 반영: 됨
- DPU cluster workload 반영: 안 됨
- Phase 2 완료 판정: 보류

### 공식 기준 재정리

- `DOCA_BF3/docs/*`
  - 사용자 작성 문서
  - 공식 근거 아님
- 공식 근거
  - 영어 문서
  - 코드
- 공식 동작 흐름
  - User creates `DPUService`
  - DPUService controller creates ArgoCD `Application`
  - ArgoCD syncs to DPU cluster
  - DPU node pod 실행
- 근거
  - `docs/public/developer-guides/architecture/system-overview.md`
  - `docs/public/developer-guides/system/conditions.md`
  - `docs/public/getting-started/helm-prerequisites.md`
  - `api/operator/v1alpha1/dpfoperatorconfig_types.go`
  - `internal/argocd/utils.go`

### 공식 기준 해석

- 현재 사용 방식
  - Host cluster에서 `DPUService` 선언
  - GitOps/Argo로 host 쪽 형상관리
- 판정
  - 큰 방향: 의도와 부합
  - 완전히 비권장 방식: 아님
- 실제 어긋난 지점
  - Argo 배치 namespace
  - DPF가 참조하는 Argo namespace
  - 두 값 불일치 시 tenant `Application` 체인 중단
- 공식 문서상 필요한 전제
  - `application.namespaces: dpf-operator-system`
  - DPF가 Argo AppProject / cluster secret 생성 가능해야 함

### 실제 확인 결과

| 구간 | 확인값 | 판정 |
|------|--------|------|
| Host cluster | `ns/doca-dev` = `Active` | 생성됨 |
| Host cluster | `dpuservice/doca-dev` = `READY=False`, `PHASE=Pending` | 미완료 |
| DPUService condition | `ApplicationsReady=False` | 미완료 |
| DPUService condition | `ApplicationPrereqsReconciled=True` | 선행 단계 완료 |
| DPUService condition | `ApplicationsReconciled=True` | Application 생성까지 완료 |
| Host ArgoCD | `argocd/dpf-doca-dev` = `Synced`, `Healthy` | host 쪽 GitOps 반영됨 |
| Tenant Application | `dpf-operator-system/dpu-cplane-tenant1-doca-dev` = `SYNC=<empty>`, `HEALTH=<empty>` | 미처리 |
| DPU cluster | `ns/doca-dev` = `Active` | namespace 생성됨 |
| DPU cluster | `ds/doca-dev` 없음 | workload 미생성 |
| DPU cluster | `pod -n doca-dev` 없음 | workload 미생성 |
| DPU cluster | `kube-system` 주요 pod `ContainerCreating` | 클러스터 기동 불안정 |
| Harbor API | 익명 `curl` = `UNAUTHORIZED` | 인증 없이 태그 확인 불가 |

### 확인 시 사용 커맨드

```bash
# Host cluster
kubectl get ns doca-dev
kubectl get dpuservice -A -o wide
kubectl get dpuservice -n doca-dev doca-dev -o yaml
kubectl describe dpuservice -n doca-dev doca-dev

# Host ArgoCD
kubectl get applications -A | rg 'doca-dev|dpf-doca-dev'
kubectl get application -n argocd dpf-doca-dev -o yaml
kubectl get application -n dpf-operator-system dpu-cplane-tenant1-doca-dev -o yaml

# DPU cluster
kubectl get secret -n dpu-cplane-tenant1 dpu-cplane-tenant1-admin-kubeconfig \
  -o go-template='{{index .data "admin.conf" | base64decode}}' \
  > /tmp/dpu-cplane-tenant1-admin.conf

kubectl --kubeconfig /tmp/dpu-cplane-tenant1-admin.conf get ns,ds,pods -A -o wide
kubectl --kubeconfig /tmp/dpu-cplane-tenant1-admin.conf get nodes -o wide

# Harbor
curl -s http://10.34.25.12:80/v2/doca/doca-dev/tags/list
```

### 현재 보이는 blocker

- DPF가 tenant용 `Application` 생성
  - 예: `dpf-operator-system/dpu-cplane-tenant1-doca-dev`
- 같은 패턴의 tenant용 `Application` 전부 `SYNC/HEALTH` 비어 있음
- 공식 설계 기준 필요한 값
  - ArgoCD `application.namespaces = dpf-operator-system`
  - DPF `spec.overrides.argoCDNamespace = argocd`
- 추정
  - ArgoCD watch 범위 / DPF Argo namespace 값 불일치 이력
  - GitOps source 와 live cluster 사이 drift 가능성
  - 결과: tenant용 `Application` 미처리
  - 결과: `DPUService` `ApplicationsReady=False`

### GitOps 수정 대상

- GitOps repo
  - `argocd/apps/00-infra/argo/values.yaml`
  - `argocd/apps/07-Doca-Platform/dpf-provisioning-config/dpfoperatorconfig.yaml`
- 필요 상태
  - `values.yaml`
    - `application.namespaces: dpf-operator-system`
  - `dpfoperatorconfig.yaml`
    - `spec.overrides.argoCDNamespace: argocd`

### 2026-04-11 추가 확인

- live 값 재확인
  - `argocd-cmd-params-cm.data.application.namespaces = dpf-operator-system`
  - `dpfoperatorconfig.spec.overrides.argoCDNamespace = argocd`
- 즉
  - 설정값 부재 단계는 이미 지난 상태
  - 현재 blocker는 다음 단계
- tenant `Application`
  - 상태: `SYNC=Unknown`, `HEALTH=Unknown`
  - 조건:
    - `InvalidSpecError`
    - `Application referencing project doca-platform-project-dpu which does not exist`
- AppProject 실제 위치
  - `dpf-operator-system/doca-platform-project-dpu`
  - `dpf-operator-system/doca-platform-project-host`
- cluster secret 실제 위치
  - `dpf-operator-system/dpu-cplane-tenant1-dpu-cplane-tenant1`
- 기대 위치
  - `argocd` namespace
- 의미
  - Argo controller는 tenant `Application` 을 보기 시작
  - 하지만 참조 project / cluster secret를 `argocd` 에서 찾지 못함
  - 결과: `Application` = `Unknown`
  - 결과: `DPUService` = `ApplicationsReady=False`

### 현재 작업 가설

- GitOps source 수정
  - 필요
  - drift 방지
- live cluster 추가 조치
  - 별도 필요 가능성
- 의심 지점
  - 현재 실행 중 DPF operator build
  - `argoCDNamespace` override는 수용
  - 그러나 AppProject / cluster secret 재배치는 미수행

### 재확인 포인트

```bash
# 1. tenant Application 처리 여부
kubectl get applications -n dpf-operator-system
kubectl get application -n dpf-operator-system dpu-cplane-tenant1-doca-dev -o yaml

# 2. ArgoCD multi-namespace watch 설정
kubectl -n argocd get cm argocd-cmd-params-cm -o yaml
kubectl -n argocd get statefulset argocd-application-controller -o yaml

# 3. cluster secret 위치 확인
kubectl get secret -A | rg 'dpu-cplane-tenant1|argocd.argoproj.io/secret-type'

# 4. DPU cluster workload 생성 여부
kubectl --kubeconfig /tmp/dpu-cplane-tenant1-admin.conf get ds,pods -n doca-dev -o wide
```

### 완료 판정 기준

- `kubectl get dpuservice -n doca-dev doca-dev`
  - `READY=True`
  - `PHASE=Ready` 또는 동등 상태
- `kubectl get application -n dpf-operator-system dpu-cplane-tenant1-doca-dev`
  - `SYNC=Synced`
  - `HEALTH=Healthy`
- `kubectl --kubeconfig /tmp/dpu-cplane-tenant1-admin.conf get ds,pods -n doca-dev -o wide`
  - `ds/doca-dev` 존재
  - pod `Running`
- `kubectl --kubeconfig /tmp/dpu-cplane-tenant1-admin.conf exec -n doca-dev <pod> -c dev -- ls /doca_devel`
  - NFS 내용 확인

---

## 트러블슈팅

| 증상 | 원인 | 해결 |
|------|------|------|
| DPUService `Synced` 안됨 | Harbor chart push 미완료 | Step 1 재실행 |
| Pod `ImagePullBackOff` | nvcr.io 접근 불가 | DPU cluster outbound 확인 |
| `/doca_devel` empty | NFS 마운트 실패 | DPU에서 `mount \| grep doca` 확인 |
| `exec` 연결 안됨 | pod not Running | `kubectl describe pod` 로 이벤트 확인 |

---

## 2026-04-13 트러블슈팅 기록

### 해결 1: DPFOperatorConfig upgrade deadlock

- **증상**
  - `dpf-operator-controller-manager` 새 이미지(`0.1.2-publicmainffafc429`) 배포 후 reconcile 무한 루프
  - 로그: `expected 2 DPUServices ... found 0`, `DPUService "multus" not found` 등
  - 하위 컴포넌트(`dpuservice-controller-manager` 등) 구버전 이미지 유지
- **근본 원인**
  - `reconcilePreUpgradeValidations` → `validateSystemComponentsReadiness` 순서
  - `UpgradeInProgress()` = `status.version != release.DPFVersion()` → `true`
  - pre-upgrade validation이 system DPUServices ready 요구
  - 하지만 system DPUServices는 `reconcileSystemComponents`에서 생성 (validation 이후 단계)
  - 결과: validation 실패 → reconcile 중단 → 컴포넌트 업데이트 불가 → 무한 루프
- **해결**
  - `status.version`을 새 버전으로 패치 → `UpgradeInProgress()` = `false` → validation skip
  ```bash
  kubectl patch dpfoperatorconfig dpfoperatorconfig -n dpf-operator-system \
    --subresource=status --type=merge \
    -p '{"status":{"version":"0.1.2-publicmainffafc429"}}'
  ```
  - 패치 후 모든 하위 Deployment 자동 rolling update 시작

### 해결 2: ArgoCD OCI chart URL format 에러

- **증상**
  - tenant Application `dpu-cplane-tenant1-doca-dev`: `SYNC=Unknown`
  - 에러: `invalid chart URL format: 10.34.25.12:80/doca`
  - `helm pull --repo 10.34.25.12:80/doca doca-dev` 실패
- **근본 원인**
  - DPF 설계: `GetArgoRepoURL()`이 `oci://` prefix 제거 후 Application에 설정
  - ArgoCD가 해당 URL을 OCI로 인식하려면 repo secret 등록 필요 (`enableOCI: true`)
  - Harbor(`10.34.25.12:80/doca`)가 ArgoCD repo로 미등록 상태
- **해결**
  - Harbor를 ArgoCD OCI repo secret으로 등록
  ```bash
  kubectl apply -f - <<'EOF'
  apiVersion: v1
  kind: Secret
  metadata:
    name: argocd-repo-harbor-oci
    namespace: argocd
    labels:
      argocd.argoproj.io/secret-type: repository
  type: Opaque
  stringData:
    type: helm
    url: "10.34.25.12:80/doca"
    enableOCI: "true"
  EOF
  ```
  - Application refresh 후 `Synced` + `Progressing` 상태 전환 확인

### 해결 3: AppProject namespace mismatch (이전 세션)

- **증상**
  - tenant Application: `Application referencing project doca-platform-project-dpu which does not exist`
- **근본 원인**
  - 구버전 operator가 AppProject/cluster secret을 `dpf-operator-system`에 생성
  - ArgoCD는 `argocd` namespace에서 탐색
- **해결**
  - 새 operator 이미지 빌드/배포 (`0.1.2-publicmainffafc429`) — `argoCDNamespace` override 지원
  - bootstrap: AppProject + cluster secret을 `argocd` ns에 수동 생성 (deadlock 해소)

### 현재 상태 (2026-04-14 최종)

| 구간 | 확인값 | 판정 |
|------|--------|------|
| dpf-operator-controller-manager | 새 이미지 Running (Harbor defaults baked) | 완료 |
| dpuservice-controller-manager | 새 이미지 Running | 완료 |
| ArgoCD Harbor repo | `enableOCI: true` 등록 | 완료 |
| 모든 Application sync | `Synced` (14개 전부) | 완료 |
| DPUService `doca-dev` | `READY=True`, `PHASE=Success` | 완료 |
| DPU 클러스터 `doca-dev` Pod | `1/1 Running` | 완료 |
| NFS `/doca_devel` 마운트 | dpu/, host/, flow_common.c 확인 | 완료 |
| flannel, multus, coredns | Running | 완료 |

### 해결 4: DPU 노드 DNS/라우팅

- **증상**
  - Pod: `Failed to create pod sandbox` — `lookup k8s.gcr.io on [::1]:53: connection refused`
- **근본 원인**
  - DPU `resolv.conf`: `# No DNS servers known.`
  - Default gateway 없음 — `ip route`: `10.32.0.0/12` 한 개만 존재
  - 외부 DNS/인터넷 접근 불가
- **해결**
  ```bash
  # DNS 설정
  sudo resolvectl dns br-comm-ch 8.8.8.8 8.8.4.4
  sudo resolvectl domain br-comm-ch '~.'
  # Default route 추가
  sudo ip route add default via 10.47.255.254 dev br-comm-ch
  ```
- **주의**: 이 설정은 재부팅 시 초기화됨 — 영구화 필요 (netplan 등)

### 해결 5: loopback CNI `name` 필드 누락

- **증상**
  - `plugin type="loopback" failed (add): missing network name`
- **근본 원인**
  - `/etc/cni/net.d/99-loopback.conf`에 `"name"` 필드 없음
- **해결**
  ```bash
  echo '{ "cniVersion": "0.3.1", "name": "lo", "type": "loopback" }' | \
    sudo tee /etc/cni/net.d/99-loopback.conf
  ```

### 해결 6: 시스템 DPUServices `dpu-networking` chart 접근 불가

- **증상**
  - 시스템 DPUServices 13개 전부 `Pending`
  - `helm pull oci://jinkernel/dpu-networking` → `lookup jinkernel: no such host`
- **근본 원인**
  - `build/defaults.yaml`의 `dpuNetworkingHelmChart: oci://jinkernel/dpu-networking`
  - `jinkernel`은 Docker Hub org이지 hostname이 아님
  - 빌드 시 `UPSTREAM_HELM_REGISTRY` 환경변수로 결정됨
  - `internal/release/templates/defaults.yaml.tmpl` → `$(ENVSUBST)` → `build/defaults.yaml` → Docker COPY
- **해결**
  1. chart를 Harbor에 push
     ```bash
     helm push hack/charts/dpu-networking-0.1.2-publicmainffafc429.tgz \
       oci://10.34.25.12:80/doca --plain-http
     ```
  2. operator 이미지 재빌드 (Harbor URL bake)
     ```bash
     make generate-manifests-release-defaults \
       TAG=0.1.2-publicmainffafc429 \
       REGISTRY=jinkernel \
       UPSTREAM_HELM_REGISTRY=oci://10.34.25.12:80/doca
     docker buildx build --no-cache ... -f Dockerfile.dpf-system .
     ```
  3. 모든 노드에서 구 이미지 캐시 삭제 후 pod 재시작
     ```bash
     # IfNotPresent + 같은 태그 → 노드 캐시 삭제 필수
     ssh joon@<node> 'sudo crictl rmi docker.io/jinkernel/dpf-system:TAG'
     kubectl delete pod -n dpf-operator-system -l app.kubernetes.io/name=dpf-operator
     ```
- **주의**: `build/defaults.yaml`을 직접 수정해도 `make` 의존성 `generate-manifests-release-defaults`가 템플릿에서 재생성하여 덮어씀 — 반드시 make 변수로 전달해야 함

### 남은 이슈 (doca-dev 동작에 영향 없음)

| Pod | 상태 | 원인 |
|-----|------|------|
| `cni-installer` | `ImagePullBackOff` | arm64 이미지 미 push |
| `ovs-cni` | `Init:ImagePullBackOff` | arm64 이미지 미 push |
| `sfc-controller` | `CrashLoopBackOff` | 설정/의존성 문제 — 별도 조사 필요 |

---

## 완료 판정 (2026-04-14)

- `doca-dev` Pod: `1/1 Running` on `tempnode-bf3-mt25476000nu`
- NFS `/doca_devel`: dpu/, host/, flow_common.c, meson.build 확인
- DPUService: `READY=True`, `PHASE=Success`
- 전체 배포 체인 정상: DPUService → ArgoCD Application → DPU 클러스터 Pod

---

## 다음 단계

→ Phase 3 — Kaniko builder 컨테이너 추가 + Harbor push (Mode A)
