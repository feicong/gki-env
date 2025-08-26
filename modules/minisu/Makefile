obj-m += minisu.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all: modules test

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

test: test.c
	gcc -o test test.c

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f test

install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

.PHONY: all modules clean install
