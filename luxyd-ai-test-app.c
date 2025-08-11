#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "luxyd-ai-ioctl.h"

#define LUXYD_AI_DEVICE		"/dev/luxyd-ai"
#define MATRIX_SIZE_MAX		(4096 * sizeof(__u16) * 2 + 4096 * sizeof(__u32))

void display_matrix16(int row, int col, __u16 *data)
{
	int i, j;
	int pos;

        for (i = 0; i < row; i++) {
		for (j = 0; j < col; j++) {
			pos = i * col + j;
                        printf("%5d ", data[pos]);
		}
		printf("\n");
	}
}

void display_matrix32(int row, int col, __u32 *data)
{
	int i, j;
	int pos;

	for (i = 0; i < row; i++) {
		for (j = 0; j < col; j++) {
			pos = i * col + j;
			printf("%5d ", data[pos]);
		}
		printf("\n");
	}
}

int main(void)
{
	struct matrix_info matrix_info;
	void *mmap_ptr;
	int mmap_size;
	int fd;
	int size;
	__u16 *a_data, *b_data;
	__u32 *p_data;
	int i, j;
	int ret = 0;

	printf("\n----- LUXYD AI Test Application -----\n");

	/* Open device file, for using IOCTL */
	fd = luxyd_dev_open(LUXYD_AI_DEVICE);
        if (fd < 0)
                return fd;

        /* Map the memory from kernel space */
	mmap_size = MATRIX_SIZE_MAX;
        mmap_ptr = luxyd_dev_init(fd, &mmap_size);
        if (!mmap_ptr)
                goto err;

	/*
	 * Initialize matrices information
	 * A[mxn] * B[nxp] = P[m*p]
	 */
	matrix_info.m = 16;
	matrix_info.n = 16;
	matrix_info.p = 16;
	matrix_info.addr_a = (__u64)(mmap_ptr + MAT_A_OFFSET);
	matrix_info.addr_b = (__u64)(mmap_ptr + MAT_B_OFFSET);
	matrix_info.addr_p = (__u64)(mmap_ptr + MAT_P_OFFSET);

	/* Allocate memory for matrices */
	size = matrix_info.m * matrix_info.n * sizeof(__u16);
	a_data = malloc(size);
	size = matrix_info.n * matrix_info.p * sizeof(__u16);
	b_data = malloc(size);
	size = matrix_info.m * matrix_info.p * sizeof(__u32);
	p_data = malloc(size);

	/* Fill up Matrix A and Matrix B with test data */
	for (i = 0; i < matrix_info.m; i++) {
		for (j = 0; j < matrix_info.n; j++) {
			a_data[i * matrix_info.n + j] = (i + 1) * 2 + (j + 1);
		}
	}

	for (i = 0; i < matrix_info.n; i++) {
		for (j = 0; j < matrix_info.p; j++) {
			b_data[i * matrix_info.p + j] = (i + 1) * 4 + (j + 1);
		}
	}

	/* Copy matrices data to device mmaped memory */
	size = matrix_info.m * matrix_info.n * sizeof(__u16);
	memcpy((void *)matrix_info.addr_a, a_data, size);
	size = matrix_info.n * matrix_info.p * sizeof(__u16);
	memcpy((void *)matrix_info.addr_b, b_data, size);

	/* Display Matrix A and Matrix B test data */
	printf("\nInitial matrices:\n");
	printf("Matrix A (%dx%d):\n", matrix_info.m, matrix_info.n);
        display_matrix16(matrix_info.m, matrix_info.n, (__u16 *)matrix_info.addr_a);
        printf("Matrix B (%dx%d):\n", matrix_info.n, matrix_info.p);
        display_matrix16(matrix_info.n, matrix_info.p, (__u16 *)matrix_info.addr_b);
	printf("Matrix P (%dx%d):\n", matrix_info.m, matrix_info.p);
	display_matrix32(matrix_info.m, matrix_info.p, (__u32 *)matrix_info.addr_p);

	/* Send matrix size information and their memory location */
	ret = luxyd_dev_matrix_load(fd, mmap_ptr, &matrix_info);
	if (ret < 0)
		goto err;

	/* For testing purpose, we clear the data in device mapped memory */
	//sleep(1); memset(mmap_ptr, 0, mmap_size); sleep(1);

	/* Send command to start multiplication and get result as Matrix P */
	ret = luxyd_dev_matrix_multiply(fd, mmap_ptr, &matrix_info);
	if (ret < 0)
		goto err;

	/* Display the result */
	printf("\nResult:\n");
	printf("Matrix A (%dx%d):\n", matrix_info.m, matrix_info.n);
	display_matrix16(matrix_info.m, matrix_info.n, (__u16 *)matrix_info.addr_a);
	printf("Matrix B (%dx%d):\n", matrix_info.n, matrix_info.p);
	display_matrix16(matrix_info.n, matrix_info.p, (__u16 *)matrix_info.addr_b);
	printf("Matrix P (%dx%d):\n", matrix_info.m, matrix_info.p);
	display_matrix32(matrix_info.m, matrix_info.p, (__u32 *)matrix_info.addr_p);

err:
	/* Dealloc memory used by the matrices */
	if (a_data)
		free(a_data);

	if (b_data)
		free(b_data);

	if (p_data)
		free(p_data);

	luxyd_dev_close(fd, mmap_ptr, mmap_size);

	return ret;
}
