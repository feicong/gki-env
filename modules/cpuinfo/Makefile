# Copyright (C) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
obj-m += cpuinfo.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
