# SPDX-License-Identifier: GPL-2.0-only

KDIR := /lib/modules/$(shell uname -r)/build
obj-m := luxyd-ai.o


all: kernel app

kernel:
	@echo "Building Linux kernel module..."
	$(MAKE) -C $(KDIR) M=$(PWD)

clean_kernel:
	@echo "Cleaning Linux kernel module..."
	$(MAKE) -C $(KDIR) M=$(PWD) clean

luxyd-ai-ioctl.o: luxyd-ai-ioctl.c luxyd-ai-ioctl.h
	gcc -c $< -o $@

libluxyd-ai-ioctl.a: luxyd-ai-ioctl.o
	ar rcs $@ $<

app: libluxyd-ai-ioctl.a
	@echo "Building test application..."
	gcc -Wall -I. luxyd-ai-test-app.c -o luxyd-ai-test-app -L. -lluxyd-ai-ioctl

clean_app:
	@echo "Cleaning test application..."
	rm -rf luxyd-ai-test-app

clean: clean_kernel clean_app

test:
	-sudo rmmod vboxvideo
	-sudo rmmod luxyd_ai
	sudo dmesg -C
	sudo insmod luxyd-ai.ko
	sudo $(PWD)/luxyd-ai-test-app
	sudo rmmod luxyd_ai
	sudo dmesg
