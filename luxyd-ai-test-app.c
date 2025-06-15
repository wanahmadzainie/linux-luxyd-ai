#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define LUXYD_AI_DEVICE		"/dev/luxyd-ai"

/* IOCTL commands */
#define LUXYD_AI_IOCTL_MAGIC            'L'
#define LUXYD_AI_LOAD_MODEL             _IOW (LUXYD_AI_IOCTL_MAGIC, 1, unsigned int)
#define LUXYD_AI_START_INFERENCE        _IOWR(LUXYD_AI_IOCTL_MAGIC, 2, unsigned int)
#define LUXYD_AI_GET_STATUS             _IOR (LUXYD_AI_IOCTL_MAGIC, 3, unsigned int)
#define LUXYD_AI_LOAD_MATRIX		_IOW (LUXYD_AI_IOCTL_MAGIC, 4, struct matrix_size)
#define LUXYD_AI_MULTIPLY_MATRIX	_IO  (LUXYD_AI_IOCTL_MAGIC, 5)

struct matrix_size {
	int m;  /* Matrix A row size, matrix AB row size */
	int n;  /* Matrix A column size, matrix B row size */
	int p;  /* Matrix B column size, matrix AB column size */
};

int main(void)
{
	int fd;
	unsigned int val;
	unsigned long page_size;
	void *buf;
	size_t bufsize;
	struct matrix_size matrix_size;
	int a_size, b_size, p_size;
	unsigned short *a_matrix, *b_matrix;
	unsigned int *p_matrix;
	int i, j;
	int ret = 0;

	/* Set the row and column size for matrix A, B and P */
	matrix_size.m = 20;
	matrix_size.n = 20;
	matrix_size.p = 20;

	/* Calculate the required memory size */
	a_size = matrix_size.m * matrix_size.n * sizeof(unsigned short);
	b_size = matrix_size.n * matrix_size.p * sizeof(unsigned short);
	p_size = matrix_size.m * matrix_size.p * sizeof(unsigned int);
	bufsize = a_size + b_size + p_size;
	printf("Required size for all matrices:%ld\n", bufsize);

	printf("\n----- LUXYD AI Test Application -----\n");

	printf("[%s] Opening device\n", LUXYD_AI_DEVICE);
	fd = open(LUXYD_AI_DEVICE, O_RDWR);
	if (fd < 0) {
		printf("[%s] Cannot open device file\n", LUXYD_AI_DEVICE);
		return fd;
	}

	/* Aligned to PAGE SIZE */
	page_size = sysconf(_SC_PAGE_SIZE);
	bufsize = (bufsize + page_size - 1) & ~(page_size - 1);

	buf = mmap(NULL, bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (buf == MAP_FAILED) {
		printf("[%s] Failed to mmap device memory\n", LUXYD_AI_DEVICE);
		goto err;
	}

	printf("[%s] %zu bytes mapped at address %p\n",
	       LUXYD_AI_DEVICE, bufsize, buf);

	a_matrix = (unsigned short *)buf;
	b_matrix = (unsigned short *)(buf + a_size);
	p_matrix = (unsigned int *)(buf + a_size + b_size);

	memset(buf, 0, bufsize);

	/* Fill up matrix A (mxn)*/
	for (i = 0; i < matrix_size.m; i++) {
		for (j = 0; j < matrix_size.n; j++) {
			a_matrix[i * matrix_size.n + j] = (i + 1) * 2 + (j + 1);
		}
	}

	/* Fill up matrix B (nxp) */
	for (i = 0; i < matrix_size.n; i++) {
		for (j = 0; j < matrix_size.p; j++) {
			b_matrix[i * matrix_size.p + j] = (i + 1) * 4 + (j + 1);
		}
	}

	/* Send matrix size information */
	ret = ioctl(fd, LUXYD_AI_LOAD_MATRIX, &matrix_size);
	if (ret) {
		printf("[%s] LUXYD_AI_LOAD_MATRIX failed\n",
		       LUXYD_AI_DEVICE);
		goto err_matrix;
	}

	printf("[%s] Matrices size information are sent successfully\n",
	       LUXYD_AI_DEVICE);

	/* Trigger multiplication */
	printf("[%s] LUXYD_AI_MULTIPLY_MATRIX sending command\n",
	       LUXYD_AI_DEVICE);
	ret = ioctl(fd, LUXYD_AI_MULTIPLY_MATRIX);
	if (ret) {
		printf("[%s] LUXYD_AI_MULTIPLY_MATRIX failed\n",
		       LUXYD_AI_DEVICE);
		goto err_matrix;
	}

	printf("[%s] LUXYD_AI_MULTIPLY_MATRIX success\n", LUXYD_AI_DEVICE);

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

	printf("[%s] LUXYD_AI_GET_STATUS test start\n", LUXYD_AI_DEVICE);
	ret = ioctl(fd, LUXYD_AI_GET_STATUS, &val);
	if (ret) {
		printf("[%s] LUXYD_AI_GET_STATUS test failed (%d)\n",
		       LUXYD_AI_DEVICE, ret);
		goto err;
	}

	printf("[%s] LUXYD_AI_GET_STATUS test success, received data 0x%08x\n",
	       LUXYD_AI_DEVICE, val);

	printf("[%s] LUXYD_AI_LOAD_MODEL test start\n", LUXYD_AI_DEVICE);
	ret = ioctl(fd, LUXYD_AI_LOAD_MODEL, &val);
	if (ret) {
		printf("[%s] LUXYD_AI_LOAD_MODEL test failed (%d)\n",
		       LUXYD_AI_DEVICE, ret);
		goto err;
	}

	printf("[%s] LUXYD_AI_LOAD_MODEL test success\n", LUXYD_AI_DEVICE);

	printf("[%s] LUXYD_AI_START_INFERENCE test start\n", LUXYD_AI_DEVICE);
	ret = ioctl(fd, LUXYD_AI_START_INFERENCE, &val);
	if (ret) {
		printf("[%s] LUXYD_AI_START_INFERENCE test failed (%d)\n",
		       LUXYD_AI_DEVICE, ret);
		goto err;
	}

	printf("[%s] LUXYD_AI_START_INFERENCE test success\n", LUXYD_AI_DEVICE);

err_matrix:
	ret = munmap(buf, bufsize);
	if (ret < 0)
		printf("[%s] Failed to unmap memory\n", LUXYD_AI_DEVICE);

err:
	/* Clean up */
	printf("[%s] Closing device\n", LUXYD_AI_DEVICE);
	close(fd);

	return ret;
}
