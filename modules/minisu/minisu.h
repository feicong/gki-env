#ifndef __MINISU_H_MINISU
#define __MINISU_H_MINISU

#ifdef __KERNEL__
// 内核空间包含
#include <linux/types.h>
#include <linux/workqueue.h>
#else
// 用户空间包含
#include <stdint.h>
#include <sys/types.h>
typedef uint32_t u32;
typedef uint64_t u64;
#endif

#define MINISU_VERSION 1000
#define KERNEL_SU_VERSION MINISU_VERSION
#define KERNEL_SU_OPTION 0xDEADBEEF

#define CMD_GRANT_ROOT 0
#define CMD_BECOME_MANAGER 1
#define CMD_GET_VERSION 2
#define CMD_ALLOW_SU 3
#define CMD_DENY_SU 4
#define CMD_GET_ALLOW_LIST 5
#define CMD_GET_DENY_LIST 6
#define CMD_REPORT_EVENT 7
#define CMD_SET_SEPOLICY 8
#define CMD_CHECK_SAFEMODE 9
#define CMD_GET_APP_PROFILE 10
#define CMD_SET_APP_PROFILE 11
#define CMD_UID_GRANTED_ROOT 12
#define CMD_UID_SHOULD_UMOUNT 13
#define CMD_IS_SU_ENABLED 14
#define CMD_ENABLE_SU 15

#define EVENT_POST_FS_DATA 1
#define EVENT_BOOT_COMPLETED 2
#define EVENT_MODULE_MOUNTED 3

#define MINISU_APP_PROFILE_VER 2
#define MINISU_MAX_PACKAGE_NAME 256
// NGROUPS_MAX for Linux is 65535 generally, but we only supports 32 groups.
#define MINISU_MAX_GROUPS 32
#define MINISU_SELINUX_DOMAIN 64

#ifdef __KERNEL__
// 内核空间函数声明
bool minisu_queue_work(struct work_struct *work);

// 内核兼容性函数
void minisu_android_ns_fs_check(void);
int minisu_access_ok(const void *addr, unsigned long size);
long minisu_strncpy_from_user_nofault(char *dst, const void __user *unsafe_addr, long count);
struct file *minisu_filp_open_compat(const char *filename, int flags, umode_t mode);
ssize_t minisu_kernel_read_compat(struct file *p, void *buf, size_t count, loff_t *pos);
ssize_t minisu_kernel_write_compat(struct file *p, const void *buf, size_t count, loff_t *pos);

// UID检查函数
bool __minisu_is_allow_uid(uid_t uid);

// 系统调用处理函数
int minisu_handle_faccessat(int *dfd, const char __user **filename_user, int *mode, int *flags);
int minisu_handle_stat(int *dfd, const char __user **filename_user, int *flags);
int minisu_handle_execveat_sucompat(int *fd, struct filename **filename_ptr, void *argv, void *envp, int *flags);
int minisu_handle_execve_sucompat(int *fd, const char __user **filename_user, void *argv, void *envp, int *flags);
int minisu_handle_execveat(int *fd, struct filename **filename_ptr, void *argv, void *envp, int *flags);

// 模块初始化/清理函数
void minisu_sucompat_init(void);
void minisu_sucompat_exit(void);
int minisu_init(void);
void minisu_exit(void);
#endif

static inline int startswith(char *s, char *prefix)
{
	return strncmp(s, prefix, strlen(prefix));
}

static inline int endswith(const char *s, const char *t)
{
	size_t slen = strlen(s);
	size_t tlen = strlen(t);
	if (tlen > slen)
		return 1;
	return strcmp(s + slen - tlen, t);
}

#endif
