---
title: "Phase 0 — Harbor insecure-registry 설정"
status: "✅ 완료 (2026-04-10)"
---

# Phase 0: Harbor insecure-registry 설정

## 상태: ✅ 완료

---

## Symptom

```
Failed to pull image "10.34.25.12:80/...":
  http: server gave HTTP response to HTTPS client
```

K8s 파드에서 Harbor 이미지 pull/push 시 `ImagePullBackOff` 발생.

---

## Root Cause

- Harbor는 HTTP(`10.34.25.12:80`)로 서비스 중
- containerd 기본 동작: 모든 레지스트리를 HTTPS로 접근 시도
- HTTP 레지스트리 예외 설정이 없으면 연결 거부

---

## Resolution

모든 K8s 노드에 containerd insecure-registry 설정 추가.

**적용 파일:** `/etc/containerd/certs.d/10.34.25.12/hosts.toml`

```toml
server = "http://10.34.25.12"

[host."http://10.34.25.12"]
  capabilities = ["pull", "resolve", "push"]
  skip_verify = true
```

**적용 방법 (자동화 스크립트):**

```bash
# 프로젝트 루트에서
bash DOCA_BF3/scripts/phase0-harbor-insecure-registry.sh
```

스크립트가 아래 노드에 SSH로 접속해 설정 적용 + containerd 재시작:

| 노드 | IP | 역할 |
|------|-----|------|
| sandbox-1 | 10.34.20.5 | host cluster worker |
| sandbox-2 | 10.34.20.6 | host cluster worker |
| sandbox-3 | 10.34.20.7 | host cluster worker |
| sandbox-4 | 10.34.20.8 | host cluster worker |
| tempnode-bf3 | 10.34.20.4 | DPU host node |
| BF3 DPU | 192.168.101.2 | DPU cluster worker (ProxyJump) |

---

## 검증 결과

```
sandbox-3에서 Harbor pull 테스트:
  image: 10.34.25.12/pydio/cells:4bb631878
  Pulled: 5.943s (146MB) ✅
  Created / Started: 정상
```

Harbor admin 크리덴셜:
- ID: `admin`
- PW: K8s secret `harbor/harbor-admin-secret` → `HARBOR_ADMIN_PASSWORD`

Harbor pull secret 생성 방법:

```bash
HARBOR_PASS=$(kubectl get secret harbor-admin-secret -n harbor \
  -o jsonpath='{.data.HARBOR_ADMIN_PASSWORD}' | base64 -d)

kubectl create secret docker-registry harbor-cred \
  --docker-server=10.34.25.12 \
  --docker-username=admin \
  --docker-password=${HARBOR_PASS} \
  -n <namespace>
```

---

## 네트워크 경로

```
K8s Pod (sandbox 또는 DPU cluster)
    │
    ▼  HTTP:80
br-dpu bridge (tempnode-bf3)
    │
    ▼
Harbor (10.34.25.12:80)
```

> **참고:** 이 경로는 K8s 관리 트래픽(이미지 pull/push)용.
> DOCA 앱의 실제 데이터는 PCIe → VF/SF 경로를 사용 (별개).

---

## 다음 단계

→ [Phase 1 — NFS + doca-dev DPUService 배포](./phase1-doca-dev-dpuservice.md)
