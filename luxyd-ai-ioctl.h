/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) 2025, Luxyd Technologies
 */

#ifndef _UAPI_LINUX_LUXYD_AI_IOCTL_H
#define _UAPI_LINUX_LUXYD_AI_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* IOCTL commands */
#define LUXYD_AI_IOCTL_MAGIC		'L'
#define LUXYD_AI_STATUS_GET		_IOR (LUXYD_AI_IOCTL_MAGIC, 1, __u32)
#define LUXYD_AI_MODEL_LOAD		_IOW (LUXYD_AI_IOCTL_MAGIC, 2, __u32)
#define LUXYD_AI_INFERENCE_START	_IOWR(LUXYD_AI_IOCTL_MAGIC, 3, __u32)
#define LUXYD_AI_MATRIX_LOAD		_IOW (LUXYD_AI_IOCTL_MAGIC, 4, __u32)
#define LUXYD_AI_MATRIX_MULTIPLY	_IOW (LUXYD_AI_IOCTL_MAGIC, 5, __u32)

#define MATRIXA_OFFSET			0x0
#define MATRIXB_OFFSET			0x2000
#define MATRIXP_OFFSET			0x4000

struct matrix_size {
	int m;  /* Matrix A row size, matrix AB row size */
	int n;  /* Matrix A column size, matrix B row size */
	int p;  /* Matrix B column size, matrix AB column size */
};

int luxyd_dev_open(const char *devname);

#endif /* _UAPI_LINUX_LUXYD_AI_IOCTL_H */
