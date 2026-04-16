---
title: "트러블슈팅 — node3 잘못된 CA로 인한 ArgoCD TLS 인증서 검증 실패"
date: "2026-04-11"
---

# 트러블슈팅: node3 잘못된 CA → ArgoCD x509 TLS 검증 실패

## 증상

ArgoCD `doca-platform`, `kamaji` application이 `OutOfSync` 상태로 진입.

```
Failed sync attempt: one or more synchronization tasks completed unsuccessfully
reason: namespace auto creation failed: failed to get api resource:
failed to discover server resources for group version v1:
Get "https://10.233.0.1:443/api/v1?timeout=32s":
tls: failed to verify certificate: x509: certificate signed by unknown authority
(possibly because of "crypto/rsa: verification error" while trying to verify
candidate authority certificate "kubernetes")
```

특징:
- `kubectl` 명령은 정상 동작 (kubeconfig 사용)
- ArgoCD pod 재시작 후에도 동일 오류 반복
- 다른 app (`cilium`, `dpf-operator` 등)은 `Synced, Healthy`

---

## 원인 분석

### 1단계: 오류 성격 파악

`10.233.0.1` = `kubernetes.default.svc` ClusterIP (K8s API server 내부 주소).

ArgoCD pod는 `~/.kube/config`가 아닌 service account의 CA cert를 사용해 API server에 접속:
```
/var/run/secrets/kubernetes.io/serviceaccount/ca.crt
```

`kubectl`이 정상 동작한다는 것은 kubeconfig의 cert는 유효하지만, service account CA cert 경로에 문제가 있다는 뜻.

### 2단계: 인증서 체인 비교

```bash
# ArgoCD pod 내부 CA cert 날짜
kubectl -n argocd exec argocd-application-controller-0 -- \
  cat /var/run/secrets/kubernetes.io/serviceaccount/ca.crt \
  | openssl x509 -noout -dates
# → notBefore: Dec 28 2025

# API server가 제공하는 인증서 날짜
echo | openssl s_client -connect 10.233.0.1:443 2>/dev/null \
  | openssl x509 -noout -dates
# → notBefore: Apr 7 2026
```

날짜는 달라 보이지만 Subject Key Identifier가 일치하는지 확인:

```bash
# API server cert의 Authority Key Identifier
echo | openssl s_client -connect 10.233.0.1:443 2>/dev/null \
  | openssl x509 -noout -text | grep "Authority Key" -A1
# → 13:C7:5D:61:49:90:22:DC:47:F8:FF:BD:7A:FC:E0:DD:2D:EC:CF:D2

# 클러스터 CA의 Subject Key Identifier
kubectl -n kube-system get configmap kube-root-ca.crt \
  -o jsonpath='{.data.ca\.crt}' | openssl x509 -noout -text \
  | grep "Subject Key" -A1
# → 13:C7:5D:61:49:90:22:DC:47:F8:FF:BD:7A:FC:E0:DD:2D:EC:CF:D2
```

일치 → 이 API server cert는 올바른 CA로 서명됨.

```bash
# openssl로 직접 검증
openssl verify -CAfile /tmp/cluster-ca.crt /tmp/apiserver.crt
# → OK
```

인증서 자체는 문제 없음.

### 3단계: kubernetes service 엔드포인트 확인

```bash
kubectl get endpoints kubernetes
# → 10.34.20.3:6443, 10.34.48.50:6443, 10.34.48.52:6443
```

3개 control-plane 노드로 로드밸런싱. 위에서 확인한 cert는 `10.233.0.1`(→ node4 응답)이었을 수 있음.

### 4단계: 각 노드별 Authority Key Identifier 비교

```bash
# node4 (10.34.20.3) cert
echo | openssl s_client -connect 10.34.20.3:6443 2>/dev/null \
  | openssl x509 -noout -text | grep "Authority Key" -A1
# → 13:C7:5D:61:...  ✅ (올바른 클러스터 CA)

# node5 (10.34.48.52) cert
echo | openssl s_client -connect 10.34.48.52:6443 2>/dev/null \
  | openssl x509 -noout -text | grep "Authority Key" -A1
# → 13:C7:5D:61:...  ✅ (올바른 클러스터 CA)

# node3 (10.34.48.50) cert
echo | openssl s_client -connect 10.34.48.50:6443 2>/dev/null \
  | openssl x509 -noout -text | grep "Authority Key" -A1
# → 47:A9:9E:0F:...  ❌ (다른 CA로 서명됨!)
```

**node3만 다른 CA로 서명된 API server cert를 가지고 있음.**

### 5단계: node3 로컬 CA 비교

```bash
# node3에 있는 CA cert fingerprint
ssh joon@10.34.48.50 \
  'sudo openssl x509 -in /etc/kubernetes/pki/ca.crt -noout -fingerprint -sha256'
# → AE:FD:2F:34:...  ❌ (클러스터 CA와 다름)

# 클러스터 CA fingerprint
kubectl -n kube-system get configmap kube-root-ca.crt \
  -o jsonpath='{.data.ca\.crt}' | openssl x509 -noout -fingerprint -sha256
# → FD:C5:CF:85:...  ✅
```

**node3의 `/etc/kubernetes/pki/ca.crt` 자체가 클러스터 CA가 아닌 별개의 CA.**

---

## 근본 원인

node3을 control-plane에 `kubeadm join`으로 추가할 때, 올바른 클러스터 CA cert/key가 `/etc/kubernetes/pki/`에 배포되지 않고 **새 CA가 로컬에서 생성됐다.**

kubeadm의 control-plane join 절차에서는 기존 클러스터의 CA cert/key를 새 노드에 미리 복사해야 한다:
```bash
# join 전 올바른 절차
scp /etc/kubernetes/pki/ca.{crt,key} new-control-plane-node:/etc/kubernetes/pki/
```

이 단계가 누락되면 kubeadm이 새 CA를 생성해 API server cert를 서명하고, 해당 CA는 클러스터의 `kube-root-ca.crt`와 다르게 된다.

### 왜 간헐적으로 발생하는가?

`kubernetes.default.svc`는 kube-proxy(iptables/ipvs)를 통해 3개 control-plane 엔드포인트로 로드밸런싱한다.

```
10.233.0.1:443 (ClusterIP)
  → 10.34.20.3:6443 (node4)  정상 CA ✅
  → 10.34.48.50:6443 (node3) 잘못된 CA ❌
  → 10.34.48.52:6443 (node5) 정상 CA ✅
```

ArgoCD가 node3에 연결되면 TLS 실패, node4/node5에 연결되면 성공. 이 때문에 간헐적으로 sync가 실패하거나 retry 중 성공처럼 보이는 현상이 발생한다.

---

## 해결

### Step 1. 올바른 클러스터 CA를 node3으로 복사

node4에서 실행:
```bash
sudo cat /etc/kubernetes/pki/ca.crt \
  | ssh joon@10.34.48.50 'cat > /tmp/ca.crt'
sudo cat /etc/kubernetes/pki/ca.key \
  | ssh joon@10.34.48.50 'cat > /tmp/ca.key'
```

### Step 2. node3에서 CA 교체 및 API server cert 재발급

node3에서 실행:
```bash
# 기존 CA 백업
sudo cp /etc/kubernetes/pki/ca.crt /etc/kubernetes/pki/ca.crt.bak
sudo cp /etc/kubernetes/pki/ca.key /etc/kubernetes/pki/ca.key.bak

# 올바른 CA로 교체
sudo cp /tmp/ca.crt /etc/kubernetes/pki/ca.crt
sudo cp /tmp/ca.key /etc/kubernetes/pki/ca.key
sudo chmod 600 /etc/kubernetes/pki/ca.key

# API server cert 재발급 (이제 올바른 CA로 서명됨)
sudo kubeadm certs renew apiserver

# kubelet 재시작 → kube-apiserver static pod 재로드
sudo systemctl restart kubelet
```

### Step 3. 검증

```bash
# node3의 새 cert Authority Key Identifier 확인
echo | openssl s_client -connect 10.34.48.50:6443 2>/dev/null \
  | openssl x509 -noout -text | grep "Authority Key" -A1
# → 13:C7:5D:61:...  ✅ (클러스터 CA와 일치)
```

### Step 4. ArgoCD controller 재시작

cert 수정 후 ArgoCD controller의 연결 캐시를 초기화:
```bash
kubectl -n argocd delete pod -l app.kubernetes.io/name=argocd-application-controller
```

---

## 배경 지식

### kubeadm control-plane 인증서 구조

```
/etc/kubernetes/pki/
  ca.crt          ← 클러스터 Root CA (모든 노드 동일해야 함)
  ca.key          ← 클러스터 CA 개인키 (control-plane에만 존재)
  apiserver.crt   ← API server 서빙 cert (CA로 서명)
  apiserver.key   ← API server 개인키
```

`ca.crt`는 모든 control-plane 노드에서 동일해야 한다. `ca.key`는 새 cert 발급에 필요하므로 control-plane 노드에만 존재.

### in-cluster TLS 인증서 경로

K8s pod 안에서 API server 접근 시 사용하는 경로:

```
/var/run/secrets/kubernetes.io/serviceaccount/
  token      ← service account JWT
  ca.crt     ← API server 검증에 사용하는 CA cert
              = kube-root-ca.crt ConfigMap 내용
              = 클러스터의 /etc/kubernetes/pki/ca.crt
```

이 경로의 `ca.crt`가 실제 API server cert를 서명한 CA와 다르면 TLS 검증 실패.

### kubectl이 정상인 이유

`~/.kube/config`는 kubeadm init 시점의 CA cert를 직접 embedded 해서 들고 있음. node3의 CA 문제와 무관하게 정상 동작.

---

## 재발 방지

control-plane 노드 추가 시 반드시 CA 배포 선행:

```bash
# 새 control-plane 노드 합류 전
ssh new-node 'sudo mkdir -p /etc/kubernetes/pki'
sudo scp /etc/kubernetes/pki/ca.{crt,key} new-node:/etc/kubernetes/pki/

# 그 후 kubeadm join --control-plane 실행
```

kubeadm 공식 문서: [HA kubeadm setup — copy certificate files](https://kubernetes.io/docs/setup/production-environment/tools/kubeadm/high-availability/#steps-for-the-first-control-plane-node)
