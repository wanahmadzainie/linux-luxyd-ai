// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, Luxyd Technologies
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "luxyd-ai-ioctl.h"

#define DRIVER_NAME	"luxyd-ai"

int luxyd_dev_open(const char *devname)
{
	int fd;

	fd = open(devname, O_RDWR);
	if (fd < 0)
		printf("[%s] cannot open %s\n", DRIVER_NAME, devname);
	else
		printf("[%s] %s opened\n", DRIVER_NAME, devname);

	return fd;
}

void *luxyd_dev_init(int fd, int *mmap_size)
{
	unsigned long page_size;
	void *mmap_ptr;

	page_size = sysconf(_SC_PAGE_SIZE);
	*mmap_size = (*mmap_size + page_size - 1) & ~(page_size - 1);

	mmap_ptr = mmap(NULL,
			*mmap_size,
			PROT_READ | PROT_WRITE,
			MAP_SHARED,
			fd,
			0);
	if (mmap_ptr == MAP_FAILED) {
		printf("[%s] failed to mmap device memory\n", DRIVER_NAME);
		return NULL;
	}

	memset(mmap_ptr, 0xff, *mmap_size);

	printf("[%s] device memory mapped at address %p\n",
	       DRIVER_NAME, mmap_ptr);

	return mmap_ptr;
}

int luxyd_dev_matrix_load(int fd, void *mmap_ptr,
			  struct matrix_info *matrix_info)
{
	int ret;

	if (!mmap_ptr) {
		printf("[%s] invalid mmap_ptr\n", DRIVER_NAME);
		return -ENOMEM;
	}

	printf("[%s] valid mmap_ptr address %p\n", DRIVER_NAME, mmap_ptr);

	/* sending matrices information to kernel space */
	printf("[%s] calling LUXYD_IO_MATRIX_LOAD ioctl command\n",
	       DRIVER_NAME);
	ret = ioctl(fd, LUXYD_AI_MATRIX_LOAD, matrix_info);
	if (ret) {
		printf("[%s] LUXYD_AI_MATRIX_LOAD failed\n", DRIVER_NAME);
		return ret;
	}

	printf("[%s] matrices information sent\n", DRIVER_NAME);

	return 0;
}

int luxyd_dev_matrix_multiply(int fd, void *mmap_ptr,
			      struct matrix_info *matrix_info)
{
	int ret;

	if (!mmap_ptr) {
		printf("[%s] invalid mmap_ptr\n", DRIVER_NAME);
		return -ENOMEM;
	}

	printf("[%s] valid mmap_ptr address %p\n", DRIVER_NAME, mmap_ptr);

	/* perform matrix multiplication */
	printf("[%s] calling LUXYD_AI_MATRIX_MULTIPLY ioctl command\n",
	       DRIVER_NAME);
	ret = ioctl(fd, LUXYD_AI_MATRIX_MULTIPLY, matrix_info);
	if (ret) {
		printf("[%s] LUXYD_AI_MATRIX_MULTIPLY failed\n", DRIVER_NAME);
		return ret;
	}

	printf("[%s] matrices multiplication completed\n", DRIVER_NAME);

	return 0;
}

int luxyd_dev_close(int fd, void *mmap_ptr, int mmap_size)
{
	int ret;

	if (mmap_ptr) {
		ret = munmap(mmap_ptr, mmap_size);

		if (ret < 0)
			printf("[%s] failed to unmap device memory\n",
			       DRIVER_NAME);
		else
			printf("[%s] device memory unmapped\n", DRIVER_NAME);
	}

	close(fd);
	printf("[%s] device closed\n", DRIVER_NAME);

	return 0;
}
