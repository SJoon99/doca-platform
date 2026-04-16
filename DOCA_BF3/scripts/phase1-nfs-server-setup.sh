#!/usr/bin/env bash
# Phase 1A: NFS 서버 설정 (tempnode-bf3에서 실행)
#
# 내보내는 디렉토리:
#   /home/joon/doca-platform/DOCA_BF3/projects/
#
# 클라이언트:
#   10.34.20.8 — node4 (VSCode 작업 머신)
#
# 범위:
#   node4 ↔ tempnode-bf3 NFS 연결만
#   DPU pod /doca_devel NFS 마운트, DPUService 배포 제외
#
# 사용법:
#   ssh joon@10.34.20.4 'bash -s' < DOCA_BF3/scripts/phase1-nfs-server-setup.sh
#   또는
#   bash DOCA_BF3/scripts/phase1-nfs-server-setup.sh  (tempnode-bf3 로컬에서)

set -euo pipefail

EXPORT_PATH="/home/joon/doca-platform/DOCA_BF3/projects"
NFS_CLIENTS=(
  "10.34.20.8"          # node4 (VSCode 작업 머신)
)

echo "=== Phase 1A: NFS 서버 설정 ==="
echo "  Export: ${EXPORT_PATH}"
echo "  Clients: ${NFS_CLIENTS[*]}"
echo ""

# 1. nfs-kernel-server 설치
echo "[1/4] nfs-kernel-server 설치..."
sudo apt-get install -y nfs-kernel-server

# 2. export 디렉토리 확인 (없으면 생성)
echo "[2/4] export 디렉토리 확인..."
if [[ ! -d "${EXPORT_PATH}" ]]; then
  mkdir -p "${EXPORT_PATH}"
  echo "  Created: ${EXPORT_PATH}"
else
  echo "  Exists: ${EXPORT_PATH}"
fi

# 3. /etc/exports 설정
echo "[3/4] /etc/exports 설정..."

# 기존에 이미 등록된 항목 제거 (중복 방지)
sudo sed -i "\|^${EXPORT_PATH}|d" /etc/exports

# 각 클라이언트에 대해 export 추가
for client in "${NFS_CLIENTS[@]}"; do
  echo "${EXPORT_PATH} ${client}(rw,sync,no_subtree_check,no_root_squash)" | \
    sudo tee -a /etc/exports > /dev/null
  echo "  Added: ${EXPORT_PATH} ${client}"
done

# 4. NFS 서버 재시작 및 export 적용
echo "[4/4] NFS 서버 재시작..."
sudo exportfs -ra
sudo systemctl enable nfs-kernel-server
sudo systemctl restart nfs-kernel-server

echo ""
echo "=== 완료 ==="
sudo exportfs -v
echo ""
echo "검증:"
echo "  node4에서: showmount -e 10.34.20.4"
echo "  node4에서: mount | grep '${EXPORT_PATH}'"
