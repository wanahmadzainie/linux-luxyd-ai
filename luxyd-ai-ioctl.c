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
		printf("[%s] cannot open device file %s\n", DRIVER_NAME, devname);
	else
		printf("[%s] device file %s opened\n", DRIVER_NAME, devname);

	return fd;
}

void *luxyd_dev_init(int fd, int *mmap_size)
{
	unsigned long page_size;
	void *mmap_ptr;

	page_size = sysconf(_SC_PAGE_SIZE);
	*mmap_size = (*mmap_size + page_size - 1) & ~(page_size - 1);

	mmap_ptr = mmap(NULL, *mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mmap_ptr == MAP_FAILED) {
		printf("[%s] failed to mmap device memory\n", DRIVER_NAME);
		return NULL;
	}

	printf("[%s] device memory mapped\n", DRIVER_NAME);

	return mmap_ptr;
}

int luxyd_dev_matrix_load(int fd, void *mmap_ptr, struct matrix_info *matrix_info)
{
	int size;
	int ret;

	/* Processing Matrix A */
	size = matrix_info->m * matrix_info->n * sizeof(__u16);
	memcpy(mmap_ptr + MAT_A_OFFSET, matrix_info->a_ptr, size);

	/* Processing Matrix B */
	memcpy(mmap_ptr + MAT_B_OFFSET, matrix_info->b_ptr, size);

	/* Update the pointers */
	matrix_info->a_ptr = mmap_ptr + MAT_A_OFFSET;
	matrix_info->b_ptr = mmap_ptr + MAT_B_OFFSET;
	matrix_info->p_ptr = mmap_ptr + MAT_P_OFFSET;

	/* Sending matrix information to kernel space */
	ret = ioctl(fd, LUXYD_AI_MATRIX_LOAD, matrix_info);
	if (ret) {
		printf("[%s] LUXYD_AI_MATRIX_LOAD failed\n", DRIVER_NAME);
		return ret;
	}

	printf("[%s] matrices information sent\n", DRIVER_NAME);

	return 0;
}

int luxyd_dev_matrix_multiply(int fd, void *mmap_ptr, struct matrix_info *matrix_info)
{
	return 0;
}

int luxyd_dev_close(int fd, void *mmap_ptr, int mmap_size)
{
	int ret;

	printf("[%s] cleaning up\n", DRIVER_NAME);
	if (mmap_ptr)
		ret = munmap(mmap_ptr, mmap_size);
	if (ret < 0)
		printf("[%s] failed to unmap memory\n", DRIVER_NAME);
	else
		printf("[%s] memory unmapped\n", DRIVER_NAME);

	printf("[%s] closing device\n", DRIVER_NAME);
	close(fd);

	return 0;
}
