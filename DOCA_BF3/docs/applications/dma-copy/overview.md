## **Host-DPU DMA 파일 전송 시스템 - 전체 로직 정리**

### **시스템 개요**

양방향 파일 전송을 지원하는 Host-DPU 간 DMA 통신 시스템. 파일이 있는 쪽에서 없는 쪽으로 고성능 DMA를 통해 전송.

### **Phase 1: 초기화 및 협상**

**Host 시작 과정**

```c
1. 파일 존재 여부 확인 → is_file_found_locally 설정
2. DMA 디바이스 열기
3. mmap 생성 및 디바이스 추가
4. 파일이 로컬에 있으면 → 버퍼 할당 후 파일 내용 로드
```

**방향 협상 (Host → DPU)**

```c
Host 송신: "파일 위치와 크기 정보"
- file_in_host: Host에 파일 있는지
- file_size: 파일 크기 (있을 경우)
```

**DPU 응답 (DPU → Host)**

```c
DPU 검증 후 응답:
- 양쪽 모두 파일 있음/없음 → 에러
- 한쪽에만 있음 → 전송 방향 확정
- DPU에 파일 있으면 크기 정보 전달
```

### **Phase 2: 메모리 준비**

**Host 메모리 준비**

```c
if (파일이 Host에 없음) {
    // Host가 수신자: DPU가 쓸 버퍼 준비
    file_size = DPU가_알려준_크기;
    memory_alloc_and_populate(mmap, size, READ_WRITE, &buffer);
}
// 파일이 Host에 있으면 이미 Phase 1에서 준비 완료
```

**mmap Export (Host → DPU)**

```c
Host 송신: mmap 디스크립터 패키지
- host_addr: Host 버퍼 물리 주소
- exported_mmap: DMA 접근 권한 정보
- export_desc_len: 디스크립터 크기
```

### **Phase 3: DPU DMA 실행**

**DPU 준비 과정**

```c
1. DMA 리소스 할당 (디바이스, 컨텍스트, PE)
2. 로컬 버퍼 할당
3. Host export 디스크립터로 원격 mmap 생성
4. 로컬/원격 DOCA 버퍼 생성
```

**전송 방향 결정**

```c
if (파일이_DPU에_있음) {
    // DPU → Host
    src_buf = local_doca_buf;   // DPU 메모리
    dst_buf = remote_doca_buf;  // Host 메모리
    fill_buffer_with_file_content(); // 파일 로드
} else {
    // Host → DPU
    src_buf = remote_doca_buf;  // Host 메모리
    dst_buf = local_doca_buf;   // DPU 메모리
}
```

**DMA 작업 실행**

```c
1. DMA memcpy 태스크 생성
2. src_buf → dst_buf 설정
3. 진행 엔진에 태스크 제출
4. 비동기 실행 + 폴링으로 완료 대기
5. 완료 시 콜백으로 결과 처리
```

### **Phase 4: 완료 처리**

**파일 저장 (필요시)**

```c
if (DPU가_수신자였음) {
    save_buffer_into_a_file(buffer, file_path);
}
```

**상태 메시지 교환**

```c
DPU → Host: 성공/실패 상태
Host: 최종 결과 확인 후 종료
```

## **핵심 설계 특징**

**양방향 지원**

- 단일 코드베이스로 Host↔DPU 양방향 전송
- `is_file_found_locally` 플래그로 역할 분담

**메모리 권한 관리**

```c
송신자: READ_ONLY 권한 → "읽어만 가"
수신자: READ_WRITE 권한 → "여기에 써도 돼"
```

**비동기 처리**

- DMA 작업은 비동기 실행
- 콜백으로 완료/에러 처리
- Progress Engine 폴링으로 진행 상태 확인

**에러 처리**

- 각 단계별 상세한 에러 검증
- 실패 시 리소스 정리 및 에러 메시지 전파