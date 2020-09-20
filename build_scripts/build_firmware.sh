#!/bin/bash

# This script creates our custom firmware image
# based on Analog Devices plutosdr-fw

# Required plutosdr-fw version
PLUTOSDR_FW_TAG="v0.32"

# Stop on the first error
set -e

# Apply linux PREEMPT_RT patch
linux_rt_patch() {
  echo "Applying PREEMPT_RT kernel patch..."
  cd $BUILD_DIR || exit 1
  wget https://mirrors.edge.kernel.org/pub/linux/kernel/projects/rt/4.19/older/patch-4.19-rt1.patch.gz
  gunzip patch-4.19-rt1.patch.gz
  cd $PLUTOSDR_FW_DIR/linux || exit 1
  patch -p1 < $BUILD_DIR/patch-4.19-rt1.patch

  # Configure RT kernel
  KERNEL_CONFIG=$PLUTOSDR_FW_DIR/linux/arch/arm/configs/zynq_pluto_defconfig
  echo "CONFIG_TUN=y" >> $KERNEL_CONFIG
  echo "CONFIG_PREEMPT_RT_FULL=y" >> $KERNEL_CONFIG
}

# Changes to buildroot
config_buildroot() {
  BR_CONFIG=$PLUTOSDR_FW_DIR/buildroot/configs/zynq_pluto_defconfig
  echo 'BR2_ROOTFS_OVERLAY="'"$FW_OVERLAY"'"' >> $BR_CONFIG
  echo 'BR2_PACKAGE_LIBCONFIG=y' >> $BR_CONFIG
  echo 'BR2_PACKAGE_FFTW_SINGLE=y' >> $BR_CONFIG
  echo 'BR2_PACKAGE_TUNCTL=y' >> $BR_CONFIG
  echo 'BR2_PACKAGE_IPERF3=y' >> $BR_CONFIG
  echo 'BR2_PACKAGE_LLDPD=y' >> $BR_CONFIG
}

#Check if env variable for Xilinx tools exists
: "${VIVADO_SETTINGS?Need to set VIVADO_SETTINGS (e.g. /opt/Xilinx/Vivado/2019.1/settings64.sh)}"
source $VIVADO_SETTINGS || exit 1

SCRIPT_DIR=`pwd`/`dirname "$0"`
BUILD_DIR=$SCRIPT_DIR/build
PLUTOSDR_FW_DIR=$BUILD_DIR/plutosdr-fw
SYSROOT_DIR=$BUILD_DIR/pluto-custom.sysroot
FW_OVERLAY=$BUILD_DIR/rootfs-overlay
SRC_DIR=$SCRIPT_DIR/..

mkdir -p $BUILD_DIR
cd $BUILD_DIR || exit 1

# Read current build status
BUILD_STATUS=$BUILD_DIR/build_status
touch $BUILD_STATUS
source $BUILD_STATUS || exit 1

# Download the current plutosdr-fw image
if [[ $STATUS_GOT_FW != "y" ]]
then
  echo "plutosdr-fw repository not found. Downloading it..."
  git clone --recurse-submodules -j8 https://github.com/analogdevicesinc/plutosdr-fw.git
  echo "STATUS_GOT_FW=y" >> $BUILD_STATUS
else
  echo "plutosdr-fw already exists. Skipping git clone."
fi


cd $PLUTOSDR_FW_DIR || exit 1
if [[ $PLUTOSDR_FW_TAG != $(git describe --tags) ]];
then
  echo "Got wrong plutosdr-fw version. Checking out "PLUTOSDR_FW_TAG
  git pull origin master
  git checkout $PLUTOSDR_FW_VERSION
  git submodule update --recursive

else
  echo "plutosdr-fw version is ok."
fi

if [[ $STATUS_PATCHED_KERNEL != "y" ]]
then
  # Apply kernel patch and configure userspace
  echo "Patching Linux kernel and configure userspace"
  linux_rt_patch
  config_buildroot
  echo "STATUS_PATCHED_KERNEL=y" >> $BUILD_STATUS
else
  echo "Linux kernel seems to be already configured."
fi


## Init rootfs overlay structure
mkdir -p $FW_OVERLAY/usr/lib
mkdir -p $FW_OVERLAY/root
mkdir -p $FW_OVERLAY/etc/init.d

cd $SCRIPT_DIR || exit 1

## Create sysroot
# Delete libfec and libliquid status vars, to force a rebuild
# of those libs when we create the FW
sed -i '/STATUS_SYSROOT_GOT_LIBFEC/d' $BUILD_STATUS
sed -i '/STATUS_SYSROOT_GOT_LIBLIQUID/d' $BUILD_STATUS
./create_sysroot.sh || exit 1

# Copy libfec and liquid-dsp to rootfs overlay. These libs are not built with buildroot
# so we have to manually copy them.
cp $SYSROOT_DIR/usr/lib/libliquid.so $FW_OVERLAY/usr/lib/
cp $SYSROOT_DIR/usr/lib/libfec.so $FW_OVERLAY/usr/lib

## Build Main Apps
./build_standalone_binaries.sh
cd $BUILD_DIR || exit 1
cp basestation client client-calib $FW_OVERLAY/root/


# Create VERSION file
touch $FW_OVERLAY/root/VERSION
echo "hnap "`git describe --tags` > $FW_OVERLAY/root/VERSION
echo "plutosdr-fw "$PLUTOSDR_FW_TAG >> $FW_OVERLAY/root/VERSION

## Copy network init script to rootfs
cd $SRC_DIR || exit 1
echo "Copy network autoconfig script to rootfs..."
cp startup_scripts/* $FW_OVERLAY/etc/init.d/


## Copy FIR filter to rootfs
echo "Copy FIR filter coefficients to rootfs..."
cp AD9361_256kSPS.ftr $FW_OVERLAY/root/

## Copy the default configuration file to rootfs
echo "Copy config.txt to rootfs..."
cp config.txt $FW_OVERLAY/root/

cd $PLUTOSDR_FW_DIR || exit 1
make || exit 1
cp $PLUTOSDR_FW_DIR/build/pluto.frm $BUILD_DIR
echo "Created pluto.frm in $BUILD_DIR"