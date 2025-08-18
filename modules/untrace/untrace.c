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

    rcu_read_lock();

    data = (struct my_data *)ri->data;
    struct pid *pid;

#if defined(CONFIG_X86_64)
    pid = (struct pid *)regs->dx;
    task = (struct task_struct *)regs->cx;
#elif defined(CONFIG_ARM64)
    pid = (struct pid *)regs->regs[2];
    task = (struct task_struct *)regs->regs[3];
#else
#error "Unsupported architecture"
#endif

    data->task = NULL;

    if (task && !(task->mm)) {
        rcu_read_unlock();
        return 0;    /* Skip kernel threads */
    }

    // Modify task->ptrace safely using task_lock
    task_lock(task);
    data->task = task;
    data->original_ptrace = task->ptrace;  // Save original TracerPid

    data->original_state = READ_ONCE(task->__state);  // Save original task state
    if (data->original_state == TASK_TRACED) {
        WRITE_ONCE(task->__state, TASK_RUNNING);  // Use WRITE_ONCE to safely modify task state
        pr_info("Modified task state from TASK_TRACED to TASK_RUNNING for process %d\n", task->pid);
    }

    task->ptrace = 0;  // Set TracerPid to 0
    task_unlock(task);

    rcu_read_unlock();

    return 0;
}

static int ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct my_data *data = (struct my_data *)ri->data;
    struct task_struct *task = data->task;

    if (!task)
        return 0;

    rcu_read_lock();

    task_lock(task);
    task->ptrace = data->original_ptrace;  // Restore original TracerPid

    if (data->original_state == TASK_TRACED) {
        WRITE_ONCE(task->__state, data->original_state);  // Restore original task state safely
        pr_info("Restored task state to TASK_TRACED for process %d\n", task->pid);
    }

    task_unlock(task);

    pr_info("Restored TracerPid for process %d to %d\n", task->pid, data->original_ptrace);

    rcu_read_unlock();

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
