# Copyright (C) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
ifeq ($(KERNEL_SRC),)
# KERNEL_SRC := android13-5.15-167/out/android13-5.15/common
KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
endif

THIS_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
M := $(dir $(THIS_MAKEFILE))
		
ifneq ($(KERNELRELEASE),)
	# call from kernel build system
	obj-m := uprobemod.o

else

modules modules_install clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) EXTRA_CFLAGS="$(EXTRA_CFLAGS)" $(@)

endif

insmod:
	@sudo dmesg -c
	@sudo insmod uprobemod.ko
	@sudo dmesg | tail -n 20

rmmod:
	@sudo rmmod uprobemod.ko
	@sudo dmesg -c

test:
	@echo "Testing uprobe with default parameters..."
	bash -c "cat /proc/self/cmdline > /dev/null"
	@sudo dmesg -c

test-uid:
	@echo "Testing uprobe with UID filtering (current user)..."
	@sudo rmmod uprobemod 2>/dev/null || true
	@sudo insmod uprobemod.ko target_uid=$(shell id -u)
	@./test_openat || cat /proc/self/comm > /dev/null
	@sudo dmesg | grep "uprobes:" | tail -5
	@sudo rmmod uprobemod

test-all-users:
	@echo "Testing uprobe for all users..."
	@sudo rmmod uprobemod 2>/dev/null || true
	@sudo insmod uprobemod.ko target_uid=-1
	@./test_openat || cat /proc/self/comm > /dev/null
	@sudo dmesg | grep "uprobes:" | tail -5
	@sudo rmmod uprobemod
