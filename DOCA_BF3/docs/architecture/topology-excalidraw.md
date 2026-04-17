---
title: Current Cluster Topology Excalidraw Guide
---

[TOC]

# 파일 위치

- Excalidraw import 파일
  - `DOCA_BF3/docs/architecture/topology.excalidraw.json`

# 이 그림에 담은 내용

- host Kubernetes cluster 큰 박스
- 각 node 별로 어떤 pod / component 가 떠 있는지
- `node4`
  - provisioning runtime
  - tenant DPU control-plane
- `tempnode-bf3`
  - BF3 host node
  - `tempnode-bf3-dms`
- BF3 DPU 자체
  - `tempnode-bf3-mt25476000nu`
  - `br-comm-ch`
  - `tmfifo_net0`
- host <-> BF3 의 `PCIe / VF pair` 관계

# 현재 반영한 live 상태

## node4

- `bfb-registry`
- `dpf-provisioning-controller-manager` x2
- `dpuservice-controller-manager`
- `kamaji-cm-controller-manager` x2
- `dpu-cplane-tenant1` control-plane pod x3
- `dpu-cplane-tenant1-keepalived`
- VIP `10.34.20.250`

## sandbox-1

- `dpf-dpu-detector`
- `dpf-operator-controller-manager`
- `kamaji-etcd-0`

## sandbox-2

- `dpf-dpu-detector`
- `kamaji-etcd-2`

## sandbox-3

- `dpf-dpu-detector`
- `kamaji-etcd-1`
- `maintenance-operator`
- `kamaji`

## sandbox-4

- `dpf-dpu-detector`

## tempnode-bf3

- `tempnode-bf3-dms`
- `br-dpu: 10.34.20.4/12`
- host side VF `enp175s0f0v0`

## BF3 DPU

- worker node: `tempnode-bf3-mt25476000nu`
- `br-comm-ch: 10.34.20.99/12`
- `tmfifo_net0: fe80::2/64`
- `oob_net0`: 존재, 현재 보조 경로

# 여는 방법

- Excalidraw 웹 UI
  - `https://excalidraw.com`
  - `Open` -> `Load from file`
  - `topology.excalidraw.json` 선택

# 참고 문서

- [`architecture/cluster-topology.md`](./cluster-topology.md)
- [`reference/build-history.md`](../reference/build-history.md)
