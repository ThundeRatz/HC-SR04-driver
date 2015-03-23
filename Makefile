KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
obj-m += hc-sr04.o

.PHONY: all local clean
all:
	make ARCH=arm -C ${KERNEL_SRC} M=$(PWD) modules

clean: 
	make -C ${KERNEL_SRC} M=$(PWD) clean
