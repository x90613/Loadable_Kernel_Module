obj-m = rootkit.o
PWD := $(shell pwd)
KDIR := /lib/modules/$(shell uname -r)/build
EXTRA_CFLAGS = -Wall -g

all:
	$(MAKE) ARCH=arm64 CROSS_COMPILE=$(CROSS) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

generateTestFile:
	gcc userTest.c -o userTest 
	gcc hsuckd.c -o hsuckd
	gcc MIT.c -o MIT
	gcc NTUST.c -o NTUST

cleanTestFile:
	rm -f userTest hsuckd MIT NTUST