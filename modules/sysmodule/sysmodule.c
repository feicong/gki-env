// Copyright (C) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/security.h> // Required for security_getenforce/setenforce
#include <linux/kprobes.h>

static struct kobject *sysmodule_kobj;

static int *kptr_restrict_p;

/* sysfs show */
static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	if (!kptr_restrict_p)
		return -ENODEV;
	return sprintf(buf, "%d\n", *kptr_restrict_p);
}

/* sysfs store */
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

typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
static kallsyms_lookup_name_t kallsyms_lookup_name_func;

static struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };

static int __init sysmodule_init(void)
{
	int ret;

	pr_info("sysmodule: initializing\n");

	ret = register_kprobe(&kp);
	if (ret < 0) {
		pr_err("Failed to register kprobe for kallsyms_lookup_name, error %d\n",
		       ret);
		return ret;
	}

	kallsyms_lookup_name_func = (kallsyms_lookup_name_t)kp.addr;
	unregister_kprobe(&kp);

	if (!kallsyms_lookup_name_func) {
		pr_err("Failed to get address of kallsyms_lookup_name\n");
		return -ENXIO;
	}

	kptr_restrict_p =
		(int *)kallsyms_lookup_name_func("kptr_restrict");
	if (!kptr_restrict_p) {
		pr_err("Failed to find kptr_restrict symbol\n");
		return -ENXIO;
	}

	/* 创建 /sys/kernel/sysmodule */
	sysmodule_kobj = kobject_create_and_add("sysmodule", kernel_kobj);
	if (!sysmodule_kobj)
		return -ENOMEM;

	ret = sysfs_create_file(sysmodule_kobj, &status_attribute.attr);
	if (ret) {
		kobject_put(sysmodule_kobj);
		pr_err("Failed to create status file in /sys/kernel/sysmodule\n");
		return ret;
	}

	return 0;
}

static void __exit sysmodule_exit(void)
{
	pr_info("sysmodule: exiting\n");
	kobject_put(sysmodule_kobj);
}

module_init(sysmodule_init);
module_exit(sysmodule_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("feicong");
MODULE_DESCRIPTION("sysmodule with status node controlling kptr_restrict");
