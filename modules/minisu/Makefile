# MiniSU Makefile
# 用于编译MiniSU内核模块和测试程序

obj-m += minisu.o

# 内核源码路径，需要根据实际情况修改
KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

# 测试程序
TEST_PROGRAMS = test_minisu test_su_compat

# 编译目标
all: module tests

# 编译内核模块
module:
	make -C $(KERNEL_SRC) M=$(PWD) modules

# 编译测试程序
tests: $(TEST_PROGRAMS)

test_minisu: test_minisu.c
	gcc -o test_minisu test_minisu.c

test_su_compat: test_su_compat.c
	gcc -o test_su_compat test_su_compat.c

# 清理目标
clean:
	make -C $(KERNEL_SRC) M=$(PWD) clean
	rm -f $(TEST_PROGRAMS)

# 安装模块
install:
	make -C $(KERNEL_SRC) M=$(PWD) modules_install

# 加载模块
load:
	sudo insmod minisu.ko

# 卸载模块
unload:
	sudo rmmod minisu

# 查看模块信息
info:
	modinfo minisu.ko

# 查看内核日志
log:
	dmesg | tail -20

# 运行基本测试
test: tests
	@echo "=== Running MiniSU tests ==="
	sudo -u "#2000" ./test_minisu || sudo -u "#1000" ./test_minisu
	@echo ""
	@echo "=== Running su compatibility tests ==="
	sudo -u "#2000" ./test_su_compat || sudo -u "#1000" ./test_su_compat

.PHONY: all module tests clean install load unload info log test
