#!/usr/bin/env bash
# Phase 1B: node4 NFS 클라이언트 마운트
#
# tempnode-bf3의 NFS export를 node4의 동일 경로에 마운트.
# VSCode가 로컬 경로를 편집하면 tempnode-bf3에 직접 기록됨.
#
# 주의: 마운트 전 로컬 파일을 tempnode-bf3으로 rsync하여 데이터 보존.
#
# 사용법:
#   ssh joon@10.34.20.8 'bash -s' < DOCA_BF3/scripts/phase1-nfs-client-node4.sh
#   또는
#   bash DOCA_BF3/scripts/phase1-nfs-client-node4.sh  (node4 로컬에서)

set -euo pipefail

NFS_SERVER="10.34.20.4"
REMOTE_PATH="/home/joon/doca-platform/DOCA_BF3/projects"
LOCAL_MOUNT="${REMOTE_PATH}"

echo "=== Phase 1B: node4 NFS 클라이언트 마운트 ==="
echo "  NFS server: ${NFS_SERVER}:${REMOTE_PATH}"
echo "  Mount point: ${LOCAL_MOUNT}"
echo ""

# 1. nfs-common 설치
echo "[1/5] nfs-common 설치..."
sudo apt-get install -y nfs-common

# 2. NFS 서버 연결 확인
echo "[2/5] NFS 서버 export 확인..."
showmount -e "${NFS_SERVER}" || {
  echo "[ERROR] NFS 서버 ${NFS_SERVER}에 연결할 수 없습니다."
  echo "  먼저 phase1-nfs-server-setup.sh를 tempnode-bf3에서 실행하세요."
  exit 1
}

# 3. 로컬 파일 → 서버로 rsync (데이터 보존)
echo "[3/5] 기존 로컬 파일 → NFS 서버로 동기화..."
if [[ -d "${LOCAL_MOUNT}" ]] && [[ -n "$(ls -A "${LOCAL_MOUNT}" 2>/dev/null)" ]]; then
  echo "  로컬 파일이 존재합니다. rsync로 서버에 먼저 복사..."
  rsync -av --progress "${LOCAL_MOUNT}/" "joon@${NFS_SERVER}:${REMOTE_PATH}/"
  echo "  동기화 완료."
else
  echo "  로컬 디렉토리가 비어 있습니다. 동기화 생략."
fi

# 4. NFS 마운트
echo "[4/5] NFS 마운트..."
mkdir -p "${LOCAL_MOUNT}"

if mountpoint -q "${LOCAL_MOUNT}"; then
  echo "  이미 마운트됨. 재마운트..."
  sudo umount "${LOCAL_MOUNT}"
fi

sudo mount -t nfs "${NFS_SERVER}:${REMOTE_PATH}" "${LOCAL_MOUNT}"
echo "  마운트 완료."

# 5. /etc/fstab 영구 등록
echo "[5/5] /etc/fstab 등록 (재부팅 후 자동 마운트)..."
FSTAB_ENTRY="${NFS_SERVER}:${REMOTE_PATH} ${LOCAL_MOUNT} nfs defaults,_netdev 0 0"
sudo sed -i "\|${NFS_SERVER}:${REMOTE_PATH}|d" /etc/fstab
echo "${FSTAB_ENTRY}" | sudo tee -a /etc/fstab > /dev/null
echo "  fstab 등록 완료."

echo ""
echo "=== 완료 ==="
df -h "${LOCAL_MOUNT}"
echo ""
echo "검증:"
echo "  echo test > ${LOCAL_MOUNT}/nfs-test.txt"
echo "  ssh joon@${NFS_SERVER} 'cat ${REMOTE_PATH}/nfs-test.txt'  # 'test' 출력이면 성공"
echo "  rm ${LOCAL_MOUNT}/nfs-test.txt"
