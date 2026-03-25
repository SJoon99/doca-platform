# **BlueField_DOCA_Example 폴더 상세 설명**

BlueField_DOCA_Example 폴더는 NVIDIA DOCA(Data Center Acceleration)를 BlueField SmartNIC에서 실습하기 위한 교육용 프로젝트

## **폴더 구조**

```bash
BlueField_DOCA_Example/
├── DOCA_Fabric-main/
│   ├── DOCA_Lab.ipynb           # 메인 교육 노트북
│   ├── README.md                # 프로젝트 설명
│   ├── .ipynb_checkpoints/      # Jupyter 체크포인트
│   ├── figs/                    # 이미지 자료
│   ├── lab_files/               # 실습 소스코드
│   └── definitions/             # 정의 파일
```

## **주요 구성 요소**

### 1. **DOCA_Lab.ipynb** (메인 교육 자료)

Jupyter Notebook 형식의 종합 교육 자료로, 다음을 포함합니다:

- **DOCA Compression 실습**: 파일 압축/해제 작업 구현
- **단계별 가이드**: 프로젝트 초기화부터 최종 실행까지
- **코드 편집 인터페이스**: 블록 단위로 코드 작성 및 테스트

### 2. **lab_files/** (핵심 프로젝트 파일)

프로젝트는 다음 파일들로 구성됩니다:

| 파일 | 역할 |
| --- | --- |
| **main.c** | 프로그램 진입점, 로깅 설정 |
| **logic.c** | 핵심 실행 로직: 초기화, 작업 설정, 제출 |
| **definitions.h** | 전역 상수, 열거형, 공유 데이터 구조 |
| **callbacks.c** | 압축 작업 완료/오류 콜백 함수 |
| **callbacks.h** | 콜백 함수 선언 |
| **utils.c** | 파일 읽기 등 유틸리티 함수 구현 |
| **utils.h** | 유틸리티 함수 선언 |
| **meson.build** | 빌드 규칙 및 DOCA 라이브러리 의존성 |
| **infile** | 압축할 입력 파일 |
| **decompressed** | 압축 해제 결과 출력 파일 |

### 3. **callbacks.c의 주요 함수**

```bash
// 압축 컨텍스트 상태 변화 처리
void compress_state_changed_callback(...)

// 해제 성공 처리
void decompress_deflate_completed_callback(...)

// 해제 실패 처리  
void decompress_deflate_error_callback(...)
```

## **실습 흐름**

### Step 4: 프로젝트 구현 단계

1. **Step 4.1**: main.c 작성 - 프로그램 진입점
2. **Step 4.2**: callbacks.c 작성 - 작업 콜백 정의
3. **Step 4.3**: logic.c 작성 - 메인 실행 로직
4. **Step 4.4**: meson.build 작성 - 빌드 설정
5. **Step 4.5**: definitions.h 작성 - 전역 정의
6. **Step 4.6**: utils.c 작성 - 유틸리티 함수
7. **Step 4.7**: utils.h 작성 - 함수 선언
8. **Step 4.8**: 코드 검증 및 빌드
9. **Step 4.9**: 출력 파일 검증

## **기술 스택**

- **DOCA SDK**: NVIDIA 데이터센터 가속화 프레임워크
- **Meson**: 빌드 시스템
- **C 프로그래밍**: 구현 언어
- **Jupyter Notebook**: 교육 플랫폼

## **실행 환경**

- **BlueField SmartNIC**: 하드웨어 가속기
- **Ubuntu 24 이미지**: 운영 체제 (Host)
- **DOCA 3.0.0**: SDK 버전
- **Fabric Testbed**: 원격 실행 환경

## **학습 목표**

1. DOCA API의 기본 사용법 습득
2. 콜백 기반 비동기 작업 처리
3. 메모리 관리 및 DMA 활용
4. BlueField 하드웨어 가속 활용
5. 실제 압축/해제 애플리케이션 개발