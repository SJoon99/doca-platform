---
title: "DPF 부트스트랩 트러블슈팅"
---

[TOC]

## 개요

- **환경** — Sandbox 클러스터 (control-plane 접근 불가, worker 4개 + tempnode-bf3 BF3 DPU)
- **방식** — ArgoCD GitOps App-of-Apps 패턴으로 DPF 스택 부트스트랩
- **범위** — cert-manager → Kamaji → kamaji-datastore → maintenance-operator → dpf-operator 순차 설치 중 발생한 이슈 전체

---

## 현재 설치 완료 상태

| 컴포넌트 | 상태 | 비고 |
|---------|------|------|
| cert-manager | ✅ Running (3/3) | `cert-manager` 네임스페이스 |
| Kamaji | ✅ Running (1/1) | `dpf-operator-system` |
| kamaji-etcd | ✅ Running (3/3) | etcd 클러스터 정상 |
| kamaji-datastore | ✅ Synced | DataStore CR 생성됨 |
| maintenance-operator | ✅ Running (1/1) | |
| dpf-operator | ✅ Running (1/1) | Docker Hub 이미지 사용 |
| DPF CRD | ✅ 34개 등록 | DPU/DPF/Kamaji 관련 |
| tempnode-bf3 레이블 | ✅ | `feature.node.kubernetes.io/dpu-enabled=true` |

---

## 이슈 1: Kamaji OCI 레지스트리 403

### 증상
- ArgoCD App sync 시 `ghcr.io/nvidia/charts` 접근 403 Forbidden
- `helm pull oci://ghcr.io/nvidia/charts/kamaji` 실패

### 원인
- NVIDIA의 ghcr.io 차트 레지스트리는 인증이 필요한 private 레지스트리
- ArgoCD values.yaml의 `repoURL: oci://ghcr.io/nvidia/charts` — 전체 차트 경로 누락

### 해결
1. ArgoCD repo secret에 GitHub PAT 등록
   ```bash
   kubectl create secret generic ghcr-nvidia-charts \
     -n argocd \
     --from-literal=type=helm \
     --from-literal=url=oci://ghcr.io/nvidia/charts \
     --from-literal=username=<github-id> \
     --from-literal=password=<github-pat>
   kubectl label secret ghcr-nvidia-charts -n argocd \
     argocd.argoproj.io/secret-type=repository
   ```
2. `repoURL`에 전체 차트 경로 포함
   ```yaml
   # 잘못됨
   repoURL: oci://ghcr.io/nvidia/charts
   # 올바름
   repoURL: oci://ghcr.io/nvidia/charts/kamaji
   ```

---

## 이슈 2: Kamaji 웹훅 부트스트랩 데드락

### 증상
- Kamaji 첫 sync 완료 후 etcd Pod이 Pending 또는 Init 상태에서 멈춤
- cert-gen Job이 실행되지 않음
- DataStore CR 생성 시 webhook timeout

### 원인
닭-달걀 순환 의존성:

```
DataStore CR 생성
  → kamaji validating webhook 호출
    → webhook 인증서가 없음 (cert-gen Job 미실행)
      → cert-gen Job은 etcd가 준비돼야 실행
        → etcd는 DataStore 없이는 시작 안 함
```

kamaji-etcd 서브차트의 `datastore.enabled: true`가 Kamaji 자체 sync와 동시에 DataStore CR을 생성하는 것이 문제.

### 해결
1. kamaji values.yaml에서 DataStore 자동 생성 비활성화
   ```yaml
   # argocd/apps/07-Doca-Platform/kamaji/values.yaml
   kamaji-etcd:
     datastore:
       enabled: false  # Datastore는 kamaji-datastore App으로 분리
   ```
2. DataStore CR을 별도 ArgoCD Application으로 분리 (syncWave 4 — Kamaji Running 확인 후)
   ```yaml
   # argocd/apps/07-Doca-Platform/values.yaml
   kamaji-datastore:
     enabled: true
     namespace: dpf-operator-system
     syncWave: "4"
     source:
       path: argocd/apps/07-Doca-Platform/kamaji-datastore
   ```
3. kamaji-datastore/datastore.yaml 별도 작성

---

## 이슈 3: Helm Hook Finalizer로 RBAC 리소스 "already exists"

### 증상
- Kamaji sync 재시도 시 에러
  ```
  Error: rendered manifests contain a resource that already exists.
  Role "kamaji-cert-gen" already exists in namespace "dpf-operator-system"
  ```

### 원인
- Kamaji Helm 차트의 cert-gen Role/RoleBinding이 `helm.sh/hook` annotation으로 관리됨
- ArgoCD가 `argocd.argoproj.io/hook-finalizer`를 붙여 삭제를 block
- sync 실패 시 hook 리소스가 남아있어 다음 sync에서 충돌

### 해결
finalizer 수동 제거 후 삭제:
```bash
for resource in role/kamaji-cert-gen rolebinding/kamaji-cert-gen; do
  kubectl patch $resource -n dpf-operator-system \
    --type json \
    -p='[{"op":"remove","path":"/metadata/finalizers"}]'
  kubectl delete $resource -n dpf-operator-system
done
```

---

## 이슈 4: Kamaji CRD annotation 크기 초과 (262144 bytes)

### 증상
- force sync 시 CRD apply 실패
  ```
  metadata.annotations: Too long: may not be longer than 262144
  ```

### 원인
- ArgoCD가 CSA(Client-Side Apply) 방식으로 sync 시 `kubectl.kubernetes.io/last-applied-configuration` annotation에 전체 CRD spec을 저장
- Kamaji CRD가 크기 제한(262144 bytes)을 초과

### 해결
1. 기존 Kamaji CRD 삭제 (CRD 삭제 시 관련 CR도 삭제되므로 주의)
   ```bash
   kubectl delete crd tenantcontrolplanes.kamaji.clastix.io
   ```
2. ArgoCD Application에 `ServerSideApply=true` 추가 (annotation 저장 불필요)
   ```yaml
   syncOptions:
     - ServerSideApply=true
   ```
3. ArgoCD sync — SSA 방식으로 CRD 재생성

---

## 이슈 5: dpf-operator Pod Pending — nodeAffinity 불일치

### 증상
```
0/6 nodes are available: 2 node(s) had untolerated taint(s),
4 node(s) didn't match Pod's node affinity/selector.
```

### 원인
- dpf-operator 차트 기본 affinity가 control-plane 전용
  ```yaml
  nodeSelectorTerms:
    - matchExpressions:
        - key: "node-role.kubernetes.io/master"
          operator: Exists
    - matchExpressions:
        - key: "node-role.kubernetes.io/control-plane"
          operator: Exists
  ```
- Sandbox 클러스터에는 taint가 있는 control-plane만 존재 → 스케줄 불가

**Helm 딥머지 함정:**
| values 설정 | 결과 |
|------------|------|
| `affinity: {}` | 차트 기본값과 딥머지 → 기본 affinity 유지 |
| `affinity: null` | 차트 기본값과 딥머지 → 기본 affinity 유지 |
| `nodeSelectorTerms: []` | 배열 교체는 되나 Kubernetes가 거부 (최소 1개 필요) |

### 해결
모든 노드에 존재하는 레이블로 catch-all nodeSelectorTerm 사용:
```yaml
# argocd/apps/07-Doca-Platform/dpf-operator/values.yaml
affinity:
  nodeAffinity:
    requiredDuringSchedulingIgnoredDuringExecution:
      nodeSelectorTerms:
        - matchExpressions:
            - key: kubernetes.io/hostname
              operator: Exists
tolerations: []
```

---

## 이슈 6: dpf-system 이미지 공개 배포 없음

### 증상
- dpf-operator values에 이미지 미설정 시 차트 기본값(`nvcr.io/nvidia/...`) 참조 → ImagePullBackOff
- NVIDIA 내부 레지스트리라 pull 불가

### 원인
- `github.com/nvidia/doca-platform` 저장소는 소스코드만 공개
- 빌드된 `dpf-system` 이미지는 NVIDIA 내부 레지스트리에만 존재

### 해결
소스에서 직접 빌드 후 Docker Hub에 push:

```bash
# 빌드 머신에서
cd doca-platform

# Go 설치 (sudo 불필요)
wget https://go.dev/dl/go1.21.8.linux-amd64.tar.gz
tar -xf go1.21.8.linux-amd64.tar.gz -C ~/go-dist

# docker buildx 설치
mkdir -p ~/.docker/cli-plugins
wget -O ~/.docker/cli-plugins/docker-buildx \
  https://github.com/docker/buildx/releases/download/v0.13.1/buildx-v0.13.1.linux-amd64
chmod +x ~/.docker/cli-plugins/docker-buildx

# 빌드
make docker-build-dpf-system-for-amd64 \
  IMAGE=10.34.25.12:80/library/dpf-system:dev

# Docker Hub push
docker tag 10.34.25.12:80/library/dpf-system:dev jinkernel/doca-platform:dpf-system-dev
docker push jinkernel/doca-platform:dpf-system-dev
```

values.yaml 업데이트:
```yaml
controllerManager:
  image:
    repository: jinkernel/doca-platform
    tag: dpf-system-dev
```

---

## 이슈 7: Harbor push EOF

### 증상
```
The push refers to repository [10.34.25.12:80/library/dpf-system]
dd843f0a96ab: Preparing
...
EOF
```
- layer가 "Preparing" 상태에서 무한 대기 후 EOF

### 원인
- 빌드 머신(10.30.0.184)이 Harbor(10.34.25.12)와 다른 네트워크 세그먼트
- 라우팅 또는 프록시 문제로 대용량 layer 전송 중 연결 끊김

### 해결
Harbor 대신 Docker Hub 사용:
```bash
docker logout 10.34.25.12:80
docker login -u jinkernel  # Docker Hub
docker tag 10.34.25.12:80/library/dpf-system:dev jinkernel/doca-platform:dpf-system-dev
docker push jinkernel/doca-platform:dpf-system-dev
```

---

## 이슈 8: Docker Hub 계정명 오타로 push 거부

### 증상
```
denied: requested access to the resource is denied
```

### 원인
- Docker Hub 실제 계정명: `jinkernel`
- 빌드 머신 `~/.docker/config.json`에 저장된 계정명: `jinkernal` (a↔e 순서 바뀜)
- 잘못된 계정으로 로그인된 상태에서 push 시도

### 해결
```bash
docker logout
docker login -u jinkernel
# 올바른 계정으로 재로그인 후 push 성공
```

---

## 이슈 9: etcd-defrag CronJob pod Pending 잔재

### 증상
- dpf-operator sync 후 `dpf-operator-kamaji-etcd-defrag-job-*` Pod이 Pending
- 이미 새 affinity로 values 업데이트했음에도 구 affinity(`master/control-plane`) 적용된 채 생성

### 원인
- CronJob spec은 새 values로 업데이트됨
- 그러나 이전 sync에서 생성된 Job(과 그 Pod)은 spec 업데이트로 자동 삭제되지 않음
- 구 Job의 Pod는 구 CronJob spec의 affinity 그대로 유지

### 해결
구 Job 수동 삭제:
```bash
kubectl delete job dpf-operator-kamaji-etcd-defrag-job-<hash> \
  -n dpf-operator-system
```

- CronJob spec은 이미 올바른 affinity 적용됨
- 다음 자정(`0 0 * * *`) 실행 시 정상 스케줄링

---

## SyncWave 최종 구성

```
syncWave 2: cert-manager
syncWave 3: kamaji (etcd 포함, datastore.enabled=false)
syncWave 4: kamaji-datastore (DataStore CR — kamaji Running 확인 후)
syncWave 5: maintenance-operator
syncWave 6: dpf-node-label (tempnode-bf3 레이블링 Job)
syncWave 7: dpf-operator
```
