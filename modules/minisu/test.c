/**
 * MiniSU - Minimal Root Solution
 * 兼容 Linux 和 Android 系统的轻量级 root 工具
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>

// 检测平台
#ifdef __ANDROID__
    #define PLATFORM_NAME "Android"
    #define DEFAULT_SHELL "/system/bin/sh"
    #define DEFAULT_PATH "/system/bin:/system/xbin:/vendor/bin:/sbin"
#else
    #define PLATFORM_NAME "Linux"
    #define DEFAULT_SHELL "/bin/sh"
    #define DEFAULT_PATH "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>

// 包含 MiniSU 协议定义
#include "minisu.h"

/**
 * 获取 MiniSU 版本
 */
int get_minisu_version(void)
{
    unsigned int version = 0;
    unsigned int result = 0;
    
    printf("Testing get_version...\n");
    int ret = prctl(KERNEL_SU_OPTION, CMD_GET_VERSION, &version, 0, &result);
    
    printf("prctl returned: %d, version: %u, result: 0x%x\n", ret, version, result);
    
    if (result == KERNEL_SU_OPTION && version > 0) {
        printf("✓ MiniSU version: %u\n", version);
        return 0;
    } else {
        printf("✗ Failed to get MiniSU version\n");
        return -1;
    }
}

/**
 * 请求 root 权限
 */
int request_root(void)
{
    unsigned int result = 0;
    
    printf("\nTesting grant_root...\n");
    printf("Current UID: %d, GID: %d\n", getuid(), getgid());
    
    int ret = prctl(KERNEL_SU_OPTION, CMD_GRANT_ROOT, 0, 0, &result);
    
    printf("prctl returned: %d, result: 0x%x\n", ret, result);
    printf("New UID: %d, GID: %d\n", getuid(), getgid());
    
    if (result == KERNEL_SU_OPTION && getuid() == 0) {
        printf("✓ Root granted successfully!\n");
        return 0;
    } else {
        printf("✗ Failed to get root (this may be expected if UID not allowed)\n");
        return -1;
    }
}

/**
 * 测试 root 权限
 */
void test_root_privileges(void)
{
    printf("\n=== Testing root privileges ===\n");
    
    // 测试能否读取 /etc/shadow
    FILE *fp = fopen("/etc/shadow", "r");
    if (fp) {
        printf("✓ Can read /etc/shadow (root privilege confirmed)\n");
        fclose(fp);
    } else {
        printf("✗ Cannot read /etc/shadow: %s\n", strerror(errno));
    }
    
    // 测试能否写入系统文件
    fp = fopen("/proc/sys/kernel/hostname", "r");
    if (fp) {
        printf("✓ Can access /proc/sys/kernel/hostname\n");
        fclose(fp);
    } else {
        printf("✗ Cannot access /proc/sys/kernel/hostname: %s\n", strerror(errno));
    }
    
    // 测试能否创建临时文件在 /tmp
    fp = fopen("/tmp/minisu_test", "w");
    if (fp) {
        fprintf(fp, "test\n");
        fclose(fp);
        unlink("/tmp/minisu_test");
        printf("✓ Can write to /tmp\n");
    } else {
        printf("✗ Cannot write to /tmp: %s\n", strerror(errno));
    }
}

/**
 * 启动 root shell
 */
void spawn_root_shell(void)
{
    printf("\n=== Spawning root shell ===\n");
    printf("Welcome to MiniSU root shell!\n");
    printf("Platform: %s\n", PLATFORM_NAME);
    printf("Current UID: %d, GID: %d\n", getuid(), getgid());
    printf("Type 'exit' to quit.\n\n");
    
    // 设置环境变量
#ifdef __ANDROID__
    setenv("PS1", "minisu# ", 1);
    setenv("PATH", DEFAULT_PATH, 1);
    setenv("ANDROID_ROOT", "/system", 1);
    setenv("ANDROID_DATA", "/data", 1);
#else
    setenv("PS1", "minisu# ", 1);
    setenv("PATH", DEFAULT_PATH, 1);
#endif
    
    // 启动shell
    char *shell = getenv("SHELL");
    if (!shell) {
        shell = DEFAULT_SHELL;
    }
    
    printf("Starting shell: %s\n", shell);
    execl(shell, shell, NULL);
    
    // 如果execl失败，尝试默认shell
    printf("Failed to start %s, trying %s\n", shell, DEFAULT_SHELL);
    execl(DEFAULT_SHELL, DEFAULT_SHELL, NULL);
    
    // 如果还是失败
    perror("Failed to start any shell");
    exit(1);
}

int main(int argc, char *argv[])
{
    printf("=== MiniSU - Minimal Root Solution ===\n");
    printf("Platform: %s\n", PLATFORM_NAME);
#ifdef __ANDROID__
    printf("Android API Level: %d\n", __ANDROID_API__);
#endif
    printf("Current process: PID=%d, UID=%d, GID=%d\n", getpid(), getuid(), getgid());
    
    // 如果已经是root，直接启动shell
    if (getuid() == 0) {
        printf("\nAlready running as root!\n");
        spawn_root_shell();
        return 0;
    }
    
    // 测试获取版本
    printf("\n=== Step 1: Version Check ===\n");
    if (get_minisu_version() != 0) {
        printf("✗ MiniSU module may not be loaded or not working properly\n");
        printf("Please make sure to load the minisu.ko module first:\n");
        printf("  sudo insmod minisu.ko\n");
        return 1;
    }
    
    // 请求 root 权限
    printf("\n=== Step 2: Requesting Root Privileges ===\n");
    if (request_root() != 0) {
        printf("✗ Failed to get root privileges\n");
        printf("Check if:\n");
        printf("  1. minisu.ko module is loaded\n");
        printf("  2. Current UID (%d) is in the allowed list\n", getuid());
        printf("  3. kprobe hooks are working\n");
        return 1;
    }
    
    // 验证 root 权限
    printf("\n=== Step 3: Verifying Root Privileges ===\n");
    test_root_privileges();
    
    // 启动 root shell
    printf("\n=== Step 4: Launching Root Shell ===\n");
    spawn_root_shell();
    
    return 0;
}
