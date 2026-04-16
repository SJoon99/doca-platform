# 실행

### 컨테이너
```bash
sudo docker run   -v /mnt/src:/doca   -v /dev/hugepages:/dev/hugepages   --privileged --net=host -it nvcr.io/nvidia/doca/doca:2.9.3-devel
```

### DOCA Program (Container)
```bash
./dpu_transfer -l 0-3 -n 2     -a auxiliary:mlx5_core.sf.6,dv_flow_en=2 -- -l 50
```

## 재조립 과정

### **1) IP 조각화 재조립**

- **상황:** 큰 IP 패킷이 네트워크 MTU보다 커서 여러 fragment로 나눠져 전송됨.
- **과정:**
    - 각 fragment가 mbuf로 수신됨.
    - `is_ipv4_fragmented()`로 조각화 여부 확인.
    - 조각화된 경우, `ip_reassemble()` 함수가 fragment들을 모아하나의 완전한 IP 패킷(mbuf)로 재조립.
    - 재조립이 완료되면, 이후 단계로 전달.

---

### **2) TCP 세그먼트 병합 (GRO)**

- **상황:** 여러 개의 작은 TCP 세그먼트가 순서대로 수신됨.
- **과정:**
    - 여러 mbuf를 GRO 컨텍스트(`gro_ctx`)에 입력.
    - `rte_gro_reassemble()` 또는 `rte_gro_timeout_flush()` 함수가합칠 수 있는 TCP 세그먼트를 하나의 큰 mbuf로 병합.
    - 병합된 mbuf는 페이로드가 커진 상태로 후속 처리.

---

### **3) TCP 스트림 재조립 (순서 보장, OOO 처리)**

- **상황:** TCP 세그먼트가 순서대로 오지 않거나, 일부가 지연되어 도착함(OOO).
- **과정:**
    - mbuf에서 TCP 헤더와 페이로드, 시퀀스 번호 추출(`extract_tcp_segment()`).
    - 각 TCP 흐름별로 `expected_seq`(다음에 와야 할 시퀀스 번호) 관리.
    - 시퀀스 번호가 예상과 같으면 바로 청크 버퍼에 추가(`append_bytes_to_chunk()`).
    - 시퀀스 번호가 다르면(OOO) `queue_ooo_segment()`로 임시 저장.
    - 앞부분이 도착하면 `drain_ooo_segments()`가 OOO 큐에서 순서대로 꺼내청크 버퍼에 추가, 스트림을 올바른 순서로 복원.

---

**정리:**

1. IP 계층에서 조각화된 패킷을 먼저 재조립
2. TCP 계층에서 GRO로 여러 세그먼트를 병합
3. TCP 스트림 레벨에서 시퀀스 번호 기반으로 순서 재조립 및 OOO 처리이 모든 과정이 패킷 수신 루프에서 반복적으로 수행되어 최종적으로 올바른 데이터 스트림생성