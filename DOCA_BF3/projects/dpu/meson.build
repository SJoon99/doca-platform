# dpu/meson.build
project('dpu_flow_rss_meta', 'c', 'cpp')

# 샘플과 동일하게 실험 API 허용
add_project_arguments('-D', 'DOCA_ALLOW_EXPERIMENTAL_API', language: ['c', 'cpp'])

# 필요 라이브러리 (pkg-config로 탐색)
doca_common = dependency('doca-common', required: true)
doca_flow   = dependency('doca-flow',   required: true)
doca_argp   = dependency('doca-argp',   required: true)
dpdk        = dependency('libdpdk',     required: true)
openssl     = dependency('openssl',     required: true)

deps = [doca_common, doca_flow, doca_argp, dpdk, openssl]

# 소스 파일 
srcs = [
  'main.c',
  'dataplaneV5(blockchain).c',
]

# 포함 경로 
incdirs = [
  include_directories('.'),
  # 샘플의 공통 헤더를 재사용한다면 아래 주석을 해제
  include_directories('/opt/mellanox/doca/samples/doca_flow'),
  include_directories('/opt/mellanox/doca/applications/common'),
]

# 샘플 공통  
srcs += [
	'/opt/mellanox/doca/samples/doca_flow/flow_common.c',
	'/opt/mellanox/doca/applications/common/dpdk_utils.c',
]

# 경고 방지 (샘플과 동일)
c_extra_args = ['-Wno-missing-braces']

executable(
  'dpu_transfer',     
  srcs,
  c_args: c_extra_args,
  dependencies: deps,
  include_directories: incdirs,
  install: false
)
