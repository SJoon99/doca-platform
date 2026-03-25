# DOCA Installation

## DOCA-Host Installation
**1. Install DOCA local repo package for host**
- [NVIDIA DOCA Downloads](https://developer.nvidia.com/networking/doca)
```bash
# unpack the deb repo
sudo dpkg -i <repo_file>

# run apt update
sudo apt update

# install doca-all
sudo apt install -y doca-all
 
# update the firmware
sudo apt install -y mlnx-fw-updater
``` 
![DOCA Profiles](https://docscontent.nvidia.com/dims4/default/d661c65/2147483647/strip/true/crop/816x896+0+0/resize/816x896!/format/webp/quality/90/?url=https%3A%2F%2Fk3-prod-nvidia-docs.s3.us-west-2.amazonaws.com%2Fbrightspot%2Fconfluence%2F00000198-0f6e-d020-a1fb-2f7f6c690000%2Fdoca%2Farchive%2Fdoca-300-vgt-update%2Fimages%2Fdownload%2Fattachments%2F3646645549%2FDOCA_Host_profiles_ROCE-version-1-modificationdate-1740639058163-api-v2.jpg)
> 참고: doca-all 설치 시 필요한 대부분의 구성 요소가 포함됨.

**2. Load the drivers**
```bash
sudo /etc/init.d/openibd restart
```

**3. Initialize MST**
```bash
sudo mst restart
```

## BF-Bundle Installation
- Bluefield에 Ubuntu 22.04 설치 +  BlueField and NIC firmware 업데이트
- 반드시 host side 설치 이후에 진행

**1. Installing Software on BlueField Using BF-Bundle**
- [NVIDIA DOCA Downloads](https://developer.nvidia.com/networking/doca)
```bash
# rshim activate
sudo systemctl restart rshim

sudo bfb-install --rshim rshim<N> --bfb <image_path.bfb>

# ex
host# sudo bfb-install --rshim rshim0 --bfb bf-bundle-2.7.0_24.04_ubuntu-22.04_prod.bfb --config bf.cfg
Pushing bfb 1.41GiB 0:02:02 [11.7MiB/s] [           <=>                                                                                                                                ]
Collecting BlueField booting status. Press Ctrl+C to stop
 INFO[PSC]: PSC BL1 START
 INFO[BL2]: start
 INFO[BL2]: boot mode (rshim)
 INFO[BL2]: VDDQ: 1120 mV
 INFO[BL2]: DDR POST passed
 INFO[BL2]: UEFI loaded
 INFO[BL31]: start
 INFO[BL31]: lifecycle GA Secured
 INFO[BL31]: VDD: 850 mV
 INFO[BL31]: runtime
 INFO[BL31]: MB ping success
 INFO[UEFI]: eMMC init
 INFO[UEFI]: eMMC probed
 INFO[UEFI]: UPVS valid
 INFO[UEFI]: PMI: updates started
 INFO[UEFI]: PMI: total updates: 1
 INFO[UEFI]: PMI: updates completed, status 0
 INFO[UEFI]: PCIe enum start
 INFO[UEFI]: PCIe enum end
 INFO[UEFI]: UEFI Secure Boot 
 INFO[UEFI]: PK configured
 INFO[UEFI]: Redfish enabled
 INFO[UEFI]: exit Boot Service
 INFO[MISC]: Found bf.cfg
 INFO[MISC]: Ubuntu installation started
 INFO[MISC]: Installing OS image
 INFO[MISC]: Changing the default password for user ubuntu
 INFO[MISC]: Ubuntu installation completed
 INFO[MISC]: Updating NIC firmware...
 INFO[MISC]: NIC firmware update done
 INFO[MISC]: Installation finished
```

### [설치 과정에서 일어날 ISSUE](02-issues.md)