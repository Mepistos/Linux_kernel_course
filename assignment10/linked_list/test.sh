#!/bin/bash

echo "spinlock"
sudo insmod ./Spinlock/src.ko
sleep 5
sudo rmmod src

echo "mutex"
sudo insmod ./Mutex/src.ko
sleep 5
sudo rmmod src

echo "RW_semaphore"
sudo insmod ./RW_semaphore/src.ko
sleep 5
sudo rmmod src

