# ARM64 VM Setup — UTM Version (Kernel v5.15 Development Environment)

This guide sets up a kernel v5.15 development environment on Apple Silicon Mac using UTM. Compared to the QEMU/Docker approach, this is simpler: UTM manages the VM, and we compile the kernel directly inside the VM — no cross-compilation, no glibc mismatch, no VirtFS workarounds.

**Why UTM instead of QEMU?**
- No Docker or cross-compilation toolchain needed
- Kernel is compiled natively inside the VM — no glibc version mismatch
- `linux-headers` and build tools install and work normally inside the VM
- Shared folder setup is done through UTM's GUI

---

## Requirements

- macOS Apple Silicon (M-series)
- UTM — free, download from utm.app or the Mac App Store
- ~30 GB free disk space

---

## Step 1 — Install UTM and Create a VM

1. Download and install UTM from [utm.app](https://mac.getutm.app/)
2. Open UTM → click **+** → **Virtualize**
3. Select **Linux**
4. Download an Ubuntu 22.04 ARM64 server ISO:
   ```
   https://cdimage.ubuntu.com/ubuntu/releases/jammy/release/ubuntu-22.04.5-live-server-arm64.iso
   ```
5. In UTM, point the ISO field to the downloaded file
6. Set RAM to **4 GB**, CPU cores to **4**, disk to **30 GB**
7. Boot and complete the Ubuntu installer (default options are fine)

After installation, log in and verify:

```bash
uname -r
# something like 5.15.0-xx-generic
```

---

## Step 2 — Install Build Dependencies

Inside the VM:

```bash
sudo apt-get update
sudo apt-get install -y \
    git gcc make bc bison flex \
    libssl-dev libelf-dev libncurses-dev \
    build-essential
```

---

## Step 3 — Download Kernel Source

```bash
git clone --depth 1 --branch v5.15 https://github.com/torvalds/linux.git
cd linux
```

`--depth 1` skips the full git history to save time and disk (~1.5 GB vs ~5 GB).

> **Expected time:** 5–15 minutes depending on your connection.

---

## Step 4 — Configure and Build the Kernel

```bash
cd ~/linux

# Start with the default ARM64 config
make ARCH=arm64 defconfig

# Enable module / debug related configs
scripts/config --enable CONFIG_MODULES
scripts/config --enable CONFIG_MODULE_UNLOAD
scripts/config --enable CONFIG_DEBUG_INFO
scripts/config --enable CONFIG_DEBUG_INFO_DWARF4
scripts/config --enable CONFIG_KALLSYMS
scripts/config --enable CONFIG_KALLSYMS_ALL

# Build the kernel image
make ARCH=arm64 -j$(nproc)

# Prepare the build tree for module compilation
make ARCH=arm64 modules_prepare
```

> **Expected time:** 20–40 minutes depending on your Mac's CPU.

---

## Step 5 — Install the Kernel and Update GRUB

```bash
cd ~/linux

# Install kernel modules into /lib/modules/
sudo make modules_install

# Install kernel image, System.map, and config into /boot/
# and automatically update GRUB
sudo make install

# Update GRUB bootloader config
sudo update-grub
```

Then reboot:

```bash
sudo reboot
```

At boot, GRUB will show the kernel selection menu. Choose **Ubuntu, with Linux 5.15.0**. If the menu doesn't appear, hold `Shift` during boot.

Verify after reboot:

```bash
uname -r
# 5.15.0
```

---

## Step 6 — Set Up Shared Folder (Host ↔ VM)

UTM supports sharing a host folder into the VM via VirtFS (same 9p protocol as QEMU).

**On the host (macOS):**

1. In UTM, select your VM → **Edit** → **Sharing**
2. Enable **Directory Share** and point it to a folder, e.g. `~/utm-shared`
3. Save and restart the VM

**Inside the VM:**

```bash
sudo apt-get install -y 9mount

mkdir -p ~/shared
sudo mount -t 9p -o trans=virtio share ~/shared
```

To auto-mount on every boot, add to `/etc/fstab`:

```
share  /root/shared  9p  trans=virtio,version=9p2000.L,rw  0  0
```

Place your module source in `~/utm-shared/` on the host — it appears at `~/shared/` inside the VM.

---

## Step 7 — Build and Load a Kernel Module

Since the kernel was compiled natively inside the VM, the build tree at `~/linux` is fully compatible — no glibc mismatch, no VirtFS write permission issues.

Create a `Makefile` in your module directory:

```makefile
obj-m = loadable_kernel_module.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	make ARCH=arm64 -C $(KDIR) M=$(PWD) modules

clean:
	make ARCH=arm64 -C $(KDIR) M=$(PWD) clean
```

After `sudo make install` from Step 5, `/lib/modules/5.15.0/build` exists and points to your kernel source — so the default `KDIR` just works.

Build and load:

```bash
cd ~/shared/Loadable_kernel_module
make
sudo insmod loadable_kernel_module.ko
dmesg | tail
lsmod | grep loadable_kernel_module
sudo rmmod loadable_kernel_module
```

---

## Troubleshooting

**GRUB menu doesn't appear:**
- Hold `Shift` immediately after the VM starts
- Or edit `/etc/default/grub`, set `GRUB_TIMEOUT=10`, then run `sudo update-grub`

**`make install` picks the wrong kernel name:**
- Run `ls /boot/vmlinuz*` to confirm the 5.15 image was installed
- Run `sudo update-grub` again and check the output for `5.15.0`

**No network in VM:**
```bash
ip link set enp0s1 up
dhclient enp0s1
```

**Shared folder not mounting:**
- Make sure the VM was restarted after enabling Directory Share in UTM
- Check `dmesg | grep 9p` for mount errors

**`insmod` fails with `Invalid module format`:**
- The `.ko` was compiled against a different kernel — make sure `uname -r` matches the kernel you compiled

---

## Directory Structure

```
~/                              (inside the VM)
├── linux/                      # kernel source + build artifacts
│   └── arch/arm64/boot/Image  # compiled kernel image
└── shared/                     # shared folder from host (~/utm-shared on Mac)
    └── Loadable_kernel_module/
        ├── loadable_kernel_module.c
        ├── loadable_kernel_module.h
        └── Makefile
```

---

## Key Concepts Summary

| Term | Meaning |
|---|---|
| **UTM** | macOS VM manager built on QEMU/Apple Hypervisor; handles the GUI and VM config |
| **Native compilation** | Kernel and modules compiled inside the VM with the same gcc — no cross-compilation |
| **GRUB** | Bootloader that lets you select which kernel to boot |
| **`make install`** | Copies the compiled kernel into `/boot/` and registers it with GRUB |
| **`make modules_install`** | Copies compiled modules into `/lib/modules/$(uname -r)/` |
| **`/lib/modules/$(uname -r)/build`** | Symlink to the kernel build tree — used by out-of-tree module builds |
| **LKM** | Loadable Kernel Module — code that runs in kernel space, loaded/unloaded at runtime |
| **`insmod` / `rmmod`** | Insert / remove a kernel module |
| **`dmesg`** | Print the kernel ring buffer (log messages from kernel and modules) |
| **KASLR** | Kernel Address Space Layout Randomization — disable with `nokaslr` for predictable addresses |
