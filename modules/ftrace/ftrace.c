// Copyright (C) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
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
#include <linux/dirent.h>  // struct linux_dirent64

MODULE_DESCRIPTION("使用 ftrace getdents64()");
MODULE_AUTHOR("feicong <fei_cong@hotmail.com>");
MODULE_LICENSE("GPL");

/* 关闭尾调用优化（尽量按函数粒度使用） */
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

/*
 * 防止 Hook 递归的两种方式：
 * - 方式1 (USE_FENTRY_OFFSET=0)：通过返回地址检测递归
 * - 方式2 (USE_FENTRY_OFFSET=1)：直接跳过 ftrace 调用位置
 */
#define USE_FENTRY_OFFSET 0

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

#if USE_FENTRY_OFFSET
	*((unsigned long*) hook->original) = hook->address + MCOUNT_INSN_SIZE;
#else
	*((unsigned long*) hook->original) = hook->address;
#endif
	return 0;
}

/* ftrace 回调：替换寄存器中的指令指针，实现跳转到我们自定义函数 */
static void notrace fh_ftrace_thunk(unsigned long ip, unsigned long parent_ip,
		struct ftrace_ops *ops, struct ftrace_regs *fregs)
{
	struct pt_regs *regs = ftrace_get_regs(fregs);
	struct ftrace_hook *hook = container_of(ops, struct ftrace_hook, ops);

#if USE_FENTRY_OFFSET
#ifdef CONFIG_ARM64
	regs->pc = (unsigned long)hook->function;
#else
	regs->ip = (unsigned long)hook->function;
#endif
#else
	if (!within_module(parent_ip, THIS_MODULE))
#ifdef CONFIG_ARM64
		regs->pc = (unsigned long)hook->function;
#else
		regs->ip = (unsigned long)hook->function;
#endif
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

/* 批量安装多个 hook */
int fh_install_hooks(struct ftrace_hook *hooks, size_t count)
{
	int err;
	size_t i;
	for (i = 0; i < count; i++) {
		err = fh_install_hook(&hooks[i]);
		if (err)
			goto error;
	}
	return 0;
error:
	while (i != 0) {
		fh_remove_hook(&hooks[--i]);
	}
	return err;
}

/* 批量卸载多个 hook */
void fh_remove_hooks(struct ftrace_hook *hooks, size_t count)
{
	size_t i;
	for (i = 0; i < count; i++)
		fh_remove_hook(&hooks[i]);
}

#if !defined(CONFIG_X86_64) && !defined(CONFIG_ARM64)
#error 目前仅支持 x86_64 与 arm64 架构
#endif

static asmlinkage long (*real_sys_getdents64)(struct pt_regs *regs);

/* 辅助：安全解析并打印用户缓冲区中 linux_dirent64 的 d_name */
static void parse_and_print_dirents_user(unsigned long user_buf, long nbytes)
{
	char *kbuf = NULL;
	long offset = 0;

	if (nbytes <= 0 || user_buf == 0)
		return;

	/* 限制最大复制大小以防 OOM（这里设置为 1MB） */
	if (nbytes > (1 << 20)) {
		pr_warn("nbytes to long: (%ld), limit 1MB\n", nbytes);
		nbytes = (1 << 20);
	}

	kbuf = kmalloc(nbytes, GFP_KERNEL);
	if (!kbuf) {
		pr_err("kmalloc failed，大小 %ld\n", nbytes);
		return;
	}

	if (copy_from_user(kbuf, (void __user *)user_buf, nbytes)) {
		pr_warn("copy_from_user failed（dirents）\n");
		kfree(kbuf);
		return;
	}

	/* 遍历 linux_dirent64 */
	while (offset < nbytes) {
		struct linux_dirent64 *d = (struct linux_dirent64 *)(kbuf + offset);
		size_t reclen;
		size_t namelen;
		char name[256];

		/* 基本检查：剩余数据是否足够 */
		if (offset + offsetof(struct linux_dirent64, d_name) >= (size_t)nbytes)
			break;

		reclen = d->d_reclen;
		if (reclen == 0)
			break;

		namelen = reclen - offsetof(struct linux_dirent64, d_name);
		if (namelen > sizeof(name) - 1)
			namelen = sizeof(name) - 1;

		if (offset + offsetof(struct linux_dirent64, d_name) + namelen > (size_t)nbytes)
			break;

		memcpy(name, d->d_name, namelen);
		name[namelen] = '\0';

		/* 打印目录项信息（只读） */
		pr_info("pid=%d uid=%u inode=%llu type=%u name=%s\n",
		        current->pid, __kuid_val(current_uid()),
		        (unsigned long long)d->d_ino, (unsigned int)d->d_type, name);

		offset += reclen;
	}

	kfree(kbuf);
}

/* 我们的 pt_regs 风格 wrapper：在调用前打印参数，调用原始实现后打印返回值并解析 names */
static ATTR_NO_TAILCALL
asmlinkage long fh_sys_getdents64(struct pt_regs *regs)
{
	unsigned long user_buf = 0;
	long ret;

	/* 从 regs 中提取参数：不同架构寄存器位置不同 */
#if defined(CONFIG_X86_64) || defined(CONFIG_X86)
	/* __x64_sys_getdents64(unsigned int fd, struct linux_dirent64 __user *dirent, unsigned int count) */
	user_buf = regs->si;
#elif defined(CONFIG_ARM64)
	user_buf = regs->regs[1];
#else
	pr_warn("unsupported arch\n");
#endif

	/* 调用原始实现（保持行为不变） */
	ret = real_sys_getdents64(regs);

	/* 调用后打印返回值 */
	pr_info("getdents64() after: pid=%d ret=%ld\n", current->pid, ret);

	/* 若返回了数据，则尝试解析用户缓冲区并打印每个 d_name（只读） */
	if (ret > 0 && user_buf)
		parse_and_print_dirents_user(user_buf, ret);

	return ret;
}

/* 不同架构下的系统调用符号名宏 */
# ifdef CONFIG_ARM64
#  define SYSCALL_NAME(name) ("__arm64_" name)
# elif defined(CONFIG_X86_64)
#  define SYSCALL_NAME(name) ("__x64_" name)
# else
# error "unsupported architecture for syscall name"
# endif

/* Hook 定义宏 */
#define HOOK(_name, _function, _original)	\
	{					\
		.name = SYSCALL_NAME(_name),	\
		.function = (_function),	\
		.original = (_original),	\
	}

/* 要安装的 Hook 列表 */
static struct ftrace_hook demo_hooks[] = {
	HOOK("sys_getdents64", fh_sys_getdents64, &real_sys_getdents64),
};

/* 模块初始化 */
static int fh_init(void)
{
	int err;
	err = fh_install_hooks(demo_hooks, ARRAY_SIZE(demo_hooks));
	if (err)
		return err;
	pr_info("fh_init done.\n");
	return 0;
}
module_init(fh_init);

/* 模块卸载 */
static void fh_exit(void)
{
	fh_remove_hooks(demo_hooks, ARRAY_SIZE(demo_hooks));
	pr_info("fh_exit done.\n");
}
module_exit(fh_exit);
