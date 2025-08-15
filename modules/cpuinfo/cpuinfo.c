// Copyright (C) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/syscalls.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/uidgid.h>
#include <asm/pgtable.h>

#ifdef CONFIG_ARM64
#include <linux/mm_types.h>
#endif

/****************** 模块参数 ******************/
static unsigned int target_uid_val = 1000;
static int target_pid_val = 0;

module_param(target_uid_val, uint, 0644);
MODULE_PARM_DESC(target_uid_val, "提供自定义 cpuinfo 的目标 UID");
module_param(target_pid_val, int, 0644);
MODULE_PARM_DESC(target_pid_val, "提供自定义 cpuinfo 的目标 PID (0表示不检查特定PID)");

/****************** 自定义输出 ******************/
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

/****************** 全局状态 ******************/
static int (*original_cpuinfo_show)(struct seq_file *, void *);
static const struct seq_operations *original_cpuinfo_op;

/****************** 运行时解析的函数指针 ******************/
/* 统一声明类型，避免编译期直接依赖符号，彻底规避 modpost undefined */
typedef int (*set_memory_rw_t)(unsigned long addr, unsigned long numpages);
typedef int (*set_memory_ro_t)(unsigned long addr, unsigned long numpages);

static set_memory_rw_t p_set_memory_rw = NULL;
static set_memory_ro_t p_set_memory_ro = NULL;

/* ARM64 ro 段回退方案 */
#ifdef CONFIG_ARM64
static void (*update_mapping_prot_fp)(phys_addr_t phys, unsigned long virt, phys_addr_t size, pgprot_t prot);
static unsigned long start_rodata;
static unsigned long init_begin;
#define section_size (init_begin - start_rodata)
#ifndef __pa_symbol
#define __pa_symbol(x) __pa(x)
#endif
#endif

/****************** 符号查找（kprobe） ******************/
static unsigned long lookup_name(const char *name)
{
    struct kprobe kp = {
        .symbol_name = "kallsyms_lookup_name"
    };
    typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
    kallsyms_lookup_name_t kallsyms_lookup_name_func;
    int ret;

    ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_err("cpuinfo_hook: 无法注册 kprobe 查找 kallsyms_lookup_name，ret=%d\n", ret);
        return 0;
    }
    kallsyms_lookup_name_func = (kallsyms_lookup_name_t)kp.addr;
    unregister_kprobe(&kp);

    if (!kallsyms_lookup_name_func) {
        pr_err("cpuinfo_hook: 无法获取 kallsyms_lookup_name 地址\n");
        return 0;
    }
    return kallsyms_lookup_name_func(name);
}

/****************** x86_64：CR0 WP 开关（作为回退） ******************/
#ifdef CONFIG_X86_64
static unsigned long saved_cr0;

static inline void write_cr0_forced(unsigned long val)
{
    /* 只保留必要约束，避免奇怪的 clobber 导致 GPF；调用方负责关抢占/关中断 */
    asm volatile("mov %0, %%cr0" :: "r"(val) : "memory");
}

static inline void x86_disable_wp(void)
{
    saved_cr0 = read_cr0();
    write_cr0_forced(saved_cr0 & ~0x00010000UL);
}

static inline void x86_enable_wp(void)
{
    write_cr0_forced(saved_cr0);
}
#endif

/****************** ARM64：ro 段映射修改（作为回退） ******************/
#ifdef CONFIG_ARM64
static int arm64_ro_symbols_init_once(void)
{
    static bool inited;
    if (inited)
        return 0;

    update_mapping_prot_fp = (void (*)(phys_addr_t, unsigned long, phys_addr_t, pgprot_t))
        lookup_name("update_mapping_prot");
    if (!update_mapping_prot_fp) {
        pr_err("cpuinfo_hook: ARM64 找不到 update_mapping_prot\n");
        return -EINVAL;
    }

    start_rodata = lookup_name("__start_rodata");
    if (!start_rodata) {
        pr_err("cpuinfo_hook: ARM64 找不到 __start_rodata\n");
        return -EINVAL;
    }

    init_begin = lookup_name("__init_begin");
    if (!init_begin) {
        pr_err("cpuinfo_hook: ARM64 找不到 __init_begin\n");
        return -EINVAL;
    }

    if (start_rodata >= init_begin) {
        pr_err("cpuinfo_hook: ARM64 ro 段边界异常: start_rodata(0x%lx) >= init_begin(0x%lx)\n",
               start_rodata, init_begin);
        return -EINVAL;
    }

    pr_info("cpuinfo_hook: ARM64 ro 符号就绪 start_rodata=0x%lx init_begin=0x%lx\n",
            start_rodata, init_begin);
    inited = true;
    return 0;
}

static inline void arm64_ro_set_rw(void)
{
    if (!update_mapping_prot_fp)
        return;
    update_mapping_prot_fp(__pa_symbol(start_rodata), (unsigned long)start_rodata,
                           section_size, PAGE_KERNEL);
}

static inline void arm64_ro_set_ro(void)
{
    if (!update_mapping_prot_fp)
        return;
    update_mapping_prot_fp(__pa_symbol(start_rodata), (unsigned long)start_rodata,
                           section_size, PAGE_KERNEL_RO);
}
#endif

/****************** set_memory_*：运行时解析与封装 ******************/
static void resolve_set_memory_symbols(void)
{
    unsigned long rw = lookup_name("set_memory_rw");
    unsigned long ro = lookup_name("set_memory_ro");

    if (rw && ro) {
        p_set_memory_rw = (set_memory_rw_t)rw;
        p_set_memory_ro = (set_memory_ro_t)ro;
        pr_info("cpuinfo_hook: 动态解析到 set_memory_rw=%px set_memory_ro=%px\n",
                p_set_memory_rw, p_set_memory_ro);
    } else {
        pr_warn("cpuinfo_hook: 未解析到 set_memory_rw/ro，将使用回退方案\n");
        p_set_memory_rw = NULL;
        p_set_memory_ro = NULL;
    }
}

static int make_addr_rw_ro_runtime(unsigned long addr, bool make_rw)
{
    /* 优先使用运行时解析到的 set_memory_* */
    if (p_set_memory_rw && p_set_memory_ro) {
        unsigned long start = addr & PAGE_MASK;
        unsigned long end   = (addr + sizeof(struct seq_operations) - 1) & PAGE_MASK;
        unsigned long pages = ((end - start) / PAGE_SIZE) + 1;
        int ret;

        if (make_rw) {
            ret = p_set_memory_rw(start, pages);
            if (ret)
                pr_err("cpuinfo_hook: set_memory_rw 失败 addr=0x%lx pages=%lu ret=%d\n", start, pages, ret);
            return ret;
        } else {
            ret = p_set_memory_ro(start, pages);
            if (ret)
                pr_err("cpuinfo_hook: set_memory_ro 失败 addr=0x%lx pages=%lu ret=%d\n", start, pages, ret);
            return ret;
        }
    }

    /* 回退路径：x86_64 / ARM64 / 其他架构 */
#if defined(CONFIG_X86_64)
    pr_info("cpuinfo_hook: 回退到 x86_64 CR0 WP 切换\n");
    if (make_rw) {
        preempt_disable();
        local_irq_disable();
        x86_disable_wp();
    } else {
        x86_enable_wp();
        local_irq_enable();
        preempt_enable();
    }
    return 0;

#elif defined(CONFIG_ARM64)
    pr_info("cpuinfo_hook: 回退到 ARM64 ro 段映射方案\n");
    if (arm64_ro_symbols_init_once() != 0)
        return -EINVAL;
    if (make_rw)
        arm64_ro_set_rw();
    else
        arm64_ro_set_ro();
    return 0;

#else
    /* 其他架构：无法安全地改页权限，只能假定可写（不推荐，仅兜底） */
    pr_warn("cpuinfo_hook: 其他架构未提供安全改写手段，直接写入（存在风险）\n");
    return 0;
#endif
}

/****************** 自定义 show 函数 ******************/
static int custom_show_cpuinfo(struct seq_file *m, void *v)
{
    kuid_t current_kuid = current_uid();
    pid_t current_pid = current->pid;
    uid_t uid = __kuid_val(current_kuid);
    bool should_override = false;

    pr_debug("cpuinfo_hook: Hook 激活 PID=%d UID=%u (目标 UID=%u, PID=%d)\n",
             current_pid, uid, target_uid_val, target_pid_val);

    if (target_uid_val != 0 && uid == target_uid_val) {
        should_override = true;
        pr_debug("cpuinfo_hook: UID 匹配 %u\n", target_uid_val);
    }

    if (!should_override && target_pid_val != 0 && current_pid == target_pid_val) {
        should_override = true;
        pr_debug("cpuinfo_hook: PID 匹配 %d\n", target_pid_val);
    }

    if (should_override) {
        pr_info("cpuinfo_hook: 为 PID=%d UID=%u 提供自定义 CPU 信息\n", current_pid, uid);
        seq_printf(m, "%s", custom_cpuinfo_output);
        return 0;
    }

    if (original_cpuinfo_show)
        return original_cpuinfo_show(m, v);

    seq_printf(m, "错误: 原始 cpuinfo show 函数不可用\n");
    return 0;
}

/****************** 安装/恢复钩子 ******************/
static int install_hook(void)
{
    unsigned long cpuinfo_op_addr;

    /* 尝试解析 set_memory_*（若导出则优先使用） */
    resolve_set_memory_symbols();

    /* 查找 cpuinfo_op */
    cpuinfo_op_addr = lookup_name("cpuinfo_op");
    if (!cpuinfo_op_addr) {
        pr_err("cpuinfo_hook: 无法找到 cpuinfo_op 符号\n");
        return -EINVAL;
    }

    original_cpuinfo_op = (const struct seq_operations *)cpuinfo_op_addr;
    if (!original_cpuinfo_op) {
        pr_err("cpuinfo_hook: cpuinfo_op 地址无效\n");
        return -EINVAL;
    }

    original_cpuinfo_show = original_cpuinfo_op->show;
    if (!original_cpuinfo_show) {
        pr_err("cpuinfo_hook: 原始 show 函数为空\n");
        return -EINVAL;
    }

    pr_info("cpuinfo_hook: 原始 show @ %px, cpuinfo_op @ %px\n",
            original_cpuinfo_show, original_cpuinfo_op);

    /* 改写前置：使页可写（优先 set_memory_*，否则回退方案） */
    if (make_addr_rw_ro_runtime((unsigned long)original_cpuinfo_op, true) != 0) {
        pr_err("cpuinfo_hook: 无法临时将 cpuinfo_op 所在区域设为可写\n");
        return -EINVAL;
    }

    /* 真正修改指针 */
    ((struct seq_operations *)original_cpuinfo_op)->show = custom_show_cpuinfo;

    /* 改写后恢复只读（若是回退到 x86 的 CR0，这里会把 WP 恢复回去） */
    if (make_addr_rw_ro_runtime((unsigned long)original_cpuinfo_op, false) != 0) {
        pr_warn("cpuinfo_hook: 写回只读失败（非致命），注意内核页权限状态\n");
    }

    pr_info("cpuinfo_hook: 钩子安装完成\n");
    return 0;
}

static void uninstall_hook(void)
{
    pr_info("cpuinfo_hook: 卸载模块，恢复原始 show...\n");

    if (original_cpuinfo_op && original_cpuinfo_show) {
        /* 临时可写 */
        if (make_addr_rw_ro_runtime((unsigned long)original_cpuinfo_op, true) != 0) {
            pr_err("cpuinfo_hook: 设为可写失败，仍尝试恢复指针\n");
        }

        ((struct seq_operations *)original_cpuinfo_op)->show = original_cpuinfo_show;

        /* 恢复只读 / 恢复 WP 或 ARM64 ro 段 */
        if (make_addr_rw_ro_runtime((unsigned long)original_cpuinfo_op, false) != 0) {
            pr_warn("cpuinfo_hook: 恢复只读失败，可能留下可写页\n");
        }
    }

    original_cpuinfo_show = NULL;
    original_cpuinfo_op = NULL;

#ifdef CONFIG_ARM64
    update_mapping_prot_fp = NULL;
    start_rodata = 0;
    init_begin = 0;
#endif

    pr_info("cpuinfo_hook: 恢复完成\n");
}

/****************** 模块入口/出口 ******************/
static int __init cpuinfo_hook_init(void)
{
    pr_info("cpuinfo_hook: 初始化... 目标 UID=%u, 目标 PID=%d\n",
            target_uid_val, target_pid_val);
    return install_hook();
}

static void __exit cpuinfo_hook_exit(void)
{
    uninstall_hook();
}

module_init(cpuinfo_hook_init);
module_exit(cpuinfo_hook_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("feicong");
MODULE_DESCRIPTION("内核修改/proc/cpuinfo");
MODULE_VERSION("1.0");
