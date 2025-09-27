// Copyright (c) 2023-2026 fei_cong (https://github.com/feicong/feicong-course)
#include <fcntl.h>
#include <unistd.h>

int main() {
    int fd = openat(AT_FDCWD, "/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) close(fd);
    fd = openat(AT_FDCWD, "/proc/self/status", O_RDONLY);
    if (fd >= 0) close(fd);
    return 0;
}
