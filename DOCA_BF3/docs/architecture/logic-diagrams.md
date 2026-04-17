---
title: "DPF 현재 클러스터 로직 다이어그램"
description: "현재 Sandbox 클러스터에 실제 적용된 DPF 전체 로직을 mermaid 다이어그램으로 시각화"
date: "2026-04-14"
---

# DPF 현재 클러스터 로직 다이어그램

현재 Sandbox 클러스터(`doca-platform`)에 실제로 동작 중인 DPF의 모든 구성요소와 데이터 흐름을 시각화한 문서.

---

## 1. 전체 아키텍처 (물리 + 논리)

BF3 DPU가 장착된 `tempnode-bf3` 노드와 DPU 위에 Kamaji tenant 클러스터가 동작하는 구조.

```mermaid
flowchart TB
    subgraph EXT["외부 네트워크"]
        DH["Docker Hub<br/>jinkernel/*"]
        NVCR["nvcr.io<br/>NVIDIA Registry"]
        GCR["k8s.gcr.io<br/>(pause image)"]
    end

    subgraph HOST["Host Kubernetes Cluster (amd64)<br/>10.32.0.0/12 내부망"]
        subgraph CP["Control Plane"]
            N3["node3<br/>10.34.48.50"]
            N4["node4<br/>10.34.20.3<br/>(VSCode, NFS client)"]
            N5["node5<br/>10.34.48.52"]
        end

        subgraph WK["Workers"]
            S1["sandbox-1~4<br/>일반 워크로드"]
            BF["tempnode-bf3<br/>10.34.20.4<br/>(NFS server, Harbor host)"]
        end

        subgraph NS_ARGO["namespace: argocd"]
            ARGOCD["ArgoCD<br/>application-controller"]
            ARGOCMD["argocd-cmd-params-cm<br/>application.namespaces:<br/>dpf-operator-system"]
            APPROJ["AppProjects<br/>doca-platform-project-dpu<br/>doca-platform-project-host"]
            CSECRET["Cluster Secret<br/>dpu-cplane-tenant1"]
            HREPO["Harbor Repo Secret<br/>enableOCI: true"]
        end

        subgraph NS_DPF["namespace: dpf-operator-system"]
            OPCFG["DPFOperatorConfig<br/>overrides.argoCDNamespace: argocd"]
            OP["dpf-operator-controller-manager<br/>0.1.2-publicmainffafc429"]
            DSC["dpuservice-controller-manager"]
            PROV["dpf-provisioning-controller-manager"]
            KAMAJI["kamaji + kamaji-etcd<br/>(tenant control-plane 호스팅)"]
            APPS["tenant Applications<br/>dpu-cplane-tenant1-*"]
            SYSDPUS["System DPUServices<br/>flannel, multus, ovs-cni,<br/>sriov-device-plugin, ..."]
        end

        subgraph NS_DOCA["namespace: doca-dev"]
            DPUS["DPUService: doca-dev<br/>repoURL: oci://10.34.25.12:80/doca"]
        end

        subgraph NS_TENANT["namespace: dpu-cplane-tenant1<br/>(Kamaji tenant control-plane)"]
            APIS["kube-apiserver<br/>(tenant)"]
            KUBECFG["admin-kubeconfig Secret"]
        end

        HARBOR["Harbor Registry<br/>10.34.25.12:80/doca<br/>(hosted on sandbox)"]
    end

    subgraph DPU_CL["DPU Kubernetes Cluster (arm64)<br/>Kamaji tenant"]
        DPU_NODE["DPU Node<br/>tempnode-bf3-mt25476000nu<br/>br-comm-ch: 10.34.20.99"]
        subgraph DPU_PODS["DPU Pods"]
            DOCADEV["doca-dev-xxxxx<br/>(NFS mount /doca_devel)"]
            FLANNEL["kube-flannel"]
            MULTUS["kube-multus"]
            COREDNS["coredns"]
        end
    end

    BF -.->|"PCIe"| DPU_NODE
    N4 -.->|"NFS mount<br/>/home/joon/doca-platform/DOCA_BF3/projects"| BF
    DPU_NODE -.->|"NFS mount<br/>/doca_devel"| BF

    OP --> OPCFG
    OP --> DSC
    OP --> SYSDPUS
    OPCFG -.->|argoCDNamespace override| APPROJ
    OPCFG -.-> CSECRET
    DSC --> APPS
    ARGOCD -.->|watches<br/>multi-namespace| APPS
    ARGOCD --> APPROJ
    APPROJ -.->|sourceNamespaces:<br/>dpf-operator-system| APPS
    APPS -.->|pulls chart via| HREPO
    HREPO -.->|url: 10.34.25.12:80/doca<br/>enableOCI: true| HARBOR
    APPS -.->|syncs to tenant<br/>via kubeconfig| CSECRET
    CSECRET -.-> APIS
    APIS -.->|schedule pods| DPU_NODE
    HARBOR --> DPUS
    HARBOR --> SYSDPUS
    NVCR --> DOCADEV
    DH --> OP
    GCR --> DPU_NODE

    classDef done fill:#d4edda,stroke:#28a745
    classDef key fill:#fff3cd,stroke:#ffc107
    classDef external fill:#e9ecef,stroke:#6c757d
    class DOCADEV,FLANNEL,MULTUS,COREDNS,OP,DSC done
    class HREPO,APPROJ,CSECRET,OPCFG key
    class DH,NVCR,GCR external
```

---

## 2. DPUService → DPU Pod 배포 시퀀스

사용자가 DPUService YAML을 작성한 시점부터 DPU 노드에서 Pod가 실행되기까지의 전체 흐름.

```mermaid
sequenceDiagram
    autonumber
    actor User
    participant GitRepo as GitOps Repo<br/>(SandBox-Infra)
    participant ArgoHost as ArgoCD<br/>(argocd ns)
    participant DPUSCtrl as dpuservice-<br/>controller-manager
    participant TenantApp as Tenant Application<br/>(dpf-operator-system ns)
    participant Harbor as Harbor Registry<br/>10.34.25.12:80/doca
    participant TenantAPI as DPU Cluster<br/>kube-apiserver
    participant DPUNode as DPU Node<br/>tempnode-bf3-mt25476000nu

    User->>GitRepo: kubectl apply -f<br/>doca-dev-dpuservice.yaml
    GitRepo->>ArgoHost: sync host app<br/>(dpf-doca-dev)
    ArgoHost->>DPUSCtrl: create DPUService<br/>doca-dev/doca-dev
    Note over DPUSCtrl: repoURL = oci://10.34.25.12:80/doca<br/>chart = doca-dev<br/>version = 0.1.0

    DPUSCtrl->>DPUSCtrl: ParseHelmChartString<br/>+ GetArgoRepoURL()<br/>(strips oci:// prefix)
    DPUSCtrl->>TenantApp: create Application<br/>dpu-cplane-tenant1-doca-dev<br/>(repoURL: 10.34.25.12:80/doca)

    ArgoHost->>ArgoHost: detect Application<br/>(multi-namespace watch)
    ArgoHost->>ArgoHost: match repoURL to<br/>argocd-repo-harbor-oci<br/>(enableOCI: true)

    ArgoHost->>Harbor: helm pull oci://10.34.25.12:80/<br/>doca/doca-dev --version 0.1.0
    Harbor-->>ArgoHost: chart tarball

    ArgoHost->>ArgoHost: resolve AppProject<br/>(doca-platform-project-dpu<br/>in argocd ns)
    ArgoHost->>ArgoHost: resolve cluster secret<br/>(dpu-cplane-tenant1<br/>in argocd ns)
    ArgoHost->>TenantAPI: apply manifests via<br/>admin kubeconfig

    TenantAPI->>TenantAPI: create namespace,<br/>DaemonSet, etc.
    TenantAPI->>DPUNode: schedule pod<br/>doca-dev-xxxxx

    DPUNode->>DPUNode: DNS resolve nvcr.io<br/>(via 8.8.8.8 on br-comm-ch)
    DPUNode->>DPUNode: pull<br/>nvcr.io/nvidia/doca/doca:3.2.2-devel

    DPUNode->>DPUNode: CNI setup<br/>(flannel via loopback+main)
    DPUNode->>DPUNode: mount NFS<br/>10.34.20.4:/home/joon/doca-platform/<br/>DOCA_BF3/projects → /doca_devel

    DPUNode-->>TenantAPI: pod Running (1/1)
    TenantAPI-->>ArgoHost: status Healthy
    ArgoHost-->>DPUSCtrl: Application Synced+Healthy
    DPUSCtrl-->>User: DPUService READY=True<br/>PHASE=Success
```

---

## 3. ArgoCD Multi-Namespace + AppProject Topology

DPF 공식 설계에서 가장 혼동되기 쉬운 부분. Application은 `dpf-operator-system`에, AppProject/Cluster Secret은 `argocd` ns에 있어야 정상 동작.

```mermaid
flowchart LR
    subgraph NS_ARGOCD["namespace: argocd"]
        ARGOCTL["application-controller"]
        CMD_CM["argocd-cmd-params-cm<br/><br/>application.namespaces:<br/>dpf-operator-system"]
        AP_DPU["AppProject<br/>doca-platform-project-dpu<br/><br/>sourceNamespaces:<br/>- dpf-operator-system"]
        AP_HOST["AppProject<br/>doca-platform-project-host<br/><br/>sourceNamespaces:<br/>- dpf-operator-system"]
        CS_TENANT["Secret<br/>dpu-cplane-tenant1<br/>(cluster type)<br/><br/>server: https://...<br/>config: {bearerToken}"]
        REPO_HARBOR["Secret<br/>argocd-repo-harbor-oci<br/>(repository type)<br/><br/>url: 10.34.25.12:80/doca<br/>enableOCI: true<br/>type: helm"]
        REPO_MELLANOX["Secret<br/>argocd-repo-mellanox<br/>(OCI)"]
        REPO_NVIDIA["Secret<br/>argocd-repo-nvidia-charts<br/>(OCI)"]
    end

    subgraph NS_DPF["namespace: dpf-operator-system"]
        APP_DOCA["Application<br/>dpu-cplane-tenant1-doca-dev<br/><br/>project: doca-platform-project-dpu<br/>destination.server: &lt;tenant API&gt;<br/>source.repoURL: 10.34.25.12:80/doca<br/>source.chart: doca-dev"]
        APP_FLANNEL["Application<br/>dpu-cplane-tenant1-flannel<br/><br/>source.repoURL:<br/>10.34.25.12:80/doca<br/>source.chart: dpu-networking"]
        APP_MULTUS["Application<br/>dpu-cplane-tenant1-multus"]
        APP_OTHERS["... 기타 시스템 Apps"]
    end

    ARGOCTL -->|watch list| APP_DOCA
    ARGOCTL -->|watch list| APP_FLANNEL
    ARGOCTL -->|watch list| APP_MULTUS
    ARGOCTL -->|watch list| APP_OTHERS
    CMD_CM -.->|enables cross-ns watch| ARGOCTL

    APP_DOCA -.->|references project| AP_DPU
    APP_FLANNEL -.->|references project| AP_DPU
    AP_DPU -.->|allows source ns| APP_DOCA

    ARGOCTL -->|get cluster config| CS_TENANT
    ARGOCTL -->|match repoURL| REPO_HARBOR

    classDef fix fill:#fff3cd,stroke:#ffc107,stroke-width:2px
    class CMD_CM,AP_DPU,AP_HOST,CS_TENANT,REPO_HARBOR fix
```

**주의 사항:**
- `AppProject`와 `Cluster Secret`은 반드시 `argocd` ns에 존재해야 함 (ArgoCD는 자신의 ns에서만 이 리소스 조회)
- `application.namespaces`로 다른 ns의 Application을 watch 허용
- `AppProject.sourceNamespaces`로 해당 ns의 Application이 이 project를 사용하도록 허용
- Harbor repo secret `url`은 `oci://` prefix 없이 저장 (`GetArgoRepoURL()`이 strip하므로 Application의 repoURL과 일치시키기 위함)

---

## 4. Helm Chart 및 Image 빌드/배포 흐름

operator가 참조하는 chart 주소가 어떻게 결정되는지, 왜 재빌드가 필요했는지 설명.

```mermaid
flowchart LR
    subgraph BUILD["빌드 서버 (10.30.0.184)"]
        TMPL["internal/release/templates/<br/>defaults.yaml.tmpl<br/><br/>dpuNetworkingHelmChart:<br/>${UPSTREAM_HELM_REGISTRY}/<br/>${DPU_NETWORKING_HELM_CHART_NAME}<br/>:${TAG}"]
        ENV["make variables<br/>REGISTRY=jinkernel<br/>UPSTREAM_HELM_REGISTRY=<br/>oci://10.34.25.12:80/doca<br/>TAG=0.1.2-publicmainffafc429"]
        ENVSUBST["envsubst"]
        DEFYAML["build/defaults.yaml<br/><br/>dpuNetworkingHelmChart:<br/>oci://10.34.25.12:80/doca/<br/>dpu-networking:TAG"]
        DOCKER["docker buildx build<br/>--no-cache<br/>-f Dockerfile.dpf-system"]
        IMG["Docker Image<br/>jinkernel/dpf-system:TAG<br/><br/>/etc/dpf-defaults.yaml<br/>baked into binary"]
        CHART_PKG["helm package<br/>hack/charts/<br/>dpu-networking-TAG.tgz"]
    end

    subgraph REGISTRIES["Registries"]
        DHUB["Docker Hub<br/>jinkernel/dpf-system:TAG"]
        HARBOR_IMG["Harbor<br/>10.34.25.12:80/doca/<br/>dpu-networking:TAG<br/>doca-dev:0.1.0"]
    end

    subgraph CLUSTER["Host Cluster"]
        NODES["Worker Nodes<br/>crictl image cache"]
        OP_POD["dpf-operator-<br/>controller-manager<br/>(pulls + reads defaults.yaml)"]
        RECONCILE["reconcileSystemComponents"]
        DPUS_OBJ["DPUService objects<br/>with repoURL +<br/>chart + version"]
        ARGO_APP["ArgoCD Application<br/>pulls from Harbor"]
    end

    TMPL --> ENVSUBST
    ENV --> ENVSUBST
    ENVSUBST --> DEFYAML
    DEFYAML -->|COPY build/defaults.yaml<br/>/etc/dpf-defaults.yaml| DOCKER
    DOCKER --> IMG
    IMG -->|docker push| DHUB
    CHART_PKG -->|helm push --plain-http| HARBOR_IMG
    DHUB -->|kubelet pull| NODES
    NODES --> OP_POD
    OP_POD --> RECONCILE
    RECONCILE -->|ParseHelmChartString| DPUS_OBJ
    DPUS_OBJ --> ARGO_APP
    ARGO_APP -->|helm pull oci://| HARBOR_IMG

    classDef crit fill:#f8d7da,stroke:#dc3545
    classDef key fill:#fff3cd,stroke:#ffc107
    class ENV,DEFYAML,CHART_PKG key
    class DOCKER crit
```

**핵심 포인트:**
- `build/defaults.yaml`을 직접 수정해도 소용없음 — `make`가 템플릿에서 재생성
- `UPSTREAM_HELM_REGISTRY`를 make 변수로 반드시 전달
- 같은 태그로 re-push할 때 `imagePullPolicy: IfNotPresent` + 노드 캐시 조합으로 새 이미지 미반영 → 모든 노드에서 `crictl rmi` 필수
- Docker buildx 캐시로 인해 `--no-cache` 필요

---

## 5. 실제로 해결한 문제 체인 (시간 순)

DPF 초기 상태 → 현재 정상 동작까지 겪은 모든 deadlock과 해결 순서.

```mermaid
sequenceDiagram
    autonumber
    participant S1 as 초기 상태
    participant S2 as 신 operator 배포
    participant S3 as 업그레이드 deadlock
    participant S4 as AppProject 이슈
    participant S5 as OCI chart URL
    participant S6 as DPU DNS/Route
    participant S7 as CNI 설정
    participant S8 as 시스템 chart 배포
    participant S9 as 정상 동작

    S1->>S2: 구 operator (45265976)<br/>→ 신 operator (ffafc429) 빌드/push
    Note over S2: argoCDNamespace override<br/>지원 버전
    S2->>S3: 신 operator 배포<br/>→ reconcile 무한루프
    Note over S3: validateSystemComponentsReadiness<br/>→ system DPUServices ready 요구<br/>→ 없어서 실패<br/>→ reconcileSystemComponents<br/>못 도달 (닭과 달걀)
    S3->>S3: kubectl patch status<br/>status.version=ffafc429
    Note over S3: UpgradeInProgress()=false<br/>validation skip
    S3->>S4: 모든 deployment rolling<br/>업데이트 완료
    Note over S4: tenant Application:<br/>Application referencing project<br/>doca-platform-project-dpu<br/>which does not exist
    S4->>S4: AppProject + cluster secret<br/>수동 bootstrap to argocd ns
    Note over S4: 신 operator도 자동 생성하지만<br/>deadlock으로 인해 미실행<br/>→ 수동 bootstrap 필요
    S4->>S5: Application 에러:<br/>invalid chart URL format:<br/>10.34.25.12:80/doca
    Note over S5: GetArgoRepoURL()이 oci:// 제거<br/>→ ArgoCD 전통 helm repo 취급<br/>→ helm pull --repo 실패
    S5->>S5: argocd-repo-harbor-oci Secret 등록<br/>enableOCI: true
    Note over S5: ArgoCD가 enableOCI 인식하여<br/>helm pull oci:// 사용
    S5->>S6: doca-dev Application Synced<br/>→ DaemonSet/Pod 생성<br/>→ ContainerCreating (23m)
    Note over S6: pause 이미지 pull 실패:<br/>lookup k8s.gcr.io failed<br/>- resolv.conf 비어있음<br/>- default route 없음
    S6->>S6: resolvectl dns br-comm-ch 8.8.8.8<br/>ip route add default via 10.47.255.254
    S6->>S7: DNS 되지만 CNI 에러:<br/>plugin loopback missing name
    S7->>S7: /etc/cni/net.d/99-loopback.conf<br/>"name": "lo" 추가
    Note over S7: 메인 CNI(flannel) 없으면<br/>여전히 pod 네트워킹 불가
    S7->>S8: 시스템 DPUServices 미배포:<br/>dpu-networking chart =<br/>oci://jinkernel/dpu-networking<br/>(resolve 불가)
    S8->>S8: 1) chart Harbor에 push<br/>2) operator 재빌드<br/>   UPSTREAM_HELM_REGISTRY=<br/>   oci://10.34.25.12:80/doca<br/>3) 모든 노드 crictl rmi<br/>4) pod 재시작
    Note over S8: flannel, multus, coredns 배포<br/>→ DPU 클러스터 CNI 설치 완료
    S8->>S9: doca-dev pod 1/1 Running<br/>NFS 마운트 확인<br/>DPUService READY=True

    rect rgb(212, 237, 218)
        Note over S9: 최종: 전체 배포 체인 정상
    end
```

---

## 6. 현재 활성화된 수동 조치 (재부팅 시 손실)

재부팅이나 재배포 시 다시 적용해야 하는 수동 조치들 — 영구화 필요.

```mermaid
flowchart TB
    subgraph MANUAL["수동 조치 (영구화 필요)"]
        M1["DPU 노드 DNS<br/>resolvectl dns br-comm-ch 8.8.8.8 8.8.4.4<br/>resolvectl domain br-comm-ch '~.'"]
        M2["DPU 노드 Default Route<br/>ip route add default via 10.47.255.254 dev br-comm-ch"]
        M3["DPU loopback CNI<br/>/etc/cni/net.d/99-loopback.conf<br/>+ name: lo 필드"]
        M4["DPFOperatorConfig status.version<br/>kubectl patch ... --subresource=status<br/>(업그레이드 시만 필요)"]
    end

    subgraph GITOPS["GitOps로 관리되는 영구 조치"]
        G1["operator 이미지 태그<br/>argocd/apps/.../dpf-operator/values.yaml<br/>tag: 0.1.2-publicmainffafc429"]
        G2["DPFOperatorConfig<br/>overrides.argoCDNamespace: argocd"]
        G3["argocd-cmd-params-cm<br/>application.namespaces:<br/>dpf-operator-system"]
        G4["ArgoCD Harbor repo secret<br/>argocd-repo-harbor-oci"]
        G5["AppProjects + tenant cluster secret<br/>(operator가 자동 생성하지만<br/>deadlock 회피용 bootstrap 필요)"]
    end

    subgraph TODO["영구화 TODO"]
        T1["netplan으로 DPU DNS/route 고정"]
        T2["DPU base image에 loopback CNI 수정 반영"]
        T3["operator 빌드에서 UPSTREAM_HELM_REGISTRY 고정"]
    end

    M1 --> T1
    M2 --> T1
    M3 --> T2

    classDef manual fill:#f8d7da,stroke:#dc3545
    classDef gitops fill:#d4edda,stroke:#28a745
    classDef todo fill:#fff3cd,stroke:#ffc107
    class M1,M2,M3,M4 manual
    class G1,G2,G3,G4,G5 gitops
    class T1,T2,T3 todo
```

---

## 7. doca-dev 개발 사이클 (현재 동작 상태)

Phase 2 완료 기준 — 사용자가 실제로 할 수 있는 개발 워크플로우.

```mermaid
sequenceDiagram
    autonumber
    actor Dev as 개발자<br/>(node4 VSCode)
    participant NFS as tempnode-bf3<br/>NFS server
    participant DocaDev as doca-dev Pod<br/>(DPU arm64)
    participant Harbor as Harbor Registry
    participant HostK8s as Host K8s

    Dev->>NFS: vim /home/joon/doca-platform/<br/>DOCA_BF3/projects/app.c
    Note over NFS: 파일 수정 즉시<br/>NFS 서버에 기록
    NFS-->>DocaDev: /doca_devel/app.c<br/>변경 반영

    Dev->>DocaDev: kubectl exec -it doca-dev-xxx<br/>-c dev -- bash
    DocaDev->>DocaDev: cd /doca_devel<br/>meson setup build<br/>ninja -C build
    Note over DocaDev: arm64 네이티브 빌드<br/>(크로스컴파일 불필요)

    DocaDev-->>Dev: 바이너리 생성

    Note over Dev,Harbor: (Phase 3에서 추가될 흐름)
    rect rgb(255, 243, 205)
        Dev->>DocaDev: kubectl exec ... -c builder<br/>(Kaniko — Phase 3)
        DocaDev->>Harbor: 이미지 빌드 + push<br/>harbor/doca/my-app:latest
        Dev->>HostK8s: kubectl apply -f<br/>dpuservice-my-app.yaml
        HostK8s->>DocaDev: 새 DPUService 배포
    end
```

---

## 참고 문서

- [Phase 0 — Harbor Registry](../dpf-guides/phases/phase0-harbor.md)
- [Phase 1 — NFS Mount](../dpf-guides/phases/phase1-nfs.md)
- [Phase 2 — doca-dev DPUService](../dpf-guides/phases/phase2-doca-dev.md)
- [DPF Cloud-Native Dev Cycle](../dpf-guides/cloud-native-dev.md)
- [Cluster Overview](./cluster-overview.md)
- [DPF Troubleshooting](../troubleshooting/README.md)
