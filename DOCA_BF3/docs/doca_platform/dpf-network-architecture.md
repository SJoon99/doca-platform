---
title: "DPF 네트워크 아키텍처"
---

[TOC]

## 현재 채택 구조

### 결론 요약

최종 채택:

```text
100G management/data 겸용 경로
  + static br-comm-ch
  = 현재 환경 제약에서 가장 현실적이고 안정적
```

- `100G` 인터페이스를 `br-dpu`로 사용 (host 관리 + cluster/data + DPU comm bootstrap 겸용)
- DPU 내부 `br-comm-ch`는 static IP (`10.34.20.99/12`) 사용
- provisioning 성공 및 `DPU Ready` 검증 완료

---

## 의사결정 배경

### 문제의 시작점

실제 마지막 블로커:

- `DPU` 상태 `DPU Cluster Config`
- `BridgeIPChecked=False`
- 메시지 — `br-comm-ch does not have an IP address`

직접 원인:

```text
코드 가정
  = br-comm-ch에 DHCP 존재

실제 환경
  = comm channel 쪽 DHCP 없음
```

- `enp61s0f1np1`에서 DHCP Discover는 나가지만 Offer가 오지 않음
- 스위치/VLAN/OOB 네트워크까지 다시 손봐야 하는 상황

---

### 검토한 선택지

#### 선택지 1: 공식 기본 흐름 유지 (기각)

- `br-comm-ch`는 DHCP 유지, 환경에서 DHCP를 제공
- 단점: 현재 환경에 DHCP 없음, 스위치/VLAN/OOB 네트워크까지 재설계 필요
- **결론: 기각** — 현재 환경 기준으로 실현 비용이 큼

#### 선택지 2: br-dpu를 1GbE OOB로 전환 (장기 검토)

- `enp61s0f1np1` 같은 `1GbE` 포트를 `br-dpu` uplink로 사용
- `100G`는 cluster/data 전용 경로로 분리
- 단점: 현재 `1GbE` 포트도 실제 별도 OOB 세그먼트로 확정 안 됨, DHCP 미응답
- **결론: 장기 검토 대상** — 물리/스위치/VLAN 전제가 준비된 뒤 재검토

```text
1GbE 포트 존재
  !=
별도 OOB 네트워크 준비 완료
```

#### 선택지 3: 100G 유지 + br-comm-ch static (채택)

- host `br-dpu`는 현재 100G 기반 경로 유지
- DPU 내부 `br-comm-ch`만 static IP 부여
- 장점: 현재 환경 제약 수용, DHCP 인프라 불필요, 실제 provisioning 성공 검증 완료
- **결론: 채택**

static `br-comm-ch` 효과 체인:

```text
br-comm-ch static IP
  -> BridgeIPChecked=True
  -> KubeletConfigured=True
  -> KubeletStarted=True
  -> DPUClusterReady=True
  -> DPU Ready
```

---

## 장기 네트워크 정리 계획

### 최종 목표 구조

```text
1GbE
  -> OOB / management
  -> br-dpu

100G
  -> cluster / data / high-speed
  -> 별도 경로

DPU 내부
  -> br-comm-ch
  -> static IP
```

현재 구조에서 이미 드러난 문제:
- Cilium direct routing device 혼선
- hostNetwork → Service 경로 문제
- `br-comm-ch DHCP` 가정과 실제 static 환경 충돌

---

### 실제 작업 항목

#### 1. 네트워크 의미 먼저 고정

운영 문서 기준으로 의미를 고정:
- `br-dpu` → OOB / management
- `100G` → cluster / data / high-speed
- `DPU comm channel` → static

이 단계 목적: 이후 netplan 변경이나 케이블 변경 전에 기준 정의 고정

#### 2. 1GbE uplink 후보 확정

- 대상: `enp61s0f1np1`
- `br-dpu` uplink 후보로 사용
- 확인 필요: 실제 OOB 스위치 연결 여부, 관리 접근 가능 여부, 필요한 VLAN/주소 체계

#### 3. br-dpu를 1GbE 기준으로 재구성

- `enp61s0f1np1` → `br-dpu`
- `enp175s0f0np0`는 `br-dpu`에서 제거
- 주의: maintenance window 필요, host 접근 경로에 영향 가능

#### 4. 100G를 별도 경로로 원복

- `100G` 경로를 다시 cluster/data 전용으로 분리
- node 간 고속 통신 / 실제 데이터 이동 / cluster 기본 통신 경로

---

### 권장 작업 순서 및 확인 항목

**작업 순서:**

1. 구조 문서 확정
2. `1GbE` OOB 포트 / 스위치 / VLAN 확인
3. maintenance window 확보
4. host netplan 초안 작성
5. `br-dpu`를 `1GbE`로 전환
6. `100G`를 cluster/data 경로로 원복
7. host/node/Cilium 확인
8. DPU reprovision 확인

**변경 후 확인 항목:**

- host 접근 정상
- node `Ready`
- Cilium 정상
- `br-dpu`가 `1GbE` 쪽에서 동작
- `100G` 경로가 cluster/data path로 유지
- `DPU` reprovision 성공
- `DPU Ready`

---

### 현재 유지할 것 (지금 건드리지 말 것)

유지:
- `br-comm-ch` static 방식 (`10.34.20.99/12`)
- `100G`를 cluster/data 전용으로 쓴다는 방향
- `host default route`를 당분간 `100G` 쪽에 유지하는 방향

지금 바로 건드리지 말 것:
- host default route를 즉시 `1GbE` 쪽으로 이동
- 이미 성공한 static `br-comm-ch` 로직 재수정
- 여러 DPU용 IP 풀 설계까지 한 번에 확장

---

### 후속 과제

- `br-comm-ch` static IP를 값 기반으로 외부화 (현재 하드코딩)
- 여러 DPU가 들어올 때 IP 할당 규칙 정의
- OOB/management와 cluster/data 경로 문서화 고도화
