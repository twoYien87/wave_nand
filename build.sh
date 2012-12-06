#!/bin/bash

export ARCH=arm
export SUBARCH=arm
export CROSS_COMPILE=/home/demo/arm-eabi-4/arm-eabi-4.4.3/bin/arm-eabi-
rm /home/demo/Documents/android_kernel_samsung_wave-jellybean/usr/initramfs_data.cpio
make wave_s8500_defconfig
make -j2
