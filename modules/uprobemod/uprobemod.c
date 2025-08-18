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

#if defined(CONFIG_X86_64)
#define TARGET_PATH "/lib/x86_64-linux-gnu/libc.so.6"
#elif defined(CONFIG_ARM64)
#define TARGET_PATH "/lib/aarch64-linux-gnu/libc.so.6"
#else
    #error "Unsupported architecture"
#endif


#define SYMBOL_NAME "openat"
// nm -D /lib/aarch64-linux-gnu/libc.so.6 | grep " openat@" | awk '{print $1}'
// nm -D /lib/x86_64-linux-gnu/libc.so.6 | grep " openat@" | awk '{print $1}'

static struct uprobe_consumer uprobe_consumer;
static struct inode *target_inode;
static unsigned long offset;

static loff_t find_symbol_offset(const char *filename, const char *symbol) {
    struct file *file;
    struct elfhdr elf_header;
    struct elf_shdr *section_headers;
    Elf64_Sym *dynsym;
    char *dynstr;
    loff_t offset = 0;
    ssize_t ret;
    int i;

    file = filp_open(filename, O_RDONLY, 0);
    if (IS_ERR(file)) {
        pr_err("Failed to open %s\n", filename);
        return -1;
    }

    // Read ELF header
    ret = kernel_read(file, &elf_header, sizeof(elf_header), 0);
    if (ret != sizeof(elf_header)) {
        pr_err("Failed to read ELF header\n");
        goto cleanup;
    }

    // Read section headers
    section_headers = kmalloc(elf_header.e_shentsize * elf_header.e_shnum, GFP_KERNEL);
    if (!section_headers)
        goto cleanup;

    ret = kernel_read(file, section_headers, elf_header.e_shentsize * elf_header.e_shnum, (loff_t *)&elf_header.e_shoff);
    if (ret < 0) {
        pr_err("Failed to read section headers\n");
        goto cleanup_section_headers;
    }

    // Find `.dynsym` and `.dynstr` sections
    for (i = 0; i < elf_header.e_shnum; i++) {
        if (section_headers[i].sh_type == SHT_DYNSYM) {
            dynsym = kmalloc(section_headers[i].sh_size, GFP_KERNEL);
            if (!dynsym)
                goto cleanup_section_headers;
            kernel_read(file, dynsym, section_headers[i].sh_size, (loff_t *)&section_headers[i].sh_offset);
        }
        if (section_headers[i].sh_type == SHT_STRTAB) {
            dynstr = kmalloc(section_headers[i].sh_size, GFP_KERNEL);
            if (!dynstr)
                goto cleanup_dynsym;
            kernel_read(file, dynstr, section_headers[i].sh_size, (loff_t *)&section_headers[i].sh_offset);
        }
    }

    // Traverse `.dynsym` and find `openat`
    for (i = 0; i < section_headers[i].sh_size / sizeof(Elf64_Sym); i++) {
        if (strcmp(dynstr + dynsym[i].st_name, symbol) == 0) {
            offset = dynsym[i].st_value;
            pr_info("Found symbol %s at offset: 0x%llx\n", symbol, offset);
            break;
        }
    }

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
    ssize_t filename_len;

#if defined(CONFIG_X86_64)
    filename = (char __user *)regs->si;
#elif defined(CONFIG_ARM64)
    filename = (char __user *)regs->regs[1];
#else
    #error "Unsupported architecture"
#endif

    // Read the filename from user space
    filename_len = strncpy_from_user(filename_buf, filename, sizeof(filename_buf) - 1);
    if (filename_len < 0) {
        pr_info("uprobes: Failed to copy filename from userspace\n");
        return -EFAULT;
    }

    // Null-terminate the string, ensuring no buffer overflow
    filename_buf[filename_len] = '\0';

    // Check if filename length is valid
    if (filename_len == 0 || filename_len >= sizeof(filename_buf)) {
        pr_info("uprobes: Filename length is invalid, too long or zero\n");
        return -EFAULT;
    }

    // Print the full filename from the buffer
    pr_info("uprobes: openat() filename: %s\n", filename_buf);

    // Replace the filename with 'a' characters
    memset(filename_buf, 'a', filename_len);
    filename_buf[filename_len] = '\0';

    // Ensure the user memory is valid before writing back
    if (!access_ok(filename, filename_len)) {
        pr_info("uprobes: Invalid user memory address\n");
        return -EFAULT;
    }

    // Copy modified filename back to user space
    // if (copy_to_user(filename, filename_buf, filename_len)) {
    //    pr_info("uprobes: Failed to copy new filename to userspace\n");
    //    return -EFAULT;
    //}

    return 0;
}

static int __init uprobe_init(void) {
    struct path path;
    int ret;

    ret = kern_path(TARGET_PATH, LOOKUP_FOLLOW, &path);
    if (ret) {
        pr_err("uprobes: Failed to resolve path\n");
        return ret;
    }

    target_inode = d_inode(path.dentry);
    path_put(&path);

    offset = find_symbol_offset(TARGET_PATH, SYMBOL_NAME);
    offset = 0x00000000000d7b80;
    if (!offset) {
        pr_err("uprobes: Failed to find symbol %s\n", SYMBOL_NAME);
        return -ENOENT;
    }

    uprobe_consumer.handler = uprobe_handler;

    ret = uprobe_register(target_inode, offset, &uprobe_consumer);
    if (ret) {
        pr_err("uprobes: Failed to register uprobe\n");
        return ret;
    }

    pr_info("uprobes: Successfully registered uprobe\n");
    return 0;
}

static void __exit uprobe_exit(void) {
    if (target_inode) {
        uprobe_unregister(target_inode, offset, &uprobe_consumer);
        target_inode = NULL;
    }
    pr_info("uprobes: Unregistered uprobe\n");
}

module_init(uprobe_init);
module_exit(uprobe_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("fei_cong");
MODULE_DESCRIPTION("Uprobe monitor for function: " SYMBOL_NAME);

