# DOCA 개발 플로우

## 1. 개발 권장 구조
![Dev-Env](https://docscontent.nvidia.com/dims4/default/777ec3d/2147483647/strip/true/crop/610x340+0+0/resize/610x340!/format/webp/quality/90/?url=https%3A%2F%2Fk3-prod-nvidia-docs.s3.us-west-2.amazonaws.com%2Fbrightspot%2Fconfluence%2F00000198-0f6e-d020-a1fb-2f7f6c690000%2Fdoca%2Farchive%2Fdoca-300-vgt-update%2Fimages%2Fdownload%2Fattachments%2F3796407520%2Fdeveloping-using-bluefield-setup-version-1-modificationdate-1744314577643-api-v2.png)
- **Host IDE**: 코드 수정
- **BlueField `doca:devel` 컨테이너**: 빌드 + 실행
- 동일한 소스 폴더를 Host ↔ BlueField ↔ 컨테이너가 공유 (NFS/SSHFS 등)
- 실행 시 컨테이너가 하드웨어 리소스(SF/VF, hugepages)에 직접 접근

---

## 2. 컨테이너 이미지 종류

- **base-rt**: 최소 런타임, 최종 배포용 경량 이미지
- **full-rt**: base-rt 확장, 모든 DOCA 런타임 컴포넌트 포함
- **devel**: full-rt + 개발 툴체인/헤더, 개발·컴파일용

**원칙**

- 개발: `doca:devel`
- 배포: `base-rt` Or `full-rt`

---

## 3. 멀티스테이지 빌드

1. **개발 단계**: `doca:devel`에서 소프트웨어 컴파일
2. **배포 단계**: 빌드 결과물을 `base-rt` Or `full-rt`로 최종 이미지 생성

장점:

- 개발 환경과 배포 환경 분리
- 운영 이미지는 경량/표준화
- 버전 재현성 보장


### HOST
```bash
docker pull nvcr.io/nvidia/doca/doca:3.2.0-devel-ubuntu24.04-host
```
- host 전용 이미지 

### DPU
```bash
docker pull nvcr.io/nvidia/doca/doca:3.2.0-devel-ubuntu24.04
```
- dpu 전용 이미지

---

## **4. 파일 공유 방식**

### NFS

**Host**

```bash
sudo apt-get update && sudo apt-get install -y nfs-kernel-server
echo "/home/netai-sys/DOCA 192.168.100.0/24(rw,sync,no_subtree_check,no_root_squash)" | sudo tee -a /etc/exports
sudo exportfs -ra
sudo systemctl restart nfs-kernel-server
```

- Host에서 NFS 서버 실행

**DPU**

```bash
sudo apt-get update && sudo apt-get install -y nfs-common
sudo mkdir -p /mnt/src
# 임시 마운트 
# sudo mount -t nfs 192.168.100.1:/home/netai-sys/DOCA /mnt/src

# 영구 마운트(선택): /etc/fstab 에 추가
echo "192.168.100.1:/home/netai-sys/DOCA /mnt/src nfs defaults,_netdev 0 0" | sudo tee -a /etc/fstab
# 자동 마운트
sudo mount -a

```

- BlueField에서 마운트 후 컨테이너에 마운트

---

## 5. 컨테이너 실행

```bash
sudo docker run \
  -v /mnt/src:/doca_devel \
  -v /dev/hugepages:/dev/hugepages \
  --privileged --net=host \
  -it nvcr.io/nvidia/doca/doca:<버전>-devel

```

- `-net=host`: 네트워크 인터페이스(SF/VF) 접근
- `-privileged`: 하드웨어 접근 권한
- `v /dev/hugepages`: HugePage 리소스 공유

---

## 6. 개발/실행 루틴

컨테이너 내부에서:

```
cd /doca_devel
meson /tmp/build
ninja -C /tmp/build
```

---

## (중요) 버전 이슈

- DPU OS(BFB) 이미지 3.9.0 이상 필요 (컨테이너 배포 가이드 전제)
- `dpkg -l | grep doca` 또는 `/opt/mellanox/doca/include/doca_version.h`로 DOCA 버전 확인
- 반드시 **컨테이너 버전(devel 이미지)** 과 동일하게 맞출 것
- **[공식이미지](https://catalog.ngc.nvidia.com/orgs/nvidia/teams/doca/containers/doca/tags)**
