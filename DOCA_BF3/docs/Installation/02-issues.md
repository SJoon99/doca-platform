# ISSUE 1

## 1. 문제 현상

- `systemctl status rshim` 또는 `journalctl -u rshim`에 **`rshim0 fall-back to uio`** 로그가 반복.
- `bfb-install --rshim rshim0 --bfb ...` 실행 시 전송/부팅 단계에서 timeout 또는 진행 중단.
- 동일 호스트의 다른 DPU는 VFIO로 정상 동작.

## 2. 원인 분석

- RShim은 **VFIO → UIO → Direct Map** 순서로 프로브하며, VFIO 경로가 성립하지 않으면 UIO로 폴백.
- **VFIO 경로 실패 원인**:
    1. **같은 IOMMU 그룹 내 함수 충돌**: `01:00.0`(NIC)과 `01:00.1`(SoC Mgmt)이 같은 그룹에 있어 한 쪽이 커널 드라이버에 붙어 있으면 그룹 전체 VFIO 할당이 불가.
    2. **드라이버 바인딩 상태 문제**: `mlx5_core`, `uio_pci_generic` 등이 점유 중.
    3. rshim 설정(`PCIE_HAS_VFIO`, `PCIE_HAS_UIO`) 또는 커널 보안 정책(lockdown) 영향.

## 3. 해결 방법

1. **rshim 중지**
    
    ```bash
    sudo systemctl stop rshim
    
    ```
    
2. **IOMMU 그룹 확인**
    
    ```bash
    readlink /sys/bus/pci/devices/0000:01:00.1/iommu_group
    ls -1 /sys/kernel/iommu_groups/<그룹번호>/devices
    
    ```
    
3. **같은 그룹의 함수 모두 VFIO 바인딩**
    
    ```bash
    echo vfio-pci | sudo tee /sys/bus/pci/devices/0000:01:00.0/driver_override
    echo vfio-pci | sudo tee /sys/bus/pci/devices/0000:01:00.1/driver_override
    sudo modprobe vfio-pci
    echo 0000:01:00.0 | sudo tee /sys/bus/pci/drivers_probe
    echo 0000:01:00.1 | sudo tee /sys/bus/pci/drivers_probe
    
    ```
    
4. **rshim 재시작 후 VFIO 연결 확인**
    
    ```bash
    sudo systemctl start rshim
    journalctl -u rshim -b --no-pager | tail -n 50
    
    ```
    
5. (옵션) `/etc/rshim.conf`에서 `PCIE_HAS_UIO=0` 설정하여 VFIO 강제.


# ISSUE 2

## 1. 문제 현상
- `bfb-install`로 `.bfb` 이미지를 전송하면 RShim 로그에서 부트 업데이트는 성공 메시지가 출력됨.
- 그러나 UEFI 단계에서 `Linux from rshim` 부팅 시도 후 **`Failed to boot 'Linux from rshim'`** 메시지가 발생.
- 이후 기존 eMMC/SSD의 Ubuntu 20.04로 롤백 부팅.

## 2. 원인 분석
- BlueField-2 E-Series 장비에서 **UEFI Secure Boot가 활성화(`SecureBoot enabled`)** 되어 있었음.
- BFB 이미지의 커널 서명 키가 현재 UEFI DB에 등록된 키와 달라 Secure Boot 검증에 실패. (Debian 운영체제는 이 방법밖에 없음)
- UEFI가 부팅을 차단하고 기존 OS로 넘어감.

## 3. 해결 방법
1. **UEFI 메뉴 진입**
   - 물리 UART 콘솔 또는 BMC SOL(Serial over LAN)을 사용하여 UEFI 접근.
   - 기본 UEFI 비밀번호는 `bluefield` 
2. **Secure Boot 비활성화**
   - UEFI → `Secure Boot Configuration` 메뉴에서 `Attempt Secure Boot` 체크 해제.
   - `Current Secure Boot State`는 읽기 전용이므로 직접 수정 불가.
3. **저장 후 재부팅**
   - UEFI 설정 저장 후 BlueField 재부팅.
   - OS에서 `mokutil --sb-state`로 Secure Boot 비활성 확인.
4. **BFB 재설치**
   ```bash
   sudo bfb-install --rshim rshim0 --bfb bf-bundle-3.0.0-135_25.04_ubuntu-22.04_prod.bfb

   
