# ARM64 VM Setup (Kernel v5.15 Development Environment)

This guide walks you through building a kernel development environment on an Apple Silicon Mac using QEMU. You'll compile a custom Linux 5.15 kernel, boot it inside a VM, and load your own kernel module — all without touching your host OS kernel.

**Why this setup?**
- You can experiment with kernel code safely — crashes stay inside the VM
- You control the exact kernel version and config
- `nokaslr` (disabled address randomization) makes debugging and symbol resolution predictable

---

## Requirements

- macOS Apple Silicon (M-series)
- Docker Desktop — used to cross-compile the kernel in a consistent Linux environment
- Homebrew — used to install QEMU

---

## Environment Setup

Install QEMU (the emulator that will run our ARM64 VM) and create a working directory:

```bash
brew install qemu
mkdir -p ~/arm-vm && cd ~/arm-vm
```

All files for this project live under `~/arm-vm/`.

---

## Step 1 — Download Kernel Source

```bash
git clone --depth 1 --branch v5.15 https://github.com/torvalds/linux.git linux-5.15
```

`--depth 1` skips the full git history to save time and disk space (~1.5 GB vs ~5 GB). You get only the v5.15 snapshot.

> **Expected time:** 5–15 minutes depending on your connection.

---

## Step 2 — Build the Builder Container

We use Docker to get a stable Linux cross-compilation environment with the right toolchain. This avoids needing to install ARM64 gcc and build tools directly on macOS.

Create `Dockerfile.builder` in `~/arm-vm/`:

```dockerfile
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    gcc bc bison flex libssl-dev \
    make libelf-dev cpio binutils \
    libncurses-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
```

**What these packages do:**
| Package | Purpose |
|---|---|
| `gcc` | C compiler |
| `bc`, `bison`, `flex` | required by the kernel build system |
| `libssl-dev` | needed for kernel module signing |
| `libelf-dev` | required for BTF/debug info generation |
| `libncurses-dev` | needed if you want to use `menuconfig` |

Build the image:

```bash
docker build -f Dockerfile.builder -t kernel-builder .
```

---

## Step 3 — Build the Kernel

Start a container with the kernel source mounted into it:

```bash
docker run --rm -it \
  -v ~/arm-vm/linux-5.15:/build/linux \
  kernel-builder bash
```

The `-v` flag mounts your local `linux-5.15` directory into the container at `/build/linux`. Changes made inside the container (compiled output) persist on your host after the container exits.

Inside the container, run:

```bash
cd /build/linux

# Start with the default ARM64 config (a reasonable baseline)
make ARCH=arm64 defconfig

# Enable module / debug related configs
# These are needed for loadable kernel modules and kernel debugging
scripts/config --enable CONFIG_MODULES        # allow .ko modules to be loaded
scripts/config --enable CONFIG_MODULE_UNLOAD  # allow modules to be unloaded (rmmod)
scripts/config --enable CONFIG_DEBUG_INFO     # include DWARF debug symbols
scripts/config --enable CONFIG_DEBUG_INFO_DWARF4  # use DWARF4 format (gdb-compatible)
scripts/config --enable CONFIG_KALLSYMS       # embed kernel symbol table in the image
scripts/config --enable CONFIG_KALLSYMS_ALL   # include all symbols (not just exported ones)

# Optional: open a menu-based config editor to browse/tweak settings
# make ARCH=arm64 menuconfig

# Build the kernel image (this takes a while)
make ARCH=arm64 -j$(nproc) Image
```

**What is `defconfig`?** It applies a known-good set of defaults for ARM64. `menuconfig` lets you interactively browse the thousands of kernel config options.

**Why `KALLSYMS`?** This embeds a symbol table into the kernel binary so tools like `dmesg`, `kprobes`, and rootkit-style hooks can resolve function addresses by name at runtime. Without it, `/proc/kallsyms` is empty.

> **Expected time:** 10–30 minutes depending on your Mac's CPU.

After building, type `exit` to leave the container. The compiled kernel image is at:

```
~/arm-vm/linux-5.15/arch/arm64/boot/Image
```

---

## Step 4 — Prepare Rootfs

Download the Ubuntu 20.04 ARM64 root filesystem tarball on your host first:

```bash
cd ~/arm-vm
curl -LO https://cloud-images.ubuntu.com/releases/focal/release/ubuntu-20.04-server-cloudimg-arm64-root.tar.xz
```

Mounting a raw disk image requires loop device support, which macOS doesn't provide. Run the image creation steps inside the same Docker builder container with `--privileged` so loop mounts work. The `-v ~/arm-vm:/work` flag writes `cloud.img` directly to your host's `~/arm-vm/` directory.

```bash
docker run --rm -it --privileged \
  -v ~/arm-vm:/work \
  -w /work \
  kernel-builder bash
```

Inside the container (you are already root — no `sudo` needed):

```bash
apt-get update && apt-get install -y qemu-utils e2fsprogs

# Create a 20 GB raw disk image and format it as ext4
qemu-img create -f raw cloud.img 20g
mkfs.ext4 cloud.img

# Mount the image and extract the rootfs into it
mount cloud.img /mnt
tar xvf ubuntu-20.04-server-cloudimg-arm64-root.tar.xz -C /mnt
sync

# Disable cloud-init (prevents boot delays waiting for cloud metadata)
touch /mnt/etc/cloud/cloud-init.disabled

# Enable root login without password
sed -i 's|^root:[^:]*:|root::|' /mnt/etc/passwd

# Unmount
umount /mnt
exit
```

**What each step does:**
- `--privileged` — grants the container the capabilities needed for loop device mounts; without it, `mount cloud.img /mnt` fails
- `qemu-img create -f raw` — creates a plain binary disk image (no compression, QEMU can use it directly)
- `mkfs.ext4` — formats the image with a Linux filesystem
- `tar xvf ... -C /mnt` — unpacks the Ubuntu userspace into the mounted image
- `cloud-init.disabled` — skips cloud-init on boot so the VM starts immediately
- `sed -i` on `passwd` — sets an empty root password so you can log in via the serial console on first boot

> **File sizes:** tarball ~200 MB, resulting `cloud.img` ~20 GB (sparse on disk).

---

## Step 5 — Start the VM

Create `start-vm.sh` in `~/arm-vm/`:

```bash
#!/bin/bash
mkdir -p ~/arm-vm/shared

qemu-system-aarch64 \
  -M virt \
  -cpu cortex-a57 \
  -m 2G \
  -smp 2 \
  -kernel ./linux-5.15/arch/arm64/boot/Image \
  -append "root=/dev/vda rw console=ttyAMA0 nokaslr" \
  -drive file=./cloud.img,if=virtio,format=raw \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -fsdev local,security_model=passthrough,id=fsdev0,path=$HOME/arm-vm/shared \
  -device virtio-9p-pci,id=fs0,fsdev=fsdev0,mount_tag=hostshare \
  -nographic
```

**Flag reference:**

| Flag | Purpose |
|---|---|
| `-M virt` | Generic ARM virtual machine type |
| `-cpu cortex-a57` | Emulate a Cortex-A57 CPU |
| `-m 2G` / `-smp 2` | 2 GB RAM, 2 CPU cores |
| `-kernel` | Boot using our custom-compiled kernel image |
| `-append` | Kernel command line arguments (see below) |
| `-drive` | Attach the Ubuntu raw disk image |
| `-netdev` / `-device virtio-net-pci` | NAT network, forwards host port 2222 to VM port 22 |
| `-fsdev` / `-device virtio-9p-pci` | Share `~/arm-vm/shared` into the VM via VirtFS |
| `-nographic` | No GUI window; use serial console in this terminal |

**Kernel command line explained:**
- `root=/dev/vda` — the raw image has no partition table; the entire disk is the root filesystem
- `rw` — mount root read-write
- `console=ttyAMA0` — send kernel log output to the serial port (your terminal)
- `nokaslr` — disable kernel address space layout randomization; keeps symbol addresses fixed, which is essential for kernel module development and debugging

Make it executable and run it:

```bash
chmod +x start-vm.sh
./start-vm.sh
```

You'll see the kernel boot log scroll by in your terminal. This is the serial console — there is no graphical window.

> **To exit the VM later:** type `poweroff` inside the VM, or press `Ctrl-A X` in the QEMU terminal to force-quit.

### Mounting the Shared Folder Inside the VM

The shared folder (`~/arm-vm/shared` on your host) is exposed to the VM via VirtFS (9p). Mount it once after booting:

```bash
mkdir -p /mnt/host
mount -t 9p -o trans=virtio hostshare /mnt/host
```

To mount it automatically on every boot, add this line to `/etc/fstab` inside the VM:

```
hostshare  /mnt/host  9p  trans=virtio,version=9p2000.L,rw  0  0
```
---

## Step 6 — Configure the VM Environment

Log in via the serial console (username `root`, no password needed on first boot) and run the following setup steps.

**Get a DHCP address** (Ubuntu cloud images rely on DHCP for networking):

```bash
dhclient
```

**Configure SSH and allow root login:**

```bash
# Regenerate host keys (required on first boot of a cloud image clone)
dpkg-reconfigure openssh-server

# Allow root login over SSH
echo "PermitRootLogin yes" >> /etc/ssh/sshd_config

# Allow login with no password (convenient for local dev VMs)
echo "PermitEmptyPasswords yes" >> /etc/ssh/sshd_config

# Remove root's password so empty-password login works
passwd -d root

systemctl restart ssh
systemctl enable ssh
```

> **Security note:** These SSH settings are intentionally insecure. This is fine because the VM is only accessible locally (port 2222 on localhost) and used only for development.

**Disable snapd** (Ubuntu ships with snapd enabled; it generates noisy pop-up messages and background activity on the QEMU serial console):

```bash
systemctl stop snapd.service snapd.socket snapd.seeded.service
systemctl disable snapd.service snapd.socket snapd.seeded.service
systemctl mask snapd.service snapd.socket
```

Alternatively, you can use SSH key authentication (more secure and avoids empty-password login):

```bash
ssh-keygen  # run on your host, generates ~/.ssh/id_rsa and ~/.ssh/id_rsa.pub

# inside the VM:
mkdir -p /root/.ssh
# paste the contents of your host's ~/.ssh/id_rsa.pub into /root/.ssh/authorized_keys
```

---

## Step 7 — SSH into the VM

With SSH running inside the VM, you can now use a proper terminal from your host instead of the serial console. Open a new terminal tab and run:

```bash
ssh -o StrictHostKeyChecking=no -p 2222 root@localhost
```

`-p 2222` connects to the forwarded port. `StrictHostKeyChecking=no` skips the host-key prompt (useful since the VM's host key will change if you rebuild).

Verify that our custom kernel is running:

```bash
uname -r
# 5.15.0
```

You now have a full shell inside a VM running your self-compiled kernel.

---

## Step 8 — Develop Your Own Kernel Module

A **kernel module** (`.ko` file) is a piece of code that can be dynamically loaded into and unloaded from a running kernel without rebooting. This is how device drivers, filesystems, and (in security research) rootkits work.

On the host, create the module directory:

```bash
mkdir -p ~/arm-vm/mymodule && cd ~/arm-vm/mymodule
```

Create `mymodule.c`:

```c
#include <linux/init.h>    // for __init / __exit macros
#include <linux/module.h>  // for MODULE_LICENSE, module_init, module_exit

MODULE_LICENSE("GPL");     // required — kernel will refuse to load non-GPL modules by default

// __init tells the kernel it can free this function's memory after boot
static int __init mymodule_init(void) {
    printk(KERN_INFO "mymodule: loaded\n");  // like printf, but writes to kernel log
    return 0;  // returning non-zero would cause insmod to fail
}

// __exit marks this as cleanup code, only needed when unloading
static void __exit mymodule_exit(void) {
    printk(KERN_INFO "mymodule: unloaded\n");
}

module_init(mymodule_init);  // register the init function
module_exit(mymodule_exit);  // register the cleanup function
```

**`printk` vs `printf`:** In kernel space there is no standard C library. `printk` writes to the kernel ring buffer, which you read with `dmesg`. `KERN_INFO` is the log severity level (also: `KERN_ERR`, `KERN_WARNING`, `KERN_DEBUG`).

Create `Makefile`:

```makefile
obj-m += mymodule.o

all:
	make ARCH=arm64 -C ~/arm-vm/linux-5.15 M=$(PWD) modules

clean:
	make ARCH=arm64 -C ~/arm-vm/linux-5.15 M=$(PWD) clean
```

`obj-m` tells the kernel build system to build `mymodule.c` as a loadable module (not built into the kernel image). `-C` points to the kernel source tree; `M=$(PWD)` tells it where your module source lives.

Build (run this on your host, not inside the VM):

```bash
make
```

This produces `mymodule.ko` — the compiled module binary.

Copy the `.ko` into the VM over SSH:

```bash
scp -P 2222 mymodule.ko root@localhost:~
```

Inside the VM, load and test it:

```bash
insmod mymodule.ko      # insert module into the running kernel
dmesg | tail            # check kernel log — should show "mymodule: loaded"
lsmod | grep mymodule   # list loaded modules — confirms it's active
rmmod mymodule          # unload the module
dmesg | tail            # should now show "mymodule: unloaded"
```

**Common errors:**
| Error | Cause |
|---|---|
| `insmod: ERROR: could not insert module: Invalid module format` | Module was compiled against a different kernel version |
| `insmod: ERROR: could not insert module: Operation not permitted` | Module signing required — check your kernel config |
| `insmod: ERROR: could not insert module: Unknown symbol in module` | Module uses a kernel function that isn't exported — check `CONFIG_KALLSYMS` |

---

## Troubleshooting

**VM won't boot / kernel panic:**
- Make sure the kernel `Image` path in `start-vm.sh` is correct
- Check that the `.qcow2` file downloaded completely (`ls -lh` to verify size ~300 MB)

**`make` fails when building the module:**
- Make sure you ran `make ARCH=arm64 defconfig` and the full kernel build inside the container first
- The module build needs the kernel's compiled headers and `Module.symvers` to exist

**SSH connection refused:**
- Make sure you ran `systemctl start ssh` inside the VM
- Confirm the VM is still running (check the QEMU serial console terminal)

**No network / `dhclient` fails:**

The network interface may not have come up automatically. Bring it up manually first:

```bash
ip link set enp0s1 up
dhclient enp0s1
```

Then verify with `ip addr show enp0s1` — you should see an assigned IP. If the interface name differs on your VM, run `ip link` to list all interfaces.

**`dmesg` shows nothing after `insmod`:**
- Try `dmesg -w` to watch live, then run `insmod` in another terminal
- Check `journalctl -k` as an alternative

---

## Directory Structure

```
~/arm-vm/
├── Dockerfile.builder                       # Docker image for kernel compilation
├── linux-5.15/                              # kernel source + build artifacts
│   └── arch/arm64/boot/Image               # compiled kernel image
├── ubuntu-20.04-server-cloudimg-arm64-root.tar.xz  # downloaded rootfs tarball
├── cloud.img                                # VM disk image (rootfs, raw format)
├── start-vm.sh                              # QEMU launch script
├── shared/                                  # host↔VM shared folder (mounted at /mnt/host inside VM)
└── mymodule/                                # your kernel module
    ├── mymodule.c
    └── Makefile
```

---

## Key Concepts Summary

| Term | Meaning |
|---|---|
| **LKM** | Loadable Kernel Module — code that runs in kernel space, loaded/unloaded at runtime |
| **kernel space** | Memory region where the kernel runs; no memory protection, full hardware access |
| **user space** | Where normal programs run; isolated from kernel by the MMU |
| **`insmod` / `rmmod`** | Insert / remove a kernel module |
| **`dmesg`** | Print the kernel ring buffer (log messages from kernel and modules) |
| **`/proc/kallsyms`** | File exposing all kernel symbol addresses (requires `CONFIG_KALLSYMS`) |
| **KASLR** | Kernel Address Space Layout Randomization — we disable it (`nokaslr`) for predictable addresses |
| **`defconfig`** | A curated set of default kernel config options for a given architecture |
