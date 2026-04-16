---
title: Current DPF Cluster And DPU Topology
---

[TOC]

# 목적

- 현재 `DPU cluster` 가 실제로 어떻게 생성되어 있는지 확인하는 방법 정리
- 현재 host cluster, DPF, Kamaji tenant control-plane, BF3 DPU 사이 관계를 한 번에 보이기
- 운영 중 빠르게 상태 점검할 때 보는 명령과 정상 기대값 정리

# 한눈에 보는 현재 상태

- host cluster: 정상
- Argo apps: `Synced`, `Healthy`
- `BFB`: `Ready`
- `DPUCluster`: `Ready`
- `DPUSet`: `READY=True`
- `DPU`: `READY=True`, `PHASE=Ready`
- tenant DPU cluster 안 worker node: `Ready`

현재 핵심 리소스 이름

- host namespace
  - `dpf-operator-system`
- tenant control-plane namespace
  - `dpu-cplane-tenant1`
- BFB
  - `bf-bundle-3-2-0`
- DPUSet
  - `bf3-dpuset`
- DPU
  - `tempnode-bf3-mt25476000nu`
- DPUCluster
  - `dpu-cplane-tenant1`

# DPU cluster 가 생성되어 있는지 확인하는 방법

## 1. DPUCluster 객체 확인

```bash
kubectl get dpucluster -A -o wide
kubectl get dpucluster -n dpu-cplane-tenant1 dpu-cplane-tenant1 -o yaml
```

정상 기대값

- `READY=True`
- `PHASE=Ready`
- `TYPE=kamaji`
- `VERSION=v1.34.0`
- `nodesCount=1`

현재 live 값

- namespace: `dpu-cplane-tenant1`
- name: `dpu-cplane-tenant1`
- type: `kamaji`
- keepalived VIP: `10.34.20.250`
- keepalived interface: `eno8`
- kubeconfig secret: `dpu-cplane-tenant1-admin-kubeconfig`
- nodesCount: `1`

## 2. tenant control-plane pod 확인

```bash
kubectl get all -n dpu-cplane-tenant1 -o wide
kubectl get secret -n dpu-cplane-tenant1
```

정상 기대값

- Kamaji control-plane pod 3개 `3/3 Running`
- keepalived pod `1/1 Running`
- admin kubeconfig secret 존재

현재 live 값

- deployment
  - `dpu-cplane-tenant1` `3/3`
- daemonset
  - `dpu-cplane-tenant1-keepalived` `1/1`
- service
  - `dpu-cplane-tenant1` `NodePort`
  - `dpu-cplane-tenant1-metrics` `ClusterIP`
- secret
  - `dpu-cplane-tenant1-admin-kubeconfig`
  - API server / scheduler / controller-manager / datastore 관련 secret 존재

## 3. 실제 DPU worker join 확인

tenant admin kubeconfig 로 tenant cluster 안 node 확인

```bash
kubectl get secret -n dpu-cplane-tenant1 dpu-cplane-tenant1-admin-kubeconfig -o go-template='{{index .data "admin.conf" | base64decode}}' > /tmp/dpu-cplane-tenant1-admin.conf
kubectl --kubeconfig /tmp/dpu-cplane-tenant1-admin.conf get nodes -o wide
```

정상 기대값

- BF3 DPU worker node 1개가 `Ready`

현재 live 값

- node
  - `tempnode-bf3-mt25476000nu`
- status
  - `Ready`
- version
  - `v1.34.2`
- OS
  - `Ubuntu 24.04.3 LTS`
- kernel
  - `6.8.0-1012-bluefield-64k`

## 4. DPU provisioning 완료 상태 확인

```bash
kubectl get bfb,dpuset,dpu -A -o wide
kubectl get dpu -n dpf-operator-system tempnode-bf3-mt25476000nu -o yaml
```

정상 기대값

- `BFB Ready`
- `DPUSet READY=True`
- `DPU READY=True`
- `DPU phase=Ready`

현재 live 값

- `BFBPrepared=True`
- `BFBReady=True`
- `OSInstalled=True`
- `Rebooted=True`
- `BridgeIPChecked=True`
- `KubeletConfigured=True`
- `KubeletStarted=True`
- `DPUClusterReady=True`
- 최종 `Ready=True`

## 5. DPF system component 확인

```bash
kubectl get pods -n dpf-operator-system -o wide
kubectl -n argocd get application dpf-operator dpf-provisioning-config dpf-provisioning-dpucluster dpf-provisioning-dpuset -o wide
```

정상 기대값

- `dpf-provisioning-controller-manager` 2개 `Running`
- `tempnode-bf3-dms` `3/3 Running`
- `bfb-registry` `1/1 Running`
- 관련 Argo app 전부 `Synced`, `Healthy`

# 현재 클러스터 구조

## 전체 구조

```text
+--------------------------------------------------------------------------------------+
|                                Host Kubernetes Cluster                               |
|                                                                                      |
|  Namespaces / Apps                                                                   |
|                                                                                      |
|  argocd                                                                              |
|    - dpf-operator                          Synced / Healthy                          |
|    - dpf-provisioning-config              Synced / Healthy                          |
|    - dpf-provisioning-dpucluster          Synced / Healthy                          |
|    - dpf-provisioning-dpuset              Synced / Healthy                          |
|                                                                                      |
|  dpf-operator-system                                                                  |
|    - DPFOperator / provisioning controllers                                          |
|    - BFB:        bf-bundle-3-2-0  -> Ready                                           |
|    - DPUSet:     bf3-dpuset       -> READY=True                                      |
|    - DPU:        tempnode-bf3-mt25476000nu -> Ready                                  |
|    - DPUNode:    tempnode-bf3                                                     |
|                                                                                      |
|  dpu-cplane-tenant1                                                                  |
|    - DPUCluster: dpu-cplane-tenant1 -> Ready                                         |
|    - Kamaji control-plane pods 3개                                                   |
|    - keepalived pod 1개                                                              |
|    - VIP: 10.34.20.250                                                               |
|                                                                                      |
+--------------------------------------------------------------------------------------+
```

## 리소스 생성 흐름

```text
GitOps / ArgoCD
  |
  +-- dpf-operator app
  |     |
  |     +-- DPF operator system components 생성
  |
  +-- dpf-provisioning-config app
  |     |
  |     +-- DPFOperatorConfig 생성
  |
  +-- dpf-provisioning-dpucluster app
  |     |
  |     +-- DPUCluster(dpu-cplane-tenant1) 생성
  |           |
  |           +-- Kamaji control-plane deployment 생성
  |           +-- keepalived daemonset 생성
  |           +-- VIP 10.34.20.250 준비
  |
  +-- dpf-provisioning-dpuset app
        |
        +-- DPUSet(bf3-dpuset) 생성
              |
              +-- DPU(tempnode-bf3-mt25476000nu) 생성
                    |
                    +-- BFB 준비 / 설치
                    +-- hostagent / dms / rshim 경로 실행
                    +-- BF3 부팅
                    +-- dpu-agent 등록
                    +-- tenant DPUCluster join
                    +-- 최종 Ready
```

## 현재 DPUCluster 내부 구조

```text
Namespace: dpu-cplane-tenant1

+----------------------------------------------------------------------------------+
| DPUCluster: dpu-cplane-tenant1                                                   |
|----------------------------------------------------------------------------------|
| type                 | kamaji                                                    |
| version              | v1.34.0                                                   |
| kubeconfig secret    | dpu-cplane-tenant1-admin-kubeconfig                       |
| maxNodes             | 10                                                        |
| keepalived interface | eno8                                                      |
| keepalived VIP       | 10.34.20.250                                              |
| nodesCount           | 1                                                         |
| phase                | Ready                                                     |
+----------------------------------------------------------------------------------+

Kamaji deployment
  - dpu-cplane-tenant1-6c8bc7b888-6fmtb  3/3 Running
  - dpu-cplane-tenant1-6c8bc7b888-l2szx  3/3 Running
  - dpu-cplane-tenant1-6c8bc7b888-xhssn  3/3 Running

Keepalived
  - dpu-cplane-tenant1-keepalived-4mp57  1/1 Running
  - VIP 10.34.20.250 제공

Services
  - dpu-cplane-tenant1          NodePort   10.233.8.121
  - dpu-cplane-tenant1-metrics  ClusterIP  10.233.53.0
```

## 현재 DPU provisioning 객체 관계

```text
BFB
  bf-bundle-3-2-0
    |
    +-- BFB file 제공
    |
    +-- bfb-registry Service/Pod 통해 hostagent 가 다운로드

DPUSet
  bf3-dpuset
    |
    +-- owner of DPU
    |
    +-- template:
          bfb          = bf-bundle-3-2-0
          dpuFlavor    = bf3-flavor
          cluster.name = dpu-cplane-tenant1

DPU
  tempnode-bf3-mt25476000nu
    |
    +-- dpuNodeName     = tempnode-bf3
    +-- pciAddress      = 0000-af-00
    +-- pf0 name        = enp175s0f0np0
    +-- BFBReady        = True
    +-- OSInstalled     = True
    +-- BridgeIPChecked = True
    +-- KubeletStarted  = True
    +-- DPUClusterReady = True
    +-- Ready           = True
```

## host node 와 BF3 DPU 구조

```text
Host node: tempnode-bf3

  Host side
    enp175s0f0np0   -> 100G management/data 겸용 경로
    br-dpu          -> host bridge
    enp175s0f0v0    -> host-side VF, br-dpu 에 연결
    tmfifo_net0     -> host <-> BF3 bootstrap / management sideband

  DPF pods on host
    tempnode-bf3-dms
      - dms
      - hostagent
      - rshim

  Cluster-side service
    bfb-registry
      - NodePort 30256
      - ClusterIP 10.233.15.169
      - BFB/BFCFG 제공

  BF3 DPU side
    pf0vf0          -> DPU-side VF
    br-comm-ch      -> static IP 10.34.20.99/12
    kubelet         -> tenant DPU cluster join
    dpu-agent       -> hostagent / controller 상태 보고
```

## 네트워크 관점 구조

```text
                                      Host Kubernetes Cluster
                                              |
                                              |
                               +--------------+--------------+
                               |                             |
                               |                             |
                       +-------v--------+            +-------v------------------+
                       | bfb-registry   |            | DPUCluster VIP           |
                       | Service/Pod    |            | 10.34.20.250            |
                       +-------+--------+            +-------+------------------+
                               |                             |
                               |                             |
                    +----------v-----------------------------v----------+
                    |           tempnode-bf3 host node                  |
                    |---------------------------------------------------|
                    | br-dpu                                            |
                    | enp175s0f0np0  (100G management/data)             |
                    | enp175s0f0v0   (host-side VF)                     |
                    | tempnode-bf3-dms                                  |
                    |   - dms                                            |
                    |   - hostagent                                      |
                    |   - rshim                                          |
                    +----------------------+----------------------------+
                                           |
                                           | PCIe / VF pair
                                           |
                    +----------------------v----------------------------+
                    |                  BF3 DPU                          |
                    |---------------------------------------------------|
                    | pf0vf0                                            |
                    | br-comm-ch = 10.34.20.99/12                       |
                    | dpu-agent                                          |
                    | kubelet                                            |
                    | worker node: tempnode-bf3-mt25476000nu            |
                    +---------------------------------------------------+
```

# 현재 구조에서 중요한 의미

- `DPUCluster` 는 host cluster 안에 생성된 또 하나의 tenant control-plane
- BF3 DPU 는 host cluster node가 아니라 `tenant DPU cluster` 의 worker node
- `DPUCluster Ready` 와 `tenant kubeconfig get nodes` 결과가 둘 다 좋아야 진짜 성공
- 현재 환경에서는
  - `100G management/data 겸용 경로`
  - `DPU 내부 static br-comm-ch`
  조합으로 운영 중

# 정상 상태를 빠르게 보는 최소 명령

가장 짧은 점검 세트

```bash
kubectl get bfb,dpucluster,dpuset,dpu -A -o wide
kubectl get pods -n dpf-operator-system -o wide
kubectl get all -n dpu-cplane-tenant1 -o wide
kubectl -n argocd get application dpf-operator dpf-provisioning-config dpf-provisioning-dpucluster dpf-provisioning-dpuset -o wide
```

worker join 까지 확인하는 세트

```bash
kubectl get secret -n dpu-cplane-tenant1 dpu-cplane-tenant1-admin-kubeconfig -o go-template='{{index .data "admin.conf" | base64decode}}' > /tmp/dpu-cplane-tenant1-admin.conf
kubectl --kubeconfig /tmp/dpu-cplane-tenant1-admin.conf get nodes -o wide
```

# 현재 기준 정상 판단

현재는 아래를 모두 만족

- `BFB Ready`
- `DPUCluster Ready`
- `DPUSet READY=True`
- `DPU Ready`
- tenant cluster 안 worker node `Ready`
- DPF 관련 Argo app `Synced`, `Healthy`

즉 현재 상태는

- DPU cluster 생성 완료
- BF3 DPU provisioning 완료
- BF3 DPU worker join 완료
- 운영 가능한 정상 상태

# 관련 문서

- `DOCA_BF3/docs/doca_platform/dpf-build-history-and-image-usage.md`
  - 어떤 이미지를 왜 빌드했는지
  - 실제 빌드 명령
  - helper image 세트가 런타임에서 어떻게 연결되는지
