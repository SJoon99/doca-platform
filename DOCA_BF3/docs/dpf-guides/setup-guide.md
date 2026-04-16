---
title: "DPF 설치 가이드"
---

[TOC]

## 사전 조건

### br-dpu 호스트 네트워크 설정

`br-dpu`는 "있으면 좋은 설정"이 아니라 실제 provisioning 진행 조건이다.

#### 한눈에 결론

- 문서 기준
  - OOB 인터페이스는 `br-dpu`에 연결
  - 브리지 이름은 항상 `br-dpu`
  - IP / gateway / nameserver는 `br-dpu`에 설정
  - Netplan 설정은 kubelet 시작 전에 적용
- 코드 기준
  - `dpudetector`가 `br-dpu` 존재 여부 검사
  - `dpudetector`가 default route가 `br-dpu`를 타는지 검사
  - 조건 미충족 시 node label / status condition이 false
  - 이후 DPU controller가 `DPU OOB bridge is not configured`로 진행 중단
- 현재 환경 기준
  - `tempnode-bf3`에 `br-dpu` 없으면 `DPU`가 `Initializing`에서 멈춤

코드 검사 조건:
1. `br-dpu`라는 이름의 bridge 존재 (`cmd/dpudetector/oob.go`, `bridgeName = "br-dpu"`)
2. default route가 그 bridge 기준

controller 차단 지점: `internal/provisioning/controllers/dpu/state/initializing.go`
- `DPUNodeConditionBridgeConfigured`가 true가 아니면 초기화 중단

#### 환경 확인 방법

```bash
# Kubernetes 상태
kubectl get dpu -n dpf-operator-system tempnode-bf3-mt25476000nu \
  -o jsonpath='{.status.phase}{"\n"}{range .status.conditions[*]}{.type}={.status}:{.reason}:{.message}{"\n"}{end}'
kubectl get dpunode -n dpf-operator-system tempnode-bf3 \
  -o jsonpath='{range .status.conditions[*]}{.type}={.status}:{.reason}:{.message}{"\n"}{end}'
kubectl get node tempnode-bf3 --show-labels

# 호스트 직접 확인 (ssh joon@10.34.20.4)
ip link show br-dpu
networkctl status br-dpu
ip route | grep '^default'
networkctl list --no-pager
```

---

### BFB 파일 준비 (Ceph RGW 업로드)

DPF의 BFB CR이 참조할 수 있도록 BFB 파일을 클러스터 내 Ceph RGW(S3)에 업로드

#### 환경

| 항목 | 값 |
|------|-----|
| BFB 파일 위치 | `joon@tempnode-bf3:~/bf-bundle-3.2.0-113_25.10_ubuntu-24.04_64k_prod.bfb` |
| 파일 크기 | 1.5 GB |
| Ceph RGW 엔드포인트 | `http://10.233.37.8:80` (ClusterIP, 클러스터 내부 전용) |
| 버킷 이름 | `bfb` |
| S3 AccessKey | 기존 pydio 사용자 키 재사용 (`rook-ceph-object-user-ceph-objectstore-pydio` Secret) |

#### 절차

**1. RGW 접근 가능 여부 확인**

tempnode-bf3에서 ClusterIP로 직접 접근 가능한지 확인:

```bash
curl -s -o /dev/null -w '%{http_code}' http://10.233.37.8:80
# → 200
```

**2. S3 크리덴셜 확인**

```bash
kubectl get secret rook-ceph-object-user-ceph-objectstore-pydio \
  -n rook-ceph \
  -o jsonpath='{.data.AccessKey}' | base64 -d

kubectl get secret rook-ceph-object-user-ceph-objectstore-pydio \
  -n rook-ceph \
  -o jsonpath='{.data.SecretKey}' | base64 -d
```

**3. 버킷 생성 + Public Read 정책 적용**

tempnode-bf3에서 실행:

```python
import boto3, json
from botocore.client import Config

s3 = boto3.client('s3',
    endpoint_url='http://10.233.37.8:80',
    aws_access_key_id='<AccessKey>',
    aws_secret_access_key='<SecretKey>',
    config=Config(signature_version='s3v4'),
    region_name='us-east-1'
)

# 버킷 생성
s3.create_bucket(Bucket='bfb')

# Public read 정책 (BFB CR이 인증 없이 다운로드 가능하도록)
policy = json.dumps({
    'Version': '2012-10-17',
    'Statement': [{
        'Effect': 'Allow',
        'Principal': '*',
        'Action': 's3:GetObject',
        'Resource': 'arn:aws:s3:::bfb/*'
    }]
})
s3.put_bucket_policy(Bucket='bfb', Policy=policy)
```

**4. BFB 파일 업로드 (멀티파트)**

```python
from boto3.s3.transfer import TransferConfig

transfer_config = TransferConfig(
    multipart_threshold=100 * 1024 * 1024,
    multipart_chunksize=100 * 1024 * 1024
)

s3.upload_file(
    '/home/joon/bf-bundle-3.2.0-113_25.10_ubuntu-24.04_64k_prod.bfb',
    'bfb',
    'bf-bundle-3.2.0-113_25.10_ubuntu-24.04_64k_prod.bfb',
    Config=transfer_config
)
```

백그라운드 실행:
```bash
nohup python3 upload.py > /tmp/bfb-upload.log 2>&1 &
```

**5. 업로드 검증**

```python
obj = s3.head_object(
    Bucket='bfb',
    Key='bf-bundle-3.2.0-113_25.10_ubuntu-24.04_64k_prod.bfb'
)
print(f"Size: {obj['ContentLength'] / 1024/1024/1024:.2f} GB")
# → Size: 1.42 GB
```

HTTP 직접 접근 테스트 (Range 지원 확인):
```bash
curl -s -o /dev/null -w '%{http_code} %{size_download}bytes' \
  "http://10.233.37.8:80/bfb/bf-bundle-3.2.0-113_25.10_ubuntu-24.04_64k_prod.bfb" \
  --range 0-1023
# → 206 1024bytes
```

#### 결과

- **버킷 URL:** `http://10.233.37.8:80/bfb/`
- **BFB 파일 URL:** `http://10.233.37.8:80/bfb/bf-bundle-3.2.0-113_25.10_ubuntu-24.04_64k_prod.bfb`
- BFB CR `spec.url`에 위 URL 사용
- `10.233.37.8`은 ClusterIP — **클러스터 외부에서 접근 불가**
- BFB CR은 DPF Operator Pod(클러스터 내부)에서 다운로드하므로 문제 없음

---

## 설치 단계

### 컴포넌트 현황 및 설치 순서

#### Gap Analysis

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

#### 컴포넌트별 설치 이유

**cert-manager**
- TLS 인증서 자동 발급/갱신
- DPF Operator 웹훅 서버 인증서 자동 관리
- Kamaji가 DPU 클러스터 API 서버 인증서 발급에 사용
- 없으면: DPF Operator 웹훅 Pod `CrashLoopBackOff`, Kamaji TenantControlPlane 생성 불가

**Kamaji**
- Host Cluster Pod 안에서 DPU 측 Kubernetes 컨트롤 플레인 실행
- BF3 DPU는 자체 K8s 컨트롤 플레인이 없음 → Kamaji가 `TenantControlPlane` CRD로 대신 생성
- DPU kubeconfig 자동 생성 (DPUService 배포 대상)
- 없으면: DPU 클러스터 자체 생성 불가

**maintenance-operator**
- 노드 유지보수 시 워크로드 자동 드레인/코든 오케스트레이션
- BFB 플래시 전 tempnode-bf3 워크로드 안전하게 이동
- 없으면: BFB 플래시 중 tempnode-bf3 실행 중 Pod 강제 종료 위험

**DPF Operator**
- DPF 전체 서브시스템 오케스트레이션 (핵심 컴포넌트)
- 등록 CRD: `DPUSet`, `DPU`, `DPUCluster`, `DPUFlavor`, `BFB`, `DPUService`, `DPUDeployment`, `DPUServiceCredentialRequest`, `DPUServiceChain`, `DPUServiceInterface`, `DPFOperatorConfig`
- 설치 후: provisioning-controller, dpuservice-controller, hostagent 등 자동 배포

#### 단계별 설치 명령

> **주의:** ArgoCD, NFD 이미 설치됨 → `prereqs.yaml` 전체 적용 금지, `--selector`로 개별 설치

**1단계 — 사전 확인**

```bash
kubectl get sc
kubectl get svc -A | grep harbor
kubectl get node tempnode-bf3 --show-labels
```

**2단계 — cert-manager**

```bash
./hack/scripts/deploy-helmfile.sh \
  -f deploy/helmfiles/prereqs.yaml \
  --selector app=cert-manager
kubectl get pods -n cert-manager
```

**3단계 — Kamaji**

```bash
./hack/scripts/deploy-helmfile.sh \
  -f deploy/helmfiles/prereqs.yaml \
  --selector app=kamaji
kubectl get pods -n kamaji-system
kubectl get crd | grep tenantcontrolplane
```

**4단계 — maintenance-operator**

```bash
./hack/scripts/deploy-helmfile.sh \
  -f deploy/helmfiles/prereqs.yaml \
  --selector app=maintenance-operator
kubectl get pods -n dpf-operator-system | grep maintenance
```

**5단계 — DPF Operator**

```bash
helm install dpf-operator ./deploy/charts/dpf-operator \
  -n dpf-operator-system --create-namespace
kubectl get pods -n dpf-operator-system
kubectl get crds | grep -E "dpu|dpf|bluefield"
# 20+ CRD 등록됨

kubectl label node tempnode-bf3 feature.node.kubernetes.io/dpu-enabled=true
```

---

### ArgoCD App-of-Apps 구성

DPF 선행 컴포넌트들을 SandBox-Infra 레포의 ArgoCD App-of-Apps 패턴으로 관리.

#### 파일 구조

```
argocd/apps/07-Doca-Platform/
├── Chart.yaml
├── templates/
│   └── application-template.yaml      — Application 생성 템플릿 (CASE 1/2/3)
├── values.yaml
├── cert-manager/values.yaml
├── kamaji/values.yaml
├── maintenance-operator/values.yaml
├── dpf-operator/values.yaml
└── node-label-bf3/
    ├── kustomization.yaml
    ├── rbac.yaml                       — ServiceAccount + ClusterRole (nodes patch)
    └── job.yaml                        — PostSync Hook Job (tempnode-bf3 레이블)
```

SyncWave 구성:
```
syncWave 2: cert-manager
syncWave 3: kamaji (etcd 포함, datastore.enabled=false)
syncWave 4: kamaji-datastore (DataStore CR — kamaji Running 확인 후)
syncWave 5: maintenance-operator
syncWave 6: dpf-node-label (tempnode-bf3 레이블링 Job)
syncWave 7: dpf-operator
syncWave 8: dpf-provisioning-config (bfb-pvc, DPFOperatorConfig, BFB, DPUFlavor)
syncWave 9: dpf-provisioning-dpuset (DPUSet)
```

#### 템플릿 CASE 구분

| CASE | 조건 | 용도 |
|------|------|------|
| CASE 1 | `source.chart` 존재 | 외부 Helm/OCI 레지스트리 차트 + SandBox-Infra values |
| CASE 2 | `source.externalPath` 존재 | 외부 Git 레포 Helm 차트 + SandBox-Infra values |
| CASE 3 | `source.path` 존재 | SandBox-Infra 내부 Manifest/Kustomize |

#### 컴포넌트별 Values 변경 내용

**cert-manager**
- 차트: `https://charts.jetstack.io` / `cert-manager` / `v1.19.3`

| 항목 | 원본 | 변경 | 이유 |
|------|------|------|------|
| `crds.enabled` | `false` | `true` | Helm이 CRD를 직접 설치하도록 |
| `startupapicheck.enabled` | `true` | `false` | ArgoCD 환경에서 post-install hook Job이 sync를 블록할 수 있음 |

**kamaji**
- 차트: `oci://ghcr.io/nvidia/charts/kamaji` / `1.2.0`

| 항목 | 원본 | 변경 | 이유 |
|------|------|------|------|
| `image.repository` | `clastix/kamaji` | `ghcr.io/nvidia/kamaji` | NVIDIA DPF 전용 fork 사용 |
| `image.tag` | `null` | `v1.34.0-25.9.3` | DPF prereqs.yaml 기준 검증된 버전 고정 |
| `kamaji-etcd.persistentVolumeClaim.storageClassName` | (미정의) | `rook-ceph-block-hot` | etcd 데이터 영속성 |

> `ghcr.io/nvidia/kamaji`는 DPF가 관리하는 fork로 DPU 클러스터 TenantControlPlane 생성에 특화된 패치가 포함됨.

**maintenance-operator**
- 차트: `oci://ghcr.io/mellanox/maintenance-operator-chart` / `0.3.0`

| 항목 | 원본 | 변경 | 이유 |
|------|------|------|------|
| `operator.tolerations` | control-plane NoSchedule taint 목록 | `[]` | Sandbox 워커 노드에 스케줄링 가능하도록 |
| `operator.affinity` | control-plane preferred affinity | `{}` | 동상 |
| `operatorConfig.deploy` | `false` | `true` | `MaintenanceOperatorConfig` CR 자동 생성 |
| `operatorConfig.maxParallelOperations` | `null` | `"60%"` | 동시 유지보수 가능한 노드 상한 |

**dpf-operator**
- 차트: `https://github.com/SJoon99/doca-platform.git` / `deploy/charts/dpf-operator` / `public-main` (CASE 2)

| 항목 | 원본 | 변경 | 이유 |
|------|------|------|------|
| `affinity` | `node-role.kubernetes.io/master` or `control-plane` requiredDuringScheduling | catch-all (`kubernetes.io/hostname: Exists`) | Sandbox 워커 노드에 스케줄링 |
| `tolerations` | control-plane NoSchedule taint | `[]` | 동상 |
| `controllerManager.image.repository` | `""` | Harbor 이미지 빌드/push 후 설정 | ImagePullBackOff 방지 |

> Helm 딥머지 함정: `affinity: {}` 또는 `affinity: null` 은 차트 기본값과 딥머지되어 기본 affinity가 유지됨. 반드시 명시적 catch-all nodeSelectorTerm 사용.

**dpf-node-label (Kustomize)**
- tempnode-bf3에 `feature.node.kubernetes.io/dpu-enabled=true` 레이블 부착
- ArgoCD `PostSync` Hook + `BeforeHookCreation` 삭제 정책 — 매 sync마다 재실행 (idempotent)

#### 사전 확인 항목

| 항목 | 확인 명령 |
|------|-----------|
| `rook-ceph-block-hot` StorageClass 존재 | `kubectl get sc` |
| ArgoCD에 OCI 레지스트리 등록 | ArgoCD UI → Settings → Repositories |
| doca-platform 레포 접근 가능 | ArgoCD UI → Settings → Repositories |

---

## 프로비저닝 리소스

### 리소스 구성 개요

DPF Operator 실행 이후 BF3 DPU를 실제로 관리하기 위한 리소스. 두 개의 ArgoCD Application으로 구성 — 설정(wave 8) → 온보딩 트리거(wave 9) 순서 보장.

```
dpf-provisioning-config (syncWave 8)
├── bfb-pvc.yaml
├── dpfoperatorconfig.yaml
├── bfb.yaml
└── dpuflavor.yaml

dpf-provisioning-dpuset (syncWave 9)
└── dpuset.yaml
```

---

### dpf-provisioning-config (Wave 8)

wave 7의 dpf-operator가 Running 상태가 된 후, DPU 온보딩에 필요한 설정 리소스를 한 번에 적용.

**bfb-pvc.yaml**

```yaml
kind: PersistentVolumeClaim
name: bfb-pvc
storageClassName: rook-ceph-block-hot
storage: 10Gi
accessModes: ReadWriteOnce
```

- DPF Provisioning Controller가 BFB 파일을 다운로드해서 저장하는 볼륨
- 없으면 BFB CR이 `Pending` 상태에서 멈춤

**dpfoperatorconfig.yaml**

```yaml
kind: DPFOperatorConfig
name: dpfoperatorconfig   # 클러스터당 1개만 존재 (싱글톤)
spec:
  provisioningController:
    bfbPVCName: bfb-pvc
    maxUnavailableDPUNodes: 1
  kamajiClusterManager: {}
  networking:
    controlPlaneMTU: 1500
```

- DPF Operator 전체 동작을 제어하는 싱글톤 설정 리소스
- 이 리소스가 생성될 때까지 프로비저닝 컨트롤러가 활성화되지 않음
- `maxUnavailableDPUNodes: 1` — Sandbox 환경 DPU 1개

**bfb.yaml**

```yaml
kind: BFB
name: bf-bundle-3-2-0
spec:
  url: http://10.233.37.8:80/bfb/bf-bundle-3.2.0-113_25.10_ubuntu-24.04_64k_prod.bfb
```

- DPU에 설치할 OS 이미지(BFB) 정의
- 현재 DPU에 설치된 버전과 동일한 BFB 지정 시 **플래시 스킵** — 재부팅 없이 Kamaji 연결만 수행

**dpuflavor.yaml**

```yaml
kind: DPUFlavor
name: bf3-flavor
spec:
  dpuMode: dpu
  nvconfig:
    - device: "*"
      parameters:
        - "SRIOV_EN=1"
        - "NUM_OF_VFS=8"
        - "PF_TOTAL_SF=20"
  dpuResources:
    cpu: "16"
    memory: "32Gi"
    "nvidia.com/sf": "20"
  systemReservedResources:
    cpu: "4"
    memory: "8Gi"
```

- DPU 시스템 수준 설정 템플릿 — 펌웨어, 리소스 할당, 동작 모드 정의
- **생성 후 변경 불가(Immutable)** — 변경 시 삭제 후 재생성 필요
- `parameters`는 `["KEY=VALUE", ...]` 배열 형식 — object/map 형식 사용 시 webhook 거부

---

### dpf-provisioning-dpuset (Wave 9)

wave 8의 설정이 모두 준비된 후 실행. DPUSet이 온보딩을 트리거하는 리소스이므로 별도 wave로 분리.

**dpuset.yaml**

```yaml
kind: DPUSet
name: bf3-dpuset
spec:
  dpuNodeSelector:
    matchLabels:
      feature.node.kubernetes.io/dpu-enabled: "true"
  strategy:
    type: RollingUpdate
    rollingUpdate:
      maxUnavailable: 1
  dpuTemplate:
    spec:
      bfb:
        name: bf-bundle-3-2-0
      dpuFlavor: bf3-flavor
      nodeEffect:
        noEffect: true        # bool 타입 — {} 오브젝트 형식 사용 시 webhook 거부
      cluster:
        nodeLabels:
          node-role: dpu-worker
          dpu-model: bf3
```

- DPUSet이 생성되는 순간 DPF Operator가 프로비저닝을 시작
- `nodeEffect: noEffect: true` — Sandbox 환경에서 호스트 노드 drain/taint 하지 않음
- wave 9로 분리한 이유: BFB 다운로드(wave 8)가 완료된 후 온보딩이 시작돼야 하기 때문

---

### 리소스 간 의존 관계

```
bfb-pvc
  ↑ 참조
dpfoperatorconfig (bfbPVCName: bfb-pvc)
  → DPF Provisioning Controller 활성화

bfb.yaml
  → DPF가 URL에서 bfb-pvc로 다운로드

dpuflavor.yaml
  → DPU 설정 템플릿

dpuset.yaml
  ├── bfb: bf-bundle-3-2-0  ← bfb.yaml 참조
  └── dpuFlavor: bf3-flavor ← dpuflavor.yaml 참조
        ↓
  DPU 온보딩 시작
        ↓
  Kamaji TenantControlPlane 자동 생성
        ↓
  DPU kubeconfig Secret 생성
        ↓
  DPUService 배포 가능
```

#### 예상 동작 (BFB 버전 일치 시)

1. wave 8 sync — PVC 생성 → DPFOperatorConfig 적용 → BFB 다운로드 시작 → DPUFlavor 등록
2. wave 9 sync — DPUSet 생성 → DPF가 tempnode-bf3 감지
3. BF3 현재 BFB 버전 확인 → `bf-bundle-3.2.0-113` 일치 → **플래시 스킵**
4. nvConfig 적용 (SR-IOV 활성화) → DPU 재부팅
5. DPU 재부팅 후 Kamaji API 서버로 kubelet 재연결
6. Kamaji TenantControlPlane 생성 → DPU kubeconfig Secret 생성
7. DPUCluster Ready → DPUService 배포 가능
