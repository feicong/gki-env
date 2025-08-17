// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mount.h>
#include <errno.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "用法: %s <fake_cpuinfo路径> <要执行的程序路径> [参数...]\n", argv[0]);
        return 1;
    }

    const char *fake_cpuinfo = argv[1];
    const char *target_prog = argv[2];

    // 创建新的 mount namespace
    if (unshare(CLONE_NEWNS) != 0) {
        perror("unshare(CLONE_NEWNS) 失败");
        return 1;
    }

    // 重新挂载 /proc 为私有的（防止影响其他 namespace）
    if (mount(NULL, "/proc", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        perror("mount --make-rprivate /proc 失败");
        return 1;
    }

    // 绑定 fake_cpuinfo 到 /proc/cpuinfo
    if (mount(fake_cpuinfo, "/proc/cpuinfo", NULL, MS_BIND, NULL) != 0) {
        fprintf(stderr, "mount --bind %s -> /proc/cpuinfo 失败: %s\n", fake_cpuinfo, strerror(errno));
        return 1;
    }

    // 执行目标程序，查看替换效果
    execlp("sh", "sh", "-c", target_prog, NULL);

    // 如果 execvp 失败
    perror("execvp 失败");
    return 1;
}
