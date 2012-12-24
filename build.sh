#!/bin/bash
MAKEFLAGS='-j32'
CROSS_COMPILE=/arm/android/source/prebuilt/linux-x86/toolchain/arm-eabi-4.4.3/bin/arm-eabi-
ARCH=arm
KERNEL_BUILD_PATH=./

export KERNEL_BUILD_PATH ARCH CROSS_COMPILE MAKEFLAGS

TIME_START=`date +%s`;

echo === Building kernel ===

cd $KERNEL_BUILD_PATH
echo === CURRENT DIRECTORY: $PWD ===

#make clean
make prepare
make zImage

if [ $? -ne 0 ]; then
echo === Kernel build FAILED! ===
exit 1;
fi

make modules

if [ $? -ne 0 ]; then
echo ===Modules build FAILED! ===
exit 1;
fi

TIME_DONE=`date +%s`;
LAPSEDM=`echo "($TIME_DONE - $TIME_START)/60" | bc`
LAPSEDS=`echo "($TIME_DONE - $TIME_START)%60" | bc`
echo ''
echo == Build complete in $LAPSEDM m $LAPSEDS s ==

echo ''
echo == Change to images directory ==
cd /arm/android/newimg
KERNEL_IMAGE=${OLDPWD}/arch/arm/boot/zImage
echo ''
echo Kernel selected:${KERNEL_IMAGE}
./packet-boot.sh $KERNEL_IMAGE
echo 'Packet done.'

