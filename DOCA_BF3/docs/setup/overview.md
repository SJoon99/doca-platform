## Bluefield-2 Spec
```bash
sudo bf-info
```

### 1. DTN-1 DPU
```bash
Firmware:
- ATF: v2.2(release):4.11.0-45-g1fb6dd744
- UEFI: 4.11.0-62-g6cd3c6dfb3
- BSP: 4.11.0.13611
- NIC Firmware: 24.45.1020
- BMC Firmware: 25.04-7
- CEC Firmware: 4.15

Drivers:
- mlnx-dpdk: 'MLNX_DPDK 22.11.2504.1.0'
- Kernel: 5.15.0-1065-bluefield

Tools:
- MFT: 4.32.0-120
- mstflint: 4.31.0-1

Storage:
- mlnx-libsnap 1.6.0-4
- mlnx-snap 3.8.0-10
- spdk 23.01.5-28
- virtio-net-controller 25.04.8-1

DOCA:
- doca-apsh-config 3.0.0058-1
- doca-bench 3.0.0058-1
- doca-caps 3.0.0058-1
- doca-comm-channel-admin 3.0.0058-1
- doca-devel 1-3.0.0058-1.25.04.0.6.1.0.bf.4.11.0.13611
- doca-devel-container 1-3.0.0058-1.25.04.0.6.1.0.bf.4.11.0.13611
- doca-devel-kernel 1-3.0.0058-1.25.04.0.6.1.0.bf.4.11.0.13611
- doca-devel-user 1-3.0.0058-1.25.04.0.6.1.0.bf.4.11.0.13611
- doca-dms 3.0.0058-1
- doca-eula 1.0.0-1
- doca-flow-tune 3.0.0058-1
- doca-openvswitch-common 3.0.0-0056-25.04-based-3.3.5
- doca-openvswitch-dev 3.0.0-0056-25.04-based-3.3.5
- doca-openvswitch-ipsec 3.0.0-0056-25.04-based-3.3.5
- doca-openvswitch-switch 3.0.0-0056-25.04-based-3.3.5
- doca-pcc-counters 3.0.0058-1
- doca-runtime 1-3.0.0058-1.25.04.0.6.1.0.bf.4.11.0.13611
- doca-runtime-container 1-3.0.0058-1.25.04.0.6.1.0.bf.4.11.0.13611
- doca-runtime-kernel 1-3.0.0058-1.25.04.0.6.1.0.bf.4.11.0.13611
- doca-runtime-user 1-3.0.0058-1.25.04.0.6.1.0.bf.4.11.0.13611
- doca-samples 3.0.0058-1
- doca-sdk-aes-gcm 3.0.0058-1
- doca-sdk-apsh 3.0.0058-1
- doca-sdk-argp 3.0.0058-1
- doca-sdk-comch 3.0.0058-1
- doca-sdk-common 3.0.0058-1
- doca-sdk-compress 3.0.0058-1
- doca-sdk-devemu 3.0.0058-1
- doca-sdk-dma 3.0.0058-1
- doca-sdk-dpa 3.0.0058-1
- doca-sdk-dpdk-bridge 3.0.0058-1
- doca-sdk-erasure-coding 3.0.0058-1
- doca-sdk-eth 3.0.0058-1
- doca-sdk-flow 3.0.0058-1
- doca-sdk-pcc 3.0.0058-1
- doca-sdk-rdma 3.0.0058-1
- doca-sdk-sha 3.0.0058-1
- doca-sdk-sta 3.0.0058-1
- doca-sdk-telemetry 3.0.0058-1
- doca-sdk-telemetry-exporter 3.0.0058-1
- doca-sdk-urom 3.0.0058-1
- doca-sha-offload-engine 3.0.0058-1
- doca-socket-relay 3.0.0058-1
- doca-sosreport 4.9.0-1
- doca-telemetry-utils 3.0.0058-1
- libdoca-sdk-aes-gcm-dev 3.0.0058-1
- libdoca-sdk-apsh-dev 3.0.0058-1
- libdoca-sdk-argp-dev 3.0.0058-1
- libdoca-sdk-comch-dev 3.0.0058-1
- libdoca-sdk-common-dev 3.0.0058-1
- libdoca-sdk-compress-dev 3.0.0058-1
- libdoca-sdk-devemu-dev 3.0.0058-1
- libdoca-sdk-dma-dev 3.0.0058-1
- libdoca-sdk-dpa-dev 3.0.0058-1
- libdoca-sdk-dpdk-bridge-dev 3.0.0058-1
- libdoca-sdk-erasure-coding-dev 3.0.0058-1
- libdoca-sdk-eth-dev 3.0.0058-1
- libdoca-sdk-flow-dev 3.0.0058-1
- libdoca-sdk-flow-trace 3.0.0058-1
- libdoca-sdk-pcc-dev 3.0.0058-1
- libdoca-sdk-rdma-dev 3.0.0058-1
- libdoca-sdk-sha-dev 3.0.0058-1
- libdoca-sdk-sta-dev 3.0.0058-1
- libdoca-sdk-telemetry-dev 3.0.0058-1
- libdoca-sdk-telemetry-exporter-dev 3.0.0058-1
- libdoca-sdk-urom-dev 3.0.0058-1
- python3-doca-openvswitch 3.0.0-0056-25.04-based-3.3.5
- collectx-clxapi: collectx-clxapi 1.21.1

FlexIO:
- dpa-gdbserver 25.04.2725
- dpa-stats 25.04.0169
- dpacc 1.11.0.6
- dpacc-extract 1.11.0.6
- flexio-samples 25.04.2725
- flexio-sdk 25.04.2725

SoC Platform:
- mmc-utils 0+git20191004.73d6c59-2

OFED:
- doca-openvswitch-common 3.0.0-0056-25.04-based-3.3.5
- doca-openvswitch-ipsec 3.0.0-0056-25.04-based-3.3.5
- doca-openvswitch-switch 3.0.0-0056-25.04-based-3.3.5
- dpcp 1.1.52-1.2504061
- ibacm 2501mlnx56-1.2504061
- ibutils2 2.1.1-0.22200.MLNX20250423.g91730569c.2504061
- ibverbs-providers:arm64 2501mlnx56-1.2504061
- ibverbs-utils 2501mlnx56-1.2504061
- infiniband-diags 2501mlnx56-1.2504061
- libibmad-dev:arm64 2501mlnx56-1.2504061
- libibmad5:arm64 2501mlnx56-1.2504061
- libibnetdisc5:arm64 2501mlnx56-1.2504061
- libibumad-dev:arm64 2501mlnx56-1.2504061
- libibumad3:arm64 2501mlnx56-1.2504061
- libibverbs-dev:arm64 2501mlnx56-1.2504061
- libibverbs1:arm64 2501mlnx56-1.2504061
- libopensm 5.23.00.MLNX20250423.ac516692-0.1.2504061
- libopensm-devel 5.23.00.MLNX20250423.ac516692-0.1.2504061
- librdmacm-dev:arm64 2501mlnx56-1.2504061
- librdmacm1:arm64 2501mlnx56-1.2504061
- libvma 9.8.71-1
- libvma-dev 9.8.71-1
- libvma-utils 9.8.71-1
- mlnx-dpdk 22.11.0-2504.1.0
- mlnx-dpdk-dev:arm64 22.11.0-2504.1.0
- mlnx-ethtool 6.11-1.2504061
- mlnx-iproute2 6.12.0-1.2504061
- mlnx-ofed-kernel-utils 25.04.OFED.25.04.0.6.1.1-1.bf.kver.5.15.0-1065-bluefield
- mlnx-tools 25.01-0.2504061
- openmpi 4.1.7rc1-1.20250428.6d9519e4c3.2504061
- opensm 5.23.00.MLNX20250423.ac516692-0.1.2504061
- perftest 25.04.0-0.84.g97da83e.2504061
- python3-pyverbs:arm64 2501mlnx56-1.2504061
- rdma-core 2501mlnx56-1.2504061
- rdmacm-utils 2501mlnx56-1.2504061
- srptools 2501mlnx56-1.2504061
- ucx 1.19.0-1.20250428.6ecd4e5ae.2504061
```

### 2. DTN-2 DPU
```bash
- libdoca-sdk-common-dev 2.9.2005-1
- libdoca-sdk-compress-dev 2.9.2005-1
- libdoca-sdk-devemu-dev 2.9.2005-1
- libdoca-sdk-dma-dev 2.9.2005-1
- libdoca-sdk-dpa-dev 2.9.2005-1
- libdoca-sdk-dpdk-bridge-dev 2.9.2005-1
- libdoca-sdk-erasure-coding-dev 2.9.2005-1
- libdoca-sdk-eth-dev 2.9.2005-1
- libdoca-sdk-flow-dev 2.9.2005-1
- libdoca-sdk-flow-trace 2.9.2005-1
- libdoca-sdk-pcc-dev 2.9.2005-1
- libdoca-sdk-rdma-dev 2.9.2005-1
- libdoca-sdk-sha-dev 2.9.2005-1
- libdoca-sdk-telemetry-dev 2.9.2005-1
- libdoca-sdk-telemetry-exporter-dev 2.9.2005-1
- libdoca-sdk-urom-dev 2.9.2005-1
- python3-doca-openvswitch 2.9.2-0010-25.02-based-3.3.3
- sfc-hbn 2.4.0-doca-2.9-3

FlexIO:
- dpa-gdbserver 24.10.2454
- dpa-stats 24.10.2407
- dpacc 1.9.0
- dpacc-extract 1.9.0
- dpaeumgmt 24.10.2407
- flexio-samples 24.10.2454
- flexio-sdk 24.10.2454

SoC Platform:
- mmc-utils 0+git20191004.73d6c59-2

OFED:
doca-openvswitch-common 2.9.2-0010-25.02-based-3.3.3
doca-openvswitch-ipsec 2.9.2-0010-25.02-based-3.3.3
doca-openvswitch-switch 2.9.2-0010-25.02-based-3.3.3
dpcp 1.1.50-1.2410068
ibacm 2410mlnx54-1.2410068
ibutils2 2.1.1-0.21902.MLNX20241029.g46cf6278.2410068
ibverbs-providers:arm64 2410mlnx54-1.2410068
ibverbs-utils 2410mlnx54-1.2410068
infiniband-diags 2410mlnx54-1.2410068
libibmad-dev:arm64 2410mlnx54-1.2410068
libibmad5:arm64 2410mlnx54-1.2410068
libibnetdisc5:arm64 2410mlnx54-1.2410068
libibumad-dev:arm64 2410mlnx54-1.2410068
libibumad3:arm64 2410mlnx54-1.2410068
libibverbs-dev:arm64 2410mlnx54-1.2410068
libibverbs1:arm64 2410mlnx54-1.2410068
libopensm 5.21.0.MLNX20241126.d9aa3dff-0.1.2410114
libopensm-devel 5.21.0.MLNX20241126.d9aa3dff-0.1.2410114
librdmacm-dev:arm64 2410mlnx54-1.2410068
librdmacm1:arm64 2410mlnx54-1.2410068
libvma 9.8.60-1
libvma-dev 9.8.60-1
libvma-utils 9.8.60-1
mlnx-dpdk 22.11.0-2410.3.0
mlnx-dpdk-dev:arm64 22.11.0-2410.3.0
mlnx-ethtool 6.9-1.2410068
mlnx-iproute2 6.10.0-1.2410218
mlnx-ofed-kernel-utils 24.10.OFED.24.10.2.1.8.1-1.bf.kver.5.15.0-1060-bluefield
mlnx-tools 24.10-0.2410068
openmpi 4.1.7rc1-1.2410218
opensm 5.21.0.MLNX20241126.d9aa3dff-0.1.2410114
perftest 24.10.0-0.65.g9093bae.2410068
python3-pyverbs:arm64 2410mlnx54-1.2410068
rdma-core 2410mlnx54-1.2410068
rdmacm-utils 2410mlnx54-1.2410068
srptools 2410mlnx54-1.2410068
ucx 1.18.0-1.2410068
```
### 3. DTN-4 DPU
```bash
Versions:
- ATF: v2.2(release):4.10.0-41-gea03e14b3
- UEFI: 4.10.0-81-gb011ce66f6
- BSP: 4.10.0.13520
- NIC Firmware: 24.44.1036
- Kernel: 5.15.0-1060-bluefield
- DOCA Base (OFED): 25.01-0.6.0
- MFT: 4.31.0-149
- mstflint: 4.29.0-1
- mlnx-dpdk:  'MLNX_DPDK 22.11.2501.1.0'
- mlx-regex: 
- collectx-clxapi: collectx-clxapi 1.20.2
- libvma: libvma 9.8.60-1
dpkg-query: no packages found matching libxlio
- 
- dpcp 1.1.52-1.2501060

Storage:
- mlnx-libsnap 1.6.0-3
- mlnx-snap 3.8.0-8
- spdk 23.01.5-27
- virtio-net-controller 25.01.9-1

DOCA:
- doca-apsh-config 2.10.0087-1
- doca-bench 2.10.0087-1
- doca-caps 2.10.0087-1
- doca-comm-channel-admin 2.10.0087-1
- doca-devel 1-2.10.0087-1.25.01.0.6.0.0.bf.4.10.0.13520
- doca-devel-container 1-2.10.0087-1.25.01.0.6.0.0.bf.4.10.0.13520
- doca-devel-kernel 1-2.10.0087-1.25.01.0.6.0.0.bf.4.10.0.13520
- doca-devel-user 1-2.10.0087-1.25.01.0.6.0.0.bf.4.10.0.13520
- doca-dms 2.10.0087-1
- doca-flow-tune 2.10.0087-1
- doca-openvswitch-common 2.10.0-0056-25.01-based-3.3.4
- doca-openvswitch-dev 2.10.0-0056-25.01-based-3.3.4
- doca-openvswitch-ipsec 2.10.0-0056-25.01-based-3.3.4
- doca-openvswitch-switch 2.10.0-0056-25.01-based-3.3.4
- doca-pcc-counters 2.10.0087-1
- doca-runtime 1-2.10.0087-1.25.01.0.6.0.0.bf.4.10.0.13520
- doca-runtime-container 1-2.10.0087-1.25.01.0.6.0.0.bf.4.10.0.13520
- doca-runtime-kernel 1-2.10.0087-1.25.01.0.6.0.0.bf.4.10.0.13520
- doca-runtime-user 1-2.10.0087-1.25.01.0.6.0.0.bf.4.10.0.13520
- doca-samples 2.10.0087-1
- doca-sdk-aes-gcm 2.10.0087-1
- doca-sdk-apsh 2.10.0087-1
- doca-sdk-argp 2.10.0087-1
- doca-sdk-comch 2.10.0087-1
- doca-sdk-common 2.10.0087-1
- doca-sdk-compress 2.10.0087-1
- doca-sdk-devemu 2.10.0087-1
- doca-sdk-dma 2.10.0087-1
- doca-sdk-dpa 2.10.0087-1
- doca-sdk-dpdk-bridge 2.10.0087-1
- doca-sdk-erasure-coding 2.10.0087-1
- doca-sdk-eth 2.10.0087-1
- doca-sdk-flow 2.10.0087-1
- doca-sdk-pcc 2.10.0087-1
- doca-sdk-rdma 2.10.0087-1
- doca-sdk-sha 2.10.0087-1
- doca-sdk-telemetry 2.10.0087-1
- doca-sdk-telemetry-exporter 2.10.0087-1
- doca-sdk-urom 2.10.0087-1
- doca-sha-offload-engine 2.10.0087-1
- doca-socket-relay 2.10.0087-1
- doca-sosreport 4.8.1
- hbn-repo 2.5.0-doca.bf
- libdoca-sdk-aes-gcm-dev 2.10.0087-1
- libdoca-sdk-apsh-dev 2.10.0087-1
- libdoca-sdk-argp-dev 2.10.0087-1
- libdoca-sdk-comch-dev 2.10.0087-1
- libdoca-sdk-common-dev 2.10.0087-1
- libdoca-sdk-compress-dev 2.10.0087-1
- libdoca-sdk-devemu-dev 2.10.0087-1
- libdoca-sdk-dma-dev 2.10.0087-1
- libdoca-sdk-dpa-dev 2.10.0087-1
- libdoca-sdk-dpdk-bridge-dev 2.10.0087-1
- libdoca-sdk-erasure-coding-dev 2.10.0087-1
- libdoca-sdk-eth-dev 2.10.0087-1
- libdoca-sdk-flow-dev 2.10.0087-1
- libdoca-sdk-flow-trace 2.10.0087-1
- libdoca-sdk-pcc-dev 2.10.0087-1
- libdoca-sdk-rdma-dev 2.10.0087-1
- libdoca-sdk-sha-dev 2.10.0087-1
- libdoca-sdk-telemetry-dev 2.10.0087-1
- libdoca-sdk-telemetry-exporter-dev 2.10.0087-1
- libdoca-sdk-urom-dev 2.10.0087-1
- python3-doca-openvswitch 2.10.0-0056-25.01-based-3.3.4
- sfc-hbn 2.5.0-doca-2.10-4

FlexIO:
- dpa-gdbserver 25.01.2608
- dpa-stats 25.01.0050
- dpacc 1.10.0
- dpacc-extract 1.10.0
- dpaeumgmt 25.01.0050
- flexio-samples 25.01.2608
- flexio-sdk 25.01.2608

SoC Platform:
- mmc-utils 0+git20191004.73d6c59-2

OFED:
doca-openvswitch-common 2.10.0-0056-25.01-based-3.3.4
doca-openvswitch-ipsec 2.10.0-0056-25.01-based-3.3.4
doca-openvswitch-switch 2.10.0-0056-25.01-based-3.3.4
dpcp 1.1.52-1.2501060
ibacm 2501mlnx56-1.2501060
ibutils2 2.1.1-0.22100.MLNX20250123.g26a775d8.2501060
ibverbs-providers:arm64 2501mlnx56-1.2501060
ibverbs-utils 2501mlnx56-1.2501060
infiniband-diags 2501mlnx56-1.2501060
libibmad-dev:arm64 2501mlnx56-1.2501060
libibmad5:arm64 2501mlnx56-1.2501060
libibnetdisc5:arm64 2501mlnx56-1.2501060
libibumad-dev:arm64 2501mlnx56-1.2501060
libibumad3:arm64 2501mlnx56-1.2501060
libibverbs-dev:arm64 2501mlnx56-1.2501060
libibverbs1:arm64 2501mlnx56-1.2501060
libopensm 5.22.11.MLNX20250130.12243119-0.1.2501060
libopensm-devel 5.22.11.MLNX20250130.12243119-0.1.2501060
librdmacm-dev:arm64 2501mlnx56-1.2501060
librdmacm1:arm64 2501mlnx56-1.2501060
libvma 9.8.60-1
libvma-dev 9.8.60-1
libvma-utils 9.8.60-1
mlnx-dpdk 22.11.0-2501.1.0
mlnx-dpdk-dev:arm64 22.11.0-2501.1.0
mlnx-ethtool 6.11-1.2501060
mlnx-iproute2 6.12.0-1.2501060
mlnx-ofed-kernel-utils 25.01.OFED.25.01.0.6.0.1-1.bf.kver.5.15.0-1060-bluefield
mlnx-tools 25.01-0.2501060
openmpi 4.1.7rc1-1.2501060
opensm 5.22.11.MLNX20250130.12243119-0.1.2501060
perftest 25.01.0-0.70.g759a5c5.2501060
python3-pyverbs:arm64 2501mlnx56-1.2501060
rdma-core 2501mlnx56-1.2501060
rdmacm-utils 2501mlnx56-1.2501060
srptools 2501mlnx56-1.2501060
ucx 1.18.0-1.2501060
```
