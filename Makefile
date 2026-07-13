obj-m = loadable_kernel_module.o
PWD := $(shell pwd)
KDIR := /lib/modules/$(shell uname -r)/build
EXTRA_CFLAGS = -Wall -g

all:
	$(MAKE) ARCH=arm64 CROSS_COMPILE=$(CROSS) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

generateTestFile:
	gcc -I. test_src/userTest.c -o userTest 
	gcc -I. test_src/hsuckd.c -o hsuckd
	gcc -I. test_src/MIT.c -o MIT
	gcc -I. test_src/NTUST.c -o NTUST

cleanTestFile:
	rm -f userTest hsuckd MIT NTUST