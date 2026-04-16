---
title: "Phase 1 — NFS 서버 설정 + node4 + DPU 마운트"
status: "✅ 완료 (2026-04-10)"
---

# Phase 1: NFS 서버 (tempnode-bf3) + node4 + DPU 마운트

## 목적

tempnode-bf3를 NFS 서버로 설정하고, node4와 BF3 DPU에서 동일 경로로 마운트.
VSCode 편집 = tempnode-bf3 파일시스템에 직접 기록 (동기화 데몬 불필요).

```
node4 (10.34.20.3, VSCode 편집)
  /home/joon/doca-platform/DOCA_BF3/projects/  ← NFS 마운트
    │
    │  NFS (10.34.20.4)
    ▼
tempnode-bf3 (10.34.20.4)  ← NFS 서버 (단일 원본)
  /home/joon/doca-platform/DOCA_BF3/projects/
    │
    │  NFS (10.34.20.4)
    ▼
BF3 DPU (10.34.20.99)
  /home/joon/doca-platform/DOCA_BF3/projects/  ← 동일 파일 접근
```

---

## 실행 순서

### Step 1. NFS 서버 설정 (tempnode-bf3에서)

```bash
ssh joon@10.34.20.4 'bash -s' < DOCA_BF3/scripts/phase1-nfs-server-setup.sh
```

완료 후 확인:
```bash
ssh joon@10.34.20.4 'sudo exportfs -v'
# /home/joon/doca-platform/DOCA_BF3/projects  10.34.20.3(...) 출력 확인
```

### Step 2. NFS 클라이언트 마운트 (node4에서)

```bash
ssh joon@10.34.20.3 'bash -s' < DOCA_BF3/scripts/phase1-nfs-client-node4.sh
```

스크립트 동작:
1. `nfs-common` 설치
2. `showmount -e 10.34.20.4` 로 서버 연결 확인
3. 기존 로컬 파일 → tempnode-bf3으로 rsync (데이터 보존)
4. `mount -t nfs` 마운트
5. `/etc/fstab` 영구 등록

### Step 3. NFS 클라이언트 마운트 (BF3 DPU에서)

```bash
# tempnode-bf3 경유 접속
ssh joon@10.34.20.4 "ssh -o StrictHostKeyChecking=no ubuntu@10.34.20.99 \
  'sudo apt-get install -y nfs-common && \
   sudo mkdir -p /home/joon/doca-platform/DOCA_BF3/projects && \
   sudo mount -t nfs 10.34.20.4:/home/joon/doca-platform/DOCA_BF3/projects \
     /home/joon/doca-platform/DOCA_BF3/projects && \
   echo \"10.34.20.4:/home/joon/doca-platform/DOCA_BF3/projects \
     /home/joon/doca-platform/DOCA_BF3/projects nfs defaults,_netdev 0 0\" \
     | sudo tee -a /etc/fstab'"
```

---

## 검증

```bash
# node4에서 파일 생성
ssh joon@10.34.20.3 "echo test > /home/joon/doca-platform/DOCA_BF3/projects/nfs-test.txt"

# tempnode-bf3에서 확인
ssh joon@10.34.20.4 "cat /home/joon/doca-platform/DOCA_BF3/projects/nfs-test.txt"
# → "test" 출력이면 성공

# DPU에서 확인
ssh joon@10.34.20.4 "ssh ubuntu@10.34.20.99 'ls /home/joon/doca-platform/DOCA_BF3/projects/'"
# → dpu/ host/ flow_common.c flow_common.h meson.build 출력이면 성공

# 정리
ssh joon@10.34.20.3 "rm /home/joon/doca-platform/DOCA_BF3/projects/nfs-test.txt"
```

---

## 트러블슈팅

| 증상 | 원인 | 해결 |
|------|------|------|
| `showmount` 연결 실패 | NFS 서버 미실행 | `ssh joon@10.34.20.4 'sudo systemctl status nfs-kernel-server'` |
| 마운트 후 권한 오류 | `root_squash` 설정 | `/etc/exports`에 `no_root_squash` 확인 |
| 재부팅 후 마운트 해제 | fstab 미등록 | `/etc/fstab`에 `_netdev` 옵션으로 등록 확인 |
| DPU SSH 불가 | 키 미설정 | tempnode-bf3 경유 (`ssh joon@10.34.20.4 "ssh ubuntu@10.34.20.99 ..."`) |

---

## 다음 단계

→ Phase 2 — doca-dev DPUService 배포 (DPU pod에서 /doca_devel 마운트)
