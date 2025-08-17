// Copyright (C) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/version.h>
#include <linux/kprobes.h>
#include <linux/uidgid.h>
#include <linux/ftrace.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/errno.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("feicong <fei_cong@hotmail.com>");
MODULE_DESCRIPTION("使用 ftrace hook /proc/cpuinfo");

/* 模块参数 */
static unsigned int target_uid_val = 1000;
static int target_pid_val = 0;
module_param(target_uid_val, uint, 0644);
MODULE_PARM_DESC(target_uid_val, "提供自定义 cpuinfo 的目标 UID");
module_param(target_pid_val, int, 0644);
MODULE_PARM_DESC(target_pid_val, "提供自定义 cpuinfo 的目标 PID (0 表示不检查特定 PID)");

/* 自定义输出（按架构选择） */
#ifdef CONFIG_X86_64
static const char *custom_cpuinfo_output =
    "processor\t: 0\n"
    "vendor_id\t: MyHookedCPU-x86_64\n"
    "cpu family\t: 6\n"
    "model\t\t: 1\n"
    "model name\t: Hooked Intel CPU (Dynamic Mode)\n"
    "stepping\t: 1\n"
    "microcode\t: 0x1\n"
    "cpu MHz\t\t: 3000.000\n"
    "cache size\t: 1024 KB\n"
    "bogomips\t: 6000.00\n"
    "flags\t\t: hooked_flag dynamic_mode\n"
    "\n";
#elif defined(CONFIG_ARM64)
static const char *custom_cpuinfo_output =
    "processor\t: 0\n"
    "BogoMIPS\t: 200.00\n"
    "Features\t: hooked_feat_arm64 dynamic_mode\n"
    "CPU implementer\t: 0x48\n"
    "CPU architecture: 8\n"
    "CPU variant\t: 0x1\n"
    "CPU part\t: 0xd42\n"
    "CPU revision\t: 0\n"
    "Hardware\t: Hooked ARM64 (Dynamic Mode)\n"
    "\n";
#else
static const char *custom_cpuinfo_output =
    "processor\t: 0\n"
    "vendor_id\t: MyHookedCPU-Generic\n"
    "model name\t: Hooked Generic CPU (Dynamic Mode)\n"
    "cpu MHz\t\t: 2000.000\n"
    "Hardware\t: Hooked Unknown Platform (Dynamic Mode)\n"
    "\n";
#endif

/* 原始 show 指针（仅用于日志/恢复时参考） */
static unsigned long orig_show_addr = 0;

/* kallsyms_lookup_name 获取（通过 kprobe） */
static unsigned long lookup_name(const char *name)
{
    struct kprobe kp = {.symbol_name = "kallsyms_lookup_name"};
    typedef unsigned long (*kallsyms_lookup_name_t)(const char *);
    kallsyms_lookup_name_t kallsyms_lookup_name_func;
    int ret;

    ret = register_kprobe(&kp);
    if (ret < 0)
    {
        pr_err("cpuinfo_ftrace: 无法注册 kprobe 查找 kallsyms_lookup_name, ret=%d\n", ret);
        return 0;
    }
    kallsyms_lookup_name_func = (kallsyms_lookup_name_t)kp.addr;
    unregister_kprobe(&kp);

    if (!kallsyms_lookup_name_func)
    {
        pr_err("cpuinfo_ftrace: kallsyms_lookup_name 获取失败\n");
        return 0;
    }
    return kallsyms_lookup_name_func(name);
}

/* custom show：与 seq_file 的 show() 原型一致：int fn(struct seq_file *, void *) */
static int custom_show_cpuinfo(struct seq_file *m, void *v)
{
    kuid_t current_kuid = current_uid();
    pid_t current_pid = current->pid;
    uid_t uid = __kuid_val(current_kuid);
    bool should_override = false;

    pr_debug("cpuinfo_ftrace: Hook 激活 PID=%d UID=%u (target UID=%u, PID=%d)\n",
             current_pid, uid, target_uid_val, target_pid_val);

    if (target_uid_val != 0 && uid == target_uid_val)
        should_override = true;
    if (!should_override && target_pid_val != 0 && current_pid == target_pid_val)
        should_override = true;

    if (should_override)
    {
        pr_info("cpuinfo_ftrace: 为 PID=%d UID=%u 提供自定义 CPU 信息\n", current_pid, uid);
        seq_printf(m, "%s", custom_cpuinfo_output);
        return 0;
    }

    /*
     * 注意：这里我们没有尝试复杂地调用原始 show（那会涉及到避免 ftrace 递归的问题）。
     * 因此未匹配时我们直接返回 0（不输出）；如果你需要在未匹配时调用原始 show，
     * 我可以把安装逻辑改成 "只 hook cpuinfo_op 的调用者" 或在 ftrace 回调中更精细地处理（更复杂）。
     */
    return 0;
}


#if LINUX_VERSION_CODE < KERNEL_VERSION(5,11,0)
#define FTRACE_OPS_FL_RECURSION FTRACE_OPS_FL_RECURSION_SAFE
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,11,0)
#define ftrace_regs pt_regs
static __always_inline struct pt_regs *ftrace_get_regs(struct ftrace_regs *fregs)
{
	return fregs;
}
#endif

/* 兼容性：有些内核没有 FTRACE_OPS_FL_RECURSION_SAFE，做下兼容定义 */
#ifdef CONFIG_ARM64
#define CPUINFO_FTRACE_FLAGS (FTRACE_OPS_FL_SAVE_REGS_IF_SUPPORTED | FTRACE_OPS_FL_RECURSION_SAFE | FTRACE_OPS_FL_IPMODIFY)
#else
#define CPUINFO_FTRACE_FLAGS (FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_RECURSION | FTRACE_OPS_FL_IPMODIFY)
#endif

/* ftrace 回调（thunk） */
static void notrace ftrace_hook_thunk(unsigned long ip, unsigned long parent_ip,
                                      struct ftrace_ops *ops, struct ftrace_regs *fregs)
{
    struct pt_regs *regs = ftrace_get_regs(fregs);
#ifdef CONFIG_ARM64
	regs->pc = (unsigned long)custom_show_cpuinfo;
#else
	regs->ip = (unsigned long)custom_show_cpuinfo;
#endif
}

/* ftrace ops - 注意 flags 使用上面的兼容宏 */
static struct ftrace_ops cpuinfo_fops = {
    .func = ftrace_hook_thunk,
    .flags = CPUINFO_FTRACE_FLAGS,
};

/* 注册 ftrace 过滤器到指定地址 */
static int ftrace_install_hook(unsigned long func_addr)
{
    int err;

    if (!func_addr)
    {
        pr_err("cpuinfo_ftrace: 无效的 func_addr\n");
        return -EINVAL;
    }

    err = ftrace_set_filter_ip(&cpuinfo_fops, func_addr, 0, 0);
    if (err)
    {
        pr_err("cpuinfo_ftrace: ftrace_set_filter_ip 失败 err=%d\n", err);
        return err;
    }

    err = register_ftrace_function(&cpuinfo_fops);
    if (err)
    {
        pr_err("cpuinfo_ftrace: register_ftrace_function 失败 err=%d\n", err);
        ftrace_set_filter_ip(&cpuinfo_fops, func_addr, 1, 0); /* undo filter */
        return err;
    }

    pr_info("cpuinfo_ftrace: 已安装 ftrace hook @ %px\n", (void *)func_addr);
    return 0;
}

static void ftrace_remove_hook(unsigned long func_addr)
{
    unregister_ftrace_function(&cpuinfo_fops);
    ftrace_set_filter_ip(&cpuinfo_fops, func_addr, 1, 0);
    pr_info("cpuinfo_ftrace: 已移除 ftrace hook @ %px\n", (void *)func_addr);
}

/* ---------- 安装/卸载 ---------- */
static int install_ftrace_cpuinfo_hook(void)
{
    unsigned long func_addr = 0;
    unsigned long cpuinfo_op = 0;

    /* 先尝试解析常见 cpuinfo show 函数符号 */
    func_addr = lookup_name("cpuinfo_show");
    if (!func_addr)
        func_addr = lookup_name("proc_cpuinfo_show");
    if (!func_addr)
    {
        /* 若找不到直接符号，尝试解析 cpuinfo_op 并读取 show 指针（偏移依赖内核 struct seq_operations 布局） */
        cpuinfo_op = lookup_name("cpuinfo_op");
        if (cpuinfo_op)
        {
            unsigned long off = 3 * sizeof(void *); /* start,next,stop,show -> show 通常在第 4 个指针 */
            func_addr = *(unsigned long *)(cpuinfo_op + off);
            pr_info("cpuinfo_ftrace: 尝试从 cpuinfo_op (%px) 读取 show @ %px\n", (void *)cpuinfo_op, (void *)func_addr);
        }
    }

    if (!func_addr)
    {
        pr_err("cpuinfo_ftrace: 无法解析 cpuinfo show 地址，无法安装 hook\n");
        return -ENOENT;
    }

    orig_show_addr = func_addr;
    return ftrace_install_hook(func_addr);
}

static void uninstall_ftrace_cpuinfo_hook(void)
{
    if (orig_show_addr)
        ftrace_remove_hook(orig_show_addr);
}

/* init / exit */
static int __init cpuinfo_ftrace_init(void)
{
    pr_info("cpuinfo_ftrace: 初始化 (target_uid=%u, target_pid=%d)\n", target_uid_val, target_pid_val);

    if (install_ftrace_cpuinfo_hook() != 0)
    {
        pr_err("cpuinfo_ftrace: 安装 hook 失败\n");
        return -EINVAL;
    }

    pr_info("cpuinfo_ftrace: 安装完成\n");
    return 0;
}

static void __exit cpuinfo_ftrace_exit(void)
{
    uninstall_ftrace_cpuinfo_hook();
    pr_info("cpuinfo_ftrace: 卸载完成\n");
}

module_init(cpuinfo_ftrace_init);
module_exit(cpuinfo_ftrace_exit);
