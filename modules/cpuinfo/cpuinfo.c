/*
 * cpuinfo.c - 自定义 /proc/cpuinfo 钩子模块
 * 
 * 这个模块会钩住 /proc/cpuinfo 的输出并为特定的 UID 或 PID 提供自定义输出
 * 支持 x86_64 和 ARM64 架构
 *
 * License: GPL-2.0
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/cred.h>     // 用于 current_uid()
#include <linux/sched.h>    // 用于 current task_struct
#include <linux/version.h>  // 用于 KERNEL_VERSION
#include <linux/fs.h>       // 用于 struct file
#include <linux/syscalls.h> // 用于系统调用定义
#include <linux/kprobes.h>  // 用于 kprobes

// 用于 ARM64 架构的特殊头文件
#ifdef CONFIG_ARM64
#include <linux/mm_types.h>  // 用于 phys_addr_t, pgprot_t
#endif

// 配置参数
static unsigned int target_uid_val = 1000;  // 目标 UID
static int target_pid_val = 0;              // 目标 PID (0表示不检查)

module_param(target_uid_val, uint, 0644);
MODULE_PARM_DESC(target_uid_val, "提供自定义 cpuinfo 的目标 UID");
module_param(target_pid_val, int, 0644);
MODULE_PARM_DESC(target_pid_val, "提供自定义 cpuinfo 的目标 PID (0表示不检查特定PID)");

// 自定义 CPU 信息输出
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

// 全局变量
static int (*original_cpuinfo_show)(struct seq_file *, void *);
static const struct seq_operations *original_cpuinfo_op;

// ARM64 架构特定变量
#ifdef CONFIG_ARM64
// 内存保护相关函数和变量
void (*update_mapping_prot)(phys_addr_t phys, unsigned long virt, phys_addr_t size, pgprot_t prot);
unsigned long start_rodata;
unsigned long init_begin;
#define section_size (init_begin - start_rodata)

// 检查是否需要定义 __pa_symbol 宏
#ifndef __pa_symbol
#define __pa_symbol(x) __pa(x)
#endif
#endif

// 用于查找 kallsyms_lookup_name 函数
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
        pr_err("cpuinfo_hook: 无法注册 kprobe 查找 kallsyms_lookup_name\n");
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

// 内存保护相关函数
#ifdef CONFIG_X86_64
static unsigned long cr0;

static inline void write_cr0_forced(unsigned long val)
{
    unsigned long __force_order;
    asm volatile("mov %0, %%cr0" : "+r"(val), "+m"(__force_order));
}

static inline void protect_memory(void)
{
#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 16, 0)
	write_cr0_forced(cr0);
#else
	write_cr0(cr0);
#endif
}

static inline void unprotect_memory(void)
{
#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 16, 0)
	write_cr0_forced(cr0 & ~0x00010000);
#else
	write_cr0(cr0 & ~0x00010000);
#endif
}

#elif defined(CONFIG_ARM64)
static inline void protect_memory(void)
{
    if (!update_mapping_prot || !start_rodata || !init_begin) {
        pr_err("cpuinfo_hook: 无法保护内存，缺少必要的符号\n");
        return;
    }
    
    if (start_rodata >= init_begin) {
        pr_err("cpuinfo_hook: 无效的段边界: start_rodata (0x%lx) >= init_begin (0x%lx)\n",
               start_rodata, init_begin);
        return;
    }
    
    update_mapping_prot(__pa_symbol(start_rodata), (unsigned long)start_rodata,
                       section_size, PAGE_KERNEL_RO);
}

static inline void unprotect_memory(void)
{
    if (!update_mapping_prot || !start_rodata || !init_begin) {
        pr_err("cpuinfo_hook: 无法解除内存保护，缺少必要的符号\n");
        return;
    }
    
    if (start_rodata >= init_begin) {
        pr_err("cpuinfo_hook: 无效的段边界: start_rodata (0x%lx) >= init_begin (0x%lx)\n",
               start_rodata, init_begin);
        return;
    }
    
    update_mapping_prot(__pa_symbol(start_rodata), (unsigned long)start_rodata,
                       section_size, PAGE_KERNEL);
}
#endif

// 自定义的 show_cpuinfo 函数
static int custom_show_cpuinfo(struct seq_file *m, void *v)
{
    kuid_t current_kuid;
    pid_t current_pid;
    bool should_override = false;

    // 获取当前进程的 PID 和 UID
    current_pid = current->pid;
    current_kuid = current_uid();

    // 记录访问信息
    pr_debug("cpuinfo_hook: Hook 被激活 PID %d, UID %u (目标 UID: %u)\n", 
             current_pid, current_kuid.val, target_uid_val);

    // UID 检查
    if (target_uid_val != 0 && current_kuid.val == target_uid_val) {
        should_override = true;
        pr_debug("cpuinfo_hook: UID 匹配 %u\n", target_uid_val);
    }

    // PID 检查
    if (!should_override && target_pid_val != 0 && current_pid == target_pid_val) {
        should_override = true;
        pr_debug("cpuinfo_hook: PID 匹配 %d\n", target_pid_val);
    }

    // 如果满足条件，输出自定义 CPU 信息
    if (should_override) {
        pr_info("cpuinfo_hook: 为 PID %d, UID %u 提供自定义 CPU 信息\n",
                current_pid, current_kuid.val);
        seq_printf(m, "%s", custom_cpuinfo_output);
        return 0;
    }

    // 否则调用原始函数
    if (original_cpuinfo_show) {
        return original_cpuinfo_show(m, v);
    }

    seq_printf(m, "错误: 原始 cpuinfo show 函数不可用\n");
    return 0;
}

// 初始化和清理函数
#ifdef CONFIG_ARM64
static int init_arm64_symbols(void)
{
    pr_info("cpuinfo_hook: 在 ARM64 架构上运行，初始化所需符号\n");
    
    update_mapping_prot = (void (*)(phys_addr_t, unsigned long, phys_addr_t, pgprot_t))
                         lookup_name("update_mapping_prot");
    if (!update_mapping_prot) {
        pr_err("cpuinfo_hook: 无法找到 update_mapping_prot 符号\n");
        return -EINVAL;
    }
    
    start_rodata = lookup_name("__start_rodata");
    if (!start_rodata) {
        pr_err("cpuinfo_hook: 无法找到 __start_rodata 符号\n");
        return -EINVAL;
    }
    
    init_begin = lookup_name("__init_begin");
    if (!init_begin) {
        pr_err("cpuinfo_hook: 无法找到 __init_begin 符号\n");
        return -EINVAL;
    }
    
    pr_info("cpuinfo_hook: ARM64 符号初始化完成 - start_rodata: 0x%lx, init_begin: 0x%lx\n", 
            start_rodata, init_begin);
            
    return 0;
}
#endif

// 模块初始化函数
static int __init cpuinfo_hook_init(void)
{
    unsigned long cpuinfo_op_addr;
    
    pr_info("cpuinfo_hook: 初始化 cpuinfo 钩子模块...\n");
    pr_info("cpuinfo_hook: 目标 UID: %u, 目标 PID: %d\n", target_uid_val, target_pid_val);
    
    // 初始化平台特定的内存保护机制
#ifdef CONFIG_X86_64
    cr0 = read_cr0();
    pr_info("cpuinfo_hook: 在 X86_64 架构上运行, CR0: 0x%lx\n", cr0);
#elif defined(CONFIG_ARM64)
    if (init_arm64_symbols() != 0) {
        pr_err("cpuinfo_hook: ARM64 符号初始化失败\n");
        return -EINVAL;
    }
#endif
    
    // 查找 cpuinfo_op 结构
    cpuinfo_op_addr = lookup_name("cpuinfo_op");
    if (!cpuinfo_op_addr) {
        pr_err("cpuinfo_hook: 无法找到 cpuinfo_op 符号\n");
        return -EINVAL;
    }
    
    // 保存原始序列操作结构
    original_cpuinfo_op = (const struct seq_operations *)cpuinfo_op_addr;
    if (!original_cpuinfo_op) {
        pr_err("cpuinfo_hook: cpuinfo_op 结构无效\n");
        return -EINVAL;
    }
    
    // 保存原始 show 函数并替换为我们的自定义函数
    original_cpuinfo_show = original_cpuinfo_op->show;
    if (!original_cpuinfo_show) {
        pr_err("cpuinfo_hook: 原始 show 函数为空\n");
        return -EINVAL;
    }
    
    pr_info("cpuinfo_hook: 原始 show 函数位于 0x%px\n", original_cpuinfo_show);
    
    // 修改函数指针
#if defined(CONFIG_X86_64) || defined(CONFIG_ARM64)
   unprotect_memory();
    ((struct seq_operations *)original_cpuinfo_op)->show = custom_show_cpuinfo;
   protect_memory();
#else
    // 对于不支持的架构，我们直接修改指针
    ((struct seq_operations *)original_cpuinfo_op)->show = custom_show_cpuinfo;
#endif
    
    pr_info("cpuinfo_hook: 钩子安装完成\n");
    return 0;
}

// 模块退出函数
static void __exit cpuinfo_hook_exit(void)
{
    pr_info("cpuinfo_hook: 卸载模块...\n");
    
    // 恢复原始函数指针
    if (original_cpuinfo_op && original_cpuinfo_show) {
        pr_info("cpuinfo_hook: 恢复原始 show 函数\n");
        
#if defined(CONFIG_X86_64) || defined(CONFIG_ARM64)
        unprotect_memory();
        ((struct seq_operations *)original_cpuinfo_op)->show = original_cpuinfo_show;
        protect_memory();
#else
        ((struct seq_operations *)original_cpuinfo_op)->show = original_cpuinfo_show;
#endif
    }
    
    // 清理资源
    original_cpuinfo_show = NULL;
    original_cpuinfo_op = NULL;
    
    // 清理特定架构资源
#ifdef CONFIG_ARM64
    update_mapping_prot = NULL;
    start_rodata = 0;
    init_begin = 0;
#endif
    
    pr_info("cpuinfo_hook: 模块已完全卸载\n");
}

module_init(cpuinfo_hook_init);
module_exit(cpuinfo_hook_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("动态 /proc/cpuinfo 钩子 (支持 x86_64, ARM64)");
MODULE_VERSION("1.0");
