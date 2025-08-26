/**
 * MiniSU - Minimal KernelSU Implementation
 */

// Standard Linux headers
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/nsproxy.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/security.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <asm/current.h>

// Project headers
#include "minisu.h"

#include <linux/printk.h>

#ifdef pr_fmt
#undef pr_fmt
#define pr_fmt(fmt) "MiniSU: " fmt
#endif

// ===========================
// Architecture Definitions (from arch.h)
// ===========================

#if defined(__aarch64__)

#define __PT_PARM1_REG regs[0]
#define __PT_PARM2_REG regs[1]
#define __PT_PARM3_REG regs[2]
#define __PT_SYSCALL_PARM4_REG regs[3]
#define __PT_CCALL_PARM4_REG regs[3]
#define __PT_PARM5_REG regs[4]
#define __PT_PARM6_REG regs[5]
#define __PT_RET_REG regs[30]
#define __PT_FP_REG regs[29] /* Works only with CONFIG_FRAME_POINTER */
#define __PT_RC_REG regs[0]
#define __PT_SP_REG sp
#define __PT_IP_REG pc

#define PRCTL_SYMBOL "__arm64_sys_prctl"
#define SYS_READ_SYMBOL "__arm64_sys_read"
#define SYS_NEWFSTATAT_SYMBOL "__arm64_sys_newfstatat"
#define SYS_FACCESSAT_SYMBOL "__arm64_sys_faccessat"
#define SYS_EXECVE_SYMBOL "__arm64_sys_execve"

#elif defined(__x86_64__)

#define __PT_PARM1_REG di
#define __PT_PARM2_REG si
#define __PT_PARM3_REG dx
/* syscall uses r10 for PARM4 */
#define __PT_SYSCALL_PARM4_REG r10
#define __PT_CCALL_PARM4_REG cx
#define __PT_PARM5_REG r8
#define __PT_PARM6_REG r9
#define __PT_RET_REG sp
#define __PT_FP_REG bp
#define __PT_RC_REG ax
#define __PT_SP_REG sp
#define __PT_IP_REG ip
#define PRCTL_SYMBOL "__x64_sys_prctl"
#define SYS_READ_SYMBOL "__x64_sys_read"
#define SYS_NEWFSTATAT_SYMBOL "__x64_sys_newfstatat"
#define SYS_FACCESSAT_SYMBOL "__x64_sys_faccessat"
#define SYS_EXECVE_SYMBOL "__x64_sys_execve"

#else
#error "Unsupported arch"
#endif

/* allow some architecutres to override `struct pt_regs` */
#ifndef __PT_REGS_CAST
#define __PT_REGS_CAST(x) (x)
#endif

#define PT_REGS_PARM1(x) (__PT_REGS_CAST(x)->__PT_PARM1_REG)
#define PT_REGS_PARM2(x) (__PT_REGS_CAST(x)->__PT_PARM2_REG)
#define PT_REGS_PARM3(x) (__PT_REGS_CAST(x)->__PT_PARM3_REG)
#define PT_REGS_SYSCALL_PARM4(x) (__PT_REGS_CAST(x)->__PT_SYSCALL_PARM4_REG)
#define PT_REGS_CCALL_PARM4(x) (__PT_REGS_CAST(x)->__PT_CCALL_PARM4_REG)
#define PT_REGS_PARM5(x) (__PT_REGS_CAST(x)->__PT_PARM5_REG)
#define PT_REGS_PARM6(x) (__PT_REGS_CAST(x)->__PT_PARM6_REG)
#define PT_REGS_RET(x) (__PT_REGS_CAST(x)->__PT_RET_REG)
#define PT_REGS_FP(x) (__PT_REGS_CAST(x)->__PT_FP_REG)
#define PT_REGS_RC(x) (__PT_REGS_CAST(x)->__PT_RC_REG)
#define PT_REGS_SP(x) (__PT_REGS_CAST(x)->__PT_SP_REG)
#define PT_REGS_IP(x) (__PT_REGS_CAST(x)->__PT_IP_REG)

#define PT_REAL_REGS(regs) ((struct pt_regs *)PT_REGS_PARM1(regs))

// ===========================
// Platform-specific Configuration
// ===========================

#ifdef __ANDROID_COMMON_KERNEL__
#define SU_PATH "/system/bin/su"
#define SH_PATH "/system/bin/sh"
#define PLATFORM_NAME "Android"
#else
#define SU_PATH "/usr/bin/su"
#define SH_PATH "/usr/bin/sh"
#define PLATFORM_NAME "Linux"
#endif

// ===========================
// MiniSU Protocol Definitions
// ===========================

#define KERNEL_SU_OPTION 0xDEADBEEF
#define CMD_GRANT_ROOT 0
#define CMD_GET_VERSION 2
#ifndef KERNEL_SU_VERSION
#define KERNEL_SU_VERSION 1000
#endif

// ===========================
// Global Variables
// ===========================

extern struct task_struct init_task;
static struct workqueue_struct *minisu_workqueue;

// Android namespace context switching support
struct minisu_ns_fs_saved
{
	struct nsproxy *ns;
	struct fs_struct *fs;
};

static bool android_context_saved_checked = false;
static bool android_context_saved_enabled = false;
static struct minisu_ns_fs_saved android_context_saved;

// ===========================
// Utility Functions
// ===========================

static void minisu_save_ns_fs(struct minisu_ns_fs_saved *ns_fs_saved)
{
	ns_fs_saved->ns = current->nsproxy;
	ns_fs_saved->fs = current->fs;
}

static void minisu_load_ns_fs(struct minisu_ns_fs_saved *ns_fs_saved)
{
	current->nsproxy = ns_fs_saved->ns;
	current->fs = ns_fs_saved->fs;
}

void minisu_android_ns_fs_check(void)
{
	if (android_context_saved_checked)
		return;
	android_context_saved_checked = true;
	task_lock(current);
	if (current->nsproxy && current->fs &&
		current->nsproxy->mnt_ns != init_task.nsproxy->mnt_ns)
	{
		android_context_saved_enabled = true;
		pr_info("android context saved enabled due to init mnt_ns(%p) != android mnt_ns(%p)\n",
				current->nsproxy->mnt_ns, init_task.nsproxy->mnt_ns);
		minisu_save_ns_fs(&android_context_saved);
	}
	else
	{
		pr_info("android context saved disabled\n");
	}
	task_unlock(current);
}

int minisu_access_ok(const void *addr, unsigned long size)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
	return access_ok(VERIFY_READ, addr, size);
#else
	return access_ok(addr, size);
#endif
}

long minisu_strncpy_from_user_nofault(char *dst, const void __user *unsafe_addr,
									  long count)
{
	return strncpy_from_user(dst, unsafe_addr, count);
}

struct file *minisu_filp_open_compat(const char *filename, int flags, umode_t mode)
{
	struct minisu_ns_fs_saved saved;
	if (android_context_saved_enabled)
	{
		pr_info("start switch current nsproxy and fs to android context\n");
		task_lock(current);
		minisu_save_ns_fs(&saved);
		minisu_load_ns_fs(&android_context_saved);
		task_unlock(current);
	}
	struct file *fp = filp_open(filename, flags, mode);
	if (android_context_saved_enabled)
	{
		task_lock(current);
		minisu_load_ns_fs(&saved);
		task_unlock(current);
		pr_info("switch current nsproxy and fs back to saved successfully\n");
	}
	return fp;
}

ssize_t minisu_kernel_read_compat(struct file *p, void *buf, size_t count,
								  loff_t *pos)
{
	return kernel_read(p, buf, count, pos);
}

ssize_t minisu_kernel_write_compat(struct file *p, const void *buf, size_t count,
								   loff_t *pos)
{
	return kernel_write(p, buf, count, pos);
}

// ===========================
// Stack Buffer Functions
// ===========================

static void __user *userspace_stack_buffer(const void *d, size_t len)
{
	char __user *p = (void __user *)current_user_stack_pointer() - len;
	return copy_to_user(p, d, len) ? NULL : p;
}

static char __user *sh_user_path(void)
{
	static const char sh_path[] = SH_PATH;
	return userspace_stack_buffer(sh_path, sizeof(sh_path));
}

static char __user *ksud_user_path(void)
{
	static const char ksud_path[] = SH_PATH;
	return userspace_stack_buffer(ksud_path, sizeof(ksud_path));
}

// ===========================
// Root Escalation Functions
// ===========================
// Core Functions
// ===========================

static void escape_to_root(void)
{
	struct cred *new_cred = prepare_creds();
	if (!new_cred)
		return;

	new_cred->uid.val = 0;
	new_cred->gid.val = 0;
	new_cred->suid.val = 0;
	new_cred->sgid.val = 0;
	new_cred->euid.val = 0;
	new_cred->egid.val = 0;
	new_cred->fsuid.val = 0;
	new_cred->fsgid.val = 0;

	commit_creds(new_cred);
}

bool __minisu_is_allow_uid(uid_t uid)
{
	// Allow root, user 1000, and user 2000 (shell on Android)
	return uid == 0 || uid == 1000 || uid == 2000;
}

#define minisu_is_allow_uid(uid) unlikely(__minisu_is_allow_uid(uid))

/**
 * Handle prctl system call for MiniSU protocol
 */
static int minisu_handle_prctl(int option, unsigned long arg2, unsigned long arg3,
							   unsigned long arg4, unsigned long arg5)
{
	u32 *result = (u32 *)arg5;
	u32 reply_ok = KERNEL_SU_OPTION;

	// Check if this is a MiniSU request
	if (KERNEL_SU_OPTION != option)
	{
		return 0;
	}

	pr_info("prctl option: 0x%x, cmd: %ld\n", option, arg2);

	// Handle CMD_GRANT_ROOT command
	if (arg2 == CMD_GRANT_ROOT)
	{
		if (minisu_is_allow_uid(current_uid().val))
		{
			pr_info("granting root to UID: %d\n", current_uid().val);
			escape_to_root();
			if (copy_to_user(result, &reply_ok, sizeof(reply_ok)))
			{
				pr_err("grant_root reply error\n");
			}
		}
		else
		{
			pr_info("denying root for UID: %d\n", current_uid().val);
		}
		return 0;
	}

	// Handle CMD_GET_VERSION command
	if (arg2 == CMD_GET_VERSION)
	{
		u32 version = KERNEL_SU_VERSION;
		pr_info("returning version: %u\n", version);
		if (copy_to_user((void __user *)arg3, &version, sizeof(version)))
		{
			pr_err("get_version reply error\n");
		}
		// Set version flags (bit 0 = module mode)
		u32 version_flags = 0;
#ifdef MODULE
		version_flags |= 0x1;
#endif
		if (arg4 && copy_to_user((void __user *)arg4, &version_flags, sizeof(version_flags)))
		{
			pr_err("get_version flags reply error\n");
		}
		if (copy_to_user(result, &reply_ok, sizeof(reply_ok)))
		{
			pr_err("get_version result reply error\n");
		}
		return 0;
	}

	return 0;
}

// ===========================
// System Call Handlers
// ===========================

int minisu_handle_faccessat(int *dfd, const char __user **filename_user, int *mode,
							int *__unused_flags)
{
	const char su[] = SU_PATH;

	if (!minisu_is_allow_uid(current_uid().val))
	{
		return 0;
	}

	char path[sizeof(su) + 1];
	memset(path, 0, sizeof(path));
	minisu_strncpy_from_user_nofault(path, *filename_user, sizeof(path));

	if (unlikely(!memcmp(path, su, sizeof(su))))
	{
		pr_info("faccessat su->sh!\n");
		*filename_user = sh_user_path();
	}

	return 0;
}

int minisu_handle_stat(int *dfd, const char __user **filename_user, int *flags)
{
	const char su[] = SU_PATH;

	if (!minisu_is_allow_uid(current_uid().val))
	{
		return 0;
	}

	if (unlikely(!filename_user))
	{
		return 0;
	}

	char path[sizeof(su) + 1];
	memset(path, 0, sizeof(path));
	minisu_strncpy_from_user_nofault(path, *filename_user, sizeof(path));

	if (unlikely(!memcmp(path, su, sizeof(su))))
	{
		pr_info("newfstatat su->sh!\n");
		*filename_user = sh_user_path();
	}

	return 0;
}

int minisu_handle_execveat_sucompat(int *fd, struct filename **filename_ptr,
									void *__never_use_argv, void *__never_use_envp,
									int *__never_use_flags)
{
	struct filename *filename;
	const char sh[] = SH_PATH;
	const char su[] = SU_PATH;

	if (unlikely(!filename_ptr))
		return 0;

	filename = *filename_ptr;
	if (IS_ERR(filename))
	{
		return 0;
	}

	if (likely(memcmp(filename->name, su, sizeof(su))))
		return 0;

	if (!minisu_is_allow_uid(current_uid().val))
		return 0;

	pr_info("do_execveat_common su found\n");
	memcpy((void *)filename->name, sh, sizeof(sh));

	escape_to_root();

	return 0;
}

int minisu_handle_execve_sucompat(int *fd, const char __user **filename_user,
								  void *__never_use_argv, void *__never_use_envp,
								  int *__never_use_flags)
{
	const char su[] = SU_PATH;
	char path[sizeof(su) + 1];

	if (unlikely(!filename_user))
		return 0;

	memset(path, 0, sizeof(path));
	minisu_strncpy_from_user_nofault(path, *filename_user, sizeof(path));

	if (likely(memcmp(path, su, sizeof(su))))
		return 0;

	if (!minisu_is_allow_uid(current_uid().val))
		return 0;

	pr_info("sys_execve su found\n");
	*filename_user = ksud_user_path();

	escape_to_root();

	return 0;
}

// ===========================
// Kprobe Handlers
// ===========================

#ifdef CONFIG_KPROBES

static int faccessat_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct pt_regs *real_regs = PT_REAL_REGS(regs);
	int *dfd = (int *)&PT_REGS_PARM1(real_regs);
	const char __user **filename_user =
		(const char **)&PT_REGS_PARM2(real_regs);
	int *mode = (int *)&PT_REGS_PARM3(real_regs);

	return minisu_handle_faccessat(dfd, filename_user, mode, NULL);
}

static int newfstatat_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct pt_regs *real_regs = PT_REAL_REGS(regs);
	int *dfd = (int *)&PT_REGS_PARM1(real_regs);
	const char __user **filename_user =
		(const char **)&PT_REGS_PARM2(real_regs);
	int *flags = (int *)&PT_REGS_SYSCALL_PARM4(real_regs);

	return minisu_handle_stat(dfd, filename_user, flags);
}

static int execve_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct pt_regs *real_regs = PT_REAL_REGS(regs);
	const char __user **filename_user =
		(const char **)&PT_REGS_PARM1(real_regs);
	int fd = AT_FDCWD;

	return minisu_handle_execve_sucompat(&fd, filename_user, NULL, NULL, NULL);
}

static int prctl_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct pt_regs *real_regs = PT_REAL_REGS(regs);
	int option = (int)PT_REGS_PARM1(real_regs);
	unsigned long arg2 = (unsigned long)PT_REGS_PARM2(real_regs);
	unsigned long arg3 = (unsigned long)PT_REGS_PARM3(real_regs);
	unsigned long arg4 = (unsigned long)PT_REGS_SYSCALL_PARM4(real_regs);
	unsigned long arg5 = (unsigned long)PT_REGS_PARM5(real_regs);

	return minisu_handle_prctl(option, arg2, arg3, arg4, arg5);
}

// ===========================
// Kprobe Management
// ===========================

static struct kprobe *init_kprobe(const char *name,
								  kprobe_pre_handler_t handler)
{
	struct kprobe *kp = kzalloc(sizeof(struct kprobe), GFP_KERNEL);
	if (!kp)
		return NULL;
	kp->symbol_name = name;
	kp->pre_handler = handler;

	int ret = register_kprobe(kp);
	pr_info("sucompat: register_%s kprobe: %d\n", name, ret);
	if (ret)
	{
		kfree(kp);
		return NULL;
	}

	return kp;
}

static void destroy_kprobe(struct kprobe **kp_ptr)
{
	struct kprobe *kp = *kp_ptr;
	if (!kp)
		return;
	unregister_kprobe(kp);
	synchronize_rcu();
	kfree(kp);
	*kp_ptr = NULL;
}

static struct kprobe *su_kps[4];

#endif

// ===========================
// Su Compatibility Layer
// ===========================

void minisu_sucompat_init(void)
{
#ifdef CONFIG_KPROBES
	su_kps[0] = init_kprobe(PRCTL_SYMBOL, prctl_handler_pre);
	su_kps[1] = init_kprobe(SYS_EXECVE_SYMBOL, execve_handler_pre);
	su_kps[2] = init_kprobe(SYS_FACCESSAT_SYMBOL, faccessat_handler_pre);
	su_kps[3] = init_kprobe(SYS_NEWFSTATAT_SYMBOL, newfstatat_handler_pre);
#endif
}

void minisu_sucompat_exit(void)
{
#ifdef CONFIG_KPROBES
	for (int i = 0; i < ARRAY_SIZE(su_kps); i++)
	{
		destroy_kprobe(&su_kps[i]);
	}
#endif
}

// ===========================
// Work Queue Functions
// ===========================

bool minisu_queue_work(struct work_struct *work)
{
	return queue_work(minisu_workqueue, work);
}

// ===========================
// Execveat Handler Interface
// ===========================

int minisu_handle_execveat(int *fd, struct filename **filename_ptr, void *argv,
						   void *envp, int *flags)
{
	return minisu_handle_execveat_sucompat(fd, filename_ptr, argv, envp, flags);
}

// ===========================
// Module Init/Exit Functions
// ===========================

int __init minisu_init(void)
{
#ifdef CONFIG_MINISU_DEBUG
	pr_alert("*************************************************************");
	pr_alert("**     NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE    **");
	pr_alert("**                                                         **");
	pr_alert("**         You are running MiniSU in DEBUG mode          **");
	pr_alert("**                                                         **");
	pr_alert("**     NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE    **");
	pr_alert("*************************************************************");
#endif

	pr_info("MiniSU initializing on %s platform\n", PLATFORM_NAME);
	pr_info("SU path: %s -> %s\n", SU_PATH, SH_PATH);

	minisu_workqueue = alloc_ordered_workqueue("minisu_work_queue", 0);

#ifdef CONFIG_KPROBES
	minisu_sucompat_init();
#else
	pr_alert("KPROBES is disabled, MiniSU may not work");
#endif

#ifdef MODULE
#ifndef CONFIG_MINISU_DEBUG
	// kobject_del(&THIS_MODULE->mkobj.kobj);
#endif
#endif
	return 0;
}

void minisu_exit(void)
{
	pr_info("MiniSU exiting...\n");

	destroy_workqueue(minisu_workqueue);

#ifdef CONFIG_KPROBES
	minisu_sucompat_exit();
#endif
}

module_init(minisu_init);
module_exit(minisu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MiniSU");
MODULE_DESCRIPTION("MiniSU - Minimal KernelSU Implementation");
MODULE_VERSION("1.0");
