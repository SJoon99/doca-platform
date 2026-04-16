---
title: DPF Build History And Image Usage
---

[TOC]

# 목적

- 이번 작업에서 실제로 어떤 이미지를 빌드했는지 정리
- 각 이미지가 런타임에서 어디에 쓰이는지 정리
- 왜 태그 세트가 여러 번 바뀌었는지 정리
- 다음에 같은 작업을 다시 할 때 바로 따라 할 수 있게 빌드 명령 정리

# 한눈에 보는 결론

이번 DPF provisioning 경로에서 실제로 중요했던 이미지는 4개

- `dpf-system`
- `hostdriver`
- `bfb-registry`
- `dpf-keepalived`

이 4개는 서로 독립 이미지이지만, 실제 배포에서는 같은 태그 세트로 맞춰야 함

이유

- `dpf-system` 안의 `/etc/dpf-defaults.yaml` 이 나머지 helper image 기본값을 들고 있음
- provisioning controller 는 이 기본값을 사용해 helper pod 이미지와 control-plane helper 이미지를 내려보냄
- 따라서 controller 이미지 태그만 바꾸고 helper 세트를 같이 안 맞추면 런타임 계약이 깨짐

# 이미지별 역할

## 1. dpf-system

역할

- `dpf-operator-controller-manager`
- `dpf-provisioning-controller-manager`
- 여러 system component의 주 이미지

중요 포인트

- `/etc/dpf-defaults.yaml` 내장
- 여기 안에
  - `dmsImage`
  - `bfbRegistryImage`
  - `keepalivedImage`
  기본값이 들어감

관련 파일

- `Makefile`
- `internal/release/templates/defaults.yaml.tmpl`
- `internal/release/defaults.go`

## 2. hostdriver

역할

- `tempnode-bf3-dms` pod 내부 `dms`, `hostagent`, `rshim` 실행 기반 이미지
- BF3 설치 과정에서 필요한 `dpu-agent` 패키지 아티팩트 제공

중요 포인트

- `--dms-image=<hostdriver>` 형태로 provisioning controller 에서 내려감
- `dpu-agent` 패키지 버전 문제도 이 이미지 산출물 경로와 직접 연결됨

관련 파일

- `internal/operator/inventory/dpu_provisioning_controller_manifests.go`
- `internal/provisioning/controllers/util/dms/util.go`

## 3. bfb-registry

역할

- BFB / BFCFG 파일을 hostagent 에 제공하는 registry pod 이미지

중요 포인트

- provisioning controller 가 `BFB_REGISTRY_IMAGE` env 로 주입
- `bfb-registry` pod 로 실행됨

관련 파일

- `internal/operator/inventory/dpu_provisioning_controller_manifests.go`
- `internal/provisioning/bfbregistry/bfb_registry_creator.go`

## 4. dpf-keepalived

역할

- DPU tenant control-plane VIP 제공
- `DPUCluster` 의 API endpoint 고정 진입점 역할

중요 포인트

- Kamaji cluster manager 가 `--keepalived-image=<image>` 로 사용
- keepalived daemonset 으로 배포됨

관련 파일

- `internal/operator/inventory/cluster_manager_manifests.go`

# 코드에서 이미지가 연결되는 지점

실제 연결 지점

- `BFB_REGISTRY_IMAGE`
  - `internal/operator/inventory/dpu_provisioning_controller_manifests.go:481`
- `--dms-image`
  - `internal/operator/inventory/dpu_provisioning_controller_manifests.go:599`
- `--keepalived-image`
  - `internal/operator/inventory/cluster_manager_manifests.go:113`
- `defaults.yaml` 내 image 기본값
  - `internal/release/templates/defaults.yaml.tmpl:2`
  - `internal/release/templates/defaults.yaml.tmpl:3`
  - `internal/release/templates/defaults.yaml.tmpl:5`
  - `internal/release/templates/defaults.yaml.tmpl:9`

즉 구조를 그리면

```text
dpf-system image
  |
  +-- /etc/dpf-defaults.yaml
        |
        +-- dmsImage          -> hostdriver
        +-- bfbRegistryImage  -> bfb-registry
        +-- keepalivedImage   -> dpf-keepalived

provisioning / cluster-manager controller
  |
  +-- helper pod / daemonset 생성 시 위 기본값 사용
```

# 실제 빌드 히스토리

## 1차 세트

태그

- `public-main-45265976-bfbpvcfix1`

목적

- helper image 계약 복구
- `/bin/sh`, `/bin/bash` 없는 단일 이미지 오배선 문제 해소
- PVC `/bfb` 관련 소스 패치 반영 시작

이미지

- `jinkernel/dpf-system:public-main-45265976-bfbpvcfix1`
- `jinkernel/hostdriver:public-main-45265976-bfbpvcfix1`
- `jinkernel/bfb-registry:public-main-45265976-bfbpvcfix1`
- `jinkernel/dpf-keepalived:public-main-45265976-bfbpvcfix1`

한계

- Debian package version 으로는 부적합한 non-semver 태그
- `dpu-agent` `.deb` 설치에서 `Version field does not start with digit` 문제 발생

## 2차 세트

태그

- `0.1.1-publicmain45265976-bfbpvcfix2`

목적

- semver / Debian-safe 버전으로 재패키징
- `dpu-agent` `.deb` 설치 실패 문제 해결

이미지

- `jinkernel/dpf-system:0.1.1-publicmain45265976-bfbpvcfix2`
- `jinkernel/hostdriver:0.1.1-publicmain45265976-bfbpvcfix2`
- `jinkernel/bfb-registry:0.1.1-publicmain45265976-bfbpvcfix2`
- `jinkernel/dpf-keepalived:0.1.1-publicmain45265976-bfbpvcfix2`

성과

- BF3 안에서 `dpu-agent` 설치 성공
- provisioning chain 이 `OS Installing` 이후 단계로 진행

## 3차 세트

태그

- `0.1.2-publicmain45265976-bfbpvcfix3`

목적

- `br-comm-ch` DHCP 가정 제거
- DPU 내부 `br-comm-ch` 를 static `10.34.20.99/12` 로 고정

이미지

- `jinkernel/dpf-system:0.1.2-publicmain45265976-bfbpvcfix3`
- `jinkernel/hostdriver:0.1.2-publicmain45265976-bfbpvcfix3`
- `jinkernel/bfb-registry:0.1.2-publicmain45265976-bfbpvcfix3`
- `jinkernel/dpf-keepalived:0.1.2-publicmain45265976-bfbpvcfix3`

성과

- `BridgeIPChecked=True`
- `KubeletStarted=True`
- `DPUClusterReady=True`
- 최종 `DPU Ready`

# 실제 사용한 빌드 절차

원격 빌드 호스트

- `10.30.0.184`
- 작업 경로
  - `/home/joon/Documents/doca-platform-build-45265976`

사전 확인

```bash
ssh joon@10.30.0.184
cd /home/joon/Documents/doca-platform-build-45265976
```

테스트

```bash
/home/joon/lib/go/bin/go test ./internal/provisioning/dpuagent/operations/netplan ./internal/provisioning/dpuagent/operations/checkbridge
```

실제 사용한 빌드 형태

```bash
export TAG=0.1.2-publicmain45265976-bfbpvcfix3
export DPF_SYSTEM_ARCH="amd64 arm64"

make docker-build-dpf-system
make docker-push-dpf-system

make docker-build-hostdriver
make docker-push-hostdriver

make docker-build-bfb-registry
make docker-push-bfb-registry

make docker-build-keepalived
make docker-push-keepalived
```

Makefile 타깃 근거

- `docker-build-dpf-system`
- `docker-push-dpf-system`
- `docker-build-hostdriver`
- `docker-push-hostdriver`
- `docker-build-bfb-registry`
- `docker-push-bfb-registry`
- `docker-build-keepalived`
- `docker-push-keepalived`

관련 Makefile 위치

- `Makefile:1383`
- `Makefile:1407`
- `Makefile:1464`
- `Makefile:1667`
- `Makefile:1631`
- `Makefile:1652`

# 빌드 결과 검증

실제 확인한 항목

## 1. multi-arch manifest 존재

검증 목적

- `amd64`, `arm64` 둘 다 push 되었는지 확인

## 2. dpf-system 안 defaults 내장 확인

검증 목적

- `dpf-system` 이 helper image 세트를 올바르게 가리키는지 확인

확인 항목

- `dmsImage`
- `bfbRegistryImage`
- `keepalivedImage`

## 3. hostdriver 안 dpu-agent package 버전 확인

검증 목적

- BF3 안에서 `.deb` 설치가 가능한 버전인지 확인

실제 확인했던 결과

- `Version: 0.1.2~publicmain45265976-bfbpvcfix3`

# 이 빌드가 실제 배포에서 어떻게 사용되는가

```text
GitOps values.yaml
  |
  +-- controllerManager.image = jinkernel/dpf-system:<TAG>
        |
        +-- dpf-operator-controller-manager
        +-- dpf-provisioning-controller-manager
              |
              +-- /etc/dpf-defaults.yaml 로 helper image 해석
                    |
                    +-- hostdriver       -> tempnode-bf3-dms
                    +-- bfb-registry     -> bfb-registry pod
                    +-- dpf-keepalived   -> DPUCluster keepalived daemonset
```

즉 운영 관점 핵심

- `dpf-system` 하나만 바꾸는 작업이 아님
- helper 3종까지 같은 태그 세트로 같이 맞춰야 함
- 이번 최종 성공 세트는 `0.1.2-publicmain45265976-bfbpvcfix3`

# 현재 문서와의 관계

함께 보면 좋은 문서

- `DOCA_BF3/docs/doca_platform/dpf-current-cluster-and-dpu-topology.md`
  - 현재 live cluster 구조
- `DOCA_BF3/docs/doca_platform/dpf-troubleshooting.md`
  - 문제 유형별 트러블슈팅
- `DOCA_BF3/docs/doca_platform/dpf-network-architecture.md`
  - 네트워크 구조 설명

# 최종 정리

이번 작업에서 중요한 포인트는 세 가지

- DPF는 단일 이미지 앱이 아니라 helper image 세트 구조
- `dpf-system` 안 defaults 가 helper image wiring 의 중심
- 최종 성공은 `0.1.2-publicmain45265976-bfbpvcfix3` 이미지 세트와 static `br-comm-ch` 수정이 함께 들어간 결과
