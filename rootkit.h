#ifndef __ROOTKIT_HW2_H
#define __ROOTKIT_HW2_H

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
#define IOCTL_MOD_MASQ  _IOR(MAGIC, 1, struct masq_proc_req)
#define IOCTL_MOD_HOOK  _IO(MAGIC, 2)
#define IOCTL_FILE_HIDE _IOR(MAGIC, 3, struct hided_file)

#endif /* __ROOTKIT_HW2_H */
