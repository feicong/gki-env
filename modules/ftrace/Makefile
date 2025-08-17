# Copyright (C) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
obj-m += ftrace.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

insmod:
	@sudo dmesg -c
	sudo insmod ftrace.ko

rmmod:
	sudo rmmod ftrace.ko
	@sudo dmesg -c

test:
	@ls
	@sudo dmesg -c
