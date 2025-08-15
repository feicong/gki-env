/*
 * syscall_ftrace_getdents64.c
 *
 * 使用 ftrace 在系统调用入口监测 getdents64 输出文件名（只读/仅记录）
 *
 * License: GPL
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/ftrace.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/uidgid.h>
#include <linux/version.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/dirent.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("非虫");
MODULE_DESCRIPTION("使用 ftrace 监测 getdents64 文件名（只读）");

/* 模块参数 */
static bool hook_getdents64 = true;
module_param(hook_getdents64, bool, 0644);
MODULE_PARM_DESC(hook_getdents64, "是否 hook getdents64");

/* ftrace ops */
static struct ftrace_ops ops_getdents64;

/* kallsyms_lookup_name via kprobe */
static unsigned long lookup_name(const char *name)
{
    struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
    typedef unsigned long (*kallsyms_lookup_name_t)(const char *);
    kallsyms_lookup_name_t kallsyms_lookup_name_func;
    int ret;
    unsigned long addr = 0;

    ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_alert("register_kprobe failed: %d\n", ret);
        return 0;
    }
    kallsyms_lookup_name_func = (kallsyms_lookup_name_t)kp.addr;
    unregister_kprobe(&kp);

    if (!kallsyms_lookup_name_func) {
        pr_alert("kallsyms_lookup_name not found\n");
        return 0;
    }

    addr = kallsyms_lookup_name_func(name);
    pr_info("lookup_name: %s @ %px\n", name, (void *)addr);
    return addr;
}

/* 兼容宏 */
#ifndef FTRACE_OPS_FL_RECURSION_SAFE
#define MONITOR_FTRACE_FLAGS (FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_RECURSION | FTRACE_OPS_FL_IPMODIFY)
#else
#define MONITOR_FTRACE_FLAGS (FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_RECURSION_SAFE | FTRACE_OPS_FL_IPMODIFY)
#endif

/* ftrace 回调：打印 getdents64 的文件名 */
static void notrace monitor_getdents64_thunk(unsigned long ip, unsigned long parent_ip,
                                             struct ftrace_ops *ops, struct ftrace_regs *fregs)
{
    struct pt_regs *regs = ftrace_get_regs(fregs);
    unsigned long fd = 0;
    struct linux_dirent64 __user *dirent_user = NULL;
    unsigned int count = 0;
    char *kbuf = NULL;

#if IS_ENABLED(CONFIG_X86) || IS_ENABLED(CONFIG_X86_64)
    fd = regs->di;
    dirent_user = (struct linux_dirent64 __user *)regs->si;
    count = regs->dx;
#elif IS_ENABLED(CONFIG_ARM64)
    fd = regs->regs[0];
    dirent_user = (struct linux_dirent64 __user *)regs->regs[1];
    count = regs->regs[2];
#else
    return;
#endif

    /* 总是打印 hook 被调用的信息，但对 count=0 做优化 */
    if (count == 0) {
        printk(KERN_INFO "monitor_getdents64_thunk called: pid=%d uid=%u fd=%lu count=0\n",
               current->pid, __kuid_val(current_uid()), fd);
        return;
    }

    printk(KERN_INFO "monitor_getdents64_thunk called: pid=%d uid=%u fd=%lu count=%u\n",
           current->pid, __kuid_val(current_uid()), fd, count);

    if (!dirent_user)
        return;

    kbuf = kmalloc(count, GFP_ATOMIC);
    if (!kbuf) {
        pr_alert("kmalloc(%u) failed\n", count);
        return;
    }

    if (copy_from_user(kbuf, dirent_user, count)) {
        pr_alert("copy_from_user failed\n");
        kfree(kbuf);
        return;
    }

    /* 遍历 dirent64 结构 */
    {
        unsigned long offset = 0;
        while (offset < count) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(kbuf + offset);
            char name[256];
            size_t nlen = d->d_reclen - offsetof(struct linux_dirent64, d_name);
            if (nlen > sizeof(name) - 1)
                nlen = sizeof(name) - 1;
            memcpy(name, d->d_name, nlen);
            name[nlen] = '\0';

            pr_alert("getdents64 pid=%d uid=%u fd=%lu name=%s\n",
                     current->pid, __kuid_val(current_uid()), fd, name);

            if (d->d_reclen == 0)
                break;
            offset += d->d_reclen;
        }
    }

    kfree(kbuf);
}

/* 安装 ftrace hook */
static int install_monitor_hook(struct ftrace_ops *ops, unsigned long func_addr)
{
    int ret;

    if (!func_addr || !ops)
        return -EINVAL;

    ret = ftrace_set_filter_ip(ops, func_addr, 0, 0);
    if (ret) {
        pr_alert("ftrace_set_filter_ip failed: %d\n", ret);
        return ret;
    }

    ret = register_ftrace_function(ops);
    if (ret) {
        pr_alert("register_ftrace_function failed: %d\n", ret);
        ftrace_set_filter_ip(ops, func_addr, 1, 0);
        return ret;
    }

    pr_info("ftrace hook installed @ %px\n", (void *)func_addr);
    return 0;
}

static void remove_monitor_hook(struct ftrace_ops *ops, unsigned long func_addr)
{
    if (!ops || !func_addr)
        return;

    unregister_ftrace_function(ops);
    ftrace_set_filter_ip(ops, func_addr, 1, 0);
    pr_info("ftrace hook removed @ %px\n", (void *)func_addr);
}

#if IS_ENABLED(CONFIG_X86) || IS_ENABLED(CONFIG_X86_64)
#define NAME64 "__x64_sys_getdents64"
#elif IS_ENABLED(CONFIG_ARM64)
#define NAME64 "__arm64_sys_getdents64"
#else
#define NAME64 "__x64_sys_getdents64"
#endif

/* 初始化与清理 */
static int __init ftrace_getdents64_init(void)
{
    unsigned long addr_getdents64 = 0;

    pr_info("ftrace_getdents64: init\n");

    memset(&ops_getdents64, 0, sizeof(ops_getdents64));
    ops_getdents64.func = monitor_getdents64_thunk;
    ops_getdents64.flags = MONITOR_FTRACE_FLAGS;

    if (hook_getdents64) {
        const char *names64 = NAME64;
        addr_getdents64 = lookup_name(names64);
        if (!addr_getdents64) {
            pr_warn("getdents64 symbol not found\n");
            return -ENOENT;
        }

        pr_info("found getdents64 symbol '%s' @ %px\n", names64, (void *)addr_getdents64);

        int ret = install_monitor_hook(&ops_getdents64, addr_getdents64);
        pr_info("install_monitor_hook returned %d\n", ret);
    }

    return 0;
}

static void __exit ftrace_getdents64_exit(void)
{
    if (ops_getdents64.func) {
        unsigned long addr = lookup_name(NAME64);
        pr_info("removing hook @ %px\n", (void *)addr);
        remove_monitor_hook(&ops_getdents64, addr);
    }

    pr_info("ftrace_getdents64: exit\n");
}

module_init(ftrace_getdents64_init);
module_exit(ftrace_getdents64_exit);

// FIXME: monitor_getdents64_thunk called: pid=4331 uid=1000 fd=18446658827301191512 count=0
