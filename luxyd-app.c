#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "luxyd-ioctl.h"

#define DEVICE_PATH	"/dev/luxyd_fpga"
#define MEM_ALIGNMENT	4096

void print_hex_dump(const char *prefix, const void *buf, size_t len)
{
	const unsigned char *p = (const unsigned char *)buf;

	for (size_t i = 0; i < len; i += 16) {
		printf("%s%08zx: ", prefix, i);

		/* Hex */
		for (size_t j = 0; j < 16; j++) {
			if (i + j < len)
				printf("%02x ", p[i + j]);
			else
				printf("   ");
		}

		printf(" ");

		/* ASCII */
		for (size_t j = 0; j < 16; j++) {
			if (i + j < len)
				printf("%c", isprint(p[i + j]) ? p[i + j] : '.');
		}

		printf("\n");
	}
}

void print_matrix_u8(const char *name, size_t rows, size_t cols, __u8 *matrix) {
	printf("--- %s (%zu x %zu) ---\n", name, rows, cols);

	for (size_t i = 0; i < rows; i++) {
		for (size_t j = 0; j < cols; j++) {
			printf("%3u ", matrix[i * cols + j]);
		}
		printf("\n");
	}
	printf("\n");
}

void print_matrix_u16(const char *name, size_t rows, size_t cols, __u16 *matrix) {
	printf("--- %s (%zu x %zu) ---\n", name, rows, cols);

	for (size_t i = 0; i < rows; i++) {
		for (size_t j = 0; j < cols; j++) {
			printf("%5u ", matrix[i * cols + j]);
		}
		printf("\n");
	}
	printf("\n");
}


void print_matrix_u32(const char *name, size_t rows, size_t cols, __u32 *matrix) {
	printf("--- %s (%zu x %zu) ---\n", name, rows, cols);

	for (size_t i = 0; i < rows; i++) {
		for (size_t j = 0; j < cols; j++) {
			printf("%7u ", matrix[i * cols + j]);
		}
		printf("\n");
	}
	printf("\n");
}

__u8 *allocate_matrix_u8(size_t rows, size_t cols) {
	void *ptr = NULL;
	size_t size = rows * cols * sizeof(__u8);

	posix_memalign(&ptr, MEM_ALIGNMENT, size);
	memset(ptr, 0, size);

	return (__u8 *)ptr;
}

__u16 *allocate_matrix_u16(size_t rows, size_t cols) {
	void *ptr = NULL;
	size_t size = rows * cols * sizeof(__u16);

	posix_memalign(&ptr, MEM_ALIGNMENT, size);
	memset(ptr, 0, size);

	return (__u16 *)ptr;
}


__u32 *allocate_matrix_u32(size_t rows, size_t cols) {
	void *ptr = NULL;
	size_t size = rows * cols * sizeof(__u32);

	posix_memalign(&ptr, MEM_ALIGNMENT, size);
	memset(ptr, 0, size);

	return (__u32 *)ptr;
}

void do_matmul_u8(__u8 *a, __u8 *b, __u32 *c, size_t m, size_t n, size_t p) {
	size_t i, j, k;

	for (i = 0; i < m; i++) {
		for (j = 0; j < p; j++) {
			c[i * p + j] = 0;
			for (k = 0; k < n; k++) {
				c[i * p + j] += a[i * n + k] * b[k * p + j];
			}
		}
	}
}

void do_matmul_u16(__u16 *a, __u16 *b, __u32 *c, size_t m, size_t n, size_t p) {
	size_t i, j, k;

	for (i = 0; i < m; i++) {
		for (j = 0; j < p; j++) {
			c[i * p + j] = 0;
			for (k = 0; k < n; k++) {
				c[i * p + j] += a[i * n + k] * b[k * p + j];
			}
		}
	}
}

int main(int argc, char *argv[])
{
	int ret = EXIT_FAILURE;

	printf("%s start\n", argv[0]);

	srand((__u32)time(NULL));

	/* Opening device */
	int fd = open(DEVICE_PATH, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "failed to open %s\n", DEVICE_PATH);
		return fd;
	}

	/* Generate random dimension between 4 and 32 */
	size_t m = (rand() % 13) + 4;
	size_t n = (rand() % 13) + 4;
	size_t p = (rand() % 13) + 4;

	/* Allocate memory */
	__u16 *A = allocate_matrix_u16(m, n);
	__u16 *B = allocate_matrix_u16(n, p);
	__u32 *P = allocate_matrix_u32(m, p);
	__u32 *P_cpu = allocate_matrix_u32(m, p);

	size_t sizeA = m * n * sizeof(__u16);
	size_t sizeB = n * p * sizeof(__u16);
	size_t sizeP = m * p * sizeof(__u32);

	if (!A || !B || !P) {
		perror("failed to allocate memory\n");
		goto cleanup;
	}

	for (size_t i = 0; i < m * n; i++)
		A[i] = rand() % 256;

	for (size_t i = 0; i < n * p; i++)
		B[i] = rand() % 256;

	/* Send matrix config */
	matrix_config config = {
		.m = m,
		.n = n,
		.p = p
	};

	printf("Sending matrix configuration...\n");
	ret = ioctl(fd, LUXYD_IOCTL_MATMUL, &config);
	if (ret < 0) {
		perror("failed to send ioctl LUXYD_IOCTL_MATMUL\n");
		goto cleanup;
	}

	printf("matrix configuration sent\n");

	/* Send matrix A */
	printf("Sending matrix A (%lu bytes)...\n", sizeA);
	ret = write(fd, A, sizeA);
	if (ret < 0) {
		perror("failed to write data\n");
		goto cleanup;
	}

	if (ret != sizeA) {
		fprintf(stderr, "incomplete: wrote %d of %lu bytes\n",
			ret, sizeA);
		ret = -EIO;
		goto cleanup;
	}

	printf("%u bytes of data sent\n", ret);

	/* Send matrix B */
	printf("Sending matrix B (%lu bytes)...\n", sizeB);
	ret = write(fd, B, sizeB);
	if (ret < 0) {
		perror("failed to write data\n");
		goto cleanup;
	}

	if (ret != sizeB) {
		fprintf(stderr, "incomplete: wrote %d of %lu bytes\n",
			ret, sizeB);
		ret = -EIO;
                goto cleanup;
        }

        printf("%u bytes of data sent\n", ret);

	/* Read result */
	printf("Receiving matrix P (%lu bytes)...\n", sizeP);
	ret = read(fd, P, sizeP);
	if (ret < 0) {
		perror("failed to read data\n");
		goto cleanup;
	}

	if (ret != sizeP) {
		fprintf(stderr, "incomplete: read %d of %lu bytes\n",
			ret, sizeP);
		ret = -EIO;
		goto cleanup;
	}

	printf("%u bytes of data received\n", ret);

	/* Compare result */
	int mismatch = 0;

	do_matmul_u16(A, B, P_cpu, m, n, p);

	for (size_t i = 0; i < m * p; i++) {
		if (P[i] != P_cpu[i]) {
			mismatch = 1;
			break;
		}
	}

	/* Display them */
	//print_matrix_u16("Matrix A", m, n, A);
	//print_matrix_u16("Matrix B", n, p, B);
	//print_matrix_u32("Matrix P", m, p, P);
	//print_matrix_u32("Matrix P_cpu", m, p, P_cpu);

	if (mismatch) {
                printf("Verification failed: FPGA and CPU results do not matched\n");
                ret = EXIT_FAILURE;
                goto cleanup;
        } else {
		printf("Verification success: FPGA and CPU results matched\n");
	}

	ret = EXIT_SUCCESS;

cleanup:
	if (A)
		free(A);

	if (B)
		free(B);

	if (P)
		free(P);

	if (P_cpu)
		free(P_cpu);

	if (fd)
		close(fd);

	printf("%s end\n", argv[0]);
	return ret;
}
