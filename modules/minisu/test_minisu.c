/**
 * MiniSU测试程序
 * 用于测试MiniSU模块的基本功能
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <errno.h>
#include <string.h>

#define MINISU_OPTION 0xDEADBEEF
#define CMD_GRANT_ROOT 0
#define CMD_GET_VERSION 2

/**
 * 获取MiniSU版本
 */
int get_minisu_version(void)
{
    unsigned int version = 0;
    unsigned int result = 0;
    
    int ret = prctl(MINISU_OPTION, CMD_GET_VERSION, &version, 0, &result);
    
    if (ret == 0 && result == MINISU_OPTION) {
        printf("MiniSU version: %u\n", version);
        return 0;
    } else {
        printf("Failed to get MiniSU version (ret=%d, result=0x%x)\n", ret, result);
        return -1;
    }
}

/**
 * 请求root权限
 */
int request_root(void)
{
    unsigned int result = 0;
    
    printf("Current UID: %d, GID: %d\n", getuid(), getgid());
    
    int ret = prctl(MINISU_OPTION, CMD_GRANT_ROOT, 0, 0, &result);
    
    if (ret == 0 && result == MINISU_OPTION) {
        printf("Root granted successfully!\n");
        printf("New UID: %d, GID: %d\n", getuid(), getgid());
        return 0;
    } else {
        printf("Failed to get root (ret=%d, result=0x%x)\n", ret, result);
        printf("This may be expected if current UID is not allowed\n");
        return -1;
    }
}

/**
 * 测试root权限
 */
void test_root_privileges(void)
{
    printf("\n=== Testing root privileges ===\n");
    
    // 测试能否读取/etc/shadow
    FILE *fp = fopen("/etc/shadow", "r");
    if (fp) {
        printf("✓ Can read /etc/shadow (root privilege confirmed)\n");
        fclose(fp);
    } else {
        printf("✗ Cannot read /etc/shadow: %s\n", strerror(errno));
    }
    
    // 测试能否写入/proc/sys/kernel/hostname
    fp = fopen("/proc/sys/kernel/hostname", "w");
    if (fp) {
        printf("✓ Can write to /proc/sys/kernel/hostname (root privilege confirmed)\n");
        fclose(fp);
    } else {
        printf("✗ Cannot write to /proc/sys/kernel/hostname: %s\n", strerror(errno));
    }
}

int main(int argc, char *argv[])
{
    printf("=== MiniSU Test Program ===\n\n");
    
    // 检测平台
    printf("=== Platform Detection ===\n");
#ifdef __ANDROID__
    printf("Platform: Android\n");
    printf("Expected SU path: /system/bin/su\n");
    printf("Expected Shell path: /system/bin/sh\n");
#else
    printf("Platform: Linux\n");
    printf("Expected SU path: /usr/bin/su\n");
    printf("Expected Shell path: /usr/bin/sh\n");
#endif
    
    // 测试获取版本
    printf("\n=== Testing version query ===\n");
    if (get_minisu_version() != 0) {
        printf("MiniSU module may not be loaded or not working properly\n");
        return 1;
    }
    
    printf("\n=== Testing root request ===\n");
    
    // 如果已经是root，先切换到普通用户进行测试
    if (getuid() == 0) {
#ifdef __ANDROID__
        printf("Already running as root, switching to UID 2000 (shell) for testing...\n");
        if (setuid(2000) != 0) {
            printf("Failed to switch to UID 2000: %s\n", strerror(errno));
            return 1;
        }
#else
        printf("Already running as root, switching to UID 1000 for testing...\n");
        if (setuid(1000) != 0) {
            printf("Failed to switch to UID 1000: %s\n", strerror(errno));
            return 1;
        }
#endif
        printf("Switched to UID: %d\n", getuid());
    }
    
    // 请求root权限
    request_root();
    
    // 测试root权限
    test_root_privileges();
    
    printf("\n=== Test completed ===\n");
    return 0;
}