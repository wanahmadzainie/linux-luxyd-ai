/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) 2026, Luxyd Technologies
 */

#ifndef _UAPI_LINUX_LUXYD_IOCTL_H
#define _UAPI_LINUX_LUXYD_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* matrix dimension */
typedef struct {
	__u32 m;		/* rows of matrix A */
	__u32 n;		/* cols of matrix A also rows of matrix B */
	__u32 p;		/* cols of matrix B */
} matrix_config;

/* IOCTL command */
#define LUXYD_IOCTL_MATMUL	_IOWR('L', 1, matrix_config *)
#define LUXYD_IOCTL_GEMV	_IO  ('L', 2)

#endif /* _UAPI_LINUX_LUXYD_IOCTL_H */
