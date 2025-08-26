/**
 * MiniSU su兼容层测试程序
 * 测试su命令的重定向功能
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>

// 平台相关的路径定义
#ifdef __ANDROID__
#define SU_PATH "/system/bin/su"
#define SH_PATH "/system/bin/sh"
#define PLATFORM "Android"
#else
#define SU_PATH "/usr/bin/su"
#define SH_PATH "/usr/bin/sh"
#define PLATFORM "Linux"
#endif

/**
 * 测试文件访问
 */
void test_file_access(void)
{
    printf("=== Testing file access ===\n");
    
    // 测试access系统调用
    printf("Testing access(%s, F_OK): ", SU_PATH);
    if (access(SU_PATH, F_OK) == 0) {
        printf("✓ File exists (redirected to %s)\n", SH_PATH);
    } else {
        printf("✗ File not found: %s\n", strerror(errno));
    }
    
    // 测试stat系统调用
    struct stat st;
    printf("Testing stat(%s): ", SU_PATH);
    if (stat(SU_PATH, &st) == 0) {
        printf("✓ Stat successful (redirected to %s)\n", SH_PATH);
        printf("  File mode: %o\n", st.st_mode & 0777);
        printf("  File size: %ld bytes\n", st.st_size);
    } else {
        printf("✗ Stat failed: %s\n", strerror(errno));
    }
}

/**
 * 测试su命令执行
 */
void test_su_execution(void)
{
    printf("\n=== Testing su execution ===\n");
    
    pid_t pid = fork();
    if (pid == 0) {
        // 子进程：执行su命令
        printf("Child: Executing %s...\n", SU_PATH);
        execl(SU_PATH, "su", "-c", "id", NULL);
        
        // 如果execl失败
        printf("Child: execl failed: %s\n", strerror(errno));
        exit(1);
    } else if (pid > 0) {
        // 父进程：等待子进程完成
        int status;
        waitpid(pid, &status, 0);
        
        if (WIFEXITED(status)) {
            printf("Parent: Child exited with status %d\n", WEXITSTATUS(status));
        } else {
            printf("Parent: Child terminated abnormally\n");
        }
    } else {
        printf("Fork failed: %s\n", strerror(errno));
    }
}

/**
 * 测试shell重定向
 */
void test_shell_redirect(void)
{
    printf("\n=== Testing shell redirect ===\n");
    
    // 直接测试shell是否可用
    printf("Testing direct shell access (%s): ", SH_PATH);
    if (access(SH_PATH, X_OK) == 0) {
        printf("✓ Shell is executable\n");
        
        // 执行简单的shell命令
        pid_t pid = fork();
        if (pid == 0) {
            execl(SH_PATH, "sh", "-c", "echo 'Shell redirect test successful'", NULL);
            exit(1);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
        }
    } else {
        printf("✗ Shell not accessible: %s\n", strerror(errno));
    }
}

int main(int argc, char *argv[])
{
    printf("=== MiniSU Su Compatibility Test ===\n");
    printf("Platform: %s\n", PLATFORM);
    printf("Current UID: %d, GID: %d\n", getuid(), getgid());
    printf("SU path: %s\n", SU_PATH);
    printf("Shell path: %s\n", SH_PATH);
    printf("\n");
    
    // 测试文件访问重定向
    test_file_access();
    
    // 测试su命令执行重定向
    test_su_execution();
    
    // 测试shell重定向
    test_shell_redirect();
    
    printf("\n=== Test completed ===\n");
    return 0;
}