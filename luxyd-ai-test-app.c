#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "luxyd-ai-ioctl.h"

#define LUXYD_AI_DEVICE		"/dev/luxyd-ai"
#define BUFSIZE			(16*1024*1024)

__u16 a[32*32];
__u16 b[32*32];
__u32 p[32*32];

int main(void)
{
	int fd;
	unsigned int val;
	unsigned long page_size;
	void *buf;
	size_t bufsize;
	int ret = 0;

	struct matrix_size matrix_size;
	__u16 *a_matrix, *b_matrix;
	__u32 *p_matrix;
	int i, j;

	/* Set the row and column size for matrix A, B and P */
	matrix_size.m = 32;
	matrix_size.n = 32;
	matrix_size.p = 32;

	printf("\n----- LUXYD AI Test Application -----\n");

	printf("[%s] Opening device\n", LUXYD_AI_DEVICE);
	fd = luxyd_dev_open(LUXYD_AI_DEVICE);
	if (fd < 0)
		return fd;

	/* Align to PAGE_SIZE */
	page_size = sysconf(_SC_PAGE_SIZE);
	bufsize = (BUFSIZE + page_size - 1) & ~(page_size - 1);

	buf = mmap(NULL, bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (buf == MAP_FAILED) {
		printf("[%s] Failed to mmap device memory\n", LUXYD_AI_DEVICE);
		goto err;
	}

	a_matrix = (__u16 *)(buf + MATRIXA_OFFSET);
	b_matrix = (__u16 *)(buf + MATRIXB_OFFSET);
	p_matrix = (__u32 *)(buf + MATRIXP_OFFSET);
	memset(buf, 0, bufsize);

	/* Fill up matrix A (mxn)*/
	for (i = 0; i < matrix_size.m; i++) {
		for (j = 0; j < matrix_size.n; j++) {
			a[i * matrix_size.n + j] = (i + 1) * 2 + (j + 1);
			a_matrix[i * matrix_size.n + j] = (i + 1) * 2 + (j + 1);
		}
	}

	/* Fill up matrix B (nxp) */
	for (i = 0; i < matrix_size.n; i++) {
		for (j = 0; j < matrix_size.p; j++) {
			b[i * matrix_size.p + j] = (i + 1) * 4 + (j + 1);
			b_matrix[i * matrix_size.p + j] = (i + 1) * 4 + (j + 1);
		}
	}

	/* Send matrix size information */
	printf("[%s] LUXYD_AI_MATRIX_LOAD sending command\n", LUXYD_AI_DEVICE);
	ret = ioctl(fd, LUXYD_AI_MATRIX_LOAD, &matrix_size);
	if (ret) {
		printf("[%s] LUXYD_AI_MATRIX_LOAD failed\n", LUXYD_AI_DEVICE);
		goto err;
	}

	printf("[%s] Matrices size information are sent successfully\n",
	       LUXYD_AI_DEVICE);

	/* Trigger multiplication */
	printf("[%s] LUXYD_AI_MATRIX_MULTIPLY sending command\n",
	       LUXYD_AI_DEVICE);
	ret = ioctl(fd, LUXYD_AI_MATRIX_MULTIPLY);
	if (ret) {
		printf("[%s] LUXYD_AI_MATRIX_MULTIPLY failed\n",
		       LUXYD_AI_DEVICE);
		goto err;
	}

	printf("[%s] LUXYD_AI_MATRIX_MULTIPLY success\n", LUXYD_AI_DEVICE);

	/* Print the result */
	printf("\nMatrix A (%dx%d):\n", matrix_size.m, matrix_size.n);
	for (i = 0; i < matrix_size.m; i++) {
		for (j = 0; j < matrix_size.n; j++) {
			printf("%5d ", a_matrix[i * matrix_size.n + j]);
		}
		printf("\n");
	}

	printf("\nMatrix B (%dx%d):\n", matrix_size.n, matrix_size.p);
	for (i = 0; i < matrix_size.n; i++) {
		for (j = 0; j < matrix_size.p; j++) {
			printf("%5d ", b_matrix[i * matrix_size.p + j]);
		}
		printf("\n");
	}

	printf("\nMatrix P (%dx%d):\n", matrix_size.m, matrix_size.p);
	for (i = 0; i < matrix_size.m; i++) {
		for (j = 0; j < matrix_size.p; j++) {
			printf("%5d ", p_matrix[i * matrix_size.p + j]);
		}
		printf("\n");
	}

	printf("\n");



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
