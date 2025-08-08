# Copyright (C) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
obj-m += diamorphine.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

insmod:
	sudo insmod diamorphine.ko

rmmod:
	sudo rmmod diamorphine.ko

