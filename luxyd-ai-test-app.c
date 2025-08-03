#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "luxyd-ai-ioctl.h"

#define LUXYD_AI_DEVICE		"/dev/luxyd-ai"

int main(void)
{
	struct matrix_info matrix_info;
	int fd;
	unsigned int val;
	void *buf;
	int bufsize;
	int size;
	int i, j;
	int ret = 0;

	/* Row and column size of Matrix A, B and P */
	matrix_info.m = 8;
	matrix_info.n = 8;
	matrix_info.p = 8;

	/* Fill up matrix A */
	size = matrix_info.m * matrix_info.n * sizeof(__u16);
	matrix_info.a_ptr = malloc(size);

	for (i = 0; i < matrix_info.m; i++) {
		for (j = 0; j < matrix_info.n; j++) {
			matrix_info.a_ptr[i * matrix_info.n + j] = (i + 1) * 2 + (j + 1);
		}
	}


	/* Fill up matrix B */
	size = matrix_info.n * matrix_info.p * sizeof(__u16);
	matrix_info.b_ptr = malloc(size);

	for (i = 0; i < matrix_info.n; i++) {
		for (j = 0; j < matrix_info.p; j++) {
			matrix_info.b_ptr[i * matrix_info.p + j] = (i + 1) * 4 + (j + 1);
		}
	}


	printf("\n----- LUXYD AI Test Application -----\n");

	fd = luxyd_dev_open(LUXYD_AI_DEVICE);
	if (fd < 0)
		return fd;

	bufsize = (32 * 32 * 3 * sizeof(__u32));
	buf = luxyd_dev_init(fd, &bufsize);
	if (!buf)
		goto err;

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
	luxyd_dev_close(fd, buf, bufsize);

	return ret;
}
