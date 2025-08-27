MODULE_NAME := minisu
obj-m := $(MODULE_NAME).o

ccflags-y += -Wno-declaration-after-statement
ccflags-y += -Wno-unused-variable
ccflags-y += -Wno-int-conversion
ccflags-y += -Wno-unused-result
ccflags-y += -Wno-unused-function
ccflags-y += -Wno-builtin-macro-redefined -U__FILE__ -D__FILE__='""'

ifeq ($(KERNEL_SRC),)
# KERNEL_SRC := android13-5.15-167/out/android13-5.15/common
KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
endif

KDIR := $(KERNEL_SRC)
MDIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

$(info -- KDIR: $(KDIR))
$(info -- MDIR: $(MDIR))

all: modules test

modules:
	$(MAKE) -C $(KDIR) M=$(MDIR) modules

insmod:
	@sudo dmesg -c
	@sudo insmod minisu.ko

rmmod:
	@sudo rmmod minisu.ko
	@sudo dmesg -c

test:
	gcc -o test test.c
	timeout 10s ./test
	rm test

compdb:
	python3 $(MDIR)/.vscode/generate_compdb.py -O $(KDIR) $(MDIR)
clean:
	make -C $(KDIR) M=$(MDIR) clean

.PHONY: all modules clean install
