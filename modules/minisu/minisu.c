/**
 * MiniSU - 精简版KernelSU实现
 * 只保留最核心的su权限申请功能
 * 去掉白名单、APK签名验证、安全模式、模块系统等复杂功能
 * 固定使用su:s0域，简化权限管理
 */

#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/lsm_hooks.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/security.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/version.h>

#include "minisu.h"

// MiniSU命令定义
#define CMD_GRANT_ROOT 0
#define CMD_GET_VERSION 2

// su兼容层相关
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/sched/task_stack.h>
#include <linux/fcntl.h>

// 架构相关的系统调用符号定义
#if defined(__aarch64__)
#define PRCTL_SYMBOL "__arm64_sys_prctl"
#define SYS_EXECVE_SYMBOL "__arm64_sys_execve"
#define SYS_FACCESSAT_SYMBOL "__arm64_sys_faccessat"
#define SYS_NEWFSTATAT_SYMBOL "__arm64_sys_newfstatat"
#elif defined(__x86_64__)
// 对于较新的内核，尝试使用__x64_sys_前缀
// 对于较老的内核，可能需要使用sys_前缀
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 17, 0)
#define PRCTL_SYMBOL "__x64_sys_prctl"
#define SYS_EXECVE_SYMBOL "__x64_sys_execve"
#define SYS_FACCESSAT_SYMBOL "__x64_sys_faccessat"
#define SYS_NEWFSTATAT_SYMBOL "__x64_sys_newfstatat"
#else
#define PRCTL_SYMBOL "sys_prctl"
#define SYS_EXECVE_SYMBOL "sys_execve"
#define SYS_FACCESSAT_SYMBOL "sys_faccessat"
#define SYS_NEWFSTATAT_SYMBOL "sys_newfstatat"
#endif
#elif defined(__i386__)
#define PRCTL_SYMBOL "sys_prctl"
#define SYS_EXECVE_SYMBOL "sys_execve"
#define SYS_FACCESSAT_SYMBOL "sys_faccessat"
#define SYS_NEWFSTATAT_SYMBOL "sys_newfstatat"
#else
#define PRCTL_SYMBOL "sys_prctl"
#define SYS_EXECVE_SYMBOL "sys_execve"
#define SYS_FACCESSAT_SYMBOL "sys_faccessat"
#define SYS_NEWFSTATAT_SYMBOL "sys_newfstatat"
#endif

// 架构相关的寄存器访问宏
#if defined(__aarch64__)
#define PT_REAL_REGS(regs) ((struct pt_regs *)((regs)->regs[0]))
#define PT_REGS_PARM1(x) ((x)->regs[0])
#define PT_REGS_PARM2(x) ((x)->regs[1])
#define PT_REGS_PARM3(x) ((x)->regs[2])
#define PT_REGS_PARM4(x) ((x)->regs[3])
#define PT_REGS_PARM5(x) ((x)->regs[4])
#elif defined(__x86_64__)
#define PT_REAL_REGS(regs) (regs)
#define PT_REGS_PARM1(x) ((x)->di)
#define PT_REGS_PARM2(x) ((x)->si)
#define PT_REGS_PARM3(x) ((x)->dx)
#define PT_REGS_PARM4(x) ((x)->r10)
#define PT_REGS_PARM5(x) ((x)->r8)
#else
#define PT_REAL_REGS(regs) (regs)
#define PT_REGS_PARM1(x) ((x)->bx)
#define PT_REGS_PARM2(x) ((x)->cx)
#define PT_REGS_PARM3(x) ((x)->dx)
#define PT_REGS_PARM4(x) ((x)->si)
#define PT_REGS_PARM5(x) ((x)->di)
#endif

// SELinux相关结构体和函数声明
struct task_security_struct {
    u32 osid;
    u32 sid;
    u32 exec_sid;
    u32 create_sid;
    u32 keycreate_sid;
    u32 sockcreate_sid;
};

// SELinux函数声明
extern int security_secctx_to_secid(const char *secdata, u32 seclen, u32 *secid);

// 动态获取kallsyms_lookup_name
typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
static kallsyms_lookup_name_t lookup_name = NULL;

#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 4, 0)
// 用于获取kallsyms_lookup_name地址的kprobe
static struct kprobe kp = {
    .symbol_name = "kallsyms_lookup_name"
};

/**
 * 动态获取kallsyms_lookup_name函数指针
 */
static int init_kallsyms_lookup(void)
{
    int ret;
    
    ret = register_kprobe(&kp);
    if (ret < 0) {
        minisu_err("register_kprobe for kallsyms_lookup_name failed: %d\n", ret);
        return ret;
    }
    
    lookup_name = (kallsyms_lookup_name_t) kp.addr;
    unregister_kprobe(&kp);
    
    if (!lookup_name) {
        minisu_err("Failed to get kallsyms_lookup_name address\n");
        return -EFAULT;
    }
    
    minisu_info("kallsyms_lookup_name found at: 0x%px\n", lookup_name);
    return 0;
}
#else
// 对于较老的内核，直接使用导出的符号
static int init_kallsyms_lookup(void)
{
    lookup_name = kallsyms_lookup_name;
    return 0;
}
#endif

// su兼容层函数声明
static int minisu_handle_faccessat(int *dfd, const char __user **filename_user, int *mode, int *flags);
static int minisu_handle_stat(int *dfd, const char __user **filename_user, int *flags);
static int minisu_handle_execveat_sucompat(int *fd, struct filename **filename_ptr, void *argv, void *envp, int *flags);
static int minisu_handle_execve_sucompat(int *fd, const char __user **filename_user, void *argv, void *envp, int *flags);

/**
 * 获取当前进程的安全结构体
 */
static inline struct task_security_struct *current_security(void)
{
    return (struct task_security_struct *)current_cred()->security;
}

/**
 * 设置进程的SELinux域
 */
static void setup_selinux_domain(void)
{
    struct cred *cred;
    struct task_security_struct *tsec;
    u32 sid;
    int error;

    // 获取当前进程凭证
    cred = (struct cred *)__task_cred(current);
    tsec = cred->security;
    
    if (!tsec) {
        pr_err("minisu: tsec is NULL!\n");
        return;
    }

    // 将域名转换为安全ID
    error = security_secctx_to_secid(MINISU_SELINUX_DOMAIN, 
                                   strlen(MINISU_SELINUX_DOMAIN), &sid);
    if (error) {
        minisu_err("security_secctx_to_secid failed: %d\n", error);
        return;
    }

    // 设置安全上下文
    tsec->sid = sid;
    tsec->create_sid = 0;
    tsec->keycreate_sid = 0;
    tsec->sockcreate_sid = 0;
}

/**
 * 禁用seccomp机制
 */
static void disable_seccomp(void)
{
    // 断言当前进程的sighand->siglock互斥锁已被持有
    // assert_spin_locked(&current->sighand->siglock);
    
#if defined(CONFIG_GENERIC_ENTRY) && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
    current_thread_info()->syscall_work &= ~SYSCALL_WORK_SECCOMP;
#else
    current_thread_info()->flags &= ~(TIF_SECCOMP | _TIF_SECCOMP);
#endif

#ifdef CONFIG_SECCOMP
    current->seccomp.mode = 0;
    current->seccomp.filter = NULL;
#endif
}

/**
 * 提权到root
 */
static void escape_to_root(void)
{
    struct cred *cred;
    kernel_cap_t full_cap = CAP_FULL_SET;

    // 准备新的凭证结构
    cred = prepare_creds();
    if (!cred) {
        minisu_err("prepare_creds failed!\n");
        return;
    }

    // 如果已经是root用户，则警告并中止凭证
    if (cred->euid.val == 0) {
        minisu_warn("Already root, don't escape!\n");
        abort_creds(cred);
        return;
    }

    minisu_info("Granting root to UID %d\n", cred->uid.val);

    // 设置UID相关字段为root
    cred->uid.val = 0;
    cred->suid.val = 0;
    cred->euid.val = 0;
    cred->fsuid.val = 0;

    // 设置GID相关字段为root
    cred->gid.val = 0;
    cred->fsgid.val = 0;
    cred->sgid.val = 0;
    cred->egid.val = 0;
    
    // 清除securebits标志
    cred->securebits = 0;

    // 设置完整的capabilities
    memcpy(&cred->cap_effective, &full_cap, sizeof(cred->cap_effective));
    memcpy(&cred->cap_permitted, &full_cap, sizeof(cred->cap_permitted));
    memcpy(&cred->cap_bset, &full_cap, sizeof(cred->cap_bset));

    // 提交新的凭证
    commit_creds(cred);

    // 禁用seccomp
    spin_lock_irq(&current->sighand->siglock);
    disable_seccomp();
    spin_unlock_irq(&current->sighand->siglock);

    // 设置SELinux上下文
    setup_selinux_domain();
}

/**
 * 检查是否允许su权限
 * 根据配置文件中的allowed_uids数组检查
 */
static bool is_allow_su(void)
{
    uid_t uid = current_uid().val;
    int i;
    
    // 遍历允许的UID列表
    for (i = 0; i < ALLOWED_UIDS_COUNT; i++) {
        if (uid == allowed_uids[i]) {
            // minisu_debug("UID %d is allowed\n", uid);
            return true;
        }
    }
    
    minisu_debug("UID %d is not allowed\n", uid);
    return false;
}

/**
 * 处理prctl系统调用
 */
static int minisu_handle_prctl(int option, unsigned long arg2, unsigned long arg3,
                              unsigned long arg4, unsigned long arg5)
{
    u32 *result = (u32 *)arg5;
    u32 reply_ok = MINISU_OPTION;

    // 检查是否是MiniSU的prctl请求
    if (MINISU_OPTION != option) {
        return 0;
    }

    minisu_debug("prctl option: 0x%x, cmd: %ld\n", option, arg2);

    // 处理CMD_GRANT_ROOT命令
    if (arg2 == CMD_GRANT_ROOT) {
        if (is_allow_su()) {
            minisu_info("allow root for UID: %d\n", current_uid().val);
            escape_to_root();
            if (copy_to_user(result, &reply_ok, sizeof(reply_ok))) {
                minisu_err("grant_root reply error\n");
            }
        } else {
            minisu_info("deny root for UID: %d\n", current_uid().val);
        }
        return 0;
    }

    // 处理CMD_GET_VERSION命令
    if (arg2 == CMD_GET_VERSION) {
        u32 version = MINISU_VERSION;
        if (copy_to_user((void __user *)arg3, &version, sizeof(version))) {
            minisu_err("get_version reply error\n");
        }
        return 0;
    }

    return 0;
}

/**
 * 在用户栈空间创建缓冲区
 */
static void __user *userspace_stack_buffer(const void *d, size_t len)
{
    char __user *p = (void __user *)current_user_stack_pointer() - len;
    return copy_to_user(p, d, len) ? NULL : p;
}

/**
 * 获取shell路径的用户空间地址
 */
static char __user *sh_user_path(void)
{
    static const char sh_path[] = SH_PATH;
    return userspace_stack_buffer(sh_path, sizeof(sh_path));
}

/**
 * 处理faccessat系统调用 - su兼容层
 */
static int minisu_handle_faccessat(int *dfd, const char __user **filename_user, int *mode, int *flags)
{
    const char su[] = SU_PATH;
    char path[sizeof(su) + 1];

    // 检查当前用户UID是否允许
    if (!is_allow_su()) {
        return 0;
    }

    // 从用户空间复制文件名
    memset(path, 0, sizeof(path));
    if (copy_from_user(path, *filename_user, sizeof(path) - 1)) {
        return 0;
    }

    // 如果是su路径，重定向到shell
    if (unlikely(!memcmp(path, su, sizeof(su)))) {
        minisu_debug("faccessat su->sh redirect\n");
        *filename_user = sh_user_path();
    }

    return 0;
}

/**
 * 处理stat系统调用 - su兼容层
 */
static int minisu_handle_stat(int *dfd, const char __user **filename_user, int *flags)
{
    const char su[] = SU_PATH;
    char path[sizeof(su) + 1];

    // 检查当前用户UID是否允许
    if (!is_allow_su()) {
        return 0;
    }

    if (unlikely(!filename_user)) {
        return 0;
    }

    // 从用户空间复制文件名
    memset(path, 0, sizeof(path));
    if (copy_from_user(path, *filename_user, sizeof(path) - 1)) {
        return 0;
    }

    // 如果是su路径，重定向到shell
    if (unlikely(!memcmp(path, su, sizeof(su)))) {
        minisu_debug("stat su->sh redirect\n");
        *filename_user = sh_user_path();
    }

    return 0;
}

/**
 * 处理execveat系统调用 - su兼容层
 */
static int minisu_handle_execveat_sucompat(int *fd, struct filename **filename_ptr, void *argv, void *envp, int *flags)
{
    struct filename *filename;
    const char su[] = SU_PATH;

    if (unlikely(!filename_ptr))
        return 0;

    filename = *filename_ptr;
    if (IS_ERR(filename)) {
        return 0;
    }

    // 如果不是su路径，直接返回
    if (likely(memcmp(filename->name, su, sizeof(su))))
        return 0;

    // 检查权限
    if (!is_allow_su())
        return 0;

    minisu_info("execveat su found, granting root\n");
    
    // 修改文件名为shell路径
    memcpy((void *)filename->name, SH_PATH, sizeof(SH_PATH));

    // 提权到root
    escape_to_root();

    return 0;
}

/**
 * 处理execve系统调用 - su兼容层
 */
static int minisu_handle_execve_sucompat(int *fd, const char __user **filename_user, void *argv, void *envp, int *flags)
{
    const char su[] = SU_PATH;
    char path[sizeof(su) + 1];

    if (unlikely(!filename_user))
        return 0;

    memset(path, 0, sizeof(path));
    if (copy_from_user(path, *filename_user, sizeof(path) - 1)) {
        return 0;
    }

    // 如果不是su路径，直接返回
    if (likely(memcmp(path, su, sizeof(su))))
        return 0;

    // 检查权限
    if (!is_allow_su())
        return 0;

    minisu_info("execve su found, granting root\n");

    // 重定向到shell路径
    *filename_user = sh_user_path();

    // 提权到root
    escape_to_root();

    return 0;
}

/**
 * prctl kprobe预处理函数
 */
static int prctl_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct pt_regs *real_regs = PT_REAL_REGS(regs);
    int option = (int)PT_REGS_PARM1(real_regs);
    unsigned long arg2 = (unsigned long)PT_REGS_PARM2(real_regs);
    unsigned long arg3 = (unsigned long)PT_REGS_PARM3(real_regs);
    unsigned long arg4 = (unsigned long)PT_REGS_PARM4(real_regs);
    unsigned long arg5 = (unsigned long)PT_REGS_PARM5(real_regs);

    return minisu_handle_prctl(option, arg2, arg3, arg4, arg5);
}

/**
 * faccessat kprobe预处理函数
 */
static int faccessat_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct pt_regs *real_regs = PT_REAL_REGS(regs);
    int *dfd = (int *)&PT_REGS_PARM1(real_regs);
    const char __user **filename_user = (const char **)&PT_REGS_PARM2(real_regs);
    int *mode = (int *)&PT_REGS_PARM3(real_regs);

    return minisu_handle_faccessat(dfd, filename_user, mode, NULL);
}

/**
 * newfstatat kprobe预处理函数
 */
static int newfstatat_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct pt_regs *real_regs = PT_REAL_REGS(regs);
    int *dfd = (int *)&PT_REGS_PARM1(real_regs);
    const char __user **filename_user = (const char **)&PT_REGS_PARM2(real_regs);
    int *flags = (int *)&PT_REGS_PARM4(real_regs);

    return minisu_handle_stat(dfd, filename_user, flags);
}

/**
 * execve kprobe预处理函数
 */
static int execve_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct pt_regs *real_regs = PT_REAL_REGS(regs);
    const char __user **filename_user = (const char **)&PT_REGS_PARM1(real_regs);

    return minisu_handle_execve_sucompat(AT_FDCWD, filename_user, NULL, NULL, NULL);
}

// kprobe结构体数组
static struct kprobe prctl_kp = {
    .symbol_name = PRCTL_SYMBOL,
    .pre_handler = prctl_handler_pre,
};

static struct kprobe faccessat_kp = {
    .symbol_name = SYS_FACCESSAT_SYMBOL,
    .pre_handler = faccessat_handler_pre,
};

static struct kprobe newfstatat_kp = {
    .symbol_name = SYS_NEWFSTATAT_SYMBOL,
    .pre_handler = newfstatat_handler_pre,
};

static struct kprobe execve_kp = {
    .symbol_name = SYS_EXECVE_SYMBOL,
    .pre_handler = execve_handler_pre,
};

/**
 * 尝试注册kprobe，支持多个可能的符号名称
 */
static int try_register_kprobe(struct kprobe *kp, const char *primary_symbol, const char *fallback_symbol, const char *name)
{
    int rc;
    
    // 首先尝试主要符号
    kp->symbol_name = primary_symbol;
    rc = register_kprobe(kp);
    if (rc == 0) {
        minisu_info("%s kprobe registered with symbol: %s\n", name, primary_symbol);
        return 0;
    }
    
    // 如果主要符号失败，尝试备用符号
    if (fallback_symbol) {
        minisu_warn("%s kprobe failed with %s (%d), trying %s\n", name, primary_symbol, rc, fallback_symbol);
        kp->symbol_name = fallback_symbol;
        rc = register_kprobe(kp);
        if (rc == 0) {
            minisu_info("%s kprobe registered with fallback symbol: %s\n", name, fallback_symbol);
            return 0;
        }
    }
    
    minisu_err("register %s kprobe failed: %d\n", name, rc);
    return rc;
}

/**
 * 初始化kprobe
 */
static int minisu_kprobe_init(void)
{
    int rc;
    
    // 注册prctl kprobe
    rc = try_register_kprobe(&prctl_kp, PRCTL_SYMBOL, "sys_prctl", "prctl");
    if (rc) {
        return rc;
    }
    
    // 注册faccessat kprobe
    rc = try_register_kprobe(&faccessat_kp, SYS_FACCESSAT_SYMBOL, "sys_faccessat", "faccessat");
    if (rc) {
        minisu_warn("faccessat kprobe registration failed, su compatibility may not work\n");
        return -1;
    }
    
    // 注册newfstatat kprobe
    rc = try_register_kprobe(&newfstatat_kp, SYS_NEWFSTATAT_SYMBOL, "sys_newfstatat", "newfstatat");
    if (rc) {
        minisu_warn("newfstatat kprobe registration failed, su compatibility may not work\n");
        return -1;
    }
    
    // 注册execve kprobe
    rc = try_register_kprobe(&execve_kp, SYS_EXECVE_SYMBOL, "sys_execve", "execve");
    if (rc) {
        minisu_warn("execve kprobe registration failed, su compatibility may not work\n");
        return -1;
    }
    
    return 0;
}

/**
 * 安全地注销kprobe
 */
static void safe_unregister_kprobe(struct kprobe *kp, const char *name)
{
    if (kp->symbol_name) {
        unregister_kprobe(kp);
        minisu_info("%s kprobe unregistered\n", name);
        kp->symbol_name = NULL;
    }
}

/**
 * 退出kprobe
 */
static void minisu_kprobe_exit(void)
{
    safe_unregister_kprobe(&execve_kp, "execve");
    safe_unregister_kprobe(&newfstatat_kp, "newfstatat");
    safe_unregister_kprobe(&faccessat_kp, "faccessat");
    safe_unregister_kprobe(&prctl_kp, "prctl");
    minisu_info("all kprobes unregistered\n");
}

/**
 * MiniSU初始化函数
 */
/**
 * 检查符号是否存在
 */
static bool symbol_exists(const char *symbol_name)
{
    if (!lookup_name) {
        return false;
    }
    return lookup_name(symbol_name) != 0;
}

static int __init minisu_init(void)
{
    int ret;
    
    minisu_info("MiniSU v%d initializing on %s...\n", MINISU_VERSION, MINISU_PLATFORM);
    minisu_info("Kernel version: %d.%d.%d\n", 
                LINUX_VERSION_CODE >> 16,
                (LINUX_VERSION_CODE >> 8) & 0xFF,
                LINUX_VERSION_CODE & 0xFF);
    
    // 首先初始化kallsyms_lookup_name
    ret = init_kallsyms_lookup();
    if (ret) {
        minisu_err("Failed to initialize kallsyms_lookup_name: %d\n", ret);
        return ret;
    }
    
    minisu_info("SU path: %s -> %s\n", SU_PATH, SH_PATH);
    minisu_info("Allowed UIDs: ");
    
    int i;
    for (i = 0; i < ALLOWED_UIDS_COUNT; i++) {
        printk(KERN_CONT "%d ", allowed_uids[i]);
    }
    printk(KERN_CONT "\n");
    
    // 检查关键符号是否存在
    minisu_info("Checking symbols:\n");
    minisu_info("  %s: %s\n", PRCTL_SYMBOL, symbol_exists(PRCTL_SYMBOL) ? "found" : "not found");
    minisu_info("  sys_prctl: %s\n", symbol_exists("sys_prctl") ? "found" : "not found");

    ret = minisu_kprobe_init();
    if (ret) {
        return ret;
    }
    minisu_info("MiniSU initialized successfully\n");
    return 0;
}

/**
 * MiniSU清理函数
 */
static void __exit minisu_exit(void)
{
    minisu_info("MiniSU exiting...\n");

    minisu_kprobe_exit();

    minisu_info("MiniSU exited\n");
}

module_init(minisu_init);
module_exit(minisu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("feicong");
MODULE_DESCRIPTION("Minimal SU implementation");
MODULE_VERSION("1.0");
