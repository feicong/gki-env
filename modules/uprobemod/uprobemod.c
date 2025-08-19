// Copyright (c) 2023-2025 fei_cong (https://github.com/feicong/ebpf-course)
#define DEBUG
#define pr_fmt(fmt) "feicong: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/uprobes.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/namei.h>
#include <linux/elf.h>
#include <linux/cred.h>
#include <linux/tty.h>  /* 用于tty操作 */

/* 直接打印到tty的函数 */
static void print_string(char *str) {
    /* 获取当前任务的tty */
    struct tty_struct *my_tty = get_current_tty();

    /* 如果my_tty为NULL，当前任务没有可以打印到的tty
     * （例如，如果它是一个守护进程）。如果是这样，我们无能为力。
     */
    if (my_tty) {
        /* my_tty->driver是一个包含函数的结构体，
         * 其中一个函数（write）用于向tty写入字符串。
         * 它可以用来接受来自用户或内核内存段的字符串。
         *
         * write函数的第一个参数是要写入的tty，因为
         * 同一个函数通常会被某种类型的所有tty使用。
         * 第二个参数是指向字符串的指针。
         * 第三个参数是字符串的长度。
         */
        const struct tty_operations *ttyops = my_tty->driver->ops;
        (ttyops->write)(my_tty, "[feicong] ", strlen("[feicong] "));
        (ttyops->write)(my_tty, str, strlen(str));

        /* tty最初是硬件设备，通常严格遵循ASCII标准。
         * 在ASCII中，要移动到新行，您需要两个字符：
         * 回车（Carriage Return, CR）和换行（Line Feed, LF）。
         * 在Unix上，ASCII的换行符（LF, `\n`）通常被同时用于这两个目的，
         * 但在原始的tty设备上，如果只发送`\n`，
         * 光标只会移动到下一行，但不会回到行首，
         * 导致下一行输出从上一行结束的列开始。
         * 这也是Unix和MS-DOS/Windows系统中文本文件换行符不同的历史原因。
         *
         * 在CP/M及其衍生系统（如MS-DOS和MS Windows）中，
         * 严格遵循ASCII标准，换行必须由CR和LF两个字符来完成。
         * 因此，为了确保在所有类型的tty上都能正确换行，
         * 我们需要显式地发送回车（`\015`）和换行（`\012`）两个字符。
         */
        (ttyops->write)(my_tty, "\015\012", 2);
    }
}

// 不同平台的目标库路径
#ifdef __ANDROID_COMMON_KERNEL__
    // Android GKI内核 - 使用APEX libc.so
    #if defined(CONFIG_X86_64)
    #define TARGET_PATH "/apex/com.android.runtime/lib64/bionic/libc.so"
    #elif defined(CONFIG_ARM64)
    #define TARGET_PATH "/apex/com.android.runtime/lib64/bionic/libc.so"
    #else
        #error "Unsupported architecture for Android"
    #endif
    #define KERNEL_TYPE "Android GKI"
#else
    // 常规Linux内核 - 使用系统libc.so
    #if defined(CONFIG_X86_64)
    #define TARGET_PATH "/lib/x86_64-linux-gnu/libc.so.6"
    #elif defined(CONFIG_ARM64)
    #define TARGET_PATH "/lib/aarch64-linux-gnu/libc.so.6"
    #else
        #error "Unsupported architecture for Linux"
    #endif
    #define KERNEL_TYPE "Linux"
#endif

#define SYMBOL_NAME "openat"
// nm -D /lib/aarch64-linux-gnu/libc.so.6 | grep " openat@" | awk '{print $1}'
// nm -D /lib/x86_64-linux-gnu/libc.so.6 | grep " openat@" | awk '{print $1}'
// sudo bpftrace -e 'uprobe:/lib/x86_64-linux-gnu/libc.so.6:openat {printf("%s\n", str(arg2));}'

// 模块参数
static int target_uid = 1000;
module_param(target_uid, int, 0644);
MODULE_PARM_DESC(target_uid, "Target UID to monitor (-1 for all users, default: 1000)");

static struct uprobe_consumer uprobe_consumer;
static struct inode *target_inode;
static unsigned long offset;

static loff_t find_symbol_offset(const char *filename, const char *symbol) {
    struct file *file;
    struct elfhdr elf_header;
    struct elf_shdr *section_headers;
    Elf64_Sym *dynsym = NULL;
    char *dynstr = NULL;
    loff_t offset = 0;
    ssize_t ret;
    int i, j;
    int dynsym_idx = -1, dynstr_idx = -1;
    size_t dynsym_size = 0;

    file = filp_open(filename, O_RDONLY, 0);
    if (IS_ERR(file)) {
        pr_err("Failed to open %s\n", filename);
        return 0;
    }

    // 读取ELF头部
    ret = kernel_read(file, &elf_header, sizeof(elf_header), 0);
    if (ret != sizeof(elf_header)) {
        pr_err("Failed to read ELF header\n");
        goto cleanup;
    }

    // 验证ELF魔数
    if (memcmp(elf_header.e_ident, ELFMAG, SELFMAG) != 0) {
        pr_err("Invalid ELF file\n");
        goto cleanup;
    }

    // 读取节头表
    section_headers = kmalloc(elf_header.e_shentsize * elf_header.e_shnum, GFP_KERNEL);
    if (!section_headers) {
        pr_err("Failed to allocate memory for section headers\n");
        goto cleanup;
    }

    loff_t shoff = elf_header.e_shoff;
    ret = kernel_read(file, section_headers, elf_header.e_shentsize * elf_header.e_shnum, &shoff);
    if (ret < 0) {
        pr_err("Failed to read section headers\n");
        goto cleanup_section_headers;
    }

    // 查找`.dynsym`和`.dynstr`节
    for (i = 0; i < elf_header.e_shnum; i++) {
        if (section_headers[i].sh_type == SHT_DYNSYM) {
            dynsym_idx = i;
            dynsym_size = section_headers[i].sh_size;
        }
        if (section_headers[i].sh_type == SHT_STRTAB && dynstr_idx == -1) {
            // 我们需要.dynstr节，通常是第一个STRTAB
            dynstr_idx = i;
        }
    }

    if (dynsym_idx == -1 || dynstr_idx == -1) {
        pr_err("Could not find .dynsym or .dynstr sections\n");
        goto cleanup_section_headers;
    }

    // 读取.dynsym节
    dynsym = kmalloc(dynsym_size, GFP_KERNEL);
    if (!dynsym) {
        pr_err("Failed to allocate memory for dynsym\n");
        goto cleanup_section_headers;
    }

    loff_t dynsym_offset = section_headers[dynsym_idx].sh_offset;
    ret = kernel_read(file, dynsym, dynsym_size, &dynsym_offset);
    if (ret < 0) {
        pr_err("Failed to read .dynsym section\n");
        goto cleanup_dynsym;
    }

    // 读取.dynstr节
    dynstr = kmalloc(section_headers[dynstr_idx].sh_size, GFP_KERNEL);
    if (!dynstr) {
        pr_err("Failed to allocate memory for dynstr\n");
        goto cleanup_dynsym;
    }

    loff_t dynstr_offset = section_headers[dynstr_idx].sh_offset;
    ret = kernel_read(file, dynstr, section_headers[dynstr_idx].sh_size, &dynstr_offset);
    if (ret < 0) {
        pr_err("Failed to read .dynstr section\n");
        goto cleanup_dynstr;
    }

    // 遍历`.dynsym`并查找符号
    for (j = 0; j < dynsym_size / sizeof(Elf64_Sym); j++) {
        if (dynsym[j].st_name < section_headers[dynstr_idx].sh_size) {
            char *sym_name = dynstr + dynsym[j].st_name;
            if (strcmp(sym_name, symbol) == 0) {
                offset = dynsym[j].st_value;
                pr_info("Found symbol %s at offset: 0x%llx\n", symbol, offset);
                break;
            }
        }
    }

    if (offset == 0) {
        pr_err("Symbol %s not found\n", symbol);
    }

cleanup_dynstr:
    kfree(dynstr);
cleanup_dynsym:
    kfree(dynsym);
cleanup_section_headers:
    kfree(section_headers);
cleanup:
    filp_close(file, NULL);
    return offset;
}

static int uprobe_handler(struct uprobe_consumer *self, struct pt_regs *regs) {
    char __user *filename;
    char filename_buf[256];
    char output_buf[512];
    ssize_t filename_len;
    uid_t current_uid;

    // 获取当前进程UID
    current_uid = from_kuid(&init_user_ns, current_uid());

    // 检查是否需要按UID过滤
    if (target_uid != -1 && current_uid != target_uid) {
        return 0; // 跳过此进程
    }

#if defined(CONFIG_X86_64)
    filename = (char __user *)regs->si;
#elif defined(CONFIG_ARM64)
    filename = (char __user *)regs->regs[1];
#else
    #error "Unsupported architecture"
#endif

    // 从用户空间读取文件名
    filename_len = strncpy_from_user(filename_buf, filename, sizeof(filename_buf) - 1);
    if (filename_len < 0) {
        print_string("uprobes: Failed to copy filename from userspace");
        return -EFAULT;
    }

    // 空终止字符串，确保没有缓冲区溢出
    filename_buf[filename_len] = '\0';

    // 检查文件名长度是否有效
    if (filename_len == 0 || filename_len >= sizeof(filename_buf)) {
        print_string("uprobes: Filename length is invalid, too long or zero");
        return -EFAULT;
    }

    // 格式化输出信息并打印到tty
    snprintf(output_buf, sizeof(output_buf), "uprobes: [UID:%u] openat() filename: %s", current_uid, filename_buf);
    print_string(output_buf);

    // 用'a'字符替换文件名
    memset(filename_buf, 'a', filename_len);
    filename_buf[filename_len] = '\0';

    // 确保用户内存在写回之前是有效的
    if (!access_ok(filename, filename_len)) {
        print_string("uprobes: Invalid user memory address");
        return -EFAULT;
    }

    // 将修改后的文件名复制回用户空间
    // if (copy_to_user(filename, filename_buf, filename_len)) {
    //    print_string("uprobes: Failed to copy new filename to userspace");
    //    return -EFAULT;
    //}

    return 0;
}

static int __init uprobe_init(void) {
    struct path path;
    int ret;
    char init_msg[256];

    // 打印内核类型和目标库
    snprintf(init_msg, sizeof(init_msg), "uprobes: Running on %s kernel, targeting %s", KERNEL_TYPE, TARGET_PATH);
    print_string(init_msg);

    // 打印配置信息
    if (target_uid == -1) {
        print_string("uprobes: Monitoring all users");
    } else {
        snprintf(init_msg, sizeof(init_msg), "uprobes: Monitoring UID %d", target_uid);
        print_string(init_msg);
    }

    ret = kern_path(TARGET_PATH, LOOKUP_FOLLOW, &path);
    if (ret) {
        pr_err("uprobes: Failed to resolve path %s (error: %d)\n", TARGET_PATH, ret);
#ifdef __ANDROID_COMMON_KERNEL__
        pr_err("uprobes: Make sure Android APEX runtime is available\n");
#endif
        return ret;
    }

    target_inode = d_inode(path.dentry);
    path_put(&path);

    offset = find_symbol_offset(TARGET_PATH, SYMBOL_NAME);
    if (!offset) {
        pr_err("uprobes: Failed to find symbol %s in %s\n", SYMBOL_NAME, TARGET_PATH);
        return -ENOENT;
    }

    uprobe_consumer.handler = uprobe_handler;

    ret = uprobe_register(target_inode, offset, &uprobe_consumer);
    if (ret) {
        pr_err("uprobes: Failed to register uprobe (error: %d)\n", ret);
        return ret;
    }

    snprintf(init_msg, sizeof(init_msg), "uprobes: Successfully registered uprobe for %s at offset 0x%lx", TARGET_PATH, offset);
    print_string(init_msg);
    return 0;
}

static void __exit uprobe_exit(void) {
    if (target_inode) {
        uprobe_unregister(target_inode, offset, &uprobe_consumer);
        target_inode = NULL;
    }
    print_string("uprobes: Unregistered uprobe");
}

module_init(uprobe_init);
module_exit(uprobe_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("feicong <fei_cong@hotmail.com>");
MODULE_DESCRIPTION("Uprobe hook sample");
MODULE_VERSION("1.0");
