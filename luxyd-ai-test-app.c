#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "luxyd-ai-ioctl.h"

#define LUXYD_AI_DEVICE		"/dev/luxyd-ai"
#define BUFSIZE			(16*1024*1024)

int main(void)
{
	int fd;
	unsigned int val;
	unsigned long page_size;
	void *buf;
	size_t bufsize;
	int ret = 0;

	printf("\n----- LUXYD AI Test Application -----\n");

	printf("[%s] Opening device\n", LUXYD_AI_DEVICE);
	fd = open(LUXYD_AI_DEVICE, O_RDWR);
	if (fd < 0) {
		printf("[%s] Cannot open device file\n", LUXYD_AI_DEVICE);
		return fd;
	}

	/* Align to PAGE_SIZE */
	page_size = sysconf(_SC_PAGE_SIZE);
	bufsize = (BUFSIZE + page_size - 1) & ~(page_size - 1);

	buf = mmap(NULL, bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (buf == MAP_FAILED) {
		printf("[%s] Failed to mmap device memory\n", LUXYD_AI_DEVICE);
		goto err;
	}

	printf("[%s] LUXYD_AI_STATUS_GET test start\n", LUXYD_AI_DEVICE);
	ret = ioctl(fd, LUXYD_AI_STATUS_GET, &val);
	if (ret) {
		printf("[%s] LUXYD_AI_STATUS_GET test failed (%d)\n",
		       LUXYD_AI_DEVICE, ret);
		goto err;
	}

	printf("[%s] LUXYD_AI_STATUS_GET test success, received 0x%08x\n",
	       LUXYD_AI_DEVICE, val);

	printf("[%s] LUXYD_AI_MODEL_LOAD test start\n", LUXYD_AI_DEVICE);
	ret = ioctl(fd, LUXYD_AI_MODEL_LOAD, &val);
	if (ret) {
		printf("[%s] LUXYD_AI_MODEL_LOAD test failed (%d)\n",
		       LUXYD_AI_DEVICE, ret);
		goto err;
	}

	printf("[%s] LUXYD_AI_MODEL_LOAD test success\n", LUXYD_AI_DEVICE);

	printf("[%s] LUXYD_AI_INFERENCE_START test start\n", LUXYD_AI_DEVICE);
	ret = ioctl(fd, LUXYD_AI_INFERENCE_START, &val);
	if (ret) {
		printf("[%s] LUXYD_AI_INFERENCE_START test failed (%d)\n",
		       LUXYD_AI_DEVICE, ret);
		goto err;
	}

	printf("[%s] LUXYD_AI_INFERENCE_START test success\n", LUXYD_AI_DEVICE);

err:
	if (buf) {
		ret = munmap(buf, bufsize);

		if (ret < 0)
			printf("[%s] Failed to unmap memory\n", LUXYD_AI_DEVICE);
	}

	/* Clean up */
	printf("[%s] Closing device\n", LUXYD_AI_DEVICE);
	close(fd);

	return ret;
}
