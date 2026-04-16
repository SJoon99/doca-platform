#!/usr/bin/env bash
# Phase 0: Harbor insecure-registry 설정
# Harbor(10.34.25.12:80)를 K8s 노드에서 HTTP로 사용할 수 있도록
# containerd에 insecure-registry 설정을 추가합니다.
#
# 적용 대상:
#   - sandbox-1~4 (10.34.20.5~8)  — host cluster worker
#   - tempnode-bf3 (10.34.20.4)    — DPU host node
#   - BF3 DPU (192.168.101.2)      — DPU cluster worker (via tempnode-bf3)
#
# 사용법:
#   ./phase0-harbor-insecure-registry.sh
#   ./phase0-harbor-insecure-registry.sh --dry-run   # 변경 없이 확인만

set -euo pipefail

HARBOR_IP="10.34.25.12"
DRY_RUN=false

[[ "${1:-}" == "--dry-run" ]] && DRY_RUN=true

# 노드 목록: "user@host" 형식
HOST_CLUSTER_NODES=(
  "joon@10.34.20.5"   # sandbox-1
  "joon@10.34.20.6"   # sandbox-2
  "joon@10.34.20.7"   # sandbox-3
  "joon@10.34.20.8"   # sandbox-4
  "joon@10.34.20.4"   # tempnode-bf3
)

# containerd insecure-registry 설정 스크립트 (원격 노드에서 실행)
read -r -d '' APPLY_SCRIPT << 'SCRIPT' || true
HARBOR_IP="__HARBOR_IP__"
CERTS_DIR="/etc/containerd/certs.d/${HARBOR_IP}"

echo "[INFO] Creating containerd insecure-registry config for ${HARBOR_IP}..."
sudo mkdir -p "${CERTS_DIR}"
sudo tee "${CERTS_DIR}/hosts.toml" > /dev/null << EOF
server = "http://${HARBOR_IP}"

[host."http://${HARBOR_IP}"]
  capabilities = ["pull", "resolve", "push"]
  skip_verify = true
EOF
echo "[INFO] Written: ${CERTS_DIR}/hosts.toml"

echo "[INFO] Restarting containerd..."
sudo systemctl restart containerd
sleep 2

echo "[INFO] Verifying containerd is running..."
sudo systemctl is-active containerd || { echo "[ERROR] containerd failed to restart!"; exit 1; }
echo "[OK] containerd is active."
SCRIPT

# 실제 HARBOR_IP 치환
APPLY_SCRIPT="${APPLY_SCRIPT//__HARBOR_IP__/$HARBOR_IP}"

apply_to_node() {
  local target="$1"
  local label="${2:-$target}"
  echo ""
  echo "=== Applying to: $label ($target) ==="

  if $DRY_RUN; then
    echo "[DRY-RUN] Would apply insecure-registry config to $target"
    return 0
  fi

  # SSH 옵션: StrictHostKeyChecking 비활성화, 타임아웃 설정
  ssh -o StrictHostKeyChecking=no \
      -o ConnectTimeout=10 \
      "$target" "bash -s" <<< "$APPLY_SCRIPT"
}

apply_to_dpu_node() {
  local jump_host="joon@10.34.20.4"
  local dpu_host="ubuntu@192.168.101.2"
  echo ""
  echo "=== Applying to: BF3 DPU ($dpu_host via $jump_host) ==="

  if $DRY_RUN; then
    echo "[DRY-RUN] Would apply insecure-registry config to DPU node via ProxyJump"
    return 0
  fi

  ssh -o StrictHostKeyChecking=no \
      -o ConnectTimeout=10 \
      -J "$jump_host" \
      "$dpu_host" "bash -s" <<< "$APPLY_SCRIPT"
}

verify_harbor_pull() {
  local target="$1"
  local via_jump="${2:-}"
  echo "[VERIFY] Testing Harbor pull from $target..."

  local ssh_opts="-o StrictHostKeyChecking=no -o ConnectTimeout=10"
  [[ -n "$via_jump" ]] && ssh_opts="$ssh_opts -J $via_jump"

  ssh $ssh_opts "$target" \
    "sudo crictl pull ${HARBOR_IP}/library/hello-world:latest 2>&1 || true" || true
}

# ── Main ──────────────────────────────────────────────────────────────────────

echo "Harbor insecure-registry 설정"
echo "  Harbor: http://${HARBOR_IP}"
echo "  Mode: $(${DRY_RUN} && echo DRY-RUN || echo APPLY)"
echo "──────────────────────────────────────────────────"

# 1. Host cluster 노드
for node in "${HOST_CLUSTER_NODES[@]}"; do
  apply_to_node "$node" || {
    echo "[WARN] Failed on $node — continuing..."
  }
done

# 2. DPU 노드 (ProxyJump)
apply_to_dpu_node || {
  echo "[WARN] Failed on DPU node — you may need to apply manually:"
  echo "  ssh joon@10.34.20.4"
  echo "  ssh ubuntu@192.168.101.2"
  echo "  (then run the containerd config commands manually)"
}

echo ""
echo "=== 완료 ==="
echo ""
echo "다음 단계: Harbor에서 실제로 이미지를 pull할 수 있는지 확인하세요."
echo ""
echo "  # 각 노드에서:"
echo "  sudo crictl pull 10.34.25.12/library/hello-world:latest"
echo ""
echo "  # 또는 K8s Pod로 테스트:"
echo "  kubectl run harbor-test --image=10.34.25.12/library/hello-world:latest --restart=Never"
echo "  kubectl get pod harbor-test"
