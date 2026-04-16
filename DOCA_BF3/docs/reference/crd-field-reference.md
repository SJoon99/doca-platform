---
title: "DPF CRD 필드 레퍼런스 (kubectl explain 기반)"
---

[TOC]

> `kubectl explain <resource> --recursive` 실행 결과를 기반으로 작성.
> `-required-` 표시된 필드는 필수값.

---

## BFB

DPU에 설치할 OS 이미지 정의.

```
kubectl explain bfb.spec
```

### spec

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `url` | string | ✅ | BFB 파일 다운로드 URL (HTTP/HTTPS) |
| `fileName` | string | | 저장할 파일명 오버라이드 (기본값: URL에서 추출) |
| `versions.atf` | string | | ATF(ARM Trusted Firmware) 버전 |
| `versions.bsp` | string | | BSP(Board Support Package) 버전 |
| `versions.doca` | string | | DOCA 버전 |
| `versions.uefi` | string | | UEFI 버전 |

### status (읽기 전용)

| 필드 | 설명 |
|------|------|
| `phase` | `Initializing` → `Downloading` → `Ready` → `Deleting` |
| `fileName` | 실제 저장된 파일명 |
| `versions.*` | 다운로드된 BFB에서 감지된 버전 정보 |

---

## DPUFlavor

DPU 시스템 수준 설정 템플릿. **생성 후 변경 불가(Immutable)**.

```
kubectl explain dpuflavor.spec
```

### spec

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `dpuMode` | string | | DPU 동작 모드: `dpu` / `zero-trust` / `nic` |
| `grub.kernelParameters` | []string | | 커널 부트 파라미터 (예: `iommu=pt`) |
| `sysctl.parameters` | []string | | 커널 sysctl 파라미터 |
| `nvconfig` | []Object | | mlxconfig 펌웨어 파라미터 설정 |
| `nvconfig[].device` | string | | 대상 디바이스: `*` / `p0` / `p1` / `P0` / `P1` |
| `nvconfig[].parameters` | []string | | mlxconfig key=value 쌍 |
| `ovs.rawConfigScript` | string | | OVS 설정 스크립트 (shell) |
| `dpuResources` | map | | DPU 전체 가용 리소스 (cpu, memory, nvidia.com/sf 등) |
| `systemReservedResources` | map | | 시스템 예약 리소스 (DPUService에 미할당) |
| `bfcfgParameters` | []string | | bf.cfg 파라미터 (BFB 설치 시 적용) |
| `configFiles` | []Object | | DPU에 배포할 설정 파일 |
| `configFiles[].path` | string | | 파일 경로 |
| `configFiles[].raw` | string | | 파일 내용 |
| `configFiles[].permissions` | string | | 파일 권한 (예: `0644`) |
| `configFiles[].operation` | string | | `override` (덮어쓰기) / `append` (추가) |
| `containerdConfig.registryEndpoint` | string | | DPU containerd 레지스트리 엔드포인트 |
| `hostNetworkInterfaceConfigs` | []Object | | 호스트 측 네트워크 인터페이스 설정 |
| `hostNetworkInterfaceConfigs[].portNumber` | integer | ✅ | 포트 번호 (0=p0, 1=p1) |
| `hostNetworkInterfaceConfigs[].mtu` | integer | | MTU 값 |
| `hostNetworkInterfaceConfigs[].dhcp` | boolean | | DHCP 활성화 여부 |
| `hostNetworkInterfaceConfigs[].nvconfig` | Object | | 해당 포트의 mlxconfig 설정 |

**nvConfig 주요 파라미터 예시:**

| 파라미터 | 설명 | 예시값 |
|---------|------|--------|
| `SRIOV_EN` | SR-IOV 활성화 | `"1"` |
| `NUM_OF_VFS` | PF당 VF 개수 | `"8"` |
| `PF_TOTAL_SF` | Scalable Function 수 | `"20"` |
| `LINK_TYPE_P0` | P0 링크 타입 | `"ETH"` / `"IB"` |
| `LINK_TYPE_P1` | P1 링크 타입 | `"ETH"` / `"IB"` |

---

## DPUSet

선택된 노드의 DPU에 BFB + DPUFlavor를 적용하는 컨트롤러.

```
kubectl explain dpuset.spec
```

### spec

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `dpuNodeSelector` | LabelSelector | | 대상 호스트 노드 선택 |
| `dpuDeviceSelector` | LabelSelector | | 대상 DPU 디바이스 추가 필터 |
| `dpuSelector` | map | | DPU 레이블 셀렉터 |
| `strategy.type` | string | | `RollingUpdate` / `OnDelete` |
| `strategy.rollingUpdate.maxUnavailable` | integer/string | | 동시 프로비저닝 최대 노드 수 |
| `dpuTemplate` | Object | ✅ | DPU 설정 템플릿 |

### spec.dpuTemplate.spec

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `bfb.name` | string | | BFB CR 이름 참조 |
| `dpuFlavor` | string | ✅ | DPUFlavor CR 이름 참조 |
| `secureBoot` | boolean | | UEFI Secure Boot 활성화 |
| `astraEnabled` | boolean | | NVIDIA Astra 연동 활성화 |
| `cluster.nodeLabels` | map | | DPU 클러스터 조인 후 노드에 붙을 레이블 |
| `cluster.selector` | LabelSelector | | 특정 DPUCluster 지정 |

### spec.dpuTemplate.spec.nodeEffect

프로비저닝 중 호스트 노드에 적용할 동작.

| 필드 | 타입 | 설명 |
|------|------|------|
| `noEffect` | boolean | 호스트 노드에 아무 영향 없음 (Sandbox 권장) |
| `taint` | Object | 호스트 노드에 Taint 추가 |
| `taint.key` | string ✅ | Taint 키 |
| `taint.effect` | string ✅ | `NoSchedule` / `NoExecute` / `PreferNoSchedule` |
| `taint.value` | string | Taint 값 |
| `drain` | boolean | 호스트 노드 drain (워크로드 이동) |
| `force` | boolean | drain 시 강제 종료 |
| `hold` | boolean | 프로비저닝을 일시 중단 상태로 유지 |
| `customAction` | string | 커스텀 스크립트/액션 |
| `customLabel` | map | 커스텀 레이블 추가 |
| `applyOnLabelChange` | boolean | 레이블 변경 시 nodeEffect 재적용 |
| `nodeMaintenanceAdditionalRequestors` | []string | 추가 maintenance requestor |

### spec.dpuTemplate.annotations

| 어노테이션 | 설명 |
|-----------|------|
| `provisioning.dpu.nvidia.com/host-power-cycle-required` | 웜 리부트 대신 콜드 부팅 강제 |

---

## DPFOperatorConfig

DPF Operator 전역 설정. 클러스터당 1개(싱글톤).

```
kubectl explain dpfoperatorconfig.spec
```

### spec.provisioningController (required)

| 필드 | 타입 | 설명 |
|------|------|------|
| `bfbPVCName` | string | BFB 저장용 PVC 이름 |
| `maxUnavailableDPUNodes` | integer | 동시 프로비저닝 최대 노드 수 |
| `maxDPUParallelInstallations` | integer | 노드당 병렬 설치 최대 수 |
| `multiDPUOperationsSyncWaitTime` | string | 멀티 DPU 동작 간 대기 시간 (예: `30s`) |
| `osInstallTimeout` | string | OS 설치 타임아웃 (예: `30m`) |
| `dmsTimeout` | integer | DMS 통신 타임아웃(초) |
| `nodeEffectRemovalTimeout` | string | nodeEffect 해제 타임아웃 |
| `disable` | boolean | Provisioning Controller 비활성화 |
| `hostAgentDNSPolicy` | string | Host Agent DNS 정책: `ClusterFirstWithHostNet` / `ClusterFirst` / `Default` / `None` |
| `enableDynamicBFCFGTemplates` | boolean | 동적 bf.cfg 템플릿 활성화 |
| `bfCFGTemplateConfigMap` | string | bf.cfg 템플릿 ConfigMap 이름 |
| `customCASecretName` | string | 커스텀 CA 인증서 Secret |
| `registry.address` | string | 내부 레지스트리 주소 |
| `registry.port` | integer | 내부 레지스트리 포트 |
| `installInterface` | Object | DPU 설치 인터페이스 방식 선택 |
| `installInterface.installViaHostAgent` | Object | Host Agent 방식 (기본) |
| `installInterface.installViaGNOI` | Object | gNOI 방식 |
| `installInterface.installViaRedfish` | Object | Redfish(BMC) 방식 |

### spec.networking

| 필드 | 타입 | 설명 |
|------|------|------|
| `controlPlaneMTU` | integer | 관리 네트워크 MTU (1280~9216, 기본 1500) |
| `highSpeedMTU` | integer | 고속 인터페이스(p0/p1) MTU (1280~9216) |

### spec.kamajiClusterManager

| 필드 | 타입 | 설명 |
|------|------|------|
| `disable` | boolean | Kamaji 클러스터 매니저 비활성화 |
| `replicas` | integer | 컨트롤러 레플리카 수 |
| `image` | string | 이미지 오버라이드 |
| `controller.resources` | Object | CPU/메모리 limits/requests |

### spec.staticClusterManager

| 필드 | 타입 | 설명 |
|------|------|------|
| `disable` | boolean | Static 클러스터 매니저 비활성화 |
| `replicas` | integer | 컨트롤러 레플리카 수 |

### spec.imagePullSecrets

```yaml
imagePullSecrets:
  - nvcr-secret   # NGC 인증 등 레지스트리 시크릿 이름
```

### 서브 컴포넌트 (모두 `disable` + `image` + `resources` 패턴)

| 컴포넌트 | 설명 |
|---------|------|
| `flannel` | DPU 클러스터 CNI (flannel) |
| `multus` | 멀티 네트워크 인터페이스 |
| `ovsCNI` | OVS CNI 플러그인 |
| `nvipam` | NVIDIA IPAM 컨트롤러 |
| `sriovDevicePlugin` | SR-IOV Device Plugin (호스트) |
| `nodeSRIOVDevicePluginController` | 노드별 SR-IOV 플러그인 컨트롤러 |
| `dpuDetector` | DPU 하드웨어 감지 데몬 |
| `dpuServiceController` | DPUService 컨트롤러 |
| `serviceSetController` | ServiceSet 컨트롤러 |
| `sfcController` | SFC(Service Function Chain) 컨트롤러 |
| `cniInstaller` | CNI 바이너리 설치 |
| `monitoring` | 모니터링 (kube-state-metrics, node-problem-detector, otel-collector) |

### spec.overrides (고급 설정)

| 필드 | 설명 |
|------|------|
| `paused` | 전체 Operator 일시 중단 |
| `kubernetesAPIServerVIP` | API 서버 VIP 오버라이드 |
| `kubernetesAPIServerPort` | API 서버 포트 오버라이드 |
| `dpuCNIPath` | DPU CNI 바이너리 경로 오버라이드 |
| `flannelSkipCNIConfigInstallation` | flannel CNI config 설치 스킵 |

---

## DPUService

DPU 클러스터에 Helm 차트를 DaemonSet으로 배포.

```
kubectl explain dpuservice.spec
```

### spec

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `helmChart` | Object | ✅ | Helm 차트 정의 |
| `helmChart.source.repoURL` | string | ✅ | Helm 레포 URL |
| `helmChart.source.chart` | string | | 차트 이름 (repoURL이 Helm 레포인 경우) |
| `helmChart.source.path` | string | | 차트 경로 (Git 소스인 경우) |
| `helmChart.source.version` | string | ✅ | 차트 버전 |
| `helmChart.source.releaseName` | string | | Helm 릴리즈 이름 오버라이드 |
| `helmChart.values` | Object | | Helm values 오버라이드 (자유 형식) |
| `interfaces` | []string | | 사용할 DPUServiceInterface 이름 목록 |
| `paused` | boolean | | 서비스 일시 중단 (배포 없음) |
| `deployInCluster` | boolean | | DPU가 아닌 Host Cluster에 배포 |
| `serviceID` | string | | 서비스 식별자 (ServiceChain 연동 시 사용) |
| `dpuClusterSelector` | LabelSelector | | 배포할 DPU 클러스터 선택 |

### spec.serviceDaemonSet

| 필드 | 타입 | 설명 |
|------|------|------|
| `labels` | map | DaemonSet Pod에 추가할 레이블 |
| `annotations` | map | DaemonSet Pod 어노테이션 |
| `nodeSelector` | Object | Pod 스케줄링 노드 셀렉터 |
| `resources` | map | 컨테이너별 리소스 limits/requests |
| `updateStrategy.type` | string | `RollingUpdate` / `OnDelete` |
| `updateStrategy.rollingUpdate.maxSurge` | int/% | 롤링 업데이트 최대 초과 Pod 수 |
| `updateStrategy.rollingUpdate.maxUnavailable` | int/% | 롤링 업데이트 최대 중단 Pod 수 |

### spec.configPorts

호스트 클러스터에서 DPU 서비스 포트에 접근하기 위한 설정.

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `serviceType` | string | ✅ | `NodePort` / `ClusterIP` / `None` |
| `ports[].name` | string | ✅ | 포트 이름 |
| `ports[].port` | integer | ✅ | 서비스 포트 |
| `ports[].protocol` | string | ✅ | `TCP` / `UDP` |
| `ports[].nodePort` | integer | | NodePort 값 (serviceType=NodePort 시) |

**중요 레이블:**
- `svc.dpu.nvidia.com/critical: "true"` — Pod 미실행 시 호스트 노드에 NoSchedule Taint 추가

---

## DPUServiceInterface

DPUService가 사용하는 네트워크 인터페이스 정의.

```
kubectl explain dpuserviceinterface.spec
```

### spec

| 필드 | 타입 | 설명 |
|------|------|------|
| `dpuClusterSelector` | LabelSelector | 적용할 DPU 클러스터 선택 |
| `template.spec.nodeSelector` | LabelSelector | 적용할 DPU 노드 선택 |

### spec.template.spec.template.spec (인터페이스 정의 핵심)

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `interfaceType` | string | ✅ | `physical` / `pf` / `vf` / `vlan` / `service` / `ovn` / `patch` |
| `node` | string | | 특정 노드 지정 |

**interfaceType별 전용 필드:**

| 타입 | 필드 | 설명 |
|------|------|------|
| `physical` | `physical.interfaceName` ✅ | 물리 인터페이스 이름 (예: `p0`, `p1`) |
| `pf` | `pf.pfID` ✅ | PF ID (0 또는 1) |
| `pf` | `pf.virtualNetwork` | 연결할 가상 네트워크 |
| `vf` | `vf.pfID` ✅ | 부모 PF ID |
| `vf` | `vf.vfID` ✅ | VF ID |
| `vf` | `vf.parentInterfaceRef` | 부모 인터페이스 참조 |
| `vf` | `vf.virtualNetwork` | 연결할 가상 네트워크 |
| `vlan` | `vlan.parentInterfaceRef` ✅ | 부모 인터페이스 |
| `vlan` | `vlan.vlanID` ✅ | VLAN ID |
| `service` | `service.serviceID` ✅ | 서비스 ID |
| `service` | `service.network` ✅ | 서비스 네트워크 이름 |
| `service` | `service.interfaceName` ✅ | 인터페이스 이름 |
| `service` | `service.virtualNetwork` | 연결할 가상 네트워크 |
| `ovn` | `ovn.externalBridge` | OVN 외부 브릿지 이름 |
| `patch` | `patch.peerBridge` ✅ | Peer 브릿지 이름 |
| `patch` | `patch.peerPatchName` | Peer patch 포트 이름 |
| `patch` | `patch.peerExternalIDs` | OVS external_ids 맵 |

---

## DPUServiceChain

서비스 간 트래픽 경로(SFC) 정의.

```
kubectl explain dpuservicechain.spec
```

### spec

| 필드 | 타입 | 설명 |
|------|------|------|
| `dpuClusterSelector` | LabelSelector | 적용할 DPU 클러스터 |
| `template.spec.nodeSelector` | LabelSelector | 적용할 DPU 노드 |

### spec.template.spec.template.spec

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `node` | string | | 특정 노드 지정 |
| `switches` | []Object | ✅ | 트래픽 스위치 정의 목록 |
| `switches[].serviceMTU` | integer | | 서비스 간 MTU |
| `switches[].ports` | []Object | ✅ | 포트 목록 |
| `switches[].ports[].serviceInterface.matchLabels` | map | ✅ | ServiceInterface 선택 레이블 |
| `switches[].ports[].serviceInterface.ipam` | Object | | IP 할당 설정 |
| `switches[].ports[].serviceInterface.ipam.matchLabels` | map | ✅ | IPAM 선택 레이블 |
| `switches[].ports[].serviceInterface.ipam.defaultGateway` | boolean | | 기본 게이트웨이 설정 |
| `switches[].ports[].serviceInterface.ipam.setDefaultRoute` | boolean | | 기본 라우트 설정 |

---

## DPUCluster

DPU 전용 Kubernetes 클러스터 컨트롤 플레인.

```
kubectl explain dpucluster.spec
```

### spec (required)

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `type` | string | ✅ | `kamaji` (Kamaji 관리) / `static` (기존 클러스터 연결) |
| `maxNodes` | integer | | 최대 노드 수 |
| `kubeconfig` | string | | Static 타입 시 kubeconfig Secret 이름 |
| `clusterEndpoint.keepalived.vip` | string | ✅ | Keepalived VIP 주소 |
| `clusterEndpoint.keepalived.interface` | string | ✅ | VIP 바인딩 인터페이스 |
| `clusterEndpoint.keepalived.virtualRouterID` | integer | ✅ | VRRP 라우터 ID (1~255) |
| `clusterEndpoint.keepalived.nodeSelector` | map | | VIP 할당 노드 선택 |

---

## DPUDeployment

BFB + DPUFlavor + DPUService + ServiceChain을 하나로 오케스트레이션. (권장 방식)

```
kubectl explain dpudeployment.spec
```

### spec.dpus (required)

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `bfb` | string | ✅ | BFB CR 이름 |
| `flavor` | string | ✅ | DPUFlavor CR 이름 |
| `nodeEffect` | Object | | 호스트 노드 영향 설정 (DPUSet과 동일) |
| `secureBoot` | boolean | | UEFI Secure Boot 활성화 |
| `astraEnabled` | boolean | | NVIDIA Astra 연동 |
| `dpuSetStrategy` | Object | | DPUSet 업데이트 전략 |
| `dpuSets` | []Object | | DPU 그룹별 세부 설정 |
| `dpuSets[].nameSuffix` | string | ✅ | DPUSet 이름 접미사 |
| `dpuSets[].dpuNodeSelector` | LabelSelector | | 노드 선택 |
| `dpuSets[].dpuClusterSelector` | map | | DPU 클러스터 선택 |

### spec.services (required)

```yaml
services:
  <서비스명>:              # DPUServiceTemplate의 deploymentServiceName과 일치
    serviceTemplate: <이름>        # DPUServiceTemplate CR 참조
    serviceConfiguration: <이름>   # DPUServiceConfiguration CR 참조
    dependsOn:                     # 의존 서비스 목록
      - name: <다른 서비스명>
```

### spec.serviceChains

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `switches` | []Object | ✅ | 트래픽 스위치 정의 (DPUServiceChain과 동일) |
| `switches[].serviceMTU` | integer | | MTU |
| `upgradePolicy.applyNodeEffect` | boolean | ✅ | 업그레이드 시 nodeEffect 적용 여부 |

| `revisionHistoryLimit` | integer | | 보관할 리비전 히스토리 수 |

---

## 필드 확인 명령어

```bash
# 특정 필드 상세 설명
kubectl explain dpuflavor.spec.nvconfig
kubectl explain dpuset.spec.dpuTemplate.spec.nodeEffect
kubectl explain dpfoperatorconfig.spec.networking
kubectl explain dpuservice.spec.configPorts
kubectl explain dpuserviceinterface.spec.template.spec.template.spec

# 유효성 검사 (클러스터에 실제 적용 없이 검증)
kubectl apply --dry-run=server -f <파일>.yaml
```
