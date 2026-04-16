# DOCA_BF3 문서

DOCA Platform Framework(DPF) + BF3 DPU 기반 클라우드 네이티브 개발 환경 문서.

## 카테고리별 인덱스

### [setup/](./setup/) — 환경 셋업
처음 클러스터 + DPU + 개발 환경을 구성할 때 시작하는 곳.
- [overview](./setup/overview.md) — 전체 셋업 개요
- [install](./setup/install.md) — 설치 절차
- [known-issues](./setup/known-issues.md) — 설치 중 알려진 이슈
- [dev-env-setup](./setup/dev-env-setup.md) — 개발 환경 구성

### [architecture/](./architecture/) — 아키텍처 (Why & How)
현재 클러스터가 어떻게 동작하는지 설명하는 문서. 구조/토폴로지/상호작용.
- [cluster-overview](./architecture/cluster-overview.md) — 클러스터 전체 개요
- [cluster-topology](./architecture/cluster-topology.md) — 노드/DPU 토폴로지 상세
- [network-architecture](./architecture/network-architecture.md) — 네트워크 구조 (br-dpu, br-comm-ch 등)
- [logic-diagrams](./architecture/logic-diagrams.md) — 현재 적용된 DPF 로직 mermaid 다이어그램
- [topology-excalidraw](./architecture/topology-excalidraw.md) — Excalidraw 시각화

### [reference/](./reference/) — 레퍼런스
빠른 조회용 — CRD 필드, 빌드 히스토리.
- [crd-reference](./reference/crd-reference.md) — DPF CRD 전체 목록
- [crd-field-reference](./reference/crd-field-reference.md) — CRD 필드별 의미/사용법
- [build-history](./reference/build-history.md) — operator 이미지 빌드 히스토리

### [dpf-guides/](./dpf-guides/) — DPF How-to 가이드
DPF로 무엇을 어떻게 하는지 — 작업 수행 중심.
- [setup-guide](./dpf-guides/setup-guide.md) — DPF 초기 셋업
- [cloud-native-dev](./dpf-guides/cloud-native-dev.md) — DPU 위 클라우드 네이티브 개발 사이클
- [devops-pipeline](./dpf-guides/devops-pipeline.md) — DevOps 파이프라인 개요
- [phases/](./dpf-guides/phases/) — 단계별 진행 (phase0~)
  - [phase0-harbor](./dpf-guides/phases/phase0-harbor.md) — Harbor 레지스트리 구축
  - [phase1-nfs](./dpf-guides/phases/phase1-nfs.md) — NFS 마운트
  - [phase2-doca-dev](./dpf-guides/phases/phase2-doca-dev.md) — doca-dev DPUService 배포

### [troubleshooting/](./troubleshooting/) — 트러블슈팅
증상 → 원인 → 해결 패턴으로 정리된 장애 대응 문서.
- [README](./troubleshooting/README.md) — 증상별 인덱스
- [node3-wrong-ca](./troubleshooting/node3-wrong-ca.md) — node3 CA 인증서 오류

### [applications/](./applications/) — DOCA 애플리케이션
개별 DOCA 앱별 문서.
- [dma-copy/](./applications/dma-copy/) — DMA copy 예제
- [doca-flow/](./applications/doca-flow/) — DOCA Flow (예정)
- [offloading/](./applications/offloading/) — MTU/OVS 오프로딩 설정

---

## 문서 작성 규칙

새 문서를 추가할 때:

| 문서 유형 | 디렉토리 |
|-----------|----------|
| 환경 설치/셋업 | `setup/` |
| 시스템 동작 설명 | `architecture/` |
| API/필드 레퍼런스 | `reference/` |
| DPF 작업 방법 | `dpf-guides/` |
| 장애 대응 | `troubleshooting/` |
| DOCA 앱별 문서 | `applications/<app>/` |

- 파일명: `kebab-case.md` (공백/특수문자 금지)
- 링크: 상대 경로 사용 (`./setup/overview.md`)
- mermaid 다이어그램: 이해를 돕는 핵심 지점에만 (1–3개/문서 권장)
- 외부 PDF/대용량 참조: `official_docs/`는 `.gitignore` 처리됨 (커밋 금지)
