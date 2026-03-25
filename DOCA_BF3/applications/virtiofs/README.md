# DOCA DevEmu Virtio-fs 실질적 동작 가이드

## 1. 전체 아키텍처 개요

```
┌─────────────────────────────────────────────────────────────────────┐
│                           Host (x86)                                │
│  ┌─────────────────────────���───────────────────────────────────┐   │
│  │          Linux Kernel (virtio-fs driver)                     │   │
│  │   - FUSE 요청 생성 (read, write, lookup, etc.)              │   │
│  │   - virtio 큐를 통해 BlueField로 전달                        │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  │ PCIe (Emulated virtio-fs device)
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                     BlueField-3 DPU (ARM)                           │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │              DOCA DevEmu Virtio-fs Application               │   │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │   │
│  │  │ VFS Device  │  │  VFS IO     │  │   NFS FSdev         │  │   │
│  │  │ (Control)   │  │  (Data Path)│  │   (Backend)         │  │   │
│  │  └─────────────┘  └─────────────┘  └─────────────────────┘  │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                  │                                  │
│                                  │ NFS Protocol                     │
│                                  ▼                                  │
│                    ┌─────────────────────────┐                     │
│                    │    NFS Server           │                     │
│                    │ (외부 파일 시스템)       │                     │
│                    └─────────────────────────┘                     │
└─────────────────────────────────────────────────────────────────────┘

```

---

## 2. 사전 환경 설정 (Prerequisites)

### 2.1 하드웨어/소프트웨어 요구사항

- **BlueField-3 DPU**
- **DOCA 2.7. 0** 이상
- **BlueField-3 펌웨어 32. 41.1000** 이상
- **DPU 모드**로 설정 필요

### 2.2 Firmware 설정 (mlxconfig)

```bash
# 1. Virtio-fs 에뮬레이션 활성화
sudo mlxconfig -d /dev/mst/mt41692_pciconf0 s VIRTIO_FS_EMULATION_ENABLE=1

# 2. Virtio-fs PF 개수와 MSIX 설정
sudo mlxconfig -d /dev/mst/mt41692_pciconf0 s \\
    VIRTIO_FS_EMULATION_NUM_PF=2 \\
    VIRTIO_FS_EMULATION_NUM_MSIX=18

# 3. (선택) Hot-plug 지원 시 PCIe 스위치 에뮬레이션 활성화
sudo mlxconfig -d /dev/mst/mt41692_pciconf0 s \\
    PCI_SWITCH_EMULATION_ENABLE=1 \\
    PCI_SWITCH_EMULATION_NUM_PORT=2

# 4. 설정 적용을 위한 시스템 재부팅
sudo reboot

```

### 2.3 Host 설정 (Hot-plug 사용 시)

- 안 하는게 추천

```bash
# /etc/default/grub 에 커널 파라미터 추가
# Intel:
intel_iommu=on iommu=pt pci=realloc

# AMD:
iommu=pt pci=realloc

```

---

## 3. 코드 구조 분석

### 3.1 주요 파일별 역할

| 파일 | 역할 |
| --- | --- |
| `virtiofs. c` | **Main 진입점**, 인자 파싱, 시그널 핸들링 |
| `virtiofs_core.c` | **핵심 초기화 로직**, 리소스 생성/시작/종료 |
| `virtiofs_device.c` | **가상 디바이스 관리**, vfs_dev 생성, FUSE 핸들러 등록 |
| `virtiofs_request.c` | **FUSE 요청 처리**, DMA 전송, 백엔드 연결 |
| `virtiofs_thread.c` | **멀티스레드 처리**, Progress Engine 관리 |
| `virtiofs_manager.c` | **디바이스 매니저**, representor 관리 |
| `virtiofs_mpool.c` | **메모리 풀**, DMA 버퍼 관리 |
| `nfs_fsdev/` | **NFS 백엔드**, 실제 파일 시스템 연결 |

---

## 4. 실행 흐름 상세 분석

### 4.1 초기화 단계

```c
// main() in virtiofs.c

// 1. 설정 초기화
struct virtiofs_cfg app_cfg = {
    .core_mask = "0x1",           // 사용할 CPU 코어
    .nfs_server = "localhost",    // NFS 서버 주소
    .nfs_export = "/VIRTUAL"      // NFS 마운트 경로
};

// 2. 인자 파싱 (DOCA ARGP)
doca_argp_init(NULL, &app_cfg);
register_virtiofs_params();  // -m, -s, -e 옵션 등록
doca_argp_start(argc, argv);

// 3. 리소스 생성
ctx = virtiofs_create((uint32_t)strtol(app_cfg.core_mask, NULL, 16));

// 4. 시작
virtiofs_start(ctx);

// 5. 정적 디바이스 생성
virtiofs_device_create_static(ctx, app_cfg.nfs_server, app_cfg.nfs_export);

```

### 4.2 리소스 생성 상세 (`virtiofs_create`)

```c
// virtiofs_core.c

struct virtiofs_resources *virtiofs_create(uint32_t core_mask) {
    // 1. DOCA DevEmu VFS 라이브러리 초기화
    virtiofs_devemu_vfs_init();

    // 2. VFS 지원 DOCA 디바이스 탐색
    doca_devinfo_create_list(&dev_list, &nb_devs);
    for (i = 0; i < nb_devs; i++) {
        doca_devemu_vfs_is_default_vfs_type_supported(dev_list[i], &is_supported);
        if (is_supported) {
            doca_dev_open(dev_list[i], &devs[num_devs]);
            num_devs++;
        }
    }

    // 3. 매니저 생성 (각 DOCA 디바이스당 하나)
    virtiofs_managers_create(ctx);

    // 4. 스레드 컨텍스트 초기화 (코어마스크 기반)
    for (i = 0; i < num_cores; i++) {
        virtiofs_thread_ctx_init(ctx, core_id, admin_thread, &ctx->threads[i]);
    }
}

```

### 4.3 디바이스 생성 및 시작

```c
// virtiofs_core.c - virtiofs_device_create_static()

// 1. NFS 백엔드 생성
virtiofs_nfs_fsdev_create(ctx, "nfs_fsdev", nfs_server, nfs_export);

// 2. 디바이스 설정
static struct virtiofs_device_config config = {
    . name = "vfs_controller0",
    .num_request_queues = 32,    // 32개의 요청 큐
    .queue_size = 256,           // 큐당 256개 엔트리
    .tag = "docavirtiofs",       // Host에서 보이는 태그
};

// 3. 사용 가능한 함수(PF) 찾기
virtiofs_function_get_available(ctx, &func);

// 4. 디바이스 생성
virtiofs_device_create(ctx, &config, "mlx5_0", func->vuid, "nfs_fsdev");

// 5. 디바이스 시작
virtiofs_device_start(ctx, "vfs_controller0", NULL, NULL);

```

### 4.4 FUSE 요청 처리 흐름

```
Host에서 파일 접근 요청
        │
        ▼
┌─────────────────────────────────────────────────────────────┐
│ 1. virtio-fs 드라이버가 FUSE 요청을 virtio 큐에 삽입       │
└─────────────────────────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────────────────────────┐
│ 2.  DOCA DevEmu가 요청 감지 → 등록된 핸들러 호출             │
│    예: virtiofs_request_read_handler()                      │
└─────────────────────────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────────────────────────┐
│ 3. virtiofs_process_request() 호출                          │
│    - dreq 구조체에 요청 정보 저장                           │
│    - virtiofs_request_handle() 호출                         │
└─────────────────────────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────────────────────────┐
│ 4. DMA를 통해 Host 메모리에서 데이터 읽기 (datain)          │
│    - virtiofs_process_datain()                              │
│    - virtiofs_dma_task_submit()                             │
└─────────────────────────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────────────────────────┐
│ 5. NFS 백엔드로 요청 전달                                    │
│    - fsdev->ops->submit()                                   │
│    - NFS 서버와 통신                                        │
└─────────────────────────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────────────────────────┐
│ 6. NFS 응답 수신 후 DMA로 Host에 결과 전송 (dataout)        │
│    - virtiofs_request_dma_to_host_progress()                │
└─────────────────────────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────────────────────────┐
│ 7. 요청 완료                                                 │
│    - virtiofs_req_complete()                                │
│    - doca_devemu_vfs_fuse_req_complete()                    │
└─────────────────────────────────────────────────────────────┘

```

---

## 5. 실행 방법

### 5.1 NFS 서버 준비 (외부 또는 로컬)

```bash
# NFS 서버에서
sudo mkdir -p /VIRTUAL
sudo chmod 777 /VIRTUAL
echo "/VIRTUAL *(rw,sync,no_subtree_check)" | sudo tee -a /etc/exports
sudo exportfs -ra
sudo systemctl restart nfs-kernel-server

```

### 5.2 애플리케이션 빌드

```bash
# BlueField DPU에서
cd applications/virtiofs
meson build
cd build
ninja

```

### 5.3 애플리케이션 실행

```bash
# 기본 실행 (root 권한 필요)
sudo ./doca_virtiofs

# 옵션 지정
sudo ./doca_virtiofs \\
    -m 0x3 \\                    # 코어 0, 1 사용
    -s 192.168.1.100 \\          # NFS 서버 IP
    -e /nfs/share               # NFS 익스포트 경로

# 테스트 모드 (NFS 없이)
export DOCA_VIRTIOFS_USE_NULL_NFS_FSDEV=1
sudo ./doca_virtiofs

```

### 5.4 Host에서 마운트

```bash
# Host Linux에서
sudo modprobe virtiofs

# 디바이스 확인
lspci | grep -i virtio

# 마운트
sudo mount -t virtiofs docavirtiofs /mnt/virtiofs

# 파일 시스템 사용
ls /mnt/virtiofs
echo "test" > /mnt/virtiofs/testfile
cat /mnt/virtiofs/testfile

```

---

## 6. 지원되는 FUSE 명령어

코드에서 등록된 FUSE 핸들러들:

| 명령어 | 설명 |
| --- | --- |
| INIT | 초기화 |
| DESTROY | 종료 |
| LOOKUP | 파일/디렉토리 조회 |
| CREATE | 파일 생성 |
| OPEN/RELEASE | 파일 열기/닫기 |
| READ/WRITE | 읽기/쓰기 |
| GETATTR/SETATTR | 속성 조회/설정 |
| READDIR/READDIRPLUS | 디렉토리 읽기 |
| MKDIR/RMDIR | 디렉토리 생성/삭제 |
| UNLINK | 파일 삭제 |
| RENAME/RENAME2 | 이름 변경 |
| SYMLINK/LINK/READLINK | 심볼릭/하드 링크 |
| FSYNC | 동기화 |
| STATFS | 파일시스템 정보 |
| GETXATTR/SETXATTR/LISTXATTR/REMOVEXATTR | 확장 속성 |
| MKNOD | 특수 파일 생성 |
| IOCTL | ioctl 명령 |
| FLUSH | 버퍼 플러시 |
| FORGET | 캐시 해제 |
| FALLOCATE | 공간 할당 |
| SYNCFS | 파일시스템 동기화 |

---

## 7. 핵심 데이터 구조

```c
// 전체 리소스 관리
struct virtiofs_resources {
    SLIST_HEAD(, virtiofs_manager) managers;  // 디바이스 매니저 리스트
    SLIST_HEAD(, virtiofs_device) devices;    // 가상 디바이스 리스트
    SLIST_HEAD(, virtiofs_fsdev) fsdevs;      // 백엔드 파일시스템 리스트
    int num_threads;
    struct virtiofs_thread_ctx threads[];     // 워커 스레드들
};

// 개별 요청 처리
struct virtiofs_request {
    struct virtiofs_device_io_ctx *dio;       // IO 컨텍스트
    struct doca_devemu_vfs_fuse_req *devemu_fuse_req;  // DOCA 요청
    struct virtiofs_io_req_data datain;       // 입력 데이터
    struct virtiofs_io_req_data dataout;      // 출력 데이터
    struct doca_task *task;                   // DMA 태스크
    vfs_doca_fsdev_io_cb cb;                  // 완료 콜백
    // ...
};

```

---

## 8. 디버깅 및 모니터링

```bash
# 로그 레벨 설정
export DOCA_LOG_LEVEL=DEBUG

# 실행 중 상태 확인
dmesg | grep -i virtio

# NFS 연결 확인
showmount -e <nfs_server_ip>

```

---

## 요약

이 애플리케이션은 **BlueField-3 DPU의 디바이스 에뮬레이션 기능**을 사용하여:

1. **Host에 가상 virtio-fs 디바이스를 노출**
2. **Host의 FUSE 요청��� DMA로 수신**
3. **NFS 백엔드를 통해 실제 파일 시스템과 연동**
4. **결과를 다시 DMA로 Host에 전송**

이를 통해 Host는 마치 로컬 파일 시스템처럼 원격 NFS 스토리지를 사용할 수 있으며, DPU가 모든 파일 시스템 처리를 오프로드