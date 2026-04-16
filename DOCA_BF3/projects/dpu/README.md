# 실행

### 컨테이너
```bash
sudo docker run   -v /mnt/src:/doca   -v /dev/hugepages:/dev/hugepages   --privileged --net=host -it nvcr.io/nvidia/doca/doca:2.9.3-devel
```

### DOCA Program (Container)
```bash
./dpu_transfer -l 0-3 -n 2     -a auxiliary:mlx5_core.sf.2,dv_flow_en=2 -- -l 50
```

## 코드 수정 방향 

### 부하가 커지는 지점

1. **메모리 복사(청킹)**
    - 모든 페이로드를 `append_bytes_to_chunk()`로 64KB 버퍼에 **memcpy**
    - 10G만 잡아도 초당 1.25 GB를 계속 복사하는 셈이라, 실제 유효 대역은 **메모리 대역폭/캐시 친화도**에 바로 묶임. (GRO로 호출 횟수는 줄지만 **바이트 복사량은 그대로**)
2. **SW CRC32C**
    - 현재 구현은 **비트 단위 SW CRC**(폴리 0x82F63B78). 매우 느림
        - 향후 Hash 가속 코드 구현
    - ARMv8/Neoverse의 **CRC32**/PMULL, x86의 **SSE4.2 CRC32**로 바꿔야 라인레이트에 근접
3. **OOO 큐 관리**
    - OOO 세그먼트마다 **malloc/free**로 동적 할당, 리스트 스캔·병합.
    - 고패킷률에서 **힙 할당 + 캐시 미스**가 큼 (게다가 `MAX_OOO_SEGMENTS`=64로 꽉 차면 신규 OOO 드랍)
4. **흐름 테이블 탐색**
    - `g_flows` 선형 탐색(O(N), N≤512). 수천 플로우만 돼도 **캐시 미스 + 분기 실패**가 증가.
    - 타임아웃 정리도 선형 스캔.
5. **소프트웨어 GRO + IP 프래그 재조립**
    - 둘 다 CPU 사이클을 꽤 씁니다. DC/컨테이너 환경에서 **IPv4 프래그먼트는 드뭅니다**—필요 없다면 과감히 **드랍**하는 편이 성능에 유리
6. **로깅**
    - 청크마다 **메타 1줄 + HEX 1줄**, 프래그 재조립 성공 시 1줄.
    - 초당 수천 청크/패킷이면 로그 자체가 병목(락·syscall·IO)
7. **폴링 모델**
    - 메인 루프에 `usleep(10000)`이 들어있어 **10ms마다 한 번**만 RX 처리
    - DPDK는 보통 **busy-poll**로 코어를 바인딩해 지연·드랍을 최소화

> 튜닝 없이 그대로면 “수백 Mbps ~ 저수 Gbps” 수준에서 안정 동작, 그 이상은 **복사/CRC/할당/로그**가 발목을 잡습니다. 25G/100G는 어려움
