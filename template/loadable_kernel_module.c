#include <linux/sched.h>
#include <linux/module.h>
#include <linux/syscalls.h>
#include <linux/dirent.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/sched/signal.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <asm/syscall.h>

#include "loadable_kernel_module.h"

#define OURMODNAME "loadable_kernel_module"

MODULE_AUTHOR("FOOBAR");
MODULE_DESCRIPTION("FOOBAR");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_VERSION("0.1");

static int major;
struct cdev *kernel_cdev;

static int lkm_open(struct inode *inode, struct file *filp)
{
	printk(KERN_INFO "%s\n", __func__);
	return 0;
}

static int lkm_release(struct inode *inode, struct file *filp)
{
	printk(KERN_INFO "%s\n", __func__);
	return 0;
}

static long lkm_ioctl(struct file *filp, unsigned int ioctl,
			  unsigned long arg)
{
	printk(KERN_INFO "%s\n", __func__);
	return 0;
}

struct file_operations fops = {
	open: lkm_open,
	unlocked_ioctl: lkm_ioctl,
	release: lkm_release,
	owner: THIS_MODULE
};

static int __init lkm_init(void)
{
	int ret;
	dev_t dev_no, dev;

	kernel_cdev = cdev_alloc();
	kernel_cdev->ops = &fops;
	kernel_cdev->owner = THIS_MODULE;

	ret = alloc_chrdev_region(&dev_no, 0, 1, "loadable_kernel_module");
	if (ret < 0) {
		pr_info("major number allocation failed\n");
		return ret;
	}

	major = MAJOR(dev_no);
	dev = MKDEV(major, 0);
	printk("The major number for your device is %d\n", major);
	ret = cdev_add(kernel_cdev, dev, 1);
	if (ret < 0) {
		pr_info(KERN_INFO "unable to allocate cdev");
		return ret;
	}

	return 0;
}

static void __exit lkm_exit(void)
{
	// TODO: unhook syscall

	pr_info("%s: removed\n", OURMODNAME);
	cdev_del(kernel_cdev);
	unregister_chrdev_region(major, 1);
}

module_init(lkm_init);
module_exit(lkm_exit);
