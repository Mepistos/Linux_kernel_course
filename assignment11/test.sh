#!/bin/bash

sudo sysctl -a | grep "vm.dirty_ratio" 

sudo insmod pxt4/pxt4.ko
sudo mount -t pxt4 /dev/nvme0n2 /mnt/test
sudo fio fio-assignment11.fio
sudo umount /mnt/test
sudo rmmod pxt4

echo "20183848 kimseungjin"
