// Copyright (c) 2023-2026 fei_cong (https://github.com/feicong/feicong-course)
#define DEBUG
#define pr_fmt(fmt) "feicong: " fmt

#include <linux/ftrace.h>
#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/linkage.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/kprobes.h>
#include <linux/seq_file.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/fs.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("feicong <fei_cong@hotmail.com>");
MODULE_DESCRIPTION("Hide library from /proc/[pid]/maps");

// 模块参数
static char *target_library = "libc.so";
module_param(target_library, charp, 0644);
MODULE_PARM_DESC(target_library, "Shared object to hide in /proc/[pid]/maps");

static int target_pid = 0;
module_param(target_pid, int, 0644);
MODULE_PARM_DESC(target_pid, "Target PID to hide library from (0 for all)");

/* 关闭尾调用优化 */
#if defined(__clang__)
#  if __has_attribute(disable_tail_calls)
#    define ATTR_NO_TAILCALL __attribute__((disable_tail_calls))
#  else
#    define ATTR_NO_TAILCALL /* 无法禁用，留空 */
#  endif
#else /* 偏向 GCC */
#  define ATTR_NO_TAILCALL __attribute__((optimize("-fno-optimize-sibling-calls")))
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,7,0)
/* 在新内核中，通过临时注册 kprobe 获取符号地址 */
static unsigned long lookup_name(const char *name)
{
	struct kprobe kp = {
		.symbol_name = name
	};
	unsigned long retval;

	if (register_kprobe(&kp) < 0) return 0;
	retval = (unsigned long) kp.addr;
	unregister_kprobe(&kp);
	return retval;
}
#else
/* 旧内核直接使用 kallsyms_lookup_name 获取符号地址 */
static unsigned long lookup_name(const char *name)
{
	return kallsyms_lookup_name(name);
}
#endif

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

/* 描述一个需要安装的 ftrace hook */
struct ftrace_hook {
	const char *name;    // 目标函数名
	void *function;      // 我们的 Hook 函数
	void *original;      // 用于保存原始函数指针的变量地址

	unsigned long address;   // 内核中函数地址
	struct ftrace_ops ops;   // ftrace 操作结构体
};

/* 解析函数地址并保存原始函数指针 */
static int fh_resolve_hook_address(struct ftrace_hook *hook)
{
	hook->address = lookup_name(hook->name);
	if (!hook->address) {
		pr_debug("lookup symbol failed: %s\n", hook->name);
		return -ENOENT;
	}

	*((unsigned long*) hook->original) = hook->address;
	return 0;
}

/* ftrace 回调：替换寄存器中的指令指针，实现跳转到我们自定义函数 */
static void notrace fh_ftrace_thunk(unsigned long ip, unsigned long parent_ip,
		struct ftrace_ops *ops, struct ftrace_regs *fregs)
{
	struct pt_regs *regs = ftrace_get_regs(fregs);
	struct ftrace_hook *hook = container_of(ops, struct ftrace_hook, ops);

	if (!within_module(parent_ip, THIS_MODULE))
#ifdef CONFIG_ARM64
		regs->pc = (unsigned long)hook->function;
#else
		regs->ip = (unsigned long)hook->function;
#endif
}

/* 安装单个 hook */
int fh_install_hook(struct ftrace_hook *hook)
{
	int err;
	err = fh_resolve_hook_address(hook);
	if (err)
		return err;

	pr_debug("target address: %s = 0x%lx", hook->name, hook->address);

	hook->ops.func = fh_ftrace_thunk;
#ifdef CONFIG_ARM64
	hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS_IF_SUPPORTED
					| FTRACE_OPS_FL_RECURSION_SAFE
					| FTRACE_OPS_FL_IPMODIFY;
#else
	hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS
	                | FTRACE_OPS_FL_RECURSION
	                | FTRACE_OPS_FL_IPMODIFY;
#endif

	err = ftrace_set_filter(&hook->ops, (unsigned char *)hook->name, strlen(hook->name), 0);
	if (err) {
		pr_debug("ftrace_set_filter() failed: %d\n", err);
		return err;
	}

	err = register_ftrace_function(&hook->ops);
	if (err) {
		pr_debug("register_ftrace_function() failed: %d\n", err);
		ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
		return err;
	}
	return 0;
}

/* 卸载单个 hook */
void fh_remove_hook(struct ftrace_hook *hook)
{
	int err;
	err = unregister_ftrace_function(&hook->ops);
	if (err) {
		pr_debug("unregister_ftrace_function() failed: %d\n", err);
	}

	err = ftrace_set_filter(&hook->ops,  NULL, 0, 1);
	if (err) {
		pr_debug("ftrace_set_filter() failed: %d\n", err);
	}
}

#if !defined(CONFIG_X86_64) && !defined(CONFIG_ARM64)
#error 目前仅支持 x86_64 与 arm64 架构
#endif

// 原始函数指针
static asmlinkage void (*real_show_map_vma)(struct seq_file *m, struct vm_area_struct *vma);

/* 我们的 show_map_vma hook 函数 */
static ATTR_NO_TAILCALL
asmlinkage void fh_show_map_vma(struct seq_file *m, struct vm_area_struct *vma)
{
	char path_buf[NAME_MAX];
	char *pathname;

	// 基本安全检查
	if (!vma || !vma->vm_file || !vma->vm_mm) {
		real_show_map_vma(m, vma);
		return;
	}

	// 检查是否需要按PID过滤
	if (target_pid > 0 && current->pid != target_pid) {
		real_show_map_vma(m, vma);
		return;
	}

	// 安全地获取文件路径
	pathname = d_path(&vma->vm_file->f_path, path_buf, sizeof(path_buf));
	if (IS_ERR(pathname)) {
		real_show_map_vma(m, vma);
		return;
	}

	// 检查是否是目标库
	if (strstr(pathname, target_library)) {
		pr_info("Hiding target library %s from PID %d maps\n",
		        target_library, current->pid);
		// 直接返回，不调用原始函数，从而隐藏这个VMA条目
		return;
	}

	// 不是目标库，调用原始函数正常显示
	real_show_map_vma(m, vma);
}

/* Hook 定义宏 */
#define HOOK(_name, _function, _original)	\
	{					\
		.name = (_name),		\
		.function = (_function),	\
		.original = (_original),	\
	}

/* 要安装的 Hook 列表 */
static struct ftrace_hook demo_hooks[] = {
	HOOK("show_map_vma", fh_show_map_vma, &real_show_map_vma),
};

/* 模块初始化 */
static int fh_init(void)
{
	int err;
	err = fh_install_hook(&demo_hooks[0]);
	if (err)
		return err;
	pr_info("hideso module loaded: hiding %s from /proc/[pid]/maps\n", target_library);
	return 0;
}

/* 模块卸载 */
static void fh_exit(void)
{
	fh_remove_hook(&demo_hooks[0]);
	pr_info("hideso module unloaded: stopped hiding %s from /proc/[pid]/maps\n", target_library);
}

module_init(fh_init);
module_exit(fh_exit);
