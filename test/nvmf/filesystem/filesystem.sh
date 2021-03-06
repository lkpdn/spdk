#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

set -e

if ! rdma_nic_available; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter fs_test

# Start up the NVMf target in another process
$rootdir/app/nvmf_tgt/nvmf_tgt -c $testdir/../nvmf.conf &
nvmfpid=$!

trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

sleep 5

modprobe -v nvme-rdma

if [ -e "/dev/nvme-fabrics" ]; then
	chmod a+rw /dev/nvme-fabrics
fi

echo 'traddr='$NVMF_FIRST_TARGET_IP',transport=rdma,nr_io_queues=1,trsvcid='$NVMF_PORT',nqn=nqn.2016-06.io.spdk:cnode1' > /dev/nvme-fabrics

mkdir -p /mnt/device

devs=`lsblk -l -o NAME | grep nvme`

for dev in $devs; do
	timing_enter parted
	parted -s /dev/$dev mklabel msdos  mkpart primary '0%' '100%'
	timing_exit parted
	sleep 1

	for fstype in "ext4" "btrfs" "xfs"; do
		timing_enter $fstype
		if [ $fstype = ext4 ]; then
			force=-F
		else
			force=-f
		fi

		mkfs.${fstype} $force /dev/${dev}p1

		mount /dev/${dev}p1 /mnt/device
		touch /mnt/device/aaa
		sync
		rm /mnt/device/aaa
		sync
		umount /mnt/device
		timing_exit $fstype
	done
done

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $nvmfpid
timing_exit fs_test
