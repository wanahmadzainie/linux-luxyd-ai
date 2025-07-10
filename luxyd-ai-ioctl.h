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

#endif /* _UAPI_LINUX_LUXYD_AI_IOCTL_H */
