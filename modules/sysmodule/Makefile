# Copyright (C) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
ifeq ($(KERNEL_SRC),)
# KERNEL_SRC := android13-5.15-167/out/android13-5.15/common
KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
endif

THIS_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
M := $(dir $(THIS_MAKEFILE))
		
ifneq ($(KERNELRELEASE),)
	# call from kernel build system
	obj-m := sysmodule.o

else

modules modules_install clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) EXTRA_CFLAGS="$(EXTRA_CFLAGS)" $(@)

endif

insmod:
	@sudo dmesg -c
	@sudo insmod sysmodule.ko
	@sudo dmesg | tail -n 20

rmmod:
	@sudo rmmod sysmodule.ko
	@sudo dmesg -c

test:
	@echo "查看原来的 kptr_restrict 和 sysmodule/status"
	@cat /proc/sys/kernel/kptr_restrict
	@cat /sys/kernel/sysmodule/status
	@tail -10 /proc/kallsyms
	@echo "设置 sysmodule/status 为 2"
	sudo bash -c "echo 2 > /sys/kernel/sysmodule/status"
	@cat /proc/sys/kernel/kptr_restrict
	@cat /sys/kernel/sysmodule/status
	@tail -10 /proc/kallsyms
	@echo "查看 sysmodule/status 为 0"
	sudo bash -c "echo 0 > /sys/kernel/sysmodule/status"
	@cat /proc/sys/kernel/kptr_restrict
	@cat /sys/kernel/sysmodule/status
	@sudo systemd-run -t --property=AmbientCapabilities=CAP_SYSLOG /bin/bash -c "tail -10 /proc/kallsyms"
