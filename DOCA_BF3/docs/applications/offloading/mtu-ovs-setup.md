# DPU 기반 네트워크 성능 트러블슈팅 가이드

## 개요

BlueField-2 DPU 기반 환경에서 발생한 **성능 저하 문제**를 해결하기 위해, 두 가지 주요 이슈 발생

1. **MTU 미일치로 인한 점보 프레임 블랙홀**
2. **OVS datapath 및 HW 오프로딩 비활성화로 인한 성능 저하**
    1. 100G 속도가 안 나옴

---

## 문제 1: MTU 미일치 (점보 프레임 블랙홀)

### 원인

- 서버 NIC과 스위치는 9000B 이상의 MTU를 사용 중.
- 그러나 **DPU 내부 포트 및 OVS 브리지**가 MTU 1500으로 설정되어 있어 **대형 프레임이 드롭**됨.
- ICMP ping은 성공하지만 `iperf3` 데이터 전송은 실패 → MTU mismatch.

### 해결

DPU 내부 인터페이스 및 OVS 포트의 MTU를 **9216**으로 설정.

```bash
# DPU 내부에서 실행 (포트 이름은 환경에 맞게 조정)
for IF in p0 pf0hpf en3f0pf0sf0 en3f0pf0sf6; do
  # 커널 인터페이스 MTU 변경
  sudo ip link set dev $IF mtu 9216 2>/dev/null || true

  # OVS에 등록된 인터페이스에도 MTU 요청값 설정
  sudo ovs-vsctl set Interface $IF mtu_request=9216 2>/dev/null || true
done

# 브리지 자체 MTU도 동일하게 맞춤
sudo ip link set dev ovsbr1 mtu 9216 2>/dev/null || true
```

> 참고: MTU는 경로 상 모든 장비(NIC, DPU, 스위치)가 동일해야 함.
> 
> 
> `ping -M do -s 8972 <상대IP>`로 점보 프레임 테스트 가능.
> 

---

## 문제 2: OVS 경로 및 HW 오프로딩 비활성화

### 원인

- **DTN4-dpu**: OVS 브리지(`ovsbr1`)가 `datapath_type=netdev`로 설정됨 → 유저스페이스 패킷 처리 → 매우 느림.
- **DTN2-dpu**: datapath는 `system`(커널)이지만, HW 오프로딩(`hw-offload`) 비활성화 상태.

### 해결 과정

### 1) DPU NIC을 switchdev 모드로 확인/설정

```bash
# 현재 DPU 내부에서 NIC 모드 확인
sudo devlink dev eswitch show pci/0000:03:00.0

# 출력이 'mode switchdev'여야 함.
# 만약 legacy라면 아래 명령으로 변경
sudo devlink dev eswitch set pci/0000:03:00.0 mode switchdev
```

### 2) OVS에서 하드웨어 오프로딩 활성화

```bash
# HW 오프로딩을 활성화
sudo ovs-vsctl set Open_vSwitch . other_config:hw-offload=true

# OVS가 flow를 TC로 내릴 때 정책 지정 (none = 제약 없음)
sudo ovs-vsctl set Open_vSwitch . other_config:tc-policy=none
```

### 3) DTN4-dpu에서 datapath를 system으로 변경

```bash
# netdev → system 커널 datapath로 변경
sudo ovs-vsctl set Bridge ovsbr1 datapath_type=system

# OVS 서비스 재시작
sudo systemctl restart openvswitch-switch \
  2>/dev/null || sudo service openvswitch-switch restart
```

### 4) NIC 하드웨어 오프로딩/큐 확장 (선택)

```bash
# HW TC 오프로딩 기능 활성화
sudo ethtool -K p0 hw-tc-offload on

# NIC 큐를 확장 (멀티코어 활용 → 단일 스트림 성능 향상)
sudo ethtool -L p0 combined 64
```

---

## 검증 단계

### 1) 점보 프레임 통과 확인

```bash
# 8972 Byte ICMP ping (Ethernet + IP + ICMP 헤더 포함 시 MTU 9216 근처)
ping -M do -s 8972 <상대방 IP>
```

### 2) OVS datapath 확인

```bash
# 현재 datapath 확인 (system만 보여야 함)
sudo ovs-appctl dpctl/show
```

### 3) 오프로드 상태 확인

```bash
# offloaded flows > 0 이 나오고, offloaded packets/bytes가 증가해야 정상
sudo ovs-appctl dpif/show
```

### 4) 성능 측정

```bash
# 단일 스트림 성능
iperf3 -c <상대IP> -t 10

# 다중 스트림 성능 (멀티큐 활용)
iperf3 -c <상대IP> -t 15 -P 8
iperf3 -c <상대IP> -t 15 -P 16

# 역방향 성능 테스트
iperf3 -c <상대IP> -t 15 -P 8 -R
```

---

## 최종 결과

- **문제 1 (MTU mismatch):** DPU 포트 및 OVS 인터페이스를 9216으로 맞춰 해결.
- **문제 2 (OVS 경로/HW 오프로드):**
    - DTN4-dpu: `netdev` → `system` 전환 및 오프로딩 활성화.
    - DTN2-dpu: 오프로딩 활성화.
- **성과:**
    - 점보 프레임 정상 통과.
    - `offloaded flows` 활성화 확인.
    - 다중 스트림(`P 8/16`) 기준 수십 Gbps 성능 확보.