# Copyright (C) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
ifeq ($(KERNEL_SRC),)
# KERNEL_SRC := android13-5.15-167/out/android13-5.15/common
KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
endif

THIS_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
M := $(dir $(THIS_MAKEFILE))
		
ifneq ($(KERNELRELEASE),)
	# call from kernel build system
	obj-m := cpuinfo.o

else

modules modules_install clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) EXTRA_CFLAGS="$(EXTRA_CFLAGS)" $(@)

endif

insmod:
	@sudo dmesg -c
	@sudo insmod cpuinfo.ko

rmmod:
	@sudo rmmod cpuinfo.ko
	@sudo dmesg -c

test:
	@cat /proc/cpuinfo
	@sudo dmesg -c
