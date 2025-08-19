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

// kretprobe的私有数据
struct my_data {
    int original_ptrace;
    long original_state;
    struct task_struct *task;
    int sequence_id;
};

// 全局序号用于记录执行顺序
static atomic_t sequence_counter = ATOMIC_INIT(0);

// Kprobe处理器
static int kprobe_pre_handler(struct kprobe *p, struct pt_regs *regs) {
    int seq = atomic_inc_return(&sequence_counter);
    pr_info("[SEQ:%d] KPROBE PRE_HANDLER: proc_pid_status called\n", seq);
    return 0;
}

static void kprobe_post_handler(struct kprobe *p, struct pt_regs *regs, unsigned long flags) {
    int seq = atomic_inc_return(&sequence_counter);
    pr_info("[SEQ:%d] KPROBE POST_HANDLER: proc_pid_status finished\n", seq);
}

static int entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct my_data *data;
    struct task_struct *task;
    struct seq_file *m;
    struct pid_namespace *ns;
    struct pid *pid;
    int seq = atomic_inc_return(&sequence_counter);

    data = (struct my_data *)ri->data;
    data->task = NULL;
    data->sequence_id = seq;

    pr_info("[SEQ:%d] KRETPROBE ENTRY_HANDLER: proc_pid_status entry\n", seq);

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
    int seq = atomic_inc_return(&sequence_counter);

    pr_info("[SEQ:%d] KRETPROBE HANDLER: proc_pid_status return (entry was SEQ:%d)\n",
            seq, data->sequence_id);

    if (!task)
        return 0;

    // 恢复任务状态
    task_lock(task);
    task->ptrace = data->original_ptrace;  // 恢复原始TracerPid

    if (data->original_state == TASK_TRACED) {
        WRITE_ONCE(task->__state, data->original_state);  // 安全地恢复原始任务状态
        pr_info("[SEQ:%d] Restored task state to TASK_TRACED for process %d\n", seq, task->pid);
    }

    task_unlock(task);

    pr_info("[SEQ:%d] Restored TracerPid for process %d to %d\n", seq, task->pid, data->original_ptrace);

    // 释放任务引用
    put_task_struct(task);

    return 0;
}

/* Kprobe structure */
static struct kprobe my_kprobe = {
    .symbol_name = "proc_pid_status",
    .pre_handler = kprobe_pre_handler,
    .post_handler = kprobe_post_handler,
};

/* Kretprobe structure */
static struct kretprobe my_kretprobe = {
    .handler = ret_handler,
    .entry_handler = entry_handler,
    .data_size = sizeof(struct my_data),
    .maxactive = 20,
    .kp = {
        .symbol_name = "proc_pid_status",
    },
};

static int __init probe_init(void) {
    int ret;

    pr_info("Starting kprobe and kretprobe test on proc_pid_status\n");

    /* Register kprobe first */
    ret = register_kprobe(&my_kprobe);
    if (ret < 0) {
        pr_err("register_kprobe failed, returned %d\n", ret);
        return ret;
    }
    pr_info("Planted kprobe at %s: %p\n", my_kprobe.symbol_name, my_kprobe.addr);

    /* Register kretprobe */
    ret = register_kretprobe(&my_kretprobe);
    if (ret < 0) {
        pr_err("register_kretprobe failed, returned %d\n", ret);
        unregister_kprobe(&my_kprobe);
        return ret;
    }
    pr_info("Planted kretprobe at %s: %p\n", my_kretprobe.kp.symbol_name, my_kretprobe.kp.addr);

    return 0;
}

static void __exit probe_exit(void) {
    unregister_kretprobe(&my_kretprobe);
    unregister_kprobe(&my_kprobe);
    pr_info("Both kprobe and kretprobe unregistered\n");
}

module_init(probe_init);
module_exit(probe_exit);
