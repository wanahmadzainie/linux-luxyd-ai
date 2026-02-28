# SPDX-License-Identifier: GPL-2.0-only

KDIR ?= /lib/modules/$(shell uname -r)/build

obj-m := luxyd-fpga-pci.o

all: kernel app

clean: clean_kernel clean_app

kernel:
	@echo "Building Linux kernel module..."
	$(MAKE) -C $(KDIR) M=$(PWD)

clean_kernel:
	@echo "Cleaning Linux kernel module..."
	$(MAKE) -C $(KDIR) M=$(PWD) clean

app:
	@echo "Building test application..."
	gcc -Wall -static luxyd-app.c -o luxyd-app
	gcc -Wall -static gemv-app.c -o gemv-app -lm
	gcc -Wall -static dma-test.c -o dma-test

clean_app:
	@echo "Cleaning test application..."
	rm -rf luxyd-app
	rm -rf gemv-app
	rm -rf dma-test
