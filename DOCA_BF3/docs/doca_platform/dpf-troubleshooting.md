---
title: "DPF 트러블슈팅"
---

[TOC]

## 부트스트랩 이슈

환경: Sandbox 클러스터 (control-plane 접근 불가, worker 4개 + tempnode-bf3 BF3 DPU)
방식: ArgoCD GitOps App-of-Apps 패턴으로 DPF 스택 부트스트랩

---

### Kamaji OCI 레지스트리 403

**Symptom**
- ArgoCD App sync 시 `ghcr.io/nvidia/charts` 접근 403 Forbidden
- `helm pull oci://ghcr.io/nvidia/charts/kamaji` 실패

**Root Cause**
- NVIDIA의 ghcr.io 차트 레지스트리는 인증이 필요한 private 레지스트리
- ArgoCD values.yaml의 `repoURL: oci://ghcr.io/nvidia/charts` — 전체 차트 경로 누락

**Resolution**
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

### Kamaji 웹훅 부트스트랩 데드락

**Symptom**
- Kamaji 첫 sync 완료 후 etcd Pod이 Pending 또는 Init 상태에서 멈춤
- cert-gen Job이 실행되지 않음
- DataStore CR 생성 시 webhook timeout

**Root Cause**
닭-달걀 순환 의존성:
```
DataStore CR 생성
  → kamaji validating webhook 호출
    → webhook 인증서가 없음 (cert-gen Job 미실행)
      → cert-gen Job은 etcd가 준비돼야 실행
        → etcd는 DataStore 없이는 시작 안 함
```
`kamaji-etcd` 서브차트의 `datastore.enabled: true`가 Kamaji 자체 sync와 동시에 DataStore CR을 생성하는 것이 문제.

**Resolution**
1. kamaji values.yaml에서 DataStore 자동 생성 비활성화
   ```yaml
   kamaji-etcd:
     datastore:
       enabled: false  # Datastore는 kamaji-datastore App으로 분리
   ```
2. DataStore CR을 별도 ArgoCD Application으로 분리 (syncWave 4 — Kamaji Running 확인 후)

---

### Helm Hook Finalizer RBAC 충돌

**Symptom**
- Kamaji sync 재시도 시 에러
  ```
  Error: rendered manifests contain a resource that already exists.
  Role "kamaji-cert-gen" already exists in namespace "dpf-operator-system"
  ```

**Root Cause**
- Kamaji Helm 차트의 cert-gen Role/RoleBinding이 `helm.sh/hook` annotation으로 관리됨
- ArgoCD가 `argocd.argoproj.io/hook-finalizer`를 붙여 삭제를 block
- sync 실패 시 hook 리소스가 남아있어 다음 sync에서 충돌

**Resolution**
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

### Kamaji CRD annotation 크기 초과

**Symptom**
- force sync 시 CRD apply 실패
  ```
  metadata.annotations: Too long: may not be longer than 262144
  ```

**Root Cause**
- ArgoCD가 CSA(Client-Side Apply) 방식으로 sync 시 `kubectl.kubernetes.io/last-applied-configuration` annotation에 전체 CRD spec을 저장
- Kamaji CRD가 크기 제한(262144 bytes)을 초과

**Resolution**
1. 기존 Kamaji CRD 삭제 (CRD 삭제 시 관련 CR도 삭제되므로 주의)
   ```bash
   kubectl delete crd tenantcontrolplanes.kamaji.clastix.io
   ```
2. ArgoCD Application에 `ServerSideApply=true` 추가
   ```yaml
   syncOptions:
     - ServerSideApply=true
   ```
3. ArgoCD sync — SSA 방식으로 CRD 재생성

---

### dpf-operator Pod Pending (nodeAffinity 불일치)

**Symptom**
```
0/6 nodes are available: 2 node(s) had untolerated taint(s),
4 node(s) didn't match Pod's node affinity/selector.
```

**Root Cause**
- dpf-operator 차트 기본 affinity가 control-plane 전용 (`node-role.kubernetes.io/master` or `control-plane`)
- Sandbox 클러스터에는 taint가 있는 control-plane만 존재 → 스케줄 불가

Helm 딥머지 함정:
| values 설정 | 결과 |
|------------|------|
| `affinity: {}` | 차트 기본값과 딥머지 → 기본 affinity 유지 |
| `affinity: null` | 차트 기본값과 딥머지 → 기본 affinity 유지 |
| `nodeSelectorTerms: []` | Kubernetes가 거부 (최소 1개 필요) |

**Resolution**
모든 노드에 존재하는 레이블로 catch-all nodeSelectorTerm 사용:
```yaml
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

### dpf-operator 이미지 공개 배포 없음

**Symptom**
- dpf-operator values에 이미지 미설정 시 `nvcr.io/nvidia/...` 참조 → ImagePullBackOff
- NVIDIA 내부 레지스트리라 pull 불가

**Root Cause**
- `github.com/nvidia/doca-platform` 저장소는 소스코드만 공개
- 빌드된 `dpf-system` 이미지는 NVIDIA 내부 레지스트리에만 존재

**Resolution**
소스에서 직접 빌드 후 Docker Hub에 push:

```bash
cd doca-platform

# build/defaults.yaml 수정 — Docker Hub 이미지 참조
# dpfSystemImage: jinkernel/doca-platform:dpf-system-dev
# dmsImage: jinkernel/doca-platform:dpf-system-dev
# bfbRegistryImage: jinkernel/doca-platform:dpf-system-dev
# ...

docker build -f Dockerfile.dpf-system \
  -t jinkernel/doca-platform:dpf-system-dev \
  --build-arg builder_image=golang:1.22 \
  --build-arg base_image=gcr.io/distroless/static:nonroot \
  --build-arg TAG=dev .
docker push jinkernel/doca-platform:dpf-system-dev
```

핵심 교훈: DPF operator 이미지 빌드 시 `build/defaults.yaml`의 이미지 주소가 컨테이너 내부 `/etc/dpf-defaults.yaml`로 bake-in됨 — 반드시 최종 pull 가능한 레지스트리로 설정할 것.

values.yaml 업데이트:
```yaml
controllerManager:
  image:
    repository: jinkernel/doca-platform
    tag: dpf-system-dev
```

---

### Harbor push EOF

**Symptom**
```
The push refers to repository [10.34.25.12:80/library/dpf-system]
dd843f0a96ab: Preparing
...
EOF
```
layer가 "Preparing" 상태에서 무한 대기 후 EOF

**Root Cause**
- 빌드 머신(10.30.0.184)이 Harbor(10.34.25.12)와 다른 네트워크 세그먼트
- 라우팅 또는 프록시 문제로 대용량 layer 전송 중 연결 끊김

**Resolution**
Harbor 대신 Docker Hub 사용:
```bash
docker logout 10.34.25.12:80
docker login -u jinkernel
docker tag 10.34.25.12:80/library/dpf-system:dev jinkernel/doca-platform:dpf-system-dev
docker push jinkernel/doca-platform:dpf-system-dev
```

---

### DPF 시스템 컨트롤러 전체 ImagePullBackOff

**Symptom**
DPFOperatorConfig 적용 후 생성된 DPF 시스템 Pod들이 전부 `ImagePullBackOff`:
```
dpf-dpu-detector-*                     0/1  ImagePullBackOff
dpf-provisioning-controller-manager-*  0/1  ContainerCreating
dpuservice-controller-manager-*        0/1  ImagePullBackOff
kamaji-cm-controller-manager-*         0/1  ImagePullBackOff
```
모두 이미지 `10.34.25.12:80/library/dpf-system:dev`를 pull 시도:
```
failed to do request: Head "https://10.34.25.12:80/v2/...": http: server gave HTTP response to HTTPS client
```

**Root Cause**
두 가지 문제 중첩:

1. `/etc/dpf-defaults.yaml` bake-in 문제
   - DPF operator 바이너리는 시작 시 컨테이너 내 `/etc/dpf-defaults.yaml`을 읽어 시스템 컴포넌트 이미지 주소를 결정
   - 이미지 빌드 시 `REGISTRY=10.34.25.12:80/library`로 설정하여 Harbor 주소가 bake-in됨

2. Harbor에 이미지가 없음
   - `10.34.25.12:80`에 `library/dpf-system` 레포지토리 자체 미존재

3. Containerd HTTP 미설정 (부가)
   - 노드의 containerd가 `10.34.25.12:80`을 HTTPS로 시도 (containerd 기본 동작)

**Resolution**
`build/defaults.yaml`을 수정하여 Docker Hub 이미지를 참조하도록 변경 후 재빌드:
```yaml
dpfSystemImage: jinkernel/doca-platform:dpf-system-dev
dmsImage: jinkernel/doca-platform:dpf-system-dev
ovsCniImage: jinkernel/doca-platform:dpf-system-dev
bfbRegistryImage: jinkernel/doca-platform:dpf-system-dev
cniInstallerImage: jinkernel/doca-platform:dpf-system-dev
keepalivedImage: jinkernel/doca-platform:dpf-system-dev
nodeSRIOVDevicePluginImage: nvcr.io/nvidia/mellanox/sriov-network-device-plugin:network-operator-v25.10.0
```
재빌드 후 operator Pod을 재시작하면 모든 시스템 Pod이 올바른 이미지로 교체됨.

---

## Provisioning 이슈

### Helper 이미지 오배선 (bfb-registry, DMS 크래시)

**Symptom**
- `bfb-registry` Pod 반복 재시작 (`/bin/sh` 없음)
- `tempnode-bf3-dms` Pod `Init:RunContainerError` (`/bin/bash` 없음)
- `dpf-provisioning-controller-manager`는 Running이지만 provisioning 실패

**Root Cause**
DPF는 역할별 이미지 분리를 전제로 설계됨:

| 역할 | 기대 이미지 | 내부 기대 바이너리 |
|------|-------------|--------------------|
| provisioning controller | `dpf-system` | `/provisioning` |
| bfb-registry | `bfb-registry` | `/bin/sh`, `envsubst`, `nginx` |
| dms / hostagent | `hostdriver` | `/bin/bash`, `/hostagent`, `rshim` |

실제로는 helper Pod까지 `jinkernel/doca-platform:dpf-system-dev` 단일 이미지로 들어가 계약 붕괴.

중요: controller Pod가 Running이라고 해서 provisioning이 정상인 것은 아님.

**Resolution**
세 이미지 세트를 같은 릴리즈 태그로 재빌드:
- `jinkernel/dpf-system:<tag>`
- `jinkernel/hostdriver:<tag>`
- `jinkernel/bfb-registry:<tag>`

`dpf-system` 이미지 내부 `/etc/dpf-defaults.yaml`에 아래 값 bake:
```yaml
dpfSystemImage: jinkernel/dpf-system:<tag>
dmsImage: jinkernel/hostdriver:<tag>
bfbRegistryImage: jinkernel/bfb-registry:<tag>
```

확인 기준:
```bash
kubectl get deployment dpf-provisioning-controller-manager -n dpf-operator-system \
  -o jsonpath='{range .spec.template.spec.containers[0].env[*]}{.name}={.value}{"\n"}{end}'
# BFB_REGISTRY_IMAGE=jinkernel/bfb-registry:...
# args에 --dms-image=jinkernel/hostdriver:...
```

---

### DPUFlavor immutable + 스키마 불일치

**Symptom**
ArgoCD sync 에러:
```
DPUFlavor.provisioning.dpu.nvidia.com "bf3-flavor" is invalid:
spec: Invalid value: "object": DPUFlavor spec is immutable
```

**Root Cause**
- 기존 `bf3-flavor`가 이미 생성되어 있음
- Git manifest 형식이 현재 API와 다름 (`parameters:` 아래 object/map 형식 → 배열 형식이어야 함)
- `DPUFlavor.spec`은 immutable → patch 시도 자체 실패

**Resolution**
1. GitOps manifest 형식 수정 (`parameters`는 배열):
   ```yaml
   spec:
     nvconfig:
       - device: "*"
         parameters:
           - "SRIOV_EN=1"
           - "NUM_OF_VFS=8"
           - "PF_TOTAL_SF=20"
   ```
2. live object 삭제 (DPUSet/DPU 없는 상태에서만 안전):
   ```bash
   kubectl delete dpuflavor -n dpf-operator-system bf3-flavor
   ```
3. ArgoCD가 새 형태로 재생성하도록 유도

---

### PVC 기반 /bfb 권한 문제

**Symptom**
```
open /bfb/bfb-...: permission denied
```
`BFB` CR이 계속 `Error` 상태

**Root Cause**
- provisioning controller는 비root (`runAsUser: 65532`)로 동작
- hostPath 모드에서는 `prepare-local-storage` init container가 `/bfb` 권한 보정
  ```text
  mkdir -p /bfb && chown -R 65532:65532 /bfb
  ```
- PVC 모드에서는 이 init container가 코드상 제거됨 (`Ensure no init container while using persistent volume` 주석)
- Ceph PVC 초기 권한은 root 계열 → 비root 컨트롤러가 쓰기 불가

코드 위치: `internal/operator/inventory/dpu_provisioning_controller_manifests.go`

**Resolution**
원본 repo 수정:
- PVC 모드에서도 `prepare-local-storage` init container 유지
- `/bfb` 경로 `mkdir/chown 65532:65532` 수행

테스트:
```bash
go test ./internal/operator/inventory -run TestProvisioningControllerObjects_GenerateManifests -count=1
```

새 이미지 세트 재빌드 후 배포. 성공 시 `BFB` 상태: `Error` → `Initializing` → `Ready`

---

### Non-semver 태그로 인한 Operator 업그레이드 검증 실패

**Symptom**
ArgoCD는 `Synced/Healthy`이지만 `dpf-provisioning-controller-manager`는 예전 이미지 유지:
```
parse requested version: invalid version public-main-45265976: invalid semantic version
```

**Root Cause**
- operator가 managed component 업그레이드 전에 semver validation 수행
- custom tag(`public-main-...` 형태)가 semver가 아니므로 업그레이드 타겟으로 인식 못함
- desired state는 바뀌었지만 실제 rollout이 막힘

**Resolution**
개발 환경 기준 — 재생성 흐름 사용 (DPUSet/DPU 없는 상태에서만):
```bash
kubectl delete dpfoperatorconfig -n dpf-operator-system dpfoperatorconfig
```
ArgoCD가 다시 생성하도록 두고, 새 스펙으로 system component 재구성.

> 운영 환경에서는 함부로 삭제 전략 사용 금지. custom tag를 계속 쓸 계획이면 semver alias 전략 고민 필요.

---

### DPUSet webhook 타입 오류 (noEffect)

**Symptom**
ArgoCD sync 실패:
```
admission webhook "mdpuset.kb.io" denied the request:
json: cannot unmarshal object into Go struct field
NodeEffect.spec.dpuTemplate.spec.nodeEffect.noEffect of type bool
```

**Root Cause**
GitOps manifest의 필드 타입 오류:
```yaml
nodeEffect:
  noEffect: {}   # 잘못됨 — object 타입
```
webhook이 기대하는 타입은 `bool`.

**Resolution**
```yaml
nodeEffect:
  noEffect: true   # bool 타입
```

---

### stuck Terminating DMS Pod

**Symptom**
기존 `tempnode-bf3-dms`가 `1/3 Terminating` 상태에서 종료되지 않음:
```
FailedKillPod / KillContainerError / context deadline exceeded
```

**Root Cause**
이전 버전의 `rshim` 컨테이너가 kubelet 종료 경로에서 정리되지 않음. upgrade/recreate 과정에서 남은 오래된 Pod 잔재.

**Resolution**
```bash
kubectl delete pod -n dpf-operator-system tempnode-bf3-dms --force --grace-period=0
```
controller가 새 `tempnode-bf3-dms`를 재생성, 최종 `3/3 Running`.

---

### DPU Agent 패키지 버전 오류

**Symptom**
- `DPU` 상태: `phase: Error`, `previousPhase: OS Installing`
- `OSInstalled=False`, `reason: InstallationTerminated`
- BF3 내부: `cloud-init status --long` → `status: error`
- `systemctl is-enabled dpu-agent.service` → `not-found`

**Root Cause**
BF3 내부 `/var/log/cloud-init-output.log`:
```
dpkg: error processing archive /var/cache/apt/archives/dpu-agent_public-main-45265976-bfbpcfix1_arm64.deb (--unpack):
 'Version' field value 'public-main-45265976-bfbpcfix1': version number does not start with digit
```

코드 경로: `Makefile`의 `DPUAGENT_PKG_VERSION = $(subst v,,$(TAG))`
- `TAG`에서 `v`만 제거 → 나머지 그대로 `.deb` 버전에 사용
- `public-main-...` 형태는 Debian `Version` 규칙 위반 (숫자로 시작해야 함)

결과 체인:
```text
커스텀 이미지 태그
  -> dpu-agent .deb Version에 그대로 반영
  -> dpkg 설치 거부
  -> dpu-agent 미설치
  -> LastStartupTime 미보고
  -> hostagent timeout
  -> provisioning Error
```

**Resolution**
빠른 해결:
- Debian-safe 형식 태그로 재빌드: `0.45265976-bfbpvcfix2` 또는 `1.0.0-publicmain45265976-bfbpvcfix2`
- 같은 태그로 이미지 세트 재빌드 (`dpf-system`, `hostdriver`, `bfb-registry`, `dpf-keepalived`)
- `dpu-agent` 패키지도 같은 새 버전으로 재패키징

정석 수정:
- `DPUAGENT_PKG_VERSION`을 Debian 규칙에 맞게 정규화
- 이미지 tag와 package version 별도 변수 사용

---

## 네트워크 이슈

### tempnode-bf3 Cilium Service ClusterIP 경로 장애

**Symptom**
- `DPU` 상태: `OSInstalled=False`, `reason: InstallationTerminated`, `message: failed to read from source file ... connection reset by peer`
- hostagent 로그: `max number of runs reached, terminating installation`

**Root Cause**
- `tempnode-bf3` host/hostNetwork 경로에서만 Service ClusterIP datapath 비정상
- Cilium이 `br-dpu`를 direct routing device로 선택하지 못함
- Cilium 재시작 로그: `unable to determine direct routing device. Use --direct-routing-device to specify it`
- `devices-controller`: `devices="[enp175s0f1np1 tmfifo_net0]"` — `br-dpu`가 후보 장치에 없음

진단 근거:
- PodIP로 직접 접근: `200 4802bytes` (정상)
- NodePort로 접근: `200 4802bytes` (정상)
- ClusterIP로 접근: `200 0bytes` (body 미수신)
- 같은 노드 일반 Pod network: 정상
- 다른 노드(sandbox-1) hostNetwork: 정상
- → tempnode-bf3 노드 특화 host/service datapath 문제

**Resolution**
node-scoped `CiliumNodeConfig` 적용:
```yaml
apiVersion: cilium.io/v2
kind: CiliumNodeConfig
metadata:
  name: tempnode-bf3-direct-routing
  namespace: kube-system
spec:
  nodeSelector:
    matchLabels:
      kubernetes.io/hostname: tempnode-bf3
  defaults:
    devices: br-dpu
    direct-routing-device: br-dpu
```

`tempnode-bf3`의 Cilium Pod 삭제 후 재기동:
```bash
kubectl -n kube-system delete pod -l k8s-app=cilium \
  --field-selector spec.nodeName=tempnode-bf3
```

복구 확인: Cilium 로그에서 `Devices changed ... [br-dpu]`, `Direct routing device detected ... br-dpu` 확인.

GitOps 영속화: `argocd/apps/00-infra/cilium/base/tempnode-bf3-ciliumnodeconfig.yaml`에 위 CiliumNodeConfig 추가.

재발 방지: BF3 worker host 추가 시 확인 항목:
- `br-dpu` 존재
- default route가 `br-dpu`
- 중복 management subnet attachment 없음
- Cilium이 `br-dpu`를 실제 service/host 경로 장치로 인식하는지 확인

빠른 판단 기준: `PodIP` 정상 + `NodePort` 정상 + `ClusterIP` 실패 → 거의 확실히 host/service datapath 문제

---

### br-comm-ch IP 미할당으로 DPU Cluster Config 단계 블로킹

**Symptom**
- `DPU` phase: `DPU Cluster Config`
- `BridgeIPChecked=False`
- `br-comm-ch does not have an IP address`
- `DPUClusterReady=False`, `node tempnode-bf3-mt25476000nu not found`
- hostagent 로그: `SFCreated=True`, `VFMacSet=True`, `BridgeIPChecked=False`

이 단계까지 성공한 항목:
- `OSInstalled=True`, `Rebooted=True`, `HostNetworkReady=True`
- `agentLastStartupTime` 존재, `dpu-agent` 통신 정상

**Root Cause**
DPU agent 코드 전제 (`internal/provisioning/dpuagent/operations/netplan/netplan.go`):
- `pf0vf0`를 `br-comm-ch`에 붙임
- `br-comm-ch`에 `DHCP4: true`
- 이후 `br-comm-ch`에 IPv4가 붙는지 검사

실제 환경:
- host `br-dpu`는 100G 망에 연결, static IP 기반
- DPU comm channel 쪽 DHCP 없음 → `br-comm-ch`가 IP를 못 받아 영구 실패

**Resolution**
`internal/provisioning/dpuagent/operations/netplan/netplan.go` 수정:
- 기존: `DHCP4: true`
- 변경: `DHCP4: false`, `DHCP6: false`, `Addresses: ["10.34.20.99/12"]`

주의:
- host에 `10.34.20.99`를 따로 추가하는 것이 아님
- DPU 내부 `br-comm-ch`에만 static IP 부여
- host 쪽 `br-dpu`는 기존 `10.34.20.4/12` 유지

새 이미지 세트로 재빌드 후 배포. old provisioning CR 삭제 후 fresh reconcile:
```bash
kubectl delete dpfoperatorconfig -n dpf-operator-system dpfoperatorconfig
# ArgoCD가 재생성하도록 유도
```

성공 확인 체인:
```text
static IP 부여
  -> BridgeIPChecked=True
  -> KubeletConfigured=True
  -> KubeletStarted=True
  -> DPUClusterReady=True
  -> DPU Ready
```

---

## 체크리스트

### 재발 방지 체크리스트

**이미지**
- `dpf-system`, `hostdriver`, `bfb-registry`, `dpf-keepalived`를 같은 태그 세트로 관리
- `dpf-system` 내부 `/etc/dpf-defaults.yaml` 확인
- live deployment에서 `--dms-image=...` 및 `BFB_REGISTRY_IMAGE=...` 확인
- `TAG`가 Debian-safe 형식인지 확인 (숫자로 시작해야 함)

**GitOps**
- `DPUFlavor` 변경 전 immutable 여부 확인 → 변경 시 삭제 후 재생성
- `nodeEffect.noEffect` 타입 확인 (`bool`, `{}` 아님)
- `DPUFlavor.nvconfig.parameters`는 `["KEY=VALUE", ...]` 배열 형식
- CRD/API 타입과 manifest 형식 일치 확인

**소스 패치**
- PVC/hostPath 분기별 `/bfb` 권한 bootstrap 동작 확인
- 테스트로 manifest 생성 결과 확인:
  ```bash
  go test ./internal/operator/inventory -run TestProvisioningControllerObjects_GenerateManifests
  ```

**클러스터 검증**
- `BFB`가 `Ready`
- `tempnode-bf3-dms`가 `3/3 Running`
- `DPUSet`가 생성됨
- `DPU` 상태와 condition 확인:
  ```bash
  kubectl get dpu -n dpf-operator-system tempnode-bf3-mt25476000nu \
    -o jsonpath='{.status.phase}{"\n"}{range .status.conditions[*]}{.type}={.status}:{.reason}{"\n"}{end}'
  ```

**호스트 검증**
- `br-dpu` 존재 (`ip link show br-dpu`)
- default route가 `br-dpu` (`ip route | grep '^default'`)
- OOB NIC가 bridge slave로 연결
- 중복 management subnet attachment 없음
- Cilium이 `br-dpu`를 service/host 경로 장치로 인식
  ```bash
  kubectl -n kube-system logs <cilium-pod-on-tempnode-bf3> --tail=50 | grep -E "device|routing"
  curl http://<bfb-registry-clusterip>:8082/bfb/bfcfg/<file>
  ```
