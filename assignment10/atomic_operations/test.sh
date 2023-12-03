#!/bin/bash

arg1=$1

sudo dmesg -c
echo "$arg1"
sudo insmod ${arg1}src.ko
sleep 2
sudo rmmod src

echo "20183848 kim seungjin"
dmesg
