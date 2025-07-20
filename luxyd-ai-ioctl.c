// SPDX-License-Identifier: GPL-2.0

#include <fcntl.h>
#include <stdio.h>

int luxyd_dev_open(const char *devname)
{
	int fd;

	fd = open(devname, O_RDWR);
	if (fd < 0)
		printf("[%s] Cannot open device file\n", devname);
	else
		printf("[%s] Device file opened\n", devname);

	return fd;
}
