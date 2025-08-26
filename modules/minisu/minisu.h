/**
 * MiniSU配置文件
 * 用户可以根据需要修改这些配置
 */

#ifndef _MINISU_CONFIG_H
#define _MINISU_CONFIG_H

// MiniSU版本号
#define MINISU_VERSION 1000

// MiniSU选项标识符
#define MINISU_OPTION 0xDEADBEEF

// 固定的SELinux域
#define MINISU_SELINUX_DOMAIN "u:r:su:s0"

// 平台检测和路径定义
#ifdef __ANDROID_COMMON_KERNEL__
    // Android/GKI内核
    #define MINISU_PLATFORM "Android"
    #define SU_PATH "/system/bin/su"
    #define SH_PATH "/system/bin/sh"
    // Android允许的UID列表
    static const uid_t allowed_uids[] = {
        1000,  // system
        2000,  // shell
        // 可以在这里添加更多允许的UID
    };
#else
    // 标准Linux内核
    #define MINISU_PLATFORM "Linux"
    #define SU_PATH "/usr/bin/su"
    #define SH_PATH "/usr/bin/sh"
    // Linux允许的UID列表
    static const uid_t allowed_uids[] = {
        0,     // root (for testing)
        1000,  // 通常是普通用户的起始UID
        // 可以在这里添加更多允许的UID
    };
#endif

#define ALLOWED_UIDS_COUNT (sizeof(allowed_uids) / sizeof(allowed_uids[0]))

// 调试选项
#define MINISU_DEBUG 1

// 日志前缀
#define MINISU_LOG_PREFIX "minisu: "

// 调试日志宏
#if MINISU_DEBUG
#define minisu_debug(fmt, ...) pr_info(MINISU_LOG_PREFIX fmt, ##__VA_ARGS__)
#else
#define minisu_debug(fmt, ...) do { } while (0)
#endif

// 信息日志宏
#define minisu_info(fmt, ...) pr_info(MINISU_LOG_PREFIX fmt, ##__VA_ARGS__)

// 错误日志宏
#define minisu_err(fmt, ...) pr_err(MINISU_LOG_PREFIX fmt, ##__VA_ARGS__)

// 警告日志宏
#define minisu_warn(fmt, ...) pr_warn(MINISU_LOG_PREFIX fmt, ##__VA_ARGS__)

#endif /* _MINISU_CONFIG_H */