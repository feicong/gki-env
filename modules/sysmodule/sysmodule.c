// Copyright (C) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/sysfs.h>
#include <linux/kprobes.h>
#include <linux/sysctl.h>
#include <linux/version.h>

static struct kobject *sysmodule_kobj;

static int *kptr_restrict_p;

/*
 * Sysctl 相关部分
 */
// 用于proc_dointvec_minmax处理函数，限制写入值的范围
static int min_val = 0;
static int max_val = 2;

// 定义sysctl条目 "status"
static struct ctl_table sysmodule_sysctl_table[] = {
	{
		.procname	= "status",
		.data		= NULL, /* 数据指针在运行时动态设置 */
		.maxlen		= sizeof(int),
		.mode		= 0664, /* 权限：所有者和同组用户可读写，其他用户只读 */
		.proc_handler	= proc_dointvec_minmax, /* 使用内核提供的标准处理函数，支持范围检查 */
		.extra1		= &min_val, /* 最小值指针 */
		.extra2		= &max_val, /* 最大值指针 */
	},
	{ /* 哨兵，标志表格结束 */ }
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 6, 0)
/* 对于 5.6 以下的旧版本内核, 需要通过嵌套的 ctl_table 来构建目录结构 */

/* "sysmodule" 目录 */
static struct ctl_table sysmodule_sysctl_dir[] = {
	{
		.procname	= "sysmodule",
		.mode		= 0555, /* 目录权限 */
		.child		= sysmodule_sysctl_table, /* 指向子表 */
	},
	{ /* 哨兵 */ }
};

/* "kernel" 根目录 */
static struct ctl_table sysmodule_root_dir[] = {
    {
        .procname = "kernel",
        .mode = 0555,
        .child = sysmodule_sysctl_dir,
    },
    { /* 哨兵 */ }
};
#endif

/* 用于保存注册后的 sysctl 表头，以便后续注销 */
static struct ctl_table_header *sysctl_header;


/* sysfs show: 读取 /sys/kernel/sysmodule/status 时调用 */
static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	if (!kptr_restrict_p)
		return -ENODEV;
	return sprintf(buf, "%d\n", *kptr_restrict_p);
}

/* sysfs store: 写入 /sys/kernel/sysmodule/status 时调用 */
static ssize_t status_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	int val;

	if (!kptr_restrict_p)
		return -ENODEV;

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	if (val < 0 || val > 2)
		return -EINVAL;

	if (*kptr_restrict_p == val)
		return count;
	pr_info("change kptr_restrict from %d to %d\n", *kptr_restrict_p, val);
	*kptr_restrict_p = val;

	return count;
}

static struct kobj_attribute status_attribute =
	__ATTR(status, 0664, status_show, status_store);

/* 定义一个函数指针，用于保存 kallsyms_lookup_name 的地址 */
typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
static kallsyms_lookup_name_t kallsyms_lookup_name_func;

/* 定义一个 kprobe，用于查找 kallsyms_lookup_name */
static struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };

static int __init sysmodule_init(void)
{
	int ret;

	pr_info("sysmodule: 模块初始化\n");

	/* 注册 kprobe 以获取 kallsyms_lookup_name 的地址 */
	ret = register_kprobe(&kp);
	if (ret < 0) {
		pr_err("注册 kprobe 失败, 错误码 %d\n",
		       ret);
		return ret;
	}

	kallsyms_lookup_name_func = (kallsyms_lookup_name_t)kp.addr;
	unregister_kprobe(&kp); /* 获取地址后立即注销 kprobe */

	if (!kallsyms_lookup_name_func) {
		pr_err("获取 kallsyms_lookup_name 地址失败\n");
		return -ENXIO;
	}

	/* 使用获取到的函数查找 kptr_restrict 的地址 */
	kptr_restrict_p =
		(int *)kallsyms_lookup_name_func("kptr_restrict");
	if (!kptr_restrict_p) {
		pr_err("查找 kptr_restrict 符号失败\n");
		return -ENXIO;
	}

	/* 创建 /sys/kernel/sysmodule 目录 */
	sysmodule_kobj = kobject_create_and_add("sysmodule", kernel_kobj);
	if (!sysmodule_kobj)
		return -ENOMEM;

	/* 在 sysmodule 目录下创建 status 文件 */
	ret = sysfs_create_file(sysmodule_kobj, &status_attribute.attr);
	if (ret) {
		kobject_put(sysmodule_kobj);
		pr_err("创建 status 文件失败 /sys/kernel/sysmodule\n");
		return ret;
	}

	/* 注册 sysctl */
	sysmodule_sysctl_table[0].data = kptr_restrict_p; /* 将 "status" 条目与 kptr_restrict_p 关联 */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
	/* 对于 5.6 及以上的新版本内核，使用 register_sysctl */
	sysctl_header = register_sysctl("kernel/sysmodule", sysmodule_sysctl_table);
#else
	/* 对于旧版本内核，使用 register_sysctl_table */
	sysctl_header = register_sysctl_table(sysmodule_root_dir);
#endif

	if (!sysctl_header) {
		/* 如果失败，清理之前创建的 sysfs 入口 */
		sysfs_remove_file(sysmodule_kobj, &status_attribute.attr);
		kobject_put(sysmodule_kobj);
		pr_err("注册 sysctl 失败\n");
		return -ENOMEM;
	}

	return 0;
}

static void __exit sysmodule_exit(void)
{
	pr_info("sysmodule: 模块退出\n");
	/* 注销 sysctl 表 */
	unregister_sysctl_table(sysctl_header);
	/* kobject_put 会自动处理 sysfs 文件的移除 */
	kobject_put(sysmodule_kobj);
}

module_init(sysmodule_init);
module_exit(sysmodule_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("feicong");
MODULE_DESCRIPTION("一个通过 sysfs 和 sysctl 控制 kptr_restrict 的内核模块");
