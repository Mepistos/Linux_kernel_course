# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the linux ext4-filesystem routines.
#

obj-m += ticket_locks.o

KDIR	:= /lib/modules/$(shell uname -r)/build
PWD	:= $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	rm *.mod.*
	rm *.ko
	rm *.o
