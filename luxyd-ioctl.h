/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) 2026, Luxyd Technologies
 */

#ifndef _UAPI_LINUX_LUXYD_IOCTL_H
#define _UAPI_LINUX_LUXYD_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define QK_K		256

// Kernel-compatible structures (mirroring user-space definitions)
typedef struct {
	__u16 d[8];		// super-block scale for quantized scales
	__u16 dmin[8];		// super-block scale for quantized mins
	__u8 scales[96];	// scales and mins, quantized with 6 bits
	__u8    qs[1024];	// 4-bit quants
} block_q4_Kx8_kernel;

typedef struct {
	float d;		// delta
	__s8 qs[QK_K];		// quants
	__s16 bsums[QK_K/16];	// sum of quants in groups of 16
} block_q8_K_kernel;

/* matrix dimension */
typedef struct {
	__u32 m;		/* rows of matrix A */
	__u32 n;		/* cols of matrix A also rows of matrix B */
	__u32 p;		/* cols of matrix B */
} matrix_config;

/* ggml gemv */
typedef struct {
	int n;			/* number of elements ? */
	size_t bs;		/* block size ? */
	int nr;			/* number of rows ? */
	int nc;			/* number of cols ? */
} gemv_config;

/* IOCTL command */
#define LUXYD_IOCTL_MATMUL	_IOWR('L', 1, matrix_config *)
#define LUXYD_IOCTL_GEMV	_IOWR('L', 2, gemv_config *)

#endif /* _UAPI_LINUX_LUXYD_IOCTL_H */
