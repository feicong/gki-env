// Copyright (c) 2023-2026 fei_cong (https://github.com/feicong/feicong-course)
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("feicong <fei_cong@hotmail.com>");
MODULE_DESCRIPTION("modify TracerPid and State value in /proc/[pid]/status");

/* per-instance private data */
struct my_data {
    int original_ptrace;
    long original_state;
    struct task_struct *task;
};

static int entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct my_data *data;
    struct task_struct *task;
    struct seq_file *m;
    struct pid_namespace *ns;
    struct pid *pid;

    data = (struct my_data *)ri->data;
    data->task = NULL;

    // 正确解析proc_pid_status的参数
    // static int proc_pid_status(struct seq_file *m, struct pid_namespace *ns, struct pid *pid, struct task_struct *task)
#if defined(CONFIG_X86_64)
    m = (struct seq_file *)regs->di;
    ns = (struct pid_namespace *)regs->si;
    pid = (struct pid *)regs->dx;
    task = (struct task_struct *)regs->cx;
#elif defined(CONFIG_ARM64)
    m = (struct seq_file *)regs->regs[0];
    ns = (struct pid_namespace *)regs->regs[1];
    pid = (struct pid *)regs->regs[2];
    task = (struct task_struct *)regs->regs[3];
#else
#error "Unsupported architecture"
#endif

    // 基本安全检查
    if (!task) {
        return 0;
    }

    // 检查是否是内核线程
    if (!task->mm) {
        return 0;    /* Skip kernel threads */
    }

    // 获取任务引用，防止任务在处理过程中被释放
    get_task_struct(task);

    // 安全地修改任务状态
    task_lock(task);
    data->task = task;
    data->original_ptrace = task->ptrace;  // 保存原始TracerPid

    data->original_state = READ_ONCE(task->__state);  // 保存原始任务状态
    if (data->original_state == TASK_TRACED) {
        WRITE_ONCE(task->__state, TASK_RUNNING);  // 安全地修改任务状态
        pr_info("Modified task state from TASK_TRACED to TASK_RUNNING for process %d\n", task->pid);
    }

    pr_info("Modified TracerPid for process %d to 0\n", task->pid);
    task->ptrace = 0;  // 设置TracerPid为0
    task_unlock(task);

    return 0;
}

static int ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct my_data *data = (struct my_data *)ri->data;
    struct task_struct *task = data->task;

    if (!task)
        return 0;

    // 恢复任务状态
    task_lock(task);
    task->ptrace = data->original_ptrace;  // 恢复原始TracerPid

    if (data->original_state == TASK_TRACED) {
        WRITE_ONCE(task->__state, data->original_state);  // 安全地恢复原始任务状态
        pr_info("Restored task state to TASK_TRACED for process %d\n", task->pid);
    }

    task_unlock(task);

    pr_info("Restored TracerPid for process %d to %d\n", task->pid, data->original_ptrace);

    // 释放任务引用
    put_task_struct(task);

    return 0;
}

static struct kretprobe my_kretprobe = {
    .handler = ret_handler,
    .entry_handler = entry_handler,
    .data_size = sizeof(struct my_data),
    .maxactive = 20,
    .kp = {
        .symbol_name = "proc_pid_status",
    },
};

static int __init kretprobe_init(void) {
    int ret = register_kretprobe(&my_kretprobe);
    if (ret < 0) {
        pr_err("register_kretprobe failed, returned %d\n", ret);
        return ret;
    }
    pr_info("Planted kretprobe at %s: %p\n", my_kretprobe.kp.symbol_name, my_kretprobe.kp.addr);
    return 0;
}

static void __exit kretprobe_exit(void) {
    unregister_kretprobe(&my_kretprobe);
    pr_info("kretprobe at %p unregistered\n", my_kretprobe.kp.addr);
}

module_init(kretprobe_init);
module_exit(kretprobe_exit);
