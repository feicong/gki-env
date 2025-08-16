// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/ebpf-course)
#include <linux/init.h>
#include <linux/module.h>

static int __init hello_init(void) {
    pr_info("Hello, kernel!\n");
    return 0;
}

static void __exit hello_exit(void) {
    pr_info("Goodbye, kernel!\n");
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("fei_cong");
MODULE_DESCRIPTION("Simple hello module");
