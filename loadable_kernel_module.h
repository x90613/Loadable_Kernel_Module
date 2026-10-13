#ifndef __LOADABLE_KERNEL_MODULE_H
#define __LOADABLE_KERNEL_MODULE_H

#define MASQ_LEN  0x20
#define NAME_LEN  0x20
struct masq_proc {
	char new_name[MASQ_LEN];
	char orig_name[MASQ_LEN];
};

struct masq_proc_req {
	size_t len;
	struct masq_proc *list;
};

struct hided_file {
	size_t len;
	char name[NAME_LEN];
};

#define MAGIC 'k'
#define IOCTL_MOD_HIDE  _IO(MAGIC, 0)
// _IOW: userland writes the parameter, the kernel reads it (copy_from_user) —
// matches what lkm_ioctl actually does for these two commands.
#define IOCTL_MOD_MASQ  _IOW(MAGIC, 1, struct masq_proc_req)
#define IOCTL_MOD_HOOK  _IO(MAGIC, 2)
#define IOCTL_FILE_HIDE _IOW(MAGIC, 3, struct hided_file)

#endif /* __LOADABLE_KERNEL_MODULE_H */
