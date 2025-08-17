# Copyright (C) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
ifeq ($(KERNEL_SRC),)
# KERNEL_SRC := android13-5.15-167/out/android13-5.15/common
KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
endif

THIS_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
M := $(dir $(THIS_MAKEFILE))
		
ifneq ($(KERNELRELEASE),)
	# call from kernel build system
	obj-m := proccpuinfo.o

else

modules modules_install clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) EXTRA_CFLAGS="$(EXTRA_CFLAGS)" $(@)

endif

insmod:
	@sudo dmesg -c
	@sudo insmod proccpuinfo.ko
	@sudo dmesg | tail -n 20

rmmod:
	@sudo rmmod proccpuinfo.ko
	@sudo dmesg -c

test:
	@cc mountcpuinfo.c -o mountcpuinfo
	sudo ./mountcpuinfo /proc/fake_cpuinfo "/bin/cat /proc/cpuinfo"
	@rm -f mountcpuinfo
