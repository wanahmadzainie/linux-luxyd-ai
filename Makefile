# SPDX-License-Identifier: GPL-2.0-only

KDIR := /lib/modules/$(shell uname -r)/build
obj-m := luxyd-ai.o


all: kernel app

kernel:
	$(MAKE) -C $(KDIR) M=$(PWD)

app: luxyd-ai-test-app.c
	gcc luxyd-ai-test-app.c -o luxyd-ai-test-app

test:
	-sudo rmmod luxyd_ai
	sudo dmesg -C
	sudo insmod luxyd-ai.ko
	sudo $(PWD)/luxyd-ai-test-app
	sudo dmesg

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -rf luxyd-ai-test-app
